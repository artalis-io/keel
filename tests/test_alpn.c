/*
 * test_alpn.c — ALPN → protocol-adapter dispatch, and the single shared REST
 * layer across HTTP/1.1 and HTTP/2.
 *
 * Part A (dispatch): drive kl_conn_on_handshake() with a mock TLS whose
 * negotiated ALPN is configurable, and assert the connection enters exactly the
 * right adapter — h2 → the HTTP/2 adapter (KL_CONN_HTTP2), everything else
 * (http/1.1, no ALPN, an unsupported value) → the HTTP/1.1 adapter
 * (KL_CONN_READING). This is the regression guard for the ALPN selection.
 *
 * Part B (single REST layer): drive an HTTP/2 request through the real h2 server
 * path (h2.c) via a capturing session, and assert it lands on the SAME router,
 * middleware, and handler an HTTP/1.1 request would — and that the HTTP/2
 * :authority pseudo-header converges onto the same "host" header HTTP/1.1 uses.
 */
#include "utest.h"
#include <keel/keel.h>
#include "mock_tls.h"
#include "net_compat.h"
#include <string.h>

/* ── shared router + handler (protocol-independent) ─────────────────── */

static KlAllocator  ta_alloc;
static KlRouter     ta_router;
static int          ta_handler_calls;
static int          ta_mw_calls;
static char         ta_seen_host[128];
static int          ta_seen_version;   /* req->version_major observed by handler */

static void ta_handler(KlRequest *req, KlResponse *res, void *ud) {
    (void)ud;
    ta_handler_calls++;
    ta_seen_version = req->version_major;
    const char *host = kl_request_header(req, "host");
    ta_seen_host[0] = '\0';
    if (host) { strncpy(ta_seen_host, host, sizeof(ta_seen_host) - 1);
                ta_seen_host[sizeof(ta_seen_host) - 1] = '\0'; }
    kl_response_json(res, 200, "{\"ok\":true}", 11);
}

static int ta_middleware(KlRequest *req, KlResponse *res, void *ud) {
    (void)req; (void)res; (void)ud;
    ta_mw_calls++;
    return 0;   /* continue */
}

static void ta_setup(void) {
    ta_alloc = kl_allocator_default();
    kl_router_init(&ta_router, &ta_alloc);
    kl_router_add(&ta_router, "GET", "/x", ta_handler, NULL, NULL);
    kl_router_use(&ta_router, "GET", "/*", ta_middleware, NULL);
    ta_handler_calls = ta_mw_calls = ta_seen_version = 0;
    ta_seen_host[0] = '\0';
    mock_tls_alpn = NULL;
}
static void ta_teardown(void) { kl_router_free(&ta_router); mock_tls_alpn = NULL; }

/* ── minimal capturing HTTP/2 session (records KEEL's callbacks) ────── */

typedef struct { KlH2ServerSession base; KlAllocator *alloc;
                 KlH2ServerCallbacks cb; void *ud; } CapSession;
static CapSession *ta_cap;

static ssize_t cap_recv(KlH2ServerSession *s, const void *d, size_t l) { (void)s;(void)d; return (ssize_t)l; }
static int cap_submit(KlH2ServerSession *s, uint32_t id, int st, const char **n,
                      const char **v, int nh, const void *b, size_t bl) {
    (void)s;(void)id;(void)st;(void)n;(void)v;(void)nh;(void)b;(void)bl; return 0;
}
static int  cap_ww(KlH2ServerSession *s) { (void)s; return 0; }
static int  cap_flush(KlH2ServerSession *s) { (void)s; return 0; }
static int  cap_sd(KlH2ServerSession *s) { (void)s; return 0; }
static void cap_destroy(KlH2ServerSession *s) { CapSession *c = (CapSession *)s; kl_free(c->alloc, c, sizeof(*c)); }

