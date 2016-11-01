#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <glib.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sapi/tpm20.h>
#include <tabd.h>
#include "tabd-priv.h"
#include "logging.h"
#include "data-message.h"
#include "thread-interface.h"
#include "session-manager.h"
#include "command-source.h"
#include "session-data.h"
#include "response-sink.h"
#include "source-interface.h"
#include "tab.h"
#include "tabd-generated.h"
#include "tcti-options.h"

#ifdef G_OS_UNIX
#include <gio/gunixfdlist.h>
#endif

/* Structure to hold data that we pass to the gmain loop as 'user_data'.
 * This data will be available to events from gmain including events from
 * the DBus.
 */
typedef struct gmain_data {
    GMainLoop *loop;
    Tpm2AccessBroker       *skeleton;
    SessionManager         *manager;
    CommandSource         *command_source;
    Sink                   *response_sink;
    Tab                    *tab;
    struct drand48_data     rand_data;
    GMutex                  init_mutex;
    Tcti                   *tcti;
} gmain_data_t;

/* This global pointer to the GMainLoop is necessary so that we can react to
 * unix signals. Only the signal handler should touch this.
 */
static GMainLoop *g_loop;

/**
 * This is a utility function that builds an array of handles as a
 * GVariant object. The handles that make up the array are passed in
 * as a GUnixFDList.
 */
static GVariant*
handle_array_variant_from_fdlist (GUnixFDList *fdlist)
{
    GVariant *tuple;
    GVariantBuilder *builder;
    gint i = 0;

    /* build array of handles as GVariant */
    builder = g_variant_builder_new (G_VARIANT_TYPE ("ah"));
    for (i = 0; i < g_unix_fd_list_get_length (fdlist); ++i)
        g_variant_builder_add (builder, "h", i);
    /* create tuple variant from builder */
    tuple = g_variant_new ("ah", builder);
    g_variant_builder_unref (builder);

    return tuple;
}

/**
 * This is a signal handler for the handle-create-connection signal from
 * the Tpm2AccessBroker DBus interface. This signal is triggered by a
 * request from a client to create a new connection with the tabd. This
 * requires a few things be done:
 * - Create a new ID (uint64) for the connection.
 * - Create a new SessionData object getting the FDs that must be returned
 *   to the client.
 * - Build up a dbus response to the client with their session ID and
 *   send / receive FDs.
 * - Send the response message back to the client.
 * - Insert the new SessionData object into the SessionManager.
 * - Notify the CommandSource of the new SessionData that it needs to
 *   watch by writing a magic value to the wakeup_send_fd.
 */
static gboolean
on_handle_create_connection (Tpm2AccessBroker      *skeleton,
                             GDBusMethodInvocation *invocation,
                             gpointer               user_data)
{
    gmain_data_t *data = (gmain_data_t*)user_data;
    SessionData *session = NULL;
    gint client_fds[2] = { 0, 0 }, ret = 0;
    GVariant *response_variants[2], *response_tuple;
    GUnixFDList *fd_list = NULL;
    guint64 id;

    /* make sure the init thread is done before we create new connections
     */
    g_mutex_lock (&data->init_mutex);
    g_mutex_unlock (&data->init_mutex);
    lrand48_r (&data->rand_data, &id);
    session = session_data_new (&client_fds[0], &client_fds[1], id);
    if (session == NULL)
        g_error ("Failed to allocate new session.");
    g_debug ("Created connection with fds: %d, %d and id: %ld",
             client_fds[0], client_fds[1], id);
    /* prepare tuple variant for response message */
    fd_list = g_unix_fd_list_new_from_array (client_fds, 2);
    response_variants[0] = handle_array_variant_from_fdlist (fd_list);
    response_variants[1] = g_variant_new_uint64 (id);
    response_tuple = g_variant_new_tuple (response_variants, 2);
    /* send response */
    g_dbus_method_invocation_return_value_with_unix_fd_list (
        invocation,
        response_tuple,
        fd_list);
    g_object_unref (fd_list);
    /* add session to manager */
    ret = session_manager_insert (data->manager, session);
    if (ret != 0)
        g_error ("Failed to add new session to session_manager.");

    return TRUE;
}
/**
 * This is a signal handler for the Cancel event emitted by the
 * Tpm2AcessBroker. It is invoked by a signal generated by a user
 * requesting that an outstanding TPM command should be canceled. It is
 * registered with the Tpm2AccessBroker in response to acquiring a name
 * on the dbus (on_name_acquired). It does X things:
 * - Ensure the init thread has completed successfully by locking and then
 *   unlocking the init mutex.
 * - Locate the SessionData object associted with the 'id' parameter in
 *   the SessionManager.
 * - If the session has a command being processed by the tabd then it's
 *   removed from the processing queue.
 * - If the session has a command being processed by the TPM then the
 *   request to cancel the command will be sent down to the TPM.
 * - If the session has no commands outstanding then an error is
 *   returned.
 */
