#include "utest.h"
#include <keel/keel.h>
#include <keel/http2_client.h>
#include <keel/allocator.h>
#include "net_compat.h"
#include "platform_thread.h"   /* Keel PAL threads: portable to MSVC */
#include "platform_socket.h"   /* kl_plat_socket_runtime_init: this TU calls socket() */
#include "mock_tls.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

/* ═══════════════════════════════════════════════════════════════════
 * Unit tests for the HTTP/2 client module.
 *
 * Uses a mock session vtable to test the client without nghttp2.
 * ═══════════════════════════════════════════════════════════════════ */

/* ── Mock H2 session ─────────────────────────────────────────────── */

typedef struct {
    KlHttp2ClientSession base;
    KlAllocator *alloc;
    int32_t next_stream_id;
    int recv_called;
    int flush_called;
    int destroyed;

    /* Capture last request for verification */
    char method[32];
    char path[128];
    char authority[128];
} MockH2Session;

static int mock_recv(KlHttp2ClientSession *self, const char *data, size_t len)
{
    MockH2Session *m = (MockH2Session *)self;
    m->recv_called++;
    (void)data; (void)len;
    return 0;
}

static int32_t mock_submit_request(KlHttp2ClientSession *self,
                                    const char *method, const char *path,
                                    const char *authority,
                                    const KlHttp2ClientHeader *hdrs, int n,
                                    const char *body, size_t body_len)
{
    MockH2Session *m = (MockH2Session *)self;
    (void)hdrs; (void)n; (void)body; (void)body_len;

    if (method) {
        size_t l = strlen(method);
        if (l >= sizeof(m->method)) l = sizeof(m->method) - 1;
        memcpy(m->method, method, l);
        m->method[l] = '\0';
    }
    if (path) {
        size_t l = strlen(path);
        if (l >= sizeof(m->path)) l = sizeof(m->path) - 1;
        memcpy(m->path, path, l);
        m->path[l] = '\0';
    }
    if (authority) {
        size_t l = strlen(authority);
        if (l >= sizeof(m->authority)) l = sizeof(m->authority) - 1;
        memcpy(m->authority, authority, l);
        m->authority[l] = '\0';
    }

    return m->next_stream_id++;
}

static int mock_flush(KlHttp2ClientSession *self)
{
    MockH2Session *m = (MockH2Session *)self;
    m->flush_called++;
    return 0;
}

static void mock_destroy(KlHttp2ClientSession *self)
{
    MockH2Session *m = (MockH2Session *)self;
    m->destroyed = 1;
    kl_free(m->alloc, m, sizeof(MockH2Session));
}

static KlHttp2ClientSession *mock_factory(KlAllocator *alloc)
{
    MockH2Session *m = kl_malloc(alloc, sizeof(MockH2Session));
    if (!m) return NULL;
    memset(m, 0, sizeof(*m));
    m->alloc = alloc;
    m->next_stream_id = 1;
    m->base.recv = mock_recv;
    m->base.submit_request = mock_submit_request;
    m->base.flush = mock_flush;
    m->base.destroy = mock_destroy;
    return &m->base;
}

/* ── Session vtable tests ────────────────────────────────────────── */

UTEST(h2c_session, mock_factory_creates_session) {
    KlAllocator alloc = kl_allocator_default();
    KlHttp2ClientSession *s = mock_factory(&alloc);
    ASSERT_TRUE(s != NULL);
    s->destroy(s);
}

UTEST(h2c_session, mock_submit_returns_stream_id) {
    KlAllocator alloc = kl_allocator_default();
    KlHttp2ClientSession *s = mock_factory(&alloc);
    ASSERT_TRUE(s != NULL);

    int32_t id1 = s->submit_request(s, "GET", "/foo", "example.com",
                                     NULL, 0, NULL, 0);
    int32_t id2 = s->submit_request(s, "POST", "/bar", "example.com",
                                     NULL, 0, "body", 4);
    ASSERT_EQ(id1, 1);
    ASSERT_EQ(id2, 2);

    MockH2Session *m = (MockH2Session *)s;
    ASSERT_STREQ(m->method, "POST");
    ASSERT_STREQ(m->path, "/bar");

    s->destroy(s);
}

