//     imgflo - Flowhub.io Image-processing runtime
//     (c) 2014 The Grid
//     imgflo may be freely distributed under the MIT license

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <libsoup/soup.h>
#include <gegl-plugin.h>

typedef struct {
	SoupServer *server;
    Registry *registry;
    GHashTable *network_map; // graph_id(string) -> Network. Network contains Graph instance
    Library *component_lib;
    gchar *hostname;
    SoupWebsocketConnection *connection; // TODO: allow multiple clients
    gchar *main_network;
} UiConnection;

static void
send_response(SoupWebsocketConnection *ws,
            const gchar *protocol, const gchar *command, JsonObject *payload)
{
    g_return_if_fail(ws);

    JsonObject *response = json_object_new();
    g_assert(response);

    json_object_set_string_member(response, "protocol", protocol);
    json_object_set_string_member(response, "command", command);
    json_object_set_object_member(response, "payload", payload);

    gsize len = 0;
    gchar *data = json_stringify(response, &len);
    GBytes *resp = g_bytes_new_take(data, len);
    g_print ("SEND: %.*s\n", (int)len, data);
    soup_websocket_connection_send(ws, SOUP_WEBSOCKET_DATA_TEXT, resp);
}

void
ui_net_state_changed(Network *network, gboolean running,
                     gboolean processing, gpointer user_data) {
    g_return_if_fail(network);

    g_assert(user_data);
    UiConnection *self = (UiConnection *)user_data;

    // TODO: send timestamp/uptime
    JsonObject *info = json_object_new();
    json_object_set_string_member(info, "graph", network->graph->id);
    json_object_set_boolean_member(info, "started", running);
    json_object_set_boolean_member(info, "running", processing);

    const gchar * cmd = (running) ? "started" : "stopped";
    if (self->connection) {
        send_response(self->connection, "network", cmd, info);
    }
}

void
send_preview_invalidated(Network *network, Processor *processor, GeglRectangle rect, gpointer user_data) {
    UiConnection *ui = (UiConnection *)user_data;
    g_return_if_fail(ui->registry);
    g_return_if_fail(ui->registry->info);
    g_return_if_fail(network->graph);

    const gchar *node = graph_find_processor_name(network->graph, processor);

    gchar url[1024];
    g_snprintf(url, 1024, "http://%s:%d/process?graph=%s&node=%s",
               ui->hostname, ui->registry->info->port, network->graph->id, node);

    JsonObject *payload = json_object_new();
    json_object_set_string_member(payload, "type", "previewurl");
    json_object_set_string_member(payload, "url", url);
    if (ui->connection) {
        send_response(ui->connection, "network", "output", payload);
    }
}

void
ui_connection_add_network(UiConnection *self, const gchar *name, Network *network) {

    network->on_processor_invalidated_data = (gpointer)self;
    network->on_processor_invalidated = send_preview_invalidated;
    g_hash_table_insert(self->network_map, (gpointer)g_strdup(name), (gpointer)network);

    network->on_state_changed = ui_net_state_changed;
    network->on_state_changed_data = self;
}