static KlH2ServerSession *cap_factory(KlAllocator *a, KlH2ServerCallbacks *cbs, void *ud) {
    CapSession *c = kl_malloc(a, sizeof(*c));
    if (!c) return NULL;
    memset(c, 0, sizeof(*c));
    c->alloc = a; c->cb = *cbs; c->ud = ud;
    c->base.recv = cap_recv; c->base.submit_response = cap_submit;
    c->base.want_write = cap_ww; c->base.flush = cap_flush;
    c->base.shutdown = cap_sd; c->base.destroy = cap_destroy;
    ta_cap = c;
    return &c->base;
}

/* Build a KlConn on a socketpair with mock TLS + h2 config, run the handshake. */
static KlConnState ta_handshake_with_alpn(KlConn *conn, KlH2ServerConfig *h2cfg,
                                          int pfd[2], const char *alpn) {
    memset(conn, 0, sizeof(*conn));
    if (kl_test_socketpair(pfd) != 0) return KL_CONN_CLOSED;
    conn->stream.fd = pfd[1];
    conn->stream.alloc = &ta_alloc;
    conn->router = &ta_router;
    conn->h2_config = h2cfg;
    conn->tls = mock_tls_create(NULL, &ta_alloc);
    conn->state = KL_CONN_TLS_HANDSHAKE;
    mock_tls_alpn = alpn;
    return kl_conn_on_handshake(conn);
}

/* ── Part A: ALPN → adapter dispatch ────────────────────────────────── */

UTEST(alpn, negotiated_h2_enters_http2_adapter) {
    ta_setup();
    KlH2ServerConfig h2cfg = { .factory = cap_factory };
    KlConn conn; int pfd[2];
    KlConnState st = ta_handshake_with_alpn(&conn, &h2cfg, pfd, "h2");
    ASSERT_EQ(st, (KlConnState)KL_CONN_HTTP2);
    ASSERT_TRUE(conn.h2 != NULL);          /* HTTP/2 adapter owns the connection */
    kl_h2_server_cleanup(&conn);
    conn.tls->destroy(conn.tls);
    kl_test_closesock(pfd[0]); kl_test_closesock(pfd[1]);
    ta_teardown();
}

UTEST(alpn, negotiated_http11_enters_http1_adapter) {
    ta_setup();
    KlH2ServerConfig h2cfg = { .factory = cap_factory };
    KlConn conn; int pfd[2];
    KlConnState st = ta_handshake_with_alpn(&conn, &h2cfg, pfd, "http/1.1");
    ASSERT_EQ(st, (KlConnState)KL_CONN_READING);   /* HTTP/1.1 adapter */
    ASSERT_TRUE(conn.h2 == NULL);                  /* no HTTP/2 engaged */
    conn.tls->destroy(conn.tls);
    kl_test_closesock(pfd[0]); kl_test_closesock(pfd[1]);
    ta_teardown();
}

UTEST(alpn, no_alpn_falls_back_to_http1) {
    ta_setup();
    KlH2ServerConfig h2cfg = { .factory = cap_factory };
    KlConn conn; int pfd[2];
    KlConnState st = ta_handshake_with_alpn(&conn, &h2cfg, pfd, NULL);
    ASSERT_EQ(st, (KlConnState)KL_CONN_READING);   /* documented no-ALPN policy: HTTP/1.1 */
    ASSERT_TRUE(conn.h2 == NULL);
    conn.tls->destroy(conn.tls);
    kl_test_closesock(pfd[0]); kl_test_closesock(pfd[1]);
    ta_teardown();
}

UTEST(alpn, unsupported_alpn_does_not_engage_http2) {
    ta_setup();
    KlH2ServerConfig h2cfg = { .factory = cap_factory };
    KlConn conn; int pfd[2];
    /* An unexpected/unsupported selection must NOT be treated as h2. */
    KlConnState st = ta_handshake_with_alpn(&conn, &h2cfg, pfd, "spdy/3.1");
    ASSERT_EQ(st, (KlConnState)KL_CONN_READING);
    ASSERT_TRUE(conn.h2 == NULL);
    conn.tls->destroy(conn.tls);
    kl_test_closesock(pfd[0]); kl_test_closesock(pfd[1]);
    ta_teardown();
}