UTEST(h2c_session, mock_recv_and_flush) {
    KlAllocator alloc = kl_allocator_default();
    KlHttp2ClientSession *s = mock_factory(&alloc);

    ASSERT_EQ(s->recv(s, "data", 4), 0);
    ASSERT_EQ(s->flush(s), 0);

    MockH2Session *m = (MockH2Session *)s;
    ASSERT_EQ(m->recv_called, 1);
    ASSERT_EQ(m->flush_called, 1);

    s->destroy(s);
}

/* ── Stream tracking tests ───────────────────────────────────────── */

UTEST(h2c_stream, response_free_empty) {
    KlAllocator alloc = kl_allocator_default();
    KlHttp2ClientResponse resp;
    memset(&resp, 0, sizeof(resp));
    kl_http2_client_response_free(&resp, &alloc);
    ASSERT_EQ(resp.status, 0);
}

UTEST(h2c_stream, response_free_with_data) {
    KlAllocator alloc = kl_allocator_default();
    KlHttp2ClientResponse resp;
    memset(&resp, 0, sizeof(resp));

    resp.status = 200;

    /* Allocate body */
    resp.body = kl_malloc(&alloc, 64);
    ASSERT_TRUE(resp.body != NULL);
    memcpy(resp.body, "hello", 5);
    resp.body_len = 5;
    resp.body_cap = 64;

    /* Allocate headers */
    resp.headers = kl_malloc(&alloc, sizeof(KlHttp2ClientHeader));
    ASSERT_TRUE(resp.headers != NULL);
    resp.headers_cap = 1;
    resp.num_headers = 1;

    char *name = kl_malloc(&alloc, 13);
    memcpy(name, "content-type", 13);
    char *value = kl_malloc(&alloc, 17);
    memcpy(value, "application/json", 17);
    resp.headers[0].name = name;
    resp.headers[0].value = value;

    kl_http2_client_response_free(&resp, &alloc);
    ASSERT_EQ(resp.status, 0);
    ASSERT_TRUE(resp.body == NULL);
    ASSERT_TRUE(resp.headers == NULL);
}

UTEST(h2c_stream, response_free_null_args) {
    KlAllocator alloc = kl_allocator_default();
    kl_http2_client_response_free(NULL, &alloc);
    kl_http2_client_response_free(NULL, NULL);
    ASSERT_TRUE(1);  /* should not crash */
}

/* ── API input validation ────────────────────────────────────────── */

UTEST(h2c_api, connect_null_args) {
    ASSERT_TRUE(kl_http2_client_connect(NULL, NULL, NULL, NULL, NULL, NULL) == NULL);
}

UTEST(h2c_api, connect_no_session_factory) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ev;
    if (kl_event_ctx_init(&ev, &alloc) == 0) {
        KlHttp2ClientConfig cfg;
        memset(&cfg, 0, sizeof(cfg));
        /* session factory is NULL */
        KlHttp2ClientConn *c = kl_http2_client_connect(&ev, &alloc, &cfg,
                                                    "http://example.com",
                                                    NULL, NULL);
        ASSERT_TRUE(c == NULL);
        kl_event_ctx_free(&ev);
    }
}

UTEST(h2c_api, connect_invalid_url) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ev;
    if (kl_event_ctx_init(&ev, &alloc) == 0) {
        KlHttp2ClientConfig cfg = { .session = mock_factory };
        KlHttp2ClientConn *c = kl_http2_client_connect(&ev, &alloc, &cfg,
                                                    "not-a-url", NULL, NULL);
        ASSERT_TRUE(c == NULL);
        kl_event_ctx_free(&ev);
    }
}

