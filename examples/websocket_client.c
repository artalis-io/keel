/*
 * websocket_client.c — WebSocket echo client
 *
 * Connects to a WebSocket server, sends a message, prints the echo,
 * and cleanly closes.
 *
 * Build:  make examples
 * Run:    ./examples/websocket_client [url]
 * Test:   First start the echo server:
 *           ./examples/websocket_server 9090
 *         Then in another terminal:
 *           ./examples/websocket_client ws://localhost:9090/ws
 */

#include <keel/keel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int done = 0;

static void on_open(KlWsClientConn *ws, void *ctx) {
    (void)ctx;
    printf("Connected!\n");
    kl_ws_client_send_text(ws, "Hello from KEEL WS client", 25);
}

static void on_message(KlWsClientConn *ws, const char *data, size_t len,
                       int is_binary, void *ctx) {
    (void)ctx; (void)is_binary;
    printf("Received: %.*s\n", (int)len, data);
    /* Close after receiving echo */
    kl_ws_client_close(ws, KL_WS_NORMAL_CLOSE, "done", 4);
}

static void on_close(KlWsClientConn *ws, uint16_t code,
                     const char *reason, size_t reason_len, void *ctx) {
    (void)ws; (void)ctx;
    printf("Closed: code=%u reason=%.*s\n",
           code, (int)reason_len, reason ? reason : "");
    done = 1;
}

static void on_error(KlWsClientConn *ws, const char *msg, void *ctx) {
    (void)ws; (void)ctx;
    fprintf(stderr, "Error: %s\n", msg);
    done = 1;
}

int main(int argc, char **argv) {
    const char *url = "ws://localhost:8080/ws";
    if (argc > 1) url = argv[1];

    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ev;
    if (kl_event_ctx_init(&ev, &alloc) < 0) {
        fprintf(stderr, "event_ctx_init failed\n");
        return 1;
    }

    KlWsClientCallbacks cbs = {
        .on_open = on_open,
        .on_message = on_message,
        .on_close = on_close,
        .on_error = on_error,
    };

    KlWsClientConn *ws = kl_ws_client_connect(&ev, &alloc, NULL, url,
                                                &cbs, NULL);
    if (!ws) {
        fprintf(stderr, "connect failed (is server running at %s?)\n", url);
        kl_event_ctx_free(&ev);
        return 1;
    }

    printf("Connecting to %s ...\n", url);

    /* Run event loop until connection closes */
    while (!done) {
        if (kl_event_ctx_run(&ev, 16, 1000) < 0) break;
    }

    kl_ws_client_free(ws);
    kl_event_ctx_free(&ev);
    return 0;
}
