/*
 * client_probe.c — drive the KEEL nghttp2 *client* against an arbitrary h2c
 * server (default: a third-party nghttpd), closing the last interop quadrant:
 * KEEL client ⇄ non-KEEL server. GETs a path and prints the status; exit 0 on a
 * 2xx/3xx/4xx response (any valid HTTP/2 response proves interop), non-zero on
 * connect/protocol failure.
 *
 * Usage: client_probe [host] [port] [path]   (defaults 127.0.0.1 18479 /)
 */
#include <keel/keel.h>
#include "keel_http2_nghttp2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct { int done, status, err; } Probe;

static void on_response(KlHttp2ClientConn *c, int32_t sid,
                        const KlHttp2ClientResponse *resp, void *ud) {
    (void)c; (void)sid;
    Probe *p = ud;
    p->status = resp->status;
    p->done = 1;
}
static void on_error(KlHttp2ClientConn *c, const char *msg, void *ud) {
    (void)c;
    Probe *p = ud;
    fprintf(stderr, "client error: %s\n", msg);
    p->err = 1; p->done = 1;
}

int main(int argc, char **argv) {
    const char *host = argc > 1 ? argv[1] : "127.0.0.1";
    const char *port = argc > 2 ? argv[2] : "18479";
    const char *path = argc > 3 ? argv[3] : "/";
    char url[256];
    snprintf(url, sizeof(url), "http://%s:%s", host, port);

    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ev;
    if (kl_event_ctx_init(&ev, &alloc) < 0) { fprintf(stderr, "ctx init\n"); return 2; }
    KlHttp2ClientConfig cfg = { .session = kl_http2_nghttp2_client_session };

    Probe p = {0};
    KlHttp2ClientConn *c = kl_http2_client_connect(&ev, &alloc, &cfg, url, on_error, &p);
    if (!c) { fprintf(stderr, "connect\n"); kl_event_ctx_free(&ev); return 2; }

    int32_t sid = -1;
    for (int i = 0; i < 500 && !p.done; i++) {
        if (sid < 0 && !p.err)
            sid = kl_http2_client_request(c, "GET", path, NULL, 0, NULL, 0, on_response, &p);
        if (kl_event_ctx_run(&ev, 16, 20) < 0) break;
    }

    kl_http2_client_free(c);
    kl_event_ctx_free(&ev);

    if (p.err || !p.done) { fprintf(stderr, "no response from %s%s\n", url, path); return 1; }
    printf("KEEL nghttp2 client -> %s%s : HTTP/2 %d\n", url, path, p.status);
    return (p.status >= 200 && p.status < 500) ? 0 : 1;
}