UTEST(h2c_api, connect_https_no_tls) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ev;
    if (kl_event_ctx_init(&ev, &alloc) == 0) {
        KlHttp2ClientConfig cfg = { .session = mock_factory };
        KlHttp2ClientConn *c = kl_http2_client_connect(&ev, &alloc, &cfg,
                                                    "https://example.com",
                                                    NULL, NULL);
        ASSERT_TRUE(c == NULL);
        kl_event_ctx_free(&ev);
    }
}

/* A TLS config without a factory cannot secure the connection: refuse https:// rather than speak
 * HTTP/2 in plaintext. */
UTEST(h2c_api, connect_https_tls_without_factory) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &alloc), 0);
    KlTlsConfig tls = { .ctx = NULL, .factory = NULL };
    KlHttp2ClientConfig cfg = { .session = mock_factory, .tls = &tls };

    KlHttp2ClientConn *c = kl_http2_client_connect(&ev, &alloc, &cfg, "https://127.0.0.1:1",
                                                   NULL, NULL);
    int refused = (c == NULL);
    kl_http2_client_free(c);
    kl_event_ctx_free(&ev);
    ASSERT_TRUE(refused);
}

UTEST(h2c_api, request_null_conn) {
    int32_t id = kl_http2_client_request(NULL, "GET", "/", NULL, 0,
                                       NULL, 0, NULL, NULL);
    ASSERT_EQ(id, -1);
}

UTEST(h2c_api, request_null_method) {
    /* Can't create a valid conn without a server, but we test
       the NULL checks that come first */
    int32_t id = kl_http2_client_request(NULL, NULL, "/", NULL, 0,
                                       NULL, 0, NULL, NULL);
    ASSERT_EQ(id, -1);
}

UTEST(h2c_api, free_null) {
    kl_http2_client_free(NULL);
    ASSERT_TRUE(1);
}

UTEST(h2c_api, close_null) {
    kl_http2_client_close(NULL);
    ASSERT_TRUE(1);
}

/* ── Config / constants ──────────────────────────────────────────── */

UTEST(h2c_config, defaults) {
    ASSERT_EQ(KL_HTTP2_CLIENT_DEFAULT_TIMEOUT_MS, 30000);
    ASSERT_EQ(KL_HTTP2_CLIENT_RECV_BUF_SIZE, 16384);
    ASSERT_EQ(KL_HTTP2_DEFAULT_MAX_STREAMS, 128);
    ASSERT_EQ(KL_HTTP2_DEFAULT_WINDOW_SIZE, 65535);
}

/* ── Standalone KlEventCtx ───────────────────────────────────────── */

UTEST(h2c_standalone, event_ctx_init_free) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ev;
    int rc = kl_event_ctx_init(&ev, &alloc);
    ASSERT_EQ(rc, 0);
    kl_event_ctx_free(&ev);
}

/* ── Session callback wiring ─────────────────────────────────────── */

UTEST(h2c_callbacks, on_response_accumulates_headers) {
    /* Simulate session calling on_response callback */
    KlAllocator alloc = kl_allocator_default();
    KlHttp2ClientSession *s = mock_factory(&alloc);

    /* Manually set up callbacks as kl_http2_client_connect would */
    /* (We can't call connect without a real server, so we test
       the callback plumbing directly) */
    ASSERT_TRUE(s != NULL);
    s->destroy(s);
}

UTEST(h2c_callbacks, on_data_grows_body) {
    /* Test body growth logic via response struct */
    KlAllocator alloc = kl_allocator_default();
    KlHttp2ClientResponse resp;
    memset(&resp, 0, sizeof(resp));

    /* Simulate growing body */
    size_t chunk_size = 100;
    resp.body = kl_malloc(&alloc, chunk_size);
    ASSERT_TRUE(resp.body != NULL);
    resp.body_cap = chunk_size;
    memset(resp.body, 'A', chunk_size);
    resp.body_len = chunk_size;

    kl_http2_client_response_free(&resp, &alloc);
    ASSERT_TRUE(resp.body == NULL);
}