static void
handle_graph_message(UiConnection *self, const gchar *command, JsonObject *payload,
                SoupWebsocketConnection *ws)
{
    g_return_if_fail(payload);

    Graph *graph = NULL;
    if (g_strcmp0(command, "clear") != 0) {
        // All other commands must have graph
        // TODO: change FBP protocol to use 'graph' instead of 'id'?
        const gchar *graph_id = json_object_get_string_member(payload, "graph");
        Network *net = (graph_id) ? g_hash_table_lookup(self->network_map, graph_id) : NULL;
        graph = (net) ? net->graph : NULL;
        g_return_if_fail(graph);
    }

    if (g_strcmp0(command, "clear") == 0) {
        const gchar *graph_id = json_object_get_string_member(payload, "id");
        Graph *graph = graph_new(graph_id, self->component_lib);

        Network *network = network_new(graph);
        ui_connection_add_network(self, graph_id, network);
    } else if (g_strcmp0(command, "addnode") == 0) {
        graph_add_node(graph,
            json_object_get_string_member(payload, "id"),
            json_object_get_string_member(payload, "component")
        );
    } else if (g_strcmp0(command, "removenode") == 0) {
        graph_remove_node(graph,
            json_object_get_string_member(payload, "id")
        );
    } else if (g_strcmp0(command, "changenode") == 0) {
        // Just metadata, ignored
    } else if (g_strcmp0(command, "addinitial") == 0) {
        JsonObject *tgt = json_object_get_object_member(payload, "tgt");
        JsonObject *src = json_object_get_object_member(payload, "src");
        GValue data = G_VALUE_INIT;
        json_node_get_value(json_object_get_member(src, "data"), &data);
        graph_add_iip(graph,
            json_object_get_string_member(tgt, "node"),
            json_object_get_string_member(tgt, "port"),
            &data
        );
        g_value_unset(&data);
    } else if (g_strcmp0(command, "removeinitial") == 0) {
        JsonObject *tgt = json_object_get_object_member(payload, "tgt");
        graph_remove_iip(graph,
            json_object_get_string_member(tgt, "node"),
            json_object_get_string_member(tgt, "port")
        );
    } else if (g_strcmp0(command, "addedge") == 0) {
        JsonObject *src = json_object_get_object_member(payload, "src");
        JsonObject *tgt = json_object_get_object_member(payload, "tgt");
        graph_add_edge(graph,
            json_object_get_string_member(src, "node"),
            json_object_get_string_member(src, "port"),
            json_object_get_string_member(tgt, "node"),
            json_object_get_string_member(tgt, "port")
        );
    } else if (g_strcmp0(command, "removeedge") == 0) {
        JsonObject *src = json_object_get_object_member(payload, "src");
        JsonObject *tgt = json_object_get_object_member(payload, "tgt");
        graph_remove_edge(graph,
            json_object_get_string_member(src, "node"),
            json_object_get_string_member(src, "port"),
            json_object_get_string_member(tgt, "node"),
            json_object_get_string_member(tgt, "port")
        );
    } else {
        g_printerr("Unhandled message on protocol 'graph', command='%s'", command);
    }
}

static void
handle_network_message(UiConnection *self, const gchar *command, JsonObject *payload,
                       SoupWebsocketConnection *ws)
{
    g_return_if_fail(payload);

    const gchar *graph_id = json_object_get_string_member(payload, "graph");
    Network *network = (graph_id) ? g_hash_table_lookup(self->network_map, graph_id) : NULL;
    g_return_if_fail(network);

    if (g_strcmp0(command, "start") == 0) {
        g_print("\tNetwork START\n");
        network_set_running(network, TRUE);
    } else if (g_strcmp0(command, "stop") == 0) {
        g_print("\tNetwork STOP\n");
        network_set_running(network, FALSE);
    } else if (g_strcmp0(command, "getstatus") == 0) {
        JsonObject *info = json_object_new();
        json_object_set_string_member(info, "graph", graph_id);
        json_object_set_boolean_member(info, "running", network->running);
        json_object_set_boolean_member(info, "started", network->running);
        send_response(ws, "network", "status", info);

    } else if (g_strcmp0(command, "debug") == 0) {
        // Ignored, not implemented
    } else {
        g_printerr("Unhandled message on protocol 'network', command='%s'", command);
    }
}

