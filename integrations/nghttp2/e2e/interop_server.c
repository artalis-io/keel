/*
 * interop_server.c — a standalone KEEL HTTP/2 (h2c prior-knowledge) server
 * backed by the nghttp2 server adapter, for third-party interop (curl --http2-
 * prior-knowledge, nghttp, h2load, h2spec). Runs until killed. Cleartext h2c on
 * 127.0.0.1:18478, route GET /hello -> JSON.
 */
#include <keel/keel.h>
#include "keel_h2_nghttp2.h"
#include <stdio.h>

#define INTEROP_PORT 18478

static void handle_hello(KlRequest *req, KlResponse *res, void *ud) {
    (void)req; (void)ud;
    kl_response_json(res, 200, "{\"msg\":\"hello h2 e2e\"}", 21);
}

int main(void) {
    KlH2ServerConfig h2cfg = { .factory = kl_h2_nghttp2_server_session };
    KlConfig cfg = { .port = INTEROP_PORT, .bind_addr = "127.0.0.1", .h2 = &h2cfg };
    KlServer s;
    if (kl_server_init(&s, &cfg) < 0) { fprintf(stderr, "server init failed\n"); return 1; }
    kl_server_route(&s, "GET", "/hello", handle_hello, NULL, NULL);
    fprintf(stderr, "interop_server: h2c on 127.0.0.1:%d\n", INTEROP_PORT);
    kl_server_run(&s);
    kl_server_free(&s);
    return 0;
}