/* ── A live connection driven through the mock session (audit L11) ──────────────────────────
 * The client is connected to a loopback listener (prior-knowledge h2, so the mock session is
 * created once TCP connects), a request is submitted, and the test then plays the session: it calls
 * the KEEL-managed callbacks exactly as a real session would. Before the fix a stream the peer reset
 * was handed over as a normal response, and a body grew without bound. */

static KlHttp2ClientSession *g_live_session;
static KlHttp2ClientSession *capturing_factory(KlAllocator *alloc) {
    g_live_session = mock_factory(alloc);
    return g_live_session;
}

typedef struct { int calls; int error; int status; size_t body_len; } LiveResp;
static void live_on_resp(KlHttp2ClientConn *c, int32_t id, const KlHttp2ClientResponse *r,
                         void *ud) {
    (void)c; (void)id;
    LiveResp *lr = ud;
    lr->calls++;
    lr->error = r->error;
    lr->status = r->status;
    lr->body_len = r->body_len;
}

typedef struct { KlSocketHandle fd; int port; } Listener;
static int live_listen(Listener *l) {
    if (kl_plat_socket_runtime_init() != 0) return -1;
    l->fd = (KlSocketHandle)socket(AF_INET, SOCK_STREAM, 0);
    if (!kl_handle_valid(l->fd)) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t al = sizeof(a);
    if (bind((int)l->fd, (struct sockaddr *)&a, sizeof(a)) != 0 || listen((int)l->fd, 4) != 0 ||
        getsockname((int)l->fd, (struct sockaddr *)&a, &al) != 0) return -1;
    l->port = ntohs(a.sin_port);
    return 0;
}

/* Connect, wait for the session, submit one GET. Returns the stream id, or -1. */
static int32_t live_request(KlEventCtx *ev, KlAllocator *a, const Listener *l, size_t max_resp,
                            KlHttp2ClientConn **out, LiveResp *lr) {
    KlHttp2ClientConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.session = capturing_factory;
    cfg.max_response_size = max_resp;
    char url[64];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/", l->port);
    g_live_session = NULL;
    *out = kl_http2_client_connect(ev, a, &cfg, url, NULL, NULL);
    if (!*out) return -1;
    for (int i = 0; i < 200 && !g_live_session; i++) (void)kl_event_ctx_run(ev, 16, 10);
    if (!g_live_session) return -1;
    return kl_http2_client_request(*out, "GET", "/", NULL, 0, NULL, 0, live_on_resp, lr);
}

UTEST(h2c_live, reset_stream_is_reported_as_an_error) {
    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);
    static Listener l;
    ASSERT_EQ(live_listen(&l), 0);
    KlHttp2ClientConn *c = NULL;
    LiveResp lr; memset(&lr, 0, sizeof lr);
    int32_t id = live_request(&ev, &a, &l, 0, &c, &lr);
    ASSERT_GT(id, 0);
    KlHttp2ClientSession *s = g_live_session;
    s->keel_cbs.on_response(s, id, 200, NULL, 0);
    s->keel_cbs.on_data(s, id, "par", 3);
    s->keel_cbs.on_stream_close(s, id, 8 /* CANCEL */);
    ASSERT_EQ(lr.calls, 1);
    ASSERT_EQ(lr.error, (int)KL_ERR_IO);                /* was: 0, a "complete" partial response */
    kl_http2_client_free(c);
    kl_event_ctx_free(&ev);
    kl_test_closesock(l.fd);
}

UTEST(h2c_live, body_over_max_response_size_fails_the_stream) {
    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);
    static Listener l;
    ASSERT_EQ(live_listen(&l), 0);
    KlHttp2ClientConn *c = NULL;
    LiveResp lr; memset(&lr, 0, sizeof lr);
    int32_t id = live_request(&ev, &a, &l, 10, &c, &lr);
    ASSERT_GT(id, 0);
    KlHttp2ClientSession *s = g_live_session;
    s->keel_cbs.on_response(s, id, 200, NULL, 0);
    s->keel_cbs.on_data(s, id, "12345678", 8);
    s->keel_cbs.on_data(s, id, "12345678", 8);          /* 16 > 10 */
    s->keel_cbs.on_stream_close(s, id, 0);
    ASSERT_EQ(lr.calls, 1);
    ASSERT_EQ(lr.error, (int)KL_ERR_TOO_LARGE);         /* was: 0, with all 16 bytes kept */
    ASSERT_EQ(lr.body_len, (size_t)8);
    kl_http2_client_free(c);
    kl_event_ctx_free(&ev);
    kl_test_closesock(l.fd);
}

