/*
 * websocket_server.c — WebSocket echo server
 *
 * Concepts: kl_http_server_ws_upgrade, KlWsServerConfig, on_open/on_message/on_close callbacks.
 *
 * Build:  make examples
 * Run:    ./examples/websocket_server [port]
 * Test:   websocat ws://localhost:8080/ws
 */

#include <keel/keel.h>
#include <stdio.h>
#include <stdlib.h>

static void on_open(KlWsServerConn *ws, void *ctx) {
    (void)ctx;
    printf("WebSocket connected: %p\n", (void *)ws);
}

static void on_message(KlWsServerConn *ws, const char *data, size_t len,
                       int is_binary, void *ctx) {
    (void)ctx;
    printf("Received %s (%zu bytes)\n", is_binary ? "binary" : "text", len);
    /* Echo back */
    if (is_binary)
        kl_ws_server_send_binary(ws, data, len);
    else
        kl_ws_server_send_text(ws, data, len);
}

static void on_close(KlWsServerConn *ws, uint16_t code, const char *reason,
                     size_t reason_len, void *ctx) {
    (void)ctx;
    printf("WebSocket closed: %p code=%u reason=%.*s\n",
           (void *)ws, code, (int)reason_len, reason ? reason : "");
}

int main(int argc, char **argv) {
    int port = 8080;
    if (argc > 1) {
        char *end;
        long val = strtol(argv[1], &end, 10);
        if (end == argv[1] || *end != '\0' || val < 1 || val > 65535) {
            fprintf(stderr, "usage: %s [port]\n", argv[0]);
            return 1;
        }
        port = (int)val;
    }

    KlHttpServer s;
    KlHttpServerConfig cfg = {
        .port = port,
        .install_signal_handlers = 1,
    };
    if (kl_http_server_init(&s, &cfg) < 0) return 1;

    KlWsServerConfig ws_cfg;
    kl_ws_server_config_init(&ws_cfg);
    ws_cfg.callbacks.on_open = on_open;
    ws_cfg.callbacks.on_message = on_message;
    ws_cfg.callbacks.on_close = on_close;

    kl_http_server_ws_upgrade(&s, "/ws", &ws_cfg);

    printf("WebSocket echo server on ws://localhost:%d/ws\n", port);
    kl_http_server_run(&s);
    kl_http_server_free(&s);
    return 0;
}
