/*
 * alpn_client.c — the KEEL HTTP/2 client offering ALPN over real TLS against the
 * alpn_server, closing the client-side matrix rows:
 *
 *   Keel client --ALPN {h2,http/1.1}--> Keel TLS server --selects h2--> h2 adapter
 *
 * The mbedTLS client ctx offers {h2, http/1.1}; the server selects h2; the h2
 * client verifies the negotiated protocol (h2c_alpn_ok) before speaking HTTP/2.
 * Exit 0 on a 2xx over the negotiated h2 connection. BYO (mbedTLS + nghttp2).
 */
#include <keel/keel.h>
#include <keel_tls_mbedtls.h>
#include "keel_h2_nghttp2.h"

#include <stdio.h>
#include <string.h>

typedef struct { int done, status, err; } Probe;

static void on_response(KlHttp2ClientConn *c, int32_t sid,
                        const KlHttp2ClientResponse *resp, void *ud) {
    (void)c; (void)sid;
    Probe *p = ud; p->status = resp->status; p->done = 1;
}
static void on_error(KlHttp2ClientConn *c, const char *msg, void *ud) {
    (void)c; Probe *p = ud;
    fprintf(stderr, "client error: %s\n", msg);
    p->err = 1; p->done = 1;
}

int main(int argc, char **argv) {
    const char *url = argc > 1 ? argv[1] : "https://127.0.0.1:18443";
    KlAllocator alloc = kl_allocator_default();

    /* Client TLS ctx: no CA (skip verify, like curl -k) + offer ALPN. */
    KlTlsCtx *ctx = kl_tls_mbedtls_client_ctx_create(NULL, &alloc);
    if (!ctx) { fprintf(stderr, "client tls ctx\n"); return 2; }
    const char *protos[] = { "h2", "http/1.1", NULL };
    if (kl_tls_mbedtls_ctx_set_alpn(ctx, protos) != 0) {
        fprintf(stderr, "set_alpn\n"); kl_tls_mbedtls_ctx_destroy(ctx); return 2;
    }
    KlTlsConfig tls = { .ctx = ctx, .factory = kl_tls_mbedtls_create,
                        .ctx_destroy = kl_tls_mbedtls_ctx_destroy };

    KlEventCtx ev;
    if (kl_event_ctx_init(&ev, &alloc) < 0) { kl_tls_mbedtls_ctx_destroy(ctx); return 2; }
    KlHttp2ClientConfig cfg = { .tls = &tls, .session = kl_http2_nghttp2_client_session };

    Probe p = {0};
    KlHttp2ClientConn *c = kl_http2_client_connect(&ev, &alloc, &cfg, url, on_error, &p);
    if (!c) { fprintf(stderr, "connect\n"); kl_event_ctx_free(&ev); kl_tls_mbedtls_ctx_destroy(ctx); return 2; }

    int32_t sid = -1;
    for (int i = 0; i < 800 && !p.done; i++) {
        if (sid < 0 && !p.err)
            sid = kl_http2_client_request(c, "GET", "/", NULL, 0, NULL, 0, on_response, &p);
        if (kl_event_ctx_run(&ev, 16, 20) < 0) break;
    }

    kl_http2_client_free(c);
    kl_event_ctx_free(&ev);
    kl_tls_mbedtls_ctx_destroy(ctx);

    if (p.err || !p.done) { fprintf(stderr, "no h2 response from %s\n", url); return 1; }
    printf("KEEL client --ALPN h2 (TLS)--> %s : HTTP/2 %d\n", url, p.status);
    return (p.status >= 200 && p.status < 300) ? 0 : 1;
}