static void
ui_connection_handle_message(UiConnection *self,
                const gchar *protocol, const gchar *command, JsonObject *payload,
                SoupWebsocketConnection *ws)
{
    if (g_strcmp0(protocol, "graph") == 0) {
        handle_graph_message(self, command, payload, ws);
    } else if (g_strcmp0(protocol, "network") == 0) {
        handle_network_message(self, command, payload, ws);
    } else if (g_strcmp0(protocol, "component") == 0 && g_strcmp0(command, "list") == 0) {
        gint no_components = 0;
        gchar **operation_names = library_list_components(self->component_lib, &no_components);
        for (int i=0; i<no_components; i++) {
            const gchar *op = operation_names[i];
            if (op) {
                JsonObject *component = library_get_component(self->component_lib, op);
                send_response(ws, "component", "component", component);
            }
        }
        g_strfreev(operation_names);
    } else if (g_strcmp0(protocol, "component") == 0 && g_strcmp0(command, "source") == 0) {
        const gchar *name = json_object_get_string_member(payload, "name");
        gchar *actual_name = library_set_source(self->component_lib,
            name,
            json_object_get_string_member(payload, "code")
        );
        if (actual_name) {
            JsonObject *component = library_get_component(self->component_lib, name);
            send_response(ws, "component", "component", component);
        } else {
            // TODO: error response
        }
        g_free(actual_name);
    } else if (g_strcmp0(protocol, "component") == 0 && g_strcmp0(command, "getsource") == 0) {
        const gchar *name = json_object_get_string_member(payload, "name");


        JsonObject *source_info = json_object_new();
        // TODO: generalize for subgraphs-as-components
        if (g_strcmp0(name, self->main_network) == 0) {

            json_object_set_string_member(source_info, "name", "main"); // FIXME: dont hardcode
            json_object_set_string_member(source_info, "library", "default"); // FIXME: dont hardcode

            Network *n = g_hash_table_lookup(self->network_map, self->main_network);
            g_assert(n);
            JsonObject *g = graph_save_json(n->graph);
            gsize len = 0;
            gchar *code = json_stringify(g, &len);
            g_assert(len);
            json_object_set_string_member(source_info, "language", "json");
            json_object_set_string_member(source_info, "code", code);
        } else {
            json_object_set_string_member(source_info, "name", name);
            gchar *code = library_get_source(self->component_lib, name);
            json_object_set_string_member(source_info, "library", "imgflo");
            json_object_set_string_member(source_info, "language", "c");
            json_object_set_string_member(source_info, "code", code);
        }

        send_response(ws, "component", "source", source_info);

    } else if (g_strcmp0(protocol, "runtime") == 0 && g_strcmp0(command, "getruntime") == 0) {

        JsonObject *runtime = json_object_new();
        json_object_set_string_member(runtime, "version", "0.4"); // protocol version
        json_object_set_string_member(runtime, "type", "imgflo");
        json_object_set_string_member(runtime, "graph", self->main_network);

        JsonArray *capabilities = json_array_new();
        json_array_add_string_element(capabilities, "protocol:component");
        json_array_add_string_element(capabilities, "protocol:graph");
        json_array_add_string_element(capabilities, "protocol:network");
        json_array_add_string_element(capabilities, "component:getsource");
        json_array_add_string_element(capabilities, "component:setsource");
        json_object_set_array_member(runtime, "capabilities", capabilities);

        send_response(ws, "runtime", "runtime", runtime);

    } else {
        g_printerr("Unhandled message: protocol='%s', command='%s'", protocol, command);
    }
}

static void
on_web_socket_open(SoupWebsocketConnection *ws, gpointer user_data)
{
	gchar *url = soup_uri_to_string(soup_websocket_connection_get_uri (ws), FALSE);
	g_print("WebSocket: client opened %s with %s\n", soup_websocket_connection_get_protocol(ws), url);

    UiConnection *self = (UiConnection *)user_data;
    g_assert(self);
    self->connection = ws;

	g_free(url);
}

static void
on_web_socket_message(SoupWebsocketConnection *ws,
                      SoupWebsocketDataType type,
                      GBytes *message,
                      void *user_data)
{
	const gchar *data;
	gsize len;

    //g_print ("%s: %p", __PRETTY_FUNCTION__, user_data);

	data = g_bytes_get_data (message, &len);
	g_print ("RECV: %.*s\n", (int)len, data);

    JsonParser *parser = json_parser_new();
    gboolean success = json_parser_load_from_data(parser, data, len, NULL);
    if (success) {
        JsonNode *r = json_parser_get_root(parser);
        g_assert(JSON_NODE_HOLDS_OBJECT(r));
        JsonObject *root = json_node_get_object(r);

        const gchar *protocol = json_object_get_string_member(root, "protocol");
        const gchar *command = json_object_get_string_member(root, "command");

        JsonNode *pnode = json_object_get_member(root, "payload");
        JsonObject *payload = JSON_NODE_HOLDS_OBJECT(pnode) ? json_object_get_object_member(root, "payload") : NULL;

        UiConnection *ui = (UiConnection *)user_data;
        ui_connection_handle_message(ui, protocol, command, payload, ws);

    } else {
        g_error("Unable to parse WebSocket message as JSON");
    }

    g_object_unref(parser);
}