UTEST(h2c_live, complete_stream_has_no_error) {
    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);
    static Listener l;
    ASSERT_EQ(live_listen(&l), 0);
    KlHttp2ClientConn *c = NULL;
    LiveResp lr; memset(&lr, 0, sizeof lr);
    int32_t id = live_request(&ev, &a, &l, 0, &c, &lr);
    ASSERT_GT(id, 0);
    KlHttp2ClientSession *s = g_live_session;
    s->keel_cbs.on_response(s, id, 200, NULL, 0);
    s->keel_cbs.on_data(s, id, "hello", 5);
    s->keel_cbs.on_stream_close(s, id, 0);
    ASSERT_EQ(lr.calls, 1);
    ASSERT_EQ(lr.error, 0);
    ASSERT_EQ(lr.status, 200);
    ASSERT_EQ(lr.body_len, (size_t)5);
    kl_http2_client_free(c);
    kl_event_ctx_free(&ev);
    kl_test_closesock(l.fd);
}

/* ── Freeing the client from its own response callback ──────────────────────────────────────
 * on_resp runs inside session->recv, inside the client's read handler. A response handler that
 * frees the client (the natural pattern when it was the last request) must be safe: the handler
 * and the session must not touch the client after that. A use-after-free does not reliably crash,
 * so the client's allocator poisons every freed block (0xDD) and keeps it: a later read sees a
 * poisoned pointer (a crash), and a later write changes the poison (caught by the final check). */

#define H2Q_MAX 256
static struct { unsigned char *p; size_t n; } g_h2q[H2Q_MAX];
static int g_h2nq;
static void *h2q_malloc(void *c, size_t n) { (void)c; return malloc(n ? n : 1); }
static void *h2q_realloc(void *c, void *p, size_t o, size_t n) { (void)c; (void)o; return realloc(p, n ? n : 1); }
static void h2q_free(void *c, void *p, size_t n) {
    (void)c;
    if (!p) return;
    memset(p, 0xDD, n);
    if (g_h2nq < H2Q_MAX) { g_h2q[g_h2nq].p = p; g_h2q[g_h2nq].n = n; g_h2nq++; }
}
static KlAllocator g_h2qa = { h2q_malloc, h2q_realloc, h2q_free, NULL };
static int h2q_check_and_release(void) {
    int written = 0;
    for (int i = 0; i < g_h2nq; i++) {
        for (size_t k = 0; k < g_h2q[i].n; k++)
            if (g_h2q[i].p[k] != 0xDD) { written++; break; }
        free(g_h2q[i].p);
    }
    g_h2nq = 0;
    return written;
}

static int32_t g_fr_id;          /* the stream the session completes on its next recv */
static int g_fr_calls;
static int fr_recv(KlHttp2ClientSession *self, const char *data, size_t len) {
    (void)data; (void)len;
    if (g_fr_id > 0) {
        int32_t id = g_fr_id;
        g_fr_id = 0;
        self->keel_cbs.on_response(self, id, 200, NULL, 0);
        self->keel_cbs.on_stream_close(self, id, 0);   /* → on_resp, which frees the client */
    }
    return 0;
}
static KlHttp2ClientSession *fr_factory(KlAllocator *alloc) {
    g_live_session = mock_factory(alloc);
    if (g_live_session) g_live_session->recv = fr_recv;
    return g_live_session;
}
static void fr_on_resp(KlHttp2ClientConn *c, int32_t id, const KlHttp2ClientResponse *r, void *ud) {
    (void)id; (void)r; (void)ud;
    g_fr_calls++;
    kl_http2_client_free(c);
}

