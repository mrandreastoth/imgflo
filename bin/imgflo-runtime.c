//     imgflo - Flowhub.io Image-processing runtime
//     (c) 2014 The Grid
//     imgflo may be freely distributed under the MIT license

#include "lib/utils.c"
#include "lib/png.c"
#include "lib/uuid.c"
#include "lib/processor.c"
#include "lib/library.c"
#include "lib/graph.c"
#include "lib/network.c"
#include "lib/registry.c"
#include "lib/ui.c"

static void
quit(int sig)
{
	/* Exit cleanly on ^C in case we're valgrinding. */
	exit(0);
}

static int port = 3569;
static int extport = 3569;
static gchar *host = ""; // defaults to autodetect/localhost
static gchar *defaultgraph = "";
static gchar *ide = "http://app.flowhub.io";
static gboolean launch_ide = FALSE;

static GOptionEntry entries[] = {
	{ "port", 'p', 0, G_OPTION_ARG_INT, &port, "Port to listen on", NULL },
    { "external-port", 'e', 0, G_OPTION_ARG_INT, &extport, "Port we are available on for clients", NULL },
    { "host", 'h', 0, G_OPTION_ARG_STRING, &host, "Hostname", NULL },
    { "graph", 'g', 0, G_OPTION_ARG_STRING, &defaultgraph, "Default graph", NULL },
    { "ide", 'i', 0, G_OPTION_ARG_STRING, &ide, "FBP IDE to use", NULL },
    { "autolaunch", 'i', 0, G_OPTION_ARG_NONE, &launch_ide, "Automatically launch FBP IDE", NULL },
	{ NULL }
};

gboolean
show_liveurl(void *user_data) {
    UiConnection *ui = (UiConnection *)user_data;

    gchar *live_url = ui_connection_get_liveurl(ui, ide);
    g_print("Live URL: %s\n", live_url);

    if (launch_ide) {
        GError *err = NULL;
        g_app_info_launch_default_for_uri(live_url, NULL, &err);
        if (err != NULL) {
            g_error("%s\n", err->message);
            g_error_free(err);
        }
    }
    g_free(live_url);
    return FALSE;
}

int
main (int argc, char **argv)
{
    // Parse options
    {
	    GOptionContext *opts;
	    GError *error = NULL;

	    opts = g_option_context_new (NULL);
	    g_option_context_add_main_entries (opts, entries, NULL);
	    if (!g_option_context_parse (opts, &argc, &argv, &error)) {
		    g_printerr("Could not parse arguments: %s\n", error->message);
		    g_printerr("%s", g_option_context_get_help (opts, TRUE, NULL));
		    exit(1);
	    }
	    if (argc != 1) {
		    g_printerr("%s", g_option_context_get_help (opts, TRUE, NULL));
		    exit(1);
	    }
	    g_option_context_free (opts);
    }

    // Run
    {
	    signal(SIGINT, quit);

        gegl_init(0, NULL);
	    UiConnection *ui = ui_connection_new(host, port, extport);

        if (strlen(defaultgraph) > 0) {
            GError *err = NULL;
            Graph *g = graph_new("default/main", ui->component_lib);
            Network *n = network_new(g);
            gboolean loaded = graph_load_json_file(g, defaultgraph, &err);
            if (!loaded) {
                g_printerr("Failed to load graph: %s", err->message);
                return 1;
            }
            ui_connection_set_default_network(ui, n);
        }

        GMainLoop *loop = g_main_loop_new(NULL, TRUE);

	    if (!ui) {
		    g_printerr("Unable to bind to server port %d\n", port);
		    exit(1);
	    }
	    g_print("\nRuntime running on port %d, external port %d\n", port, extport);

        ui_connection_try_register(ui);

        g_idle_add_full(G_PRIORITY_LOW, (GSourceFunc)show_liveurl, ui, NULL);

	    g_main_loop_run (loop);

        g_main_loop_unref(loop);
        ui_connection_free(ui);
        gegl_exit();
    }

	return 0;
}