static void
on_web_socket_error(SoupWebsocketConnection *ws, GError *error, gpointer user_data)
{
    UiConnection *ui = (UiConnection *)user_data;
    ui->connection = NULL;
    g_printerr("WebSocket: error: %s\n", error->message);
}

static void
on_web_socket_close(SoupWebsocketConnection *ws, gpointer user_data)
{
    UiConnection *ui = (UiConnection *)user_data;
    ui->connection = NULL;

	gushort code = soup_websocket_connection_get_close_code(ws);
	if (code != 0) {
		g_printerr("WebSocket: close: %d %s\n", code,
			    soup_websocket_connection_get_close_data(ws));
	} else {
		g_printerr("WebSocket: close\n");
    }
}

void websocket_callback(SoupServer *server,
					    const char *path,
					    SoupWebsocketConnection *connection,
					    SoupClientContext *client,
					    gpointer user_data)
{
	g_signal_connect(connection, "open", G_CALLBACK(on_web_socket_open), user_data);
	g_signal_connect(connection, "message", G_CALLBACK(on_web_socket_message), user_data);
	g_signal_connect(connection, "error", G_CALLBACK(on_web_socket_error), user_data);
	g_signal_connect(connection, "close", G_CALLBACK(on_web_socket_close), user_data);
}

static void
process_image_callback (SoupServer *server, SoupMessage *msg,
		 const char *path, GHashTable *query,
		 SoupClientContext *context, gpointer user_data) {

    UiConnection *self = (UiConnection *)user_data;

    // Lookup network
    Network *network = NULL;
    {
        const gchar *graph_id = g_hash_table_lookup(query, "graph");
        network = (graph_id) ? g_hash_table_lookup(self->network_map, graph_id) : NULL;
    }
    if (!network) {
        soup_message_set_status_full(msg, SOUP_STATUS_BAD_REQUEST, "'graph' not specified or wrong");
        return;
    }

    // Lookup node
    Processor *processor = NULL;
    {
        const gchar *node_id = g_hash_table_lookup(query, "node");
        processor = (node_id) ? network_processor(network, node_id) : NULL;
    }
    if (!processor) {
        soup_message_set_status_full(msg, SOUP_STATUS_BAD_REQUEST, "'node' not specified or wrong");
        return;
    }

    // Render output
    // FIXME: allow region-of-interest and scale as query params
    gchar *rgba = NULL;
    GeglRectangle roi;
    const gboolean success = processor_blit(processor, babl_format("R'G'B'A u8"), &roi, &rgba);
    if (!success) {
        soup_message_set_status(msg, SOUP_STATUS_BAD_REQUEST);
        g_free(rgba);
        return;
    }
    if (!(roi.width > 0 && roi.height > 0)) {
        soup_message_set_status(msg, SOUP_STATUS_BAD_REQUEST);
        g_free(rgba);
        return;
    }

    // Compress to PNG
    {
        PngEncoder *encoder = png_encoder_new();
        png_encoder_encode_rgba(encoder, roi.width, roi.height, rgba);
        char *png = encoder->buffer;
        const size_t len = encoder->size;
        soup_message_set_status(msg, SOUP_STATUS_OK);
        soup_message_set_response(msg, "image/png", SOUP_MEMORY_COPY, png, len);
        png_encoder_free(encoder);
    }
}

/*
var addr = window.location.origin.replace("http://", "ws://");
var ide = "http://app.flowhub.io";
console.log(ide+"/#runtime/endpoint?protocol=websocket&address=encodeURIComponent(addr))
Uncaught SyntaxError: Unexpected token ILLEGAL VM207:732
console.log(ide+"/#runtime/endpoint?protocol=websocket&address="+encodeURIComponent(addr))
*/