static gboolean
on_handle_cancel (Tpm2AccessBroker      *skeleton,
                  GDBusMethodInvocation *invocation,
                  gint64                 id,
                  gpointer               user_data)
{
    gmain_data_t *data = (gmain_data_t*)user_data;
    SessionData *session = NULL;
    GVariant *uint32_variant, *tuple_variant;

    g_info ("on_handle_cancel for id 0x%x", id);
    g_mutex_lock (&data->init_mutex);
    g_mutex_unlock (&data->init_mutex);
    session = session_manager_lookup_id (data->manager, id);
    if (session == NULL) {
        g_warning ("no active session for id: 0x%x", id);
        return FALSE;
    }
    g_info ("canceling command for session 0x%x", session);
    /* cancel any existing commands for the session */
    /* setup and send return value */
    uint32_variant = g_variant_new_uint32 (TSS2_RC_SUCCESS);
    tuple_variant = g_variant_new_tuple (&uint32_variant, 1);
    g_dbus_method_invocation_return_value (invocation, tuple_variant);

    return TRUE;
}
/**
 * This is a signal handler for the handle-set-locality signal from the
 * Tpm2AccessBroker DBus interface. This signal is triggered by a request
 * from a client to set the locality for TPM commands associated with the
 * session (the 'id' parameter). This requires a few things be done:
 * - Ensure the initialization thread has completed successfully by
 *   locking and unlocking the init_mutex.
 * - Find the SessionData object associated with the 'id' parameter.
 * - Set the locality for the SessionData object.
 * - Pass result of the operation back to the user.
 */
static gboolean
on_handle_set_locality (Tpm2AccessBroker      *skeleton,
                        GDBusMethodInvocation *invocation,
                        gint64                 id,
                        guint8                 locality,
                        gpointer               user_data)
{
    gmain_data_t *data = (gmain_data_t*)user_data;
    SessionData *session = NULL;
    GVariant *uint32_variant, *tuple_variant;

    g_info ("on_handle_set_locality for id 0x%x", id);
    g_mutex_lock (&data->init_mutex);
    g_mutex_unlock (&data->init_mutex);
    session = session_manager_lookup_id (data->manager, id);
    if (session == NULL) {
        g_warning ("no active session for id: 0x%x", id);
        return FALSE;
    }
    g_info ("setting locality for session 0x%x to: 0x%x",
            session, locality);
    /* set locality for an existing session */
    /* setup and send return value */
    uint32_variant = g_variant_new_uint32 (TSS2_RC_SUCCESS);
    tuple_variant = g_variant_new_tuple (&uint32_variant, 1);
    g_dbus_method_invocation_return_value (invocation, tuple_variant);

    return TRUE;
}
/**
 * This is a signal handler of type GBusAcquiredCallback. It is registered
 * by the g_bus_own_name function and invoked then a connectiont to a bus
 * is acquired in response to a request for the parameter 'name'.
 */