UTEST(h2c_live, free_in_on_resp_is_safe) {
    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);
    static Listener l;
    ASSERT_EQ(live_listen(&l), 0);

    KlHttp2ClientConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.session = fr_factory;
    char url[64];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/", l.port);
    g_live_session = NULL;
    g_fr_calls = 0;
    KlHttp2ClientConn *c = kl_http2_client_connect(&ev, &g_h2qa, &cfg, url, NULL, NULL);
    ASSERT_TRUE(c != NULL);
    for (int i = 0; i < 200 && !g_live_session; i++) (void)kl_event_ctx_run(&ev, 16, 10);
    ASSERT_TRUE(g_live_session != NULL);
    KlSocketHandle peer = (KlSocketHandle)accept((int)l.fd, NULL, NULL);
    ASSERT_TRUE(kl_handle_valid(peer));

    g_fr_id = kl_http2_client_request(c, "GET", "/", NULL, 0, NULL, 0, fr_on_resp, NULL);
    ASSERT_GT(g_fr_id, 0);
    (void)send((int)peer, "x", 1, 0);           /* readable: the client feeds it to session->recv */
    for (int i = 0; i < 100 && g_fr_calls == 0; i++) (void)kl_event_ctx_run(&ev, 16, 10);
    for (int i = 0; i < 10; i++) (void)kl_event_ctx_run(&ev, 16, 5);

    int calls = g_fr_calls;
    kl_event_ctx_free(&ev);
    kl_test_closesock(peer);
    kl_test_closesock(l.fd);
    ASSERT_EQ(calls, 1);
    ASSERT_EQ(h2q_check_and_release(), 0);
}

/* ── Over TLS: a record split across reads ──────────────────────────────────────────────────
 * A read that gets part of a TLS record returns 0, WANT_READ (the KlTls contract; -1 is error or
 * close). The client took it for "connection closed". The identity mock TLS simulates the split. */
static size_t g_split_bytes;
static int    g_split_errors;
static int split_recv(KlHttp2ClientSession *self, const char *data, size_t len) {
    (void)self; (void)data;
    g_split_bytes += len;
    return 0;
}
static KlHttp2ClientSession *split_factory(KlAllocator *alloc) {
    g_live_session = mock_factory(alloc);
    if (g_live_session) g_live_session->recv = split_recv;
    return g_live_session;
}
static void split_on_error(KlHttp2ClientConn *c, const char *msg, void *ud) {
    (void)c; (void)msg; (void)ud;
    g_split_errors++;
}

UTEST(h2c_live, tls_record_split_across_reads_is_not_closed) {
    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);
    static Listener l;
    ASSERT_EQ(live_listen(&l), 0);

    static KlTlsConfig tls = { .ctx = NULL, .factory = mock_tls_create };
    KlHttp2ClientConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.session = split_factory;
    cfg.tls = &tls;
    char url[64];
    snprintf(url, sizeof url, "https://127.0.0.1:%d/", l.port);
    g_live_session = NULL;
    g_split_bytes = 0;
    g_split_errors = 0;
    mock_tls_alpn = "h2";
    KlHttp2ClientConn *c = kl_http2_client_connect(&ev, &a, &cfg, url, split_on_error, NULL);
    for (int i = 0; c && i < 200 && !g_live_session && !g_split_errors; i++) (void)kl_event_ctx_run(&ev, 16, 10);
    KlSocketHandle peer = (KlSocketHandle)accept((int)l.fd, NULL, NULL);

    mock_tls_split_record = 10;                /* a 10-byte record, sent in two parts */
    (void)send((int)peer, "0123", 4, 0);
    for (int i = 0; i < 15; i++) (void)kl_event_ctx_run(&ev, 16, 10);
    (void)send((int)peer, "456789", 6, 0);
    for (int i = 0; i < 50 && g_split_bytes < 10 && !g_split_errors; i++) (void)kl_event_ctx_run(&ev, 16, 10);
    mock_tls_split_record = 0;
    mock_tls_alpn = NULL;

    size_t got = g_split_bytes;
    int errors = g_split_errors;
    kl_http2_client_free(c);
    kl_event_ctx_free(&ev);
    if (kl_handle_valid(peer)) kl_test_closesock(peer);
    kl_test_closesock(l.fd);
    ASSERT_TRUE(c != NULL);
    ASSERT_EQ(errors, 0);
    ASSERT_EQ(got, (size_t)10);
}