static void
serve_frontpage(SoupServer *server, SoupMessage *msg,
		 const char *path, GHashTable *query,
		 SoupClientContext *context, gpointer user_data) {

    gchar *params = soup_form_encode(
        "protocol", "websocket",
        "address", "ws://localhost:3569",
        NULL
    );
    gchar *flowhub_url = g_strdup_printf("http://app.flowhub.io#runtime/endpoint?%s", params);
    g_free(params);
    static const gchar *html = \
        "\n<a id=\"flowhub_url\">Open in Flowhub</a>"
        "\n<script>"
        "\n   var addr = window.location.origin.replace(\"http://\", \"ws://\");"
        "\n   addr = addr.replace(\"https://\", \"ws://\");"
        "\n   var ide = \"http://app.flowhub.io\";"
        "\n   var url = ide+\"/#runtime/endpoint?protocol=websocket&address=\"+encodeURIComponent(addr);"
        "\n   var a = document.getElementById(\"flowhub_url\");"
        "\n   a.href = url;"
        "\n</script>"
        ;
    //gchar *html = g_strdup_printf(html_template, flowhub_url, "Open in Flowhub");
    g_free(flowhub_url);

    const gsize len = strlen(html);
    soup_message_set_status(msg, SOUP_STATUS_OK);
    soup_message_set_response(msg, "text/html", SOUP_MEMORY_COPY, html, len);
    //g_free(html);
}

static void
server_callback (SoupServer *server, SoupMessage *msg,
		 const char *path, GHashTable *query,
		 SoupClientContext *context, gpointer data)
{

    SoupMessageHeadersIter iter;
    const char *name, *value;

    g_print("%s %s HTTP/1.%d\n", msg->method, path,
         soup_message_get_http_version(msg));

    if (g_strcmp0(path, "/process") == 0 && msg->method == SOUP_METHOD_GET) {
        process_image_callback(server, msg, path, query, context, data);
    } else if (g_strcmp0(path, "/") == 0 && msg->method == SOUP_METHOD_GET) {
        serve_frontpage(server, msg, path, query, context, data);
    } else {
        soup_message_headers_iter_init(&iter, msg->request_headers);
        while (soup_message_headers_iter_next(&iter, &name, &value))
            g_print("%s: %s\n", name, value);
        if (msg->request_body->length)
            g_print("%s\n", msg->request_body->data);

        g_print("  -> %d %s\n\n", msg->status_code, msg->reason_phrase);

        soup_message_set_status(msg, SOUP_STATUS_NOT_IMPLEMENTED);
    }
}

gboolean
ui_connection_try_register(UiConnection *self) {
    if (self->registry->info->user_id) {
        gboolean success = registry_register(self->registry);
        if (success) {
            registry_start_pinging(self->registry);
        }
        return success;
    }
    return FALSE;
}

UiConnection *
ui_connection_new(const gchar *hostname, int internal_port, int external_port) {
    UiConnection *self = g_new(UiConnection, 1);

    self->connection = NULL;
    self->main_network = NULL;
    self->network_map = g_hash_table_new_full(g_str_hash, g_str_equal,
                                              g_free, (GDestroyNotify)network_free);
    self->hostname = g_strdup(hostname);
    self->registry = registry_new(runtime_info_new_from_env(hostname, external_port));
    self->component_lib = library_new();

	self->server = soup_server_new(SOUP_SERVER_SERVER_HEADER, "imgflo-runtime", NULL);
    if (!self->server) {
        g_free(self);
        return NULL;
    }

    soup_server_add_websocket_handler(self->server, NULL, NULL, NULL,
        websocket_callback, self, NULL);
    soup_server_add_handler(self->server, NULL,
        server_callback, self, NULL);

    soup_server_listen_all(self->server, internal_port, SOUP_SERVER_LISTEN_IPV4_ONLY, NULL);

    return self;
}

void
ui_connection_free(UiConnection *self) {

    g_hash_table_destroy(self->network_map);
    g_free(self->hostname);
    g_object_unref(self->server);
    library_free(self->component_lib);
    g_free(self->main_network);

    g_free(self);
}

// Takes ownership of Network
void
ui_connection_set_default_network(UiConnection *self, Network *net) {
    g_return_if_fail(self);
    g_return_if_fail(net);
    gchar * id = net->graph->id;
    g_assert(id);

    ui_connection_add_network(self, id, net);

    self->main_network = g_strdup(id);
    g_assert(self->main_network);
    network_set_running(net, TRUE);
}