static void
on_bus_acquired (GDBusConnection *connection,
                 const gchar     *name,
                 gpointer         user_data)
{
    g_info ("on_bus_acquired: %s", name);
}
/**
 * This is a signal handler of type GBusNameAcquiredCallback. It is
 * registered by the g_bus_own_name function and invoked when the parameter
 * 'name' is acquired on the requested bus. It does 3 things:
 * - Obtains a new Tpm2AccessBroker instance and stores a reference in
 *   the 'user_data' parameter (which is a reference to the gmain_data_t.
 * - Register signal handlers for the CreateConnection, Cancel and
 *   SetLocality signals.
 * - Export the Tpm2AccessBroker interface (skeleton) on the DBus
 *   connection.
 */
static void
on_name_acquired (GDBusConnection *connection,
                  const gchar     *name,
                  gpointer         user_data)
{
    g_info ("on_name_acquired: %s", name);
    guint registration_id;
    gmain_data_t *gmain_data;
    GError *error = NULL;
    gboolean ret;

    if (user_data == NULL)
        g_error ("bus_acquired but user_data is NULL");
    gmain_data = (gmain_data_t*)user_data;
    if (gmain_data->skeleton == NULL)
        gmain_data->skeleton = tpm2_access_broker_skeleton_new ();
    g_signal_connect (gmain_data->skeleton,
                      "handle-create-connection",
                      G_CALLBACK (on_handle_create_connection),
                      user_data);
    g_signal_connect (gmain_data->skeleton,
                      "handle-cancel",
                      G_CALLBACK (on_handle_cancel),
                      user_data);
    g_signal_connect (gmain_data->skeleton,
                      "handle-set-locality",
                      G_CALLBACK (on_handle_set_locality),
                      user_data);
    ret = g_dbus_interface_skeleton_export (
        G_DBUS_INTERFACE_SKELETON (gmain_data->skeleton),
        connection,
        TAB_DBUS_PATH,
        &error);
    if (ret == FALSE)
        g_warning ("failed to export interface: %s", error->message);
}
/**
 * This is a simple function to do sanity checks before calling
 * g_main_loop_quit.
 */
static void
main_loop_quit (GMainLoop *loop)
{
    g_info ("main_loop_quit");
    if (loop && g_main_loop_is_running (loop))
        g_main_loop_quit (loop);
}
/**
 * This is a signal handler of type GBusNameLostCallback. It is
 * registered with the g_dbus_own_name function and is invoked when the
 * parameter 'name' is lost on the requested bus. It does one thing:
 * - Ends the GMainLoop.
 */
static void
on_name_lost (GDBusConnection *connection,
              const gchar     *name,
              gpointer         user_data)
{
    g_info ("on_name_lost: %s", name);

    gmain_data_t *data = (gmain_data_t*)user_data;
    main_loop_quit (data->loop);
}
/**
 * This is a very poorly named signal handling function. It is invoked in
 * response to a Unix signal. It does one thing:
 * - Shuts down the GMainLoop.
 */
static void
signal_handler (int signum)
{
    g_info ("handling signal");
    /* this is the only place the global poiner to the GMainLoop is accessed */
    main_loop_quit (g_loop);
}
/**
 * This function seeds the parameter 'rand_data' (state object for the
 * rand48_r functions)with entropy from the parameter 'fname' file. It
 * does 3 things:
 * - Opens the parameter 'file' read only.
 * - Reads sizeof (long int) bytes of data (presumably entropy) from it.
 * - Uses the seed data to initialize the 'rand_data' state object.
 */
