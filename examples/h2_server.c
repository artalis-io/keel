/**
 * HTTP/2 Server Example
 *
 * Demonstrates wiring a stub HTTP/2 session into KEEL's pluggable H2 vtable.
 * In a real deployment, replace StubH2Session with an nghttp2-based wrapper.
 *
 * This example only enables h2c (cleartext HTTP/2 via Upgrade or preface).
 * For TLS-based h2, configure KlTlsConfig with ALPN "h2" support.
 *
 * Build: make examples
 * Run:   ./examples/h2_server [port]
 */
#include <keel/keel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Stub H2 Session ─────────────────────────────────────────────── */

typedef struct {
    KlH2ServerSession base;
    KlH2ServerCallbacks callbacks;
    void *cb_user_data;
    KlAllocator *alloc;
} StubH2Session;

static kl_ssize_t stub_recv(KlH2ServerSession *self, const void *data, size_t len) {
    (void)self; (void)data;
    /* A real implementation would parse HTTP/2 frames here */
    return (ssize_t)len;
}

static int stub_submit_response(KlH2ServerSession *self, uint32_t stream_id,
                                 int status, const char **hdr_names,
                                 const char **hdr_values, int num_headers,
                                 const void *body, size_t body_len) {
    (void)self; (void)stream_id; (void)status;
    (void)hdr_names; (void)hdr_values; (void)num_headers;
    (void)body; (void)body_len;
    return 0;
}

static int stub_want_write(KlH2ServerSession *self) {
    (void)self;
    return 0;
}

static int stub_flush(KlH2ServerSession *self) {
    (void)self;
    return 0;
}

static int stub_shutdown(KlH2ServerSession *self) {
    (void)self;
    return 0;
}

static void stub_destroy(KlH2ServerSession *self) {
    StubH2Session *s = (StubH2Session *)self;
    kl_free(s->alloc, s, sizeof(*s));
}

static KlH2ServerSession *stub_factory(KlAllocator *alloc,
                                  KlH2ServerCallbacks *callbacks,
                                  void *user_data) {
    StubH2Session *s = kl_malloc(alloc, sizeof(*s));
    if (!s) return NULL;
    memset(s, 0, sizeof(*s));
    s->alloc = alloc;
    s->base.recv = stub_recv;
    s->base.submit_response = stub_submit_response;
    s->base.want_write = stub_want_write;
    s->base.flush = stub_flush;
    s->base.shutdown = stub_shutdown;
    s->base.destroy = stub_destroy;
    s->callbacks = *callbacks;
    s->cb_user_data = user_data;
    return &s->base;
}

/* ── Handlers ─────────────────────────────────────────────────────── */

static void handle_hello(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_http_response_json(res, 200, "{\"msg\":\"hello from h2\"}", 22);
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

    KlH2ServerConfig h2_cfg = {
        .factory = stub_factory,
        /* 0 = use defaults (128 streams, 65535 window) */
    };

    KlHttpServer s;
    KlHttpServerConfig cfg = {
        .port = port,
        .h2 = &h2_cfg,
    };

    if (kl_http_server_init(&s, &cfg) < 0) return 1;
    kl_http_server_route(&s, "GET", "/hello", handle_hello, NULL, NULL);

    fprintf(stderr, "h2_server: HTTP/2 enabled on port %d\n", port);
    kl_http_server_run(&s);
    kl_http_server_free(&s);
    return 0;
}