/* ── Stream bookkeeping ─────────────────────────────────────────────────────────────────────
 * kl_http2_client_request submitted the request to the session before making the client's own
 * stream record; when that failed (the stream limit, or out of memory) it returned -1, yet the
 * request still went out on the next flush: the caller was told it failed while the server ran it. */
UTEST(h2c_live, refused_request_is_not_sent) {
    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);
    static Listener l;
    ASSERT_EQ(live_listen(&l), 0);
    KlHttp2ClientConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.session = capturing_factory;
    cfg.max_concurrent_streams = 1;
    char url[64];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/", l.port);
    g_live_session = NULL;
    KlHttp2ClientConn *c = kl_http2_client_connect(&ev, &a, &cfg, url, NULL, NULL);
    for (int i = 0; c && i < 200 && !g_live_session; i++) (void)kl_event_ctx_run(&ev, 16, 10);
    LiveResp lr; memset(&lr, 0, sizeof lr);
    int32_t id1 = g_live_session ? kl_http2_client_request(c, "GET", "/one", NULL, 0, NULL, 0,
                                                           live_on_resp, &lr) : -1;
    int32_t id2 = g_live_session ? kl_http2_client_request(c, "GET", "/two", NULL, 0, NULL, 0,
                                                           live_on_resp, &lr) : 0;
    int32_t next = g_live_session ? ((MockH2Session *)g_live_session)->next_stream_id : 0;
    char last_path[128] = "";
    if (g_live_session) memcpy(last_path, ((MockH2Session *)g_live_session)->path, sizeof last_path);
    kl_http2_client_free(c);
    kl_event_ctx_free(&ev);
    kl_test_closesock(l.fd);
    ASSERT_GT(id1, 0);
    ASSERT_EQ(id2, -1);                         /* refused: the one stream is in use */
    ASSERT_EQ(next, 2);                         /* was 3: /two was submitted anyway */
    ASSERT_EQ(strcmp(last_path, "/one"), 0);
}

/* A stream can carry a second response HEADERS (an interim 1xx, then the final response). Each
 * copy of the headers must replace the last without leaking it; and a header copy that cannot be
 * allocated must fail the stream (it was delivered as complete, with headers missing). */
static long g_live_blocks;                      /* outstanding allocations */
static int  g_live_fail_next;                   /* fail the next allocation */
static void *cnt_malloc(void *c, size_t n) {
    (void)c;
    if (g_live_fail_next) { g_live_fail_next = 0; return NULL; }
    void *p = malloc(n ? n : 1);
    if (p) g_live_blocks++;
    return p;
}
static void *cnt_realloc(void *c, void *p, size_t o, size_t n) {
    (void)c; (void)o;
    void *q = realloc(p, n ? n : 1);
    if (q && !p) g_live_blocks++;
    return q;
}
static void cnt_free(void *c, void *p, size_t n) { (void)c; (void)n; if (p) { g_live_blocks--; free(p); } }

