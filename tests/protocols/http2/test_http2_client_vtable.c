/*
 * test_http2_client_vtable.c: kl_http2_client_connect must reject a session whose factory returns a
 * table missing a REQUIRED op (recv/submit_request/flush/destroy) WITHOUT crashing. Core calls those
 * four unconditionally, so a malformed session would fault later.
 *
 * Public boundary: connect over a real loopback listener, then drive the event loop. The session is
 * created once the socket finishes connecting (synchronously when connect returns 0, otherwise on the
 * first writable event); either way the factory runs, then the required-subset gate fires. A malformed
 * table drives the connection into its error path (on_error); a valid one wires up and stays live.
 * destroy is called only when present (a table omitting it cannot be freed generically); the session
 * here is a shared static with stateless ops, so no case leaks regardless.
 */
#include "utest.h"

#ifndef _WIN32
#include <keel/http2_client.h>
#include <keel/allocator.h>
#include <keel/event_ctx.h>
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

/* ── configurable stub session (one required op omitted per test) ──────── */
static int      stub_recv(KlHttp2ClientSession *s, const char *d, size_t n) { (void)s;(void)d; return (int)n; }
static int32_t  stub_submit(KlHttp2ClientSession *s, const char *m, const char *p, const char *a,
                            const KlHttp2ClientHeader *h, int n, const char *b, size_t bl) {
    (void)s;(void)m;(void)p;(void)a;(void)h;(void)n;(void)b;(void)bl; return 1;
}
static int      stub_flush(KlHttp2ClientSession *s) { (void)s; return 0; }
static void     stub_destroy(KlHttp2ClientSession *s) { (void)s; }

enum { OMIT_NONE = -1, OMIT_RECV, OMIT_SUBMIT, OMIT_FLUSH, OMIT_DESTROY };
static int                  g_omit;
static int                  g_factory_called;   /* session-creation site reached */
static int                  g_error_fired;      /* on_error callback ran (rejected/failed) */
static KlHttp2ClientSession g_session;          /* shared; ops stateless (no heap => no leak) */

static KlHttp2ClientSession *vtable_factory(KlAllocator *alloc) {
    (void)alloc;
    g_factory_called = 1;
    memset(&g_session, 0, sizeof(g_session));
    g_session.recv           = stub_recv;
    g_session.submit_request = stub_submit;
    g_session.flush          = stub_flush;
    g_session.destroy        = stub_destroy;
    switch (g_omit) {
        case OMIT_RECV:    g_session.recv           = NULL; break;
        case OMIT_SUBMIT:  g_session.submit_request = NULL; break;
        case OMIT_FLUSH:   g_session.flush          = NULL; break;
        case OMIT_DESTROY: g_session.destroy        = NULL; break;
        default: break;
    }
    return &g_session;
}

static void on_error(KlHttp2ClientConn *c, const char *msg, void *ud) {
    (void)c; (void)msg; (void)ud; g_error_fired = 1;
}

/* Open a loopback TCP listener; return fd and write the bound port to *port. */
static int loopback_listener(int *port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { close(fd); return -1; }
    if (listen(fd, 8) < 0) { close(fd); return -1; }
    socklen_t sl = sizeof(a);
    if (getsockname(fd, (struct sockaddr *)&a, &sl) < 0) { close(fd); return -1; }
    *port = ntohs(a.sin_port);
    return fd;
}

/* Connect, then pump the loop until the session-creation site is reached (factory ran) or a bounded
 * number of ticks elapse. Returns the connection handle (caller frees). */
static KlHttp2ClientConn *connect_and_settle(KlEventCtx *ev, KlAllocator *alloc, int port) {
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/", port);
    KlHttp2ClientConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.session = vtable_factory;
    g_factory_called = 0; g_error_fired = 0;
    KlHttp2ClientConn *c = kl_http2_client_connect(ev, alloc, &cfg, url, on_error, NULL);
    for (int i = 0; i < 100 && !g_factory_called; i++)
        kl_event_ctx_run(ev, 16, 20);
    return c;
}

/* A fully-valid vtable connects and wires up: factory ran, no error. */
UTEST(h2c_vtable, valid_vtable_connects) {
    int port, lfd = loopback_listener(&port);
    ASSERT_GE(lfd, 0);
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &alloc), 0);
    g_omit = OMIT_NONE;
    KlHttp2ClientConn *c = connect_and_settle(&ev, &alloc, port);
    ASSERT_TRUE(c != NULL);
    ASSERT_EQ(g_factory_called, 1);
    ASSERT_EQ(g_error_fired, 0);      /* a valid vtable is not rejected */
    kl_http2_client_free(c);
    kl_event_ctx_free(&ev);
    close(lfd);
}

/* Each missing required op => the session is rejected: factory ran, then on_error fired. */
UTEST(h2c_vtable, missing_required_op_rejected) {
    int cases[] = { OMIT_RECV, OMIT_SUBMIT, OMIT_FLUSH, OMIT_DESTROY };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int port, lfd = loopback_listener(&port);
        ASSERT_GE(lfd, 0);
        KlAllocator alloc = kl_allocator_default();
        KlEventCtx ev;
        ASSERT_EQ(kl_event_ctx_init(&ev, &alloc), 0);
        g_omit = cases[i];
        KlHttp2ClientConn *c = connect_and_settle(&ev, &alloc, port);
        ASSERT_TRUE(c != NULL);
        ASSERT_EQ(g_factory_called, 1);
        ASSERT_EQ(g_error_fired, 1);   /* rejected via the connection error path */
        kl_http2_client_free(c);
        kl_event_ctx_free(&ev);
        close(lfd);
    }
}

UTEST_MAIN();

#else  /* _WIN32: this loopback test uses POSIX sockets */
UTEST_MAIN();
#endif