static int
seed_rand_data (const char *fname, struct drand48_data *rand_data)
{
    gint rand_fd;
    long int rand_seed;
    ssize_t read_ret;

    /* seed rand with some entropy from TABD_RAND_FILE */
    g_debug ("opening entropy source: %s", fname);
    rand_fd = open (fname, O_RDONLY);
    if (rand_fd == -1) {
        g_warning ("failed to open entropy source %s: %s",
                   fname,
                   strerror (errno));
        return 1;
    }
    g_debug ("reading from entropy source: %s", fname);
    read_ret = read (rand_fd, &rand_seed, sizeof (rand_seed));
    if (read_ret == -1) {
        g_warning ("failed to read from entropy source %s, %s",
                   fname,
                   strerror (errno));
        return 1;
    }
    g_debug ("seeding rand with %ld", rand_seed);
    srand48_r (rand_seed, rand_data);

    return 0;
}
/**
 * This function initializes and configures all of the long-lived objects
 * in the tabd system. It is invoked on a thread separate from the main
 * thread as a way to get the main thread listening for connections on
 * DBus as quickly as possible. Any incomming DBus requests will block
 * on the 'init_mutex' until this thread completes but they won't be
 * timing etc. This function does X things:
 * - Locks the init_mutex.
 * - Registers a handler for UNIX signals for SIGINT and SIGTERM.
 * - Seeds the RNG state from an entropy source.
 * - Creates the SessionManager.
 * - Creates the TCTI instance used by the Tab.
 * - Creates and wires up the objects that make up the TPM command
 *   processing pipeline.
 * - Starts all of the threads in the command processing pipeline.
 * - Unlocks the init_mutex.
 */
static gpointer
init_thread_func (gpointer user_data)
{
    gmain_data_t *data = (gmain_data_t*)user_data;
    gint ret;
    Tcti *tcti;
    TSS2_RC rc;

    g_info ("init_thread_func start");
    g_mutex_lock (&data->init_mutex);
    /* Setup program signals */
    signal (SIGINT, signal_handler);
    signal (SIGTERM, signal_handler);

    if (seed_rand_data (TABD_RAND_FILE, &data->rand_data) != 0)
        g_error ("failed to seed random number generator");

    data->manager = session_manager_new();
    if (data->manager == NULL)
        g_error ("failed to allocate connection_manager");
    g_debug ("SessionManager: 0x%x", data->manager);

    /**
     * this isn't strictly necessary but it allows us to detect a failure in
     * the TCTI before we start communicating with clients
     */
    rc = tcti_initialize (data->tcti);
    if (rc != TSS2_RC_SUCCESS)
        g_error ("failed to initialize TCTI: 0x%x", rc);

    /**
     * Instantiate and the objects that make up the TPM command processing
     * pipeline.
     */
    data->command_source =
        command_source_new (data->manager);
    g_debug ("created session source: 0x%x", data->command_source);
    data->tab = tab_new (data->tcti);
    g_debug ("created tab: 0x%x", data->tab);
    data->response_sink = SINK (response_sink_new ());
    g_debug ("created response source: 0x%x", data->response_sink);
    /**
     * Wire up the TPM command processing pipeline. TPM command buffers
     * flow from the CommandSource, to the Tab then finally back to the
     * caller through the ResponseSink.
     */
    source_add_sink (SOURCE (data->command_source),
                     SINK   (data->tab));
    source_add_sink (SOURCE (data->tab),
                     SINK   (data->response_sink));
    /**
     * Start the TPM command processing pipeline.
     */
    ret = thread_start (THREAD (data->command_source));
    if (ret != 0)
        g_error ("failed to start connection_source");
    ret = thread_start (THREAD (data->tab));
    if (ret != 0)
        g_error ("failed to start Tab: %s", strerror (errno));
    ret = thread_start (THREAD (data->response_sink));
    if (ret != 0)
        g_error ("failed to start response_source");

    g_mutex_unlock (&data->init_mutex);
    g_info ("init_thread_func done");
}
/**
 * This function parses the parameter argument vector and populates the
 * parameter 'options' structure with data needed to configure the tabd.
 * We rely heavily on the GOption module here and we get our GOptionEntry
 * array from two places:
 * - The TctiOption module.
 * - The local application options specified here.
 * Then we do a bit of sanity checking and setting up default values if
 * none were supplied.
 */