/* No h2_config → even an "h2" ALPN result stays HTTP/1.1 (server didn't enable h2). */
UTEST(alpn, h2_alpn_without_h2_config_stays_http1) {
    ta_setup();
    KlConn conn; int pfd[2];
    KlConnState st = ta_handshake_with_alpn(&conn, NULL, pfd, "h2");
    ASSERT_EQ(st, (KlConnState)KL_CONN_READING);
    ASSERT_TRUE(conn.h2 == NULL);
    conn.tls->destroy(conn.tls);
    kl_test_closesock(pfd[0]); kl_test_closesock(pfd[1]);
    ta_teardown();
}

/* ── Part B: single REST layer — same router/mw/handler, converged authority ── */

UTEST(alpn, http2_request_uses_shared_rest_layer) {
    ta_setup();
    KlH2ServerConfig h2cfg = { .factory = cap_factory };
    ta_cap = NULL;

    /* Confirm the SAME router entry an HTTP/1.1 request would hit. */
    KlRoute *route = NULL; KlParam params[8]; int nparams = 0;
    int rr = kl_router_match(&ta_router, "GET", 3, "/x", 2, &route, params, &nparams);
    ASSERT_EQ(rr, 200);
    ASSERT_TRUE(route != NULL && route->handler == ta_handler);

    /* Bring up HTTP/2 on a conn and drive one request through the real h2 path. */
    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);
    KlConn conn; memset(&conn, 0, sizeof(conn));
    conn.stream.fd = pfd[1]; conn.stream.alloc = &ta_alloc;
    int up = kl_h2_server_upgrade(&conn, &ta_router, &h2cfg, NULL, 0);
    ASSERT_EQ(up, (int)KL_CONN_HTTP2);
    ASSERT_TRUE(ta_cap != NULL);

    /* HTTP/2 HEADERS with :authority, then END_STREAM → the shared handler runs. */
    int rc = ta_cap->cb.on_request(ta_cap->ud, 1, "GET", 3, "/x", 2,
                                   "example.com", 11, NULL, NULL, NULL, NULL, 0);
    ASSERT_EQ(rc, 0);
    ta_cap->cb.on_stream_end(ta_cap->ud, 1);

    /* Same handler + same middleware executed; :authority converged onto host. */
    ASSERT_EQ(ta_handler_calls, 1);
    ASSERT_EQ(ta_mw_calls, 1);
    ASSERT_EQ(ta_seen_version, 2);                       /* it was the HTTP/2 path */
    ASSERT_STREQ(ta_seen_host, "example.com");           /* :authority -> host */

    kl_h2_server_cleanup(&conn);
    kl_test_closesock(pfd[0]); kl_test_closesock(pfd[1]);
    ta_teardown();
}

/* HTTP/1.1-style: a Host header reaches the handler as the same "host" field,
 * proving both protocols converge on one authority in the shared model. */
UTEST(alpn, http1_host_and_http2_authority_converge) {
    ta_setup();
    /* Simulate the handler reading host from an HTTP/1.1 request. */
    KlRequest req; memset(&req, 0, sizeof(req));
    req.method = "GET"; req.method_len = 3;
    req.path = "/x"; req.path_len = 2;
    req.version_major = 1; req.version_minor = 1;
    req.headers[0].name = "Host"; req.headers[0].name_len = 4;
    req.headers[0].value = "example.com"; req.headers[0].value_len = 11;
    req.num_headers = 1;
    KlResponse res; memset(&res, 0, sizeof(res));
    kl_response_init(&res, &ta_alloc);
    ta_handler(&req, &res, NULL);
    ASSERT_EQ(ta_seen_version, 1);
    ASSERT_STREQ(ta_seen_host, "example.com");
    kl_response_free(&res);
    ta_teardown();
}

UTEST_MAIN();