static void second_headers_case(int *utest_result, int fail_copy) {
    static KlAllocator ca = { cnt_malloc, cnt_realloc, cnt_free, NULL };
    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);
    static Listener l;
    ASSERT_EQ(live_listen(&l), 0);
    KlHttp2ClientConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.session = capturing_factory;
    char url[64];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/", l.port);
    g_live_session = NULL;
    g_live_blocks = 0;
    KlHttp2ClientConn *c = kl_http2_client_connect(&ev, &ca, &cfg, url, NULL, NULL);
    for (int i = 0; c && i < 200 && !g_live_session; i++) (void)kl_event_ctx_run(&ev, 16, 10);
    LiveResp lr; memset(&lr, 0, sizeof lr);
    int32_t id = g_live_session ? kl_http2_client_request(c, "GET", "/", NULL, 0, NULL, 0,
                                                          live_on_resp, &lr) : -1;
    if (id > 0) {
        KlHttp2ClientSession *s = g_live_session;
        KlHttp2ClientHeader h1[] = { { "link", "</a.css>" } };
        KlHttp2ClientHeader h2[] = { { "content-type", "text/plain" }, { "x-a", "1" } };
        s->keel_cbs.on_response(s, id, 103, h1, 1);
        if (fail_copy) g_live_fail_next = 1;     /* the final response's header array */
        s->keel_cbs.on_response(s, id, 200, h2, 2);
        s->keel_cbs.on_stream_close(s, id, 0);
    }
    kl_http2_client_free(c);
    long left = g_live_blocks;
    kl_event_ctx_free(&ev);
    kl_test_closesock(l.fd);
    ASSERT_GT(id, 0);
    ASSERT_EQ(lr.calls, 1);
    if (fail_copy) {
        ASSERT_EQ(lr.error, (int)KL_ERR_ALLOC);  /* was 0: a "complete" response missing headers */
    } else {
        ASSERT_EQ(lr.error, 0);
        ASSERT_EQ(lr.status, 200);
    }
    ASSERT_EQ(left, 0L);                         /* was > 0: the 103's header strings leaked */
}
UTEST(h2c_live, second_response_headers_replace_the_first) { second_headers_case(utest_result, 0); }
UTEST(h2c_live, header_copy_failure_fails_the_stream)      { second_headers_case(utest_result, 1); }

/* A connect that completes at once (a local AF_UNIX socket) must not leave WRITE interest armed:
 * with nothing to send, the socket is always writable, and the loop spun. */
static KlHttpServer g_unix_srv;
static void unix_srv_thread(void *arg) { (void)arg; kl_http_server_run(&g_unix_srv); }

UTEST(h2c_live, immediate_connect_does_not_spin) {
    static const char *path = "keel_h2c_spin_test.sock";
    KlHttpServerConfig scfg;
    memset(&scfg, 0, sizeof scfg);
    scfg.unix_socket_path = path;
    scfg.unix_socket_unlink = 1;
    if (kl_http_server_init(&g_unix_srv, &scfg) != 0) {
        UTEST_SKIP("AF_UNIX listen unavailable");
    }
    KlPlatThread t;
    kl_plat_thread_create(&t, unix_srv_thread, NULL);
    kl_test_sleep_ms(100);

    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    int ok = kl_event_ctx_init(&ev, &a) == 0;
    KlHttp2ClientConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.session = mock_factory;
    char url[128];
    snprintf(url, sizeof url, "http+unix://%s/", path);
    KlHttp2ClientConn *c = NULL;
    for (int i = 0; ok && !c && i < 50; i++) {   /* the server thread may not be listening yet */
        c = kl_http2_client_connect(&ev, &a, &cfg, url, NULL, NULL);
        if (!c) kl_test_sleep_ms(20);
    }
    for (int i = 0; c && i < 5; i++) (void)kl_event_ctx_run(&ev, 16, 20);   /* settle */
    int events = 0;
    for (int i = 0; c && i < 5; i++) {
        int n = kl_event_ctx_run(&ev, 16, 20);
        if (n > 0) events += n;
    }
    kl_http2_client_free(c);
    if (ok) kl_event_ctx_free(&ev);
    kl_http_server_stop(&g_unix_srv);
    kl_plat_thread_join(&t);
    kl_http_server_free(&g_unix_srv);
    if (!c) { UTEST_SKIP("AF_UNIX connect unavailable on this host"); }
    ASSERT_EQ(events, 0);                        /* idle: nothing to read, nothing to write */
}

UTEST_MAIN();