gint
parse_opts (gint            argc,
            gchar          *argv[],
            tabd_options_t *options)
{
    gchar *logger_name = "stdout";
    GOptionContext *ctx;
    GError *err = NULL;
    gboolean system_bus = FALSE;
    gint ret = 0;

    GOptionEntry entries[] = {
        { "logger", 'l', 0, G_OPTION_ARG_STRING, &logger_name,
          "The name of desired logger, stdout is default.", "[stdout|syslog]"},
        { "system", 's', 0, G_OPTION_ARG_NONE, &system_bus,
          "Connect to the system dbus." },
        { NULL },
    };

    g_debug ("creating tcti_options object");
    options->tcti_options = tcti_options_new ();
    ctx = g_option_context_new (" - TPM2 software stack Access Broker Daemon (tabd)");
    g_option_context_add_main_entries (ctx, entries, NULL);
    g_option_context_add_group (ctx, tcti_options_get_group (options->tcti_options));
    if (!g_option_context_parse (ctx, &argc, &argv, &err)) {
        g_print ("Failed to initialize: %s\n", err->message);
        g_clear_error (&err);
        ret = 1;
        goto out;
    }
    /* select the bus type, default to G_BUS_TYPE_SESSION */
    options->bus = system_bus ? G_BUS_TYPE_SYSTEM : G_BUS_TYPE_SESSION;
    if (set_logger (logger_name) == -1) {
        g_print ("Unknown logger: %s, try --help\n", logger_name);
        ret = 1;
    }
out:
    g_option_context_free (ctx);
    return ret;
}
void
thread_cleanup (Thread *thread)
{
    thread_cancel (thread);
    thread_join (thread);
    g_object_unref (thread);
}
/**
 * This is the entry point for the TPM2 Access Broker and Resource Manager
 * daemon. It is responsible for the top most initialization and
 * coordination before blocking on the GMainLoop (g_main_loop_run):
 * - Collects / parses command line options.
 * - Creates the initialization thread and kicks it off.
 * - Registers / owns a name on a DBus.
 * - Blocks on the main loop.
 * At this point all of the tabd processing is being done on other threads.
 * When the daemon shutsdown (for any reason) we do cleanup here:
 * - Join / cleanup the initialization thread.
 * - Release the name on the DBus.
 * - Cancel and join all of the threads started by the init thread.
 * - Cleanup all of the objects created by the init thread.
 */
int
main (int argc, char *argv[])
{
    guint owner_id;
    gmain_data_t gmain_data = { 0 };
    GThread *init_thread;
    tabd_options_t options = { 0 };

    g_info ("tabd startup");
    if (parse_opts (argc, argv, &options) != 0)
        return 1;
    gmain_data.tcti = tcti_options_get_tcti (options.tcti_options);
    if (gmain_data.tcti == NULL)
        g_error ("Failed to get TCTI object from TctiOptions");

    g_mutex_init (&gmain_data.init_mutex);
    g_loop = gmain_data.loop = g_main_loop_new (NULL, FALSE);
    /**
     * Initialize program data on a separate thread. The main thread needs to
     * acquire the dbus name and get into the GMainLoop ASAP to be responsive to
     * bus clients.
     */
    init_thread = g_thread_new (TABD_INIT_THREAD_NAME,
                                init_thread_func,
                                &gmain_data);
    owner_id = g_bus_own_name (options.bus,
                               TAB_DBUS_NAME,
                               G_BUS_NAME_OWNER_FLAGS_NONE,
                               on_bus_acquired,
                               on_name_acquired,
                               on_name_lost,
                               &gmain_data,
                               NULL);
    /**
     * If we call this for the first time from a thread other than the main
     * thread we get a segfault. Not sure why.
     */
    thread_get_type ();
    sink_get_type ();
    source_get_type ();
    g_info ("entering g_main_loop");
    g_main_loop_run (gmain_data.loop);
    g_info ("g_main_loop_run done, cleaning up");
    g_thread_join (init_thread);
    /* cleanup glib stuff first so we stop getting events */
    g_bus_unown_name (owner_id);
    if (gmain_data.skeleton != NULL)
        g_object_unref (gmain_data.skeleton);
    /* tear down the command processing pipeline */
    thread_cleanup (THREAD (gmain_data.command_source));
    thread_cleanup (THREAD (gmain_data.tab));
    thread_cleanup (THREAD (gmain_data.response_sink));
    /* clean up what remains */
    g_object_unref (gmain_data.manager);
    g_object_unref (options.tcti_options);
    return 0;
}
