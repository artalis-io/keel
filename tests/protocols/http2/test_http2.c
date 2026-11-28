#include "utest.h"
#include "../../../src/protocols/http/http_conn_internal.h"
#include <keel/keel.h>
#include <keel/http2.h>
#include <keel/http2_server.h>
#include "http2_internal.h"
#include <keel/http_connection.h>
#include <keel/http_router.h>
#include <keel/http_body_reader.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "net_compat.h"
#include "atomic_flags.h"    /* the mock's recv_count, polled across threads by one test */
#include "platform_thread.h"   /* Keel PAL threads: portable to MSVC */
#include "http_internal.h"     /* kl_http_server_sweep_conn_timeouts, kl_http_server_settle_h2 */
#include "event_caps.h"        /* kl_event_caps: is the server's loop a completion loop? */
#include "kl_atomic.h"         /* a destroy count read from the test thread */
#include <keel/clock.h>

#if defined(_WIN32)
#define T_SHUT_WR SD_SEND
#else
#define T_SHUT_WR SHUT_WR
#endif

/* ═══════════════════════════════════════════════════════════════════
 * Mock H2 Session
 *
 * Embeds KlHttp2ServerSession base + tracking fields for test assertions.
 * Factory stores KEEL's callbacks so tests can invoke them directly.
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    KlHttp2ServerSession base;

    /* Tracking */
    int recv_count;
    int submit_count;
    int want_write_count;
    int flush_count;
    int shutdown_count;
    int destroy_count;

    /* Captured submit_response args */
    uint32_t last_stream_id;
    int last_status;
    char last_body[4096];
    size_t last_body_len;
    char last_hdr_names[16][256];
    char last_hdr_values[16][256];
    int last_num_headers;

    /* Configurable return values */
    kl_ssize_t recv_return;
    int submit_return;
    int want_write_return;
    int flush_return;
    int shutdown_return;
    void (*on_flush)(void);           /* runs inside flush, as the session's send would call out */
    int resume_fail;                  /* this many resume_body calls fail (no memory), then succeed */

    /* If set, factory skips setting vtable (for vtable validation tests) */
    int skip_vtable_init;

    /* h2c Upgrade: 1 = the factory installs the upgrade op */
    int with_upgrade;
    int upgrade_count;
    char upgrade_settings[64];
    int upgrade_head;
    int upgrade_fail;                 /* the upgrade op refuses the settings (a bad value, say) */

    /* want_read: 1 = the factory installs it, returning want_read_return */
    int with_want_read;
    int want_read_return;

    /* flush writes this many bytes through KEEL's send callback */
    size_t flush_send_len;

    /* The first bytes fed to recv */
    char first_recv[64];
    size_t first_recv_len;

    /* close_on_flush: flush reports a close (on_stream_reset) for every stream answered since the
     * last flush, from inside the flush, as a real session does once a response's END_STREAM goes
     * out on a stream the client has already half-closed. */
    int close_on_flush;
    uint32_t to_close[16];
    int n_to_close;
    int closes_reported;

    /* Streamed responses: 1 = the factory installs submit_stream_response / resume_body /
     * reset_stream. */
    int with_stream;
    int stream_submit_count;
    int resume_count;
    int reset_count;
    uint32_t last_reset_id;
    uint32_t last_reset_code;

    /* A live connection's output: once recv sees done_marker, flush writes out_total bytes of a
     * pattern through KEEL's send callback (as much as it takes each time), and the session is done
     * (wants neither read nor write) once all of it went. live_out[i] is the i-th session's total. */
    size_t goaway_len;                /* shutdown queues a GOAWAY of this size, sent by the next flush */
    int goaway_pending;
    const char *done_marker;
    size_t live_out[4];
    int sessions;
    size_t out_total, out_sent;
    int done_after_out;

    /* KEEL's callbacks (stored by factory) */
    KlHttp2ServerCallbacks callbacks;
    void *cb_user_data;
} MockH2Session;

static unsigned char live_pattern(size_t i) { return (unsigned char)(i % 251u); }

static int mock_has_marker(const MockH2Session *m, const void *data, size_t len) {
    if (!m->done_marker || !data) return 0;
    size_t ml = strlen(m->done_marker);
    const char *d = data;
    for (size_t i = 0; ml && i + ml <= len; i++)
        if (memcmp(d + i, m->done_marker, ml) == 0) return 1;
    return 0;
}

static kl_ssize_t mock_recv(KlHttp2ServerSession *self, const void *data, size_t len) {
    MockH2Session *m = (MockH2Session *)self;
    if (mock_has_marker(m, data, len) && !m->done_after_out) {
        m->out_total = m->live_out[(m->sessions - 1) & 3];
        m->out_sent = 0;
        m->done_after_out = 1;
    }
    if (m->recv_count == 0 && data) {
        m->first_recv_len = len < sizeof(m->first_recv) ? len : sizeof(m->first_recv);
        memcpy(m->first_recv, data, m->first_recv_len);
    }
    T_INC(m->recv_count);
    if (m->recv_return >= 0)
        return (kl_ssize_t)len;
    return m->recv_return;
}

static int mock_submit_response(KlHttp2ServerSession *self, uint32_t stream_id,
                                 int status, const char **hdr_names,
                                 const char **hdr_values, int num_headers,
                                 const void *body, size_t body_len) {
    MockH2Session *m = (MockH2Session *)self;
    m->submit_count++;
    if (m->close_on_flush && m->n_to_close < 16)
        m->to_close[m->n_to_close++] = stream_id;
    m->last_stream_id = stream_id;
    m->last_status = status;
    m->last_num_headers = num_headers;
    if (body && body_len > 0 && body_len < sizeof(m->last_body)) {
        memcpy(m->last_body, body, body_len);
        m->last_body_len = body_len;
    } else {
        m->last_body_len = 0;
    }
    for (int i = 0; i < num_headers && i < 16; i++) {
        if (hdr_names[i])
            strncpy(m->last_hdr_names[i], hdr_names[i],
                    sizeof(m->last_hdr_names[i]) - 1);
        if (hdr_values[i])
            strncpy(m->last_hdr_values[i], hdr_values[i],
                    sizeof(m->last_hdr_values[i]) - 1);
    }
    return m->submit_return;
}

static int mock_want_write(KlHttp2ServerSession *self) {
    MockH2Session *m = (MockH2Session *)self;
    m->want_write_count++;
    return m->want_write_return || m->out_sent < m->out_total || m->goaway_pending;
}

static int mock_flush(KlHttp2ServerSession *self) {
    MockH2Session *m = (MockH2Session *)self;
    m->flush_count++;
    if (m->flush_send_len > 0) {
        static const char frames[64] = {0};
        size_t n = m->flush_send_len < sizeof frames ? m->flush_send_len : sizeof frames;
        (void)m->callbacks.send(m->cb_user_data, frames, n);
    }
    if (m->close_on_flush) {
        uint32_t ids[16];
        int n = m->n_to_close;
        memcpy(ids, m->to_close, sizeof(ids));
        m->n_to_close = 0;
        for (int i = 0; i < n; i++) {
            m->closes_reported++;
            m->callbacks.on_stream_reset(m->cb_user_data, ids[i], 0);
        }
    }
    if (m->goaway_pending) {
        static const char goaway[64] = {0};
        size_t n = m->goaway_len < sizeof goaway ? m->goaway_len : sizeof goaway;
        if (m->callbacks.send(m->cb_user_data, goaway, n) == (kl_ssize_t)n) m->goaway_pending = 0;
    }
    while (m->out_sent < m->out_total) {
        unsigned char chunk[4096];
        size_t n = m->out_total - m->out_sent;
        if (n > sizeof chunk) n = sizeof chunk;
        for (size_t i = 0; i < n; i++) chunk[i] = live_pattern(m->out_sent + i);
        kl_ssize_t w = m->callbacks.send(m->cb_user_data, chunk, n);
        if (w <= 0) break;                                 /* would block: kept for the next flush */
        m->out_sent += (size_t)w;
    }
    if (m->on_flush) m->on_flush();
    return m->flush_return;
}

static int mock_want_read(KlHttp2ServerSession *self) {
    const MockH2Session *m = (const MockH2Session *)self;
    if (m->done_after_out && m->out_sent >= m->out_total) return 0;   /* GOAWAY out, nothing left */
    return m->want_read_return;
}

static int mock_shutdown(KlHttp2ServerSession *self) {
    MockH2Session *m = (MockH2Session *)self;
    m->shutdown_count++;
    if (m->goaway_len) m->goaway_pending = 1;
    return m->shutdown_return;
}

static int mock_upgrade(KlHttp2ServerSession *self, const char *settings, size_t len, int head) {
    MockH2Session *m = (MockH2Session *)self;
    m->upgrade_count++;
    m->upgrade_head = head;
    if (len >= sizeof(m->upgrade_settings)) len = sizeof(m->upgrade_settings) - 1;
    memcpy(m->upgrade_settings, settings, len);
    m->upgrade_settings[len] = '\0';
    return m->upgrade_fail ? -1 : 0;
}

static int mock_submit_stream_response(KlHttp2ServerSession *self, uint32_t stream_id, int status,
                                       const char **names, const char **values, int n) {
    (void)names; (void)values; (void)n;
    MockH2Session *m = (MockH2Session *)self;
    m->stream_submit_count++;
    m->last_stream_id = stream_id;
    m->last_status = status;
    return 0;
}

static int mock_resume_body(KlHttp2ServerSession *self, uint32_t stream_id) {
    (void)stream_id;
    MockH2Session *m = (MockH2Session *)self;
    m->resume_count++;
    if (m->resume_fail > 0) { m->resume_fail--; return -1; }
    return 0;
}

static int mock_reset_stream(KlHttp2ServerSession *self, uint32_t stream_id, uint32_t code) {
    MockH2Session *m = (MockH2Session *)self;
    m->reset_count++;
    m->last_reset_id = stream_id;
    m->last_reset_code = code;
    return 0;
}

static int g_mock_destroys;      /* every destroy, read across threads (kl_atomic) */
static void mock_destroy(KlHttp2ServerSession *self) {
    MockH2Session *m = (MockH2Session *)self;
    m->destroy_count++;
    kl_atomic_store_int(&g_mock_destroys, kl_atomic_load_int(&g_mock_destroys) + 1);
}

/* Global mock pointer for factory access in tests */
static MockH2Session *g_mock_session = NULL;

static KlHttp2ServerSession *mock_factory(KlAllocator *alloc,
                                  KlHttp2ServerCallbacks *callbacks,
                                  void *user_data) {
    (void)alloc;
    MockH2Session *m = g_mock_session;

    if (!m->skip_vtable_init) {
        m->base.recv = mock_recv;
        m->base.submit_response = mock_submit_response;
        m->base.want_write = mock_want_write;
        m->base.flush = mock_flush;
        m->base.shutdown = mock_shutdown;
        m->base.destroy = mock_destroy;
        m->base.upgrade = m->with_upgrade ? mock_upgrade : NULL;
        m->base.want_read = m->with_want_read ? mock_want_read : NULL;
        m->base.submit_stream_response = m->with_stream ? mock_submit_stream_response : NULL;
        m->base.resume_body = m->with_stream ? mock_resume_body : NULL;
        m->base.reset_stream = m->with_stream ? mock_reset_stream : NULL;
    }

    m->sessions++;
    m->out_total = m->out_sent = 0;
    m->done_after_out = 0;

    /* Store callbacks so tests can invoke them */
    m->callbacks = *callbacks;
    m->cb_user_data = user_data;

    return &m->base;
}

static void mock_init(MockH2Session *m) {
    memset(m, 0, sizeof(*m));
    m->recv_return = 0; /* success by default */
    m->submit_return = 0;
    m->want_write_return = 0;
    m->flush_return = 0;
    m->shutdown_return = 0;
}

/* ── Test handler ─────────────────────────────────────────────────── */

static int handler_called = 0;
static int handler_status = 200;
static const char *handler_body = "{\"ok\":true}";
static size_t handler_body_len = 11;

static void test_handler(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)req; (void)ud;
    handler_called++;
    kl_http_response_json(res, handler_status, handler_body, handler_body_len);
}

static int middleware_called = 0;
static int middleware_return = 0;  /* 0 = continue */

static int test_middleware(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)req; (void)ud;
    middleware_called++;
    if (middleware_return != 0) {
        kl_http_response_error(res, 403, "Forbidden");
    }
    return middleware_return;
}

/* ── Test body reader ─────────────────────────────────────────────── */

typedef struct {
    KlHttpBodyReader base;
    char data[4096];
    size_t data_len;
    int complete_count;
    int error_count;
    int destroy_count;
} TestBodyReader;

static int test_br_on_data(KlHttpBodyReader *self, const char *data, size_t len) {
    TestBodyReader *br = (TestBodyReader *)self;
    if (br->data_len + len > sizeof(br->data)) return -1;
    memcpy(br->data + br->data_len, data, len);
    br->data_len += len;
    return 0;
}

static void test_br_on_complete(KlHttpBodyReader *self) {
    ((TestBodyReader *)self)->complete_count++;
}

static void test_br_on_error(KlHttpBodyReader *self) {
    ((TestBodyReader *)self)->error_count++;
}

static void test_br_destroy(KlHttpBodyReader *self) {
    ((TestBodyReader *)self)->destroy_count++;
}

static TestBodyReader g_test_br;

static KlHttpBodyReader *test_br_factory(KlAllocator *alloc, const KlHttpRequest *req,
                                      void *ud) {
    (void)alloc; (void)req; (void)ud;
    memset(&g_test_br, 0, sizeof(g_test_br));
    g_test_br.base.on_data = test_br_on_data;
    g_test_br.base.on_complete = test_br_on_complete;
    g_test_br.base.on_error = test_br_on_error;
    g_test_br.base.destroy = test_br_destroy;
    return &g_test_br.base;
}

/* ── Helper: set up a minimal KlHttpConn with pipes for I/O ──────────── */

static KlAllocator test_alloc;
static KlHttpRouter test_router;
static KlHttp2ServerConfig test_h2_cfg;

static void test_setup(void) {
    test_alloc = kl_allocator_default();
    kl_http_router_init(&test_router, &test_alloc);
    handler_called = 0;
    handler_status = 200;
    handler_body = "{\"ok\":true}";
    handler_body_len = 11;
    middleware_called = 0;
    middleware_return = 0;
    memset(&test_h2_cfg, 0, sizeof(test_h2_cfg));
    test_h2_cfg.factory = mock_factory;
}

static void test_teardown(void) {
    kl_http_router_free(&test_router);
}

/* ═══════════════════════════════════════════════════════════════════
 * Tests
 * ═══════════════════════════════════════════════════════════════════ */

UTEST(h2, config_defaults) {
    /* max_streams=0 should use 128, window_size=0 should use 65535 */
    KlHttp2ServerConfig cfg = {0};
    cfg.factory = mock_factory;
    ASSERT_EQ(cfg.max_concurrent_streams, 0);
    ASSERT_EQ(cfg.initial_window_size, 0);
    ASSERT_EQ(KL_HTTP2_DEFAULT_MAX_STREAMS, 128);
    ASSERT_EQ(KL_HTTP2_DEFAULT_WINDOW_SIZE, 65535);
}

UTEST(h2, session_vtable_validation) {
    /* A session with NULL vtable pointers should cause upgrade to fail */
    test_setup();
    MockH2Session mock;
    mock_init(&mock);
    g_mock_session = &mock;

    /* Create a mock factory that returns a session with NULL recv */
    /* We test via kl_http2_server_upgrade: need a minimal conn */
    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);

    KlHttpConn conn;
    memset(&conn, 0, sizeof(conn));
    conn.stream.fd = pfd[1];
    conn.stream.alloc = &test_alloc;

    /* Tell factory to skip vtable init: we set pointers manually
     * with NULL recv to test validation */
    mock.skip_vtable_init = 1;
    mock.base.recv = NULL;
    mock.base.submit_response = mock_submit_response;
    mock.base.want_write = mock_want_write;
    mock.base.flush = mock_flush;
    mock.base.shutdown = mock_shutdown;
    mock.base.destroy = mock_destroy;

    int r = kl_http2_server_upgrade(&conn, &test_router, &test_h2_cfg, NULL, 0);
    ASSERT_EQ(r, (int)KL_HTTP_CONN_CLOSED);
    /* destroy should have been called during cleanup */
    ASSERT_EQ(mock.destroy_count, 1);

    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();
}

/* Parity: each REQUIRED op missing (recv/submit_response/want_write/flush/shutdown/destroy)
 * causes the upgrade to fail (KL_HTTP_CONN_CLOSED), without crashing during cleanup. */
UTEST(h2, session_vtable_each_missing_required_rejected) {
    test_setup();
    for (int omit = 0; omit < 6; omit++) {
        MockH2Session mock; mock_init(&mock); g_mock_session = &mock;
        int pfd[2]; ASSERT_EQ(kl_test_socketpair(pfd), 0);
        KlHttpConn conn; memset(&conn, 0, sizeof(conn));
        conn.stream.fd = pfd[1]; conn.stream.alloc = &test_alloc;
        mock.skip_vtable_init = 1;
        mock.base.recv = mock_recv;
        mock.base.submit_response = mock_submit_response;
        mock.base.want_write = mock_want_write;
        mock.base.flush = mock_flush;
        mock.base.shutdown = mock_shutdown;
        mock.base.destroy = mock_destroy;
        switch (omit) {
            case 0: mock.base.recv = NULL; break;
            case 1: mock.base.submit_response = NULL; break;
            case 2: mock.base.want_write = NULL; break;
            case 3: mock.base.flush = NULL; break;
            case 4: mock.base.shutdown = NULL; break;
            case 5: mock.base.destroy = NULL; break;
        }
        int r = kl_http2_server_upgrade(&conn, &test_router, &test_h2_cfg, NULL, 0);
        ASSERT_EQ(r, (int)KL_HTTP_CONN_CLOSED);
        kl_test_closesock(pfd[0]);
        kl_test_closesock(pfd[1]);
    }
    test_teardown();
}

/* Parity: the OPTIONAL want_read slot may be NULL; the upgrade still succeeds. */
UTEST(h2, session_vtable_optional_want_read_null_accepted) {
    test_setup();
    MockH2Session mock; mock_init(&mock); g_mock_session = &mock;
    int pfd[2]; ASSERT_EQ(kl_test_socketpair(pfd), 0);
    KlHttpConn conn; memset(&conn, 0, sizeof(conn));
    conn.stream.fd = pfd[1]; conn.stream.alloc = &test_alloc;
    /* Full required set via the normal factory; want_read left NULL (mock never sets it). */
    mock.skip_vtable_init = 0;
    int r = kl_http2_server_upgrade(&conn, &test_router, &test_h2_cfg, NULL, 0);
    ASSERT_EQ(r, (int)KL_HTTP_CONN_HTTP2);
    ASSERT_TRUE(conn.h2 != NULL);
    ASSERT_TRUE(conn.h2->session->want_read == NULL);   /* optional slot stayed NULL, still accepted */
    kl_http2_server_cleanup(&conn);
    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();
}

UTEST(h2, conn_init_and_free) {
    test_setup();
    MockH2Session mock;
    mock_init(&mock);
    g_mock_session = &mock;

    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);

    KlHttpConn conn;
    memset(&conn, 0, sizeof(conn));
    conn.stream.fd = pfd[1];
    conn.stream.alloc = &test_alloc;

    int r = kl_http2_server_upgrade(&conn, &test_router, &test_h2_cfg, NULL, 0);
    ASSERT_EQ(r, (int)KL_HTTP_CONN_HTTP2);
    ASSERT_TRUE(conn.h2 != NULL);
    ASSERT_TRUE(conn.h2->session != NULL);
    ASSERT_TRUE(conn.h2->streams != NULL);
    ASSERT_EQ(conn.h2->max_streams, KL_HTTP2_DEFAULT_MAX_STREAMS);
    ASSERT_EQ(conn.h2->num_streams, 0);

    kl_http2_server_cleanup(&conn);
    ASSERT_TRUE(conn.h2 == NULL);
    ASSERT_EQ(mock.destroy_count, 1);

    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();
}

UTEST(h2, stream_create) {
    test_setup();
    MockH2Session mock;
    mock_init(&mock);
    g_mock_session = &mock;

    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);

    KlHttpConn conn;
    memset(&conn, 0, sizeof(conn));
    conn.stream.fd = pfd[1];
    conn.stream.alloc = &test_alloc;
    kl_http_router_add(&test_router, "GET", "/test", test_handler, NULL, NULL);

    int r = kl_http2_server_upgrade(&conn, &test_router, &test_h2_cfg, NULL, 0);
    ASSERT_EQ(r, (int)KL_HTTP_CONN_HTTP2);

    /* Invoke the on_request callback to create a stream */
    const char *hdr_names[] = {};
    const char *hdr_values[] = {};
    size_t hdr_name_lens[] = {};
    size_t hdr_value_lens[] = {};

    int rc = mock.callbacks.on_request(mock.cb_user_data, 1,
                                        "GET", 3, "/test", 5,
                                        NULL, 0,
                                        hdr_names, hdr_values,
                                        hdr_name_lens, hdr_value_lens, 0);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(conn.h2->num_streams, 1);
    ASSERT_EQ(conn.h2->streams[0].stream_id, (uint32_t)1);

    kl_http2_server_cleanup(&conn);
    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();
}

UTEST(h2, stream_max_limit) {
    test_setup();
    MockH2Session mock;
    mock_init(&mock);
    g_mock_session = &mock;

    /* Set max_concurrent_streams to 2 */
    test_h2_cfg.max_concurrent_streams = 2;

    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);

    KlHttpConn conn;
    memset(&conn, 0, sizeof(conn));
    conn.stream.fd = pfd[1];
    conn.stream.alloc = &test_alloc;
    kl_http_router_add(&test_router, "POST", "/data", test_handler, NULL,
                  test_br_factory);

    int r = kl_http2_server_upgrade(&conn, &test_router, &test_h2_cfg, NULL, 0);
    ASSERT_EQ(r, (int)KL_HTTP_CONN_HTTP2);
    ASSERT_EQ(conn.h2->max_streams, 2);

    /* Create stream 1: POST with body, no stream_end yet */
    const char *hn[] = {"content-length"};
    const char *hv[] = {"5"};
    size_t hnl[] = {14};
    size_t hvl[] = {1};

    int rc = mock.callbacks.on_request(mock.cb_user_data, 1,
                                        "POST", 4, "/data", 5,
                                        NULL, 0, hn, hv, hnl, hvl, 1);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(conn.h2->num_streams, 1);

    /* Create stream 3 */
    rc = mock.callbacks.on_request(mock.cb_user_data, 3,
                                   "POST", 4, "/data", 5,
                                   NULL, 0, hn, hv, hnl, hvl, 1);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(conn.h2->num_streams, 2);

    /* Stream 5 is over the limit: refused with 503 on that stream; the connection carries on */
    rc = mock.callbacks.on_request(mock.cb_user_data, 5,
                                   "POST", 4, "/data", 5,
                                   NULL, 0, hn, hv, hnl, hvl, 1);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(mock.last_stream_id, 5u);
    ASSERT_EQ(mock.last_status, 503);
    ASSERT_EQ(conn.h2->num_streams, 2);

    kl_http2_server_cleanup(&conn);
    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();
}

UTEST(h2, stream_destroy_cleanup) {
    test_setup();
    MockH2Session mock;
    mock_init(&mock);
    g_mock_session = &mock;

    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);

    KlHttpConn conn;
    memset(&conn, 0, sizeof(conn));
    conn.stream.fd = pfd[1];
    conn.stream.alloc = &test_alloc;
    kl_http_router_add(&test_router, "POST", "/upload", test_handler, NULL,
                  test_br_factory);

    kl_http2_server_upgrade(&conn, &test_router, &test_h2_cfg, NULL, 0);

    /* Create a stream with body reader */
    const char *hn[] = {"content-length"};
    const char *hv[] = {"100"};
    size_t hnl[] = {14};
    size_t hvl[] = {3};

    mock.callbacks.on_request(mock.cb_user_data, 1,
                               "POST", 4, "/upload", 7,
                               NULL, 0, hn, hv, hnl, hvl, 1);
    ASSERT_EQ(conn.h2->num_streams, 1);
    ASSERT_TRUE(conn.h2->streams[0].body_reader != NULL);

    /* Reset stream: should call on_error and destroy body reader */
    mock.callbacks.on_stream_reset(mock.cb_user_data, 1, 0);
    ASSERT_EQ(conn.h2->num_streams, 0);
    ASSERT_EQ(g_test_br.error_count, 1);
    ASSERT_EQ(g_test_br.destroy_count, 1);

    kl_http2_server_cleanup(&conn);
    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();
}

UTEST(h2, cb_on_request_creates_stream) {
    test_setup();
    MockH2Session mock;
    mock_init(&mock);
    g_mock_session = &mock;

    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);

    KlHttpConn conn;
    memset(&conn, 0, sizeof(conn));
    conn.stream.fd = pfd[1];
    conn.stream.alloc = &test_alloc;
    kl_http_router_add(&test_router, "GET", "/hello", test_handler, NULL, NULL);

    kl_http2_server_upgrade(&conn, &test_router, &test_h2_cfg, NULL, 0);

    const char *hn[] = {"host", "accept"};
    const char *hv[] = {"localhost", "application/json"};
    size_t hnl[] = {4, 6};
    size_t hvl[] = {9, 16};

    int rc = mock.callbacks.on_request(mock.cb_user_data, 1,
                                        "GET", 3, "/hello", 6,
                                        "localhost", 9,
                                        hn, hv, hnl, hvl, 2);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(conn.h2->num_streams, 1);

    KlHttp2ServerStream *s = &conn.h2->streams[0];
    ASSERT_EQ(s->stream_id, (uint32_t)1);
    ASSERT_EQ(s->req.method_len, (size_t)3);
    ASSERT_EQ(memcmp(s->req.method, "GET", 3), 0);
    ASSERT_EQ(s->req.path_len, (size_t)6);
    ASSERT_EQ(memcmp(s->req.path, "/hello", 6), 0);
    ASSERT_EQ(s->req.version_major, 2);
    ASSERT_EQ(s->req.version_minor, 0);
    ASSERT_EQ(s->req.keep_alive, 1);
    ASSERT_EQ(s->req.num_headers, 2);

    kl_http2_server_cleanup(&conn);
    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();
}

/* A request with as many fields as KEEL keeps, an :authority and no host field: the host field
 * made from :authority would be one field past the cap. The stream is refused with 431, as a
 * request over the cap is, rather than a field the client sent being dropped silently to make room
 * (the last one gave way). */
UTEST(h2, synthetic_host_past_the_header_cap_refuses_the_stream) {
    test_setup();
    MockH2Session mock;
    mock_init(&mock);
    g_mock_session = &mock;

    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);

    KlHttpConn conn;
    memset(&conn, 0, sizeof(conn));
    conn.stream.fd = pfd[1];
    conn.stream.alloc = &test_alloc;
    kl_http_router_add(&test_router, "GET", "/hello", test_handler, NULL, NULL);

    kl_http2_server_upgrade(&conn, &test_router, &test_h2_cfg, NULL, 0);

    static char names[KL_MAX_HEADERS][16];
    const char *hn[KL_MAX_HEADERS], *hv[KL_MAX_HEADERS];
    size_t hnl[KL_MAX_HEADERS], hvl[KL_MAX_HEADERS];
    for (int i = 0; i < KL_MAX_HEADERS; i++) {
        int l = snprintf(names[i], sizeof names[i], "x-f%d", i);
        hn[i] = names[i];
        hnl[i] = (size_t)l;
        hv[i] = "v";
        hvl[i] = 1;
    }
    int rc = mock.callbacks.on_request(mock.cb_user_data, 1, "GET", 3, "/hello", 6,
                                       "example.com", 11, hn, hv, hnl, hvl, KL_MAX_HEADERS);
    int streams = conn.h2->num_streams;
    int submits = mock.submit_count;
    int status = mock.last_status;
    uint32_t sid = mock.last_stream_id;

    kl_http2_server_cleanup(&conn);
    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();

    ASSERT_EQ(rc, 0);                                      /* the stream, not the connection */
    ASSERT_EQ(streams, 0);                                 /* was 1, its last field dropped */
    ASSERT_EQ(submits, 1);
    ASSERT_EQ(status, 431);
    ASSERT_EQ(sid, (uint32_t)1);
}

UTEST(h2, cb_on_request_routes) {
    test_setup();
    MockH2Session mock;
    mock_init(&mock);
    g_mock_session = &mock;

    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);

    KlHttpConn conn;
    memset(&conn, 0, sizeof(conn));
    conn.stream.fd = pfd[1];
    conn.stream.alloc = &test_alloc;
    kl_http_router_add(&test_router, "GET", "/users/:id", test_handler, NULL, NULL);

    kl_http2_server_upgrade(&conn, &test_router, &test_h2_cfg, NULL, 0);

    const char **hn = NULL;
    const char **hv = NULL;
    size_t *hnl = NULL;
    size_t *hvl = NULL;

    mock.callbacks.on_request(mock.cb_user_data, 1,
                               "GET", 3, "/users/42", 9,
                               NULL, 0, hn, hv, hnl, hvl, 0);

    KlHttp2ServerStream *s = &conn.h2->streams[0];
    ASSERT_EQ(s->route_result, 200);
    ASSERT_TRUE(s->route != NULL);
    ASSERT_EQ(s->num_params, 1);
    ASSERT_EQ(s->params[0].name_len, (size_t)2);
    ASSERT_EQ(memcmp(s->params[0].name, "id", 2), 0);
    ASSERT_EQ(s->params[0].value_len, (size_t)2);
    ASSERT_EQ(memcmp(s->params[0].value, "42", 2), 0);

    kl_http2_server_cleanup(&conn);
    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();
}

UTEST(h2, cb_on_request_middleware) {
    test_setup();
    MockH2Session mock;
    mock_init(&mock);
    g_mock_session = &mock;
    middleware_return = 1;  /* short-circuit */

    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);

    KlHttpConn conn;
    memset(&conn, 0, sizeof(conn));
    conn.stream.fd = pfd[1];
    conn.stream.alloc = &test_alloc;
    kl_http_router_add(&test_router, "GET", "/protected", test_handler, NULL, NULL);
    kl_http_router_use(&test_router, "*", "/*", test_middleware, NULL);

    kl_http2_server_upgrade(&conn, &test_router, &test_h2_cfg, NULL, 0);

    mock.callbacks.on_request(mock.cb_user_data, 1,
                               "GET", 3, "/protected", 10,
                               NULL, 0, NULL, NULL, NULL, NULL, 0);

    ASSERT_EQ(middleware_called, 1);
    /* Middleware short-circuited: response submitted, stream destroyed */
    ASSERT_EQ(mock.submit_count, 1);
    ASSERT_EQ(mock.last_status, 403);
    ASSERT_EQ(conn.h2->num_streams, 0);

    kl_http2_server_cleanup(&conn);
    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();
}

UTEST(h2, cb_on_data_forwards) {
    test_setup();
    MockH2Session mock;
    mock_init(&mock);
    g_mock_session = &mock;

    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);

    KlHttpConn conn;
    memset(&conn, 0, sizeof(conn));
    conn.stream.fd = pfd[1];
    conn.stream.alloc = &test_alloc;
    kl_http_router_add(&test_router, "POST", "/data", test_handler, NULL,
                  test_br_factory);

    kl_http2_server_upgrade(&conn, &test_router, &test_h2_cfg, NULL, 0);

    const char *hn[] = {"content-length"};
    const char *hv[] = {"5"};
    size_t hnl[] = {14};
    size_t hvl[] = {1};

    mock.callbacks.on_request(mock.cb_user_data, 1,
                               "POST", 4, "/data", 5,
                               NULL, 0, hn, hv, hnl, hvl, 1);

    /* Send data */
    int rc = mock.callbacks.on_data(mock.cb_user_data, 1, "hello", 5);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(g_test_br.data_len, (size_t)5);
    ASSERT_EQ(memcmp(g_test_br.data, "hello", 5), 0);

    kl_http2_server_cleanup(&conn);
    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();
}

/* A stream Keel has already answered and destroyed (here: pre-body middleware rejected a POST) keeps
 * receiving the client's DATA and END_STREAM. Those must be ignored: a -1 is fatal to the whole
 * session in the nghttp2 adapter, which aborted every other multiplexed stream. */
UTEST(h2, data_for_a_finished_stream_is_ignored) {
    test_setup();
    MockH2Session mock; mock_init(&mock); g_mock_session = &mock;
    middleware_return = 1;                        /* short-circuit with 403 */
    int pfd[2]; ASSERT_EQ(kl_test_socketpair(pfd), 0);
    KlHttpConn conn; memset(&conn, 0, sizeof(conn));
    conn.stream.fd = pfd[1]; conn.stream.alloc = &test_alloc;
    kl_http_router_add(&test_router, "POST", "/data", test_handler, NULL, test_br_factory);
    kl_http_router_use(&test_router, "*", "/*", test_middleware, NULL);
    kl_http2_server_upgrade(&conn, &test_router, &test_h2_cfg, NULL, 0);

    const char *hn[] = {"content-length"}; const char *hv[] = {"5"};
    size_t hnl[] = {14}; size_t hvl[] = {1};
    mock.callbacks.on_request(mock.cb_user_data, 1, "POST", 4, "/data", 5,
                              NULL, 0, hn, hv, hnl, hvl, 1);
    ASSERT_EQ(mock.last_status, 403);
    int rd = mock.callbacks.on_data(mock.cb_user_data, 1, "hello", 5);
    int re = mock.callbacks.on_stream_end(mock.cb_user_data, 1);

    kl_http2_server_cleanup(&conn);
    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();
    ASSERT_EQ(rd, 0);
    ASSERT_EQ(re, 0);
}

/* content-length is accumulated in a size_t: SIZE_MAX itself is a valid value, one more is not. The
 * last digit of SIZE_MAX is 5 (4294967295, 18446744073709551615), so SIZE_MAX + 1 is the same
 * digits ending in 6: a guard that only checks the running value against SIZE_MAX / 10 lets that
 * final `* 10 + 6` wrap to 0 and the stream would be accepted as having no body. */
UTEST(h2, content_length_one_past_size_max_is_rejected) {
    test_setup();
    MockH2Session mock; mock_init(&mock); g_mock_session = &mock;
    int pfd[2]; ASSERT_EQ(kl_test_socketpair(pfd), 0);
    KlHttpConn conn; memset(&conn, 0, sizeof(conn));
    conn.stream.fd = pfd[1]; conn.stream.alloc = &test_alloc;
    kl_http_router_add(&test_router, "POST", "/data", test_handler, NULL, test_br_factory);
    kl_http2_server_upgrade(&conn, &test_router, &test_h2_cfg, NULL, 0);

    char max_s[32], over_s[32];
    int n = snprintf(max_s, sizeof max_s, "%zu", (size_t)SIZE_MAX);
    ASSERT_TRUE(n > 1 && max_s[n - 1] == '5');
    memcpy(over_s, max_s, (size_t)n + 1);
    over_s[n - 1] = '6';                          /* SIZE_MAX + 1 */

    const char *hn[] = {"content-length"};
    size_t hnl[] = {14};
    const char *hv_max[] = {max_s};
    size_t hvl[] = {(size_t)n};
    int rc_max = mock.callbacks.on_request(mock.cb_user_data, 1, "POST", 4, "/data", 5,
                                           NULL, 0, hn, hv_max, hnl, hvl, 1);
    int streams_after_max = conn.h2->num_streams;
    size_t cl_max = streams_after_max == 1 ? conn.h2->streams[0].req.content_length : 0;

    const char *hv_over[] = {over_s};
    int rc_over = mock.callbacks.on_request(mock.cb_user_data, 3, "POST", 4, "/data", 5,
                                            NULL, 0, hn, hv_over, hnl, hvl, 1);
    int streams_after_over = conn.h2->num_streams;

    kl_http2_server_cleanup(&conn);
    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();
    ASSERT_EQ(rc_max, 0);
    ASSERT_EQ(streams_after_max, 1);
    ASSERT_TRUE(cl_max == (size_t)SIZE_MAX);
    ASSERT_EQ(rc_over, -1);
    ASSERT_EQ(streams_after_over, 1);
}

/* A stream whose body passes max_body_size is answered 413 on that stream, and the call succeeds:
 * the -1 it returned was fatal to the whole session, so one oversized upload aborted every other
 * request multiplexed on the connection, and sent no 413. */
UTEST(h2, over_limit_body_answers_413_on_its_stream_only) {
    test_setup();
    MockH2Session mock; mock_init(&mock); g_mock_session = &mock;
    int pfd[2]; ASSERT_EQ(kl_test_socketpair(pfd), 0);
    KlHttpConn conn; memset(&conn, 0, sizeof(conn));
    conn.stream.fd = pfd[1]; conn.stream.alloc = &test_alloc;
    conn.max_body_size = 4;
    kl_http_router_add(&test_router, "POST", "/data", test_handler, NULL, test_br_factory);
    kl_http2_server_upgrade(&conn, &test_router, &test_h2_cfg, NULL, 0);

    mock.callbacks.on_request(mock.cb_user_data, 1, "POST", 4, "/data", 5,
                              NULL, 0, NULL, NULL, NULL, NULL, 0);   /* no content-length */
    int rd = mock.callbacks.on_data(mock.cb_user_data, 1, "0123456789", 10);
    int status = mock.last_status;
    int streams = conn.h2->num_streams;
    int rd2 = mock.callbacks.on_data(mock.cb_user_data, 1, "more", 4);   /* the client sends on */

    kl_http2_server_cleanup(&conn);
    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();
    ASSERT_EQ(rd, 0);                             /* was -1: fatal to the session */
    ASSERT_EQ(status, 413);
    ASSERT_EQ(streams, 0);
    ASSERT_EQ(rd2, 0);
}

/* With the stream table full, a new stream is refused with a 503 on that stream, and the call
 * succeeds: a -1 there was fatal to the connection, and its streams with it. */
UTEST(h2, stream_table_full_refuses_the_new_stream_only) {
    test_setup();
    test_h2_cfg.max_concurrent_streams = 1;
    MockH2Session mock; mock_init(&mock); g_mock_session = &mock;
    int pfd[2]; ASSERT_EQ(kl_test_socketpair(pfd), 0);
    KlHttpConn conn; memset(&conn, 0, sizeof(conn));
    conn.stream.fd = pfd[1]; conn.stream.alloc = &test_alloc;
    kl_http_router_add(&test_router, "POST", "/data", test_handler, NULL, test_br_factory);
    kl_http2_server_upgrade(&conn, &test_router, &test_h2_cfg, NULL, 0);

    const char *hn[] = {"content-length"}; const char *hv[] = {"5"};
    size_t hnl[] = {14}; size_t hvl[] = {1};
    int r1 = mock.callbacks.on_request(mock.cb_user_data, 1, "POST", 4, "/data", 5,
                                       NULL, 0, hn, hv, hnl, hvl, 1);   /* stays open: body due */
    int r3 = mock.callbacks.on_request(mock.cb_user_data, 3, "POST", 4, "/data", 5,
                                       NULL, 0, hn, hv, hnl, hvl, 1);
    uint32_t sid = mock.last_stream_id;
    int status = mock.last_status;

    kl_http2_server_cleanup(&conn);
    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();
    ASSERT_EQ(r1, 0);
    ASSERT_EQ(r3, 0);
    ASSERT_EQ(sid, 3u);
    ASSERT_EQ(status, 503);
}

/* HTTP/2 frames a body by END_STREAM; content-length is optional. A body sent without one must
 * still reach the route's body reader (it had none, and the bytes were dropped). */
UTEST(h2, body_without_content_length_reaches_the_reader) {
    test_setup();
    MockH2Session mock; mock_init(&mock); g_mock_session = &mock;
    int pfd[2]; ASSERT_EQ(kl_test_socketpair(pfd), 0);
    KlHttpConn conn; memset(&conn, 0, sizeof(conn));
    conn.stream.fd = pfd[1]; conn.stream.alloc = &test_alloc;
    kl_http_router_add(&test_router, "POST", "/data", test_handler, NULL, test_br_factory);
    kl_http2_server_upgrade(&conn, &test_router, &test_h2_cfg, NULL, 0);
    memset(&g_test_br, 0, sizeof g_test_br);

    mock.callbacks.on_request(mock.cb_user_data, 1, "POST", 4, "/data", 5,
                              NULL, 0, NULL, NULL, NULL, NULL, 0);
    int rd = mock.callbacks.on_data(mock.cb_user_data, 1, "hello", 5);
    size_t got = g_test_br.data_len;
    int same = got == 5 && memcmp(g_test_br.data, "hello", 5) == 0;
    int re = mock.callbacks.on_stream_end(mock.cb_user_data, 1);

    kl_http2_server_cleanup(&conn);
    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();
    ASSERT_EQ(rd, 0);
    ASSERT_EQ(got, (size_t)5);
    ASSERT_TRUE(same);
    ASSERT_EQ(re, 0);
    ASSERT_EQ(handler_called, 1);
}

/* An HTTP/2 config without a session factory would be called on the first HTTP/2 connection:
 * reject it at server init, as a TLS config without a factory is. */
UTEST(h2, server_init_rejects_h2_without_factory) {
    KlHttp2ServerConfig h2 = {0};
    KlHttpServerConfig cfg = { .port = 0, .h2 = &h2 };
    static KlHttpServer srv;
    int rc = kl_http_server_init(&srv, &cfg);
    if (rc == 0) kl_http_server_free(&srv);
    ASSERT_EQ(rc, -1);
}

/* A failed kl_http_server_init frees what it allocated before failing. The parsed PROXY trust list
 * is allocated early and was freed only by kl_http_server_free, which a caller does not call after
 * a failed init: every later failure path (here the h2 config without a factory) leaked it. */
static long g_init_live;
static void *il_malloc(void *c, size_t n) { (void)c; void *p = malloc(n ? n : 1); if (p) g_init_live++; return p; }
static void *il_realloc(void *c, void *p, size_t o, size_t n) { (void)c; (void)o; void *q = realloc(p, n ? n : 1); if (q && !p) g_init_live++; return q; }
static void il_free(void *c, void *p, size_t n) { (void)c; (void)n; if (p) { g_init_live--; free(p); } }

UTEST(h2, failed_server_init_frees_the_proxy_trust_list) {
    KlAllocator a = { il_malloc, il_realloc, il_free, NULL };
    KlHttp2ServerConfig h2 = {0};                 /* no factory: init fails after the CIDR parse */
    KlHttpServerConfig cfg = { .port = 0, .h2 = &h2, .alloc = &a,
                               .proxy_trusted_cidrs = "10.0.0.0/8,192.168.0.0/16" };
    static KlHttpServer srv;
    g_init_live = 0;
    int rc = kl_http_server_init(&srv, &cfg);
    if (rc == 0) kl_http_server_free(&srv);
    ASSERT_EQ(rc, -1);
    ASSERT_EQ(g_init_live, 0L);                   /* was: the trust list left allocated */
}

UTEST(h2, cb_on_data_reject) {
    test_setup();
    MockH2Session mock;
    mock_init(&mock);
    g_mock_session = &mock;

    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);

    KlHttpConn conn;
    memset(&conn, 0, sizeof(conn));
    conn.stream.fd = pfd[1];
    conn.stream.alloc = &test_alloc;
    kl_http_router_add(&test_router, "POST", "/data", test_handler, NULL,
                  test_br_factory);

    kl_http2_server_upgrade(&conn, &test_router, &test_h2_cfg, NULL, 0);

    const char *hn[] = {"content-length"};
    const char *hv[] = {"5"};
    size_t hnl[] = {14};
    size_t hvl[] = {1};

    mock.callbacks.on_request(mock.cb_user_data, 1,
                               "POST", 4, "/data", 5,
                               NULL, 0, hn, hv, hnl, hvl, 1);

    /* Overflow the test body reader's 4096-byte buffer: the reader refuses the data, and the
     * stream (not the session) is answered 413. */
    char big[5000];
    memset(big, 'A', sizeof(big));
    int rc = mock.callbacks.on_data(mock.cb_user_data, 1, big, sizeof(big));
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(mock.last_status, 413);
    ASSERT_EQ(conn.h2->num_streams, 0);

    kl_http2_server_cleanup(&conn);
    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();
}

UTEST(h2, cb_on_stream_end_handler) {
    test_setup();
    MockH2Session mock;
    mock_init(&mock);
    g_mock_session = &mock;

    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);

    KlHttpConn conn;
    memset(&conn, 0, sizeof(conn));
    conn.stream.fd = pfd[1];
    conn.stream.alloc = &test_alloc;
    kl_http_router_add(&test_router, "GET", "/hello", test_handler, NULL, NULL);

    kl_http2_server_upgrade(&conn, &test_router, &test_h2_cfg, NULL, 0);

    mock.callbacks.on_request(mock.cb_user_data, 1,
                               "GET", 3, "/hello", 6,
                               NULL, 0, NULL, NULL, NULL, NULL, 0);

    /* End stream triggers handler */
    int rc = mock.callbacks.on_stream_end(mock.cb_user_data, 1);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(handler_called, 1);
    ASSERT_EQ(mock.submit_count, 1);
    ASSERT_EQ(mock.last_status, 200);
    ASSERT_EQ(mock.last_body_len, (size_t)11);
    ASSERT_EQ(memcmp(mock.last_body, "{\"ok\":true}", 11), 0);
    /* Stream destroyed after response */
    ASSERT_EQ(conn.h2->num_streams, 0);

    kl_http2_server_cleanup(&conn);
    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();
}

UTEST(h2, cb_on_stream_end_404) {
    test_setup();
    MockH2Session mock;
    mock_init(&mock);
    g_mock_session = &mock;

    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);

    KlHttpConn conn;
    memset(&conn, 0, sizeof(conn));
    conn.stream.fd = pfd[1];
    conn.stream.alloc = &test_alloc;
    /* No routes registered */

    kl_http2_server_upgrade(&conn, &test_router, &test_h2_cfg, NULL, 0);

    mock.callbacks.on_request(mock.cb_user_data, 1,
                               "GET", 3, "/missing", 8,
                               NULL, 0, NULL, NULL, NULL, NULL, 0);
    mock.callbacks.on_stream_end(mock.cb_user_data, 1);

    ASSERT_EQ(mock.submit_count, 1);
    ASSERT_EQ(mock.last_status, 404);

    kl_http2_server_cleanup(&conn);
    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();
}

UTEST(h2, cb_on_stream_reset_cleanup) {
    test_setup();
    MockH2Session mock;
    mock_init(&mock);
    g_mock_session = &mock;

    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);

    KlHttpConn conn;
    memset(&conn, 0, sizeof(conn));
    conn.stream.fd = pfd[1];
    conn.stream.alloc = &test_alloc;
    kl_http_router_add(&test_router, "POST", "/upload", test_handler, NULL,
                  test_br_factory);

    kl_http2_server_upgrade(&conn, &test_router, &test_h2_cfg, NULL, 0);

    const char *hn[] = {"content-length"};
    const char *hv[] = {"100"};
    size_t hnl[] = {14};
    size_t hvl[] = {3};

    mock.callbacks.on_request(mock.cb_user_data, 1,
                               "POST", 4, "/upload", 7,
                               NULL, 0, hn, hv, hnl, hvl, 1);
    ASSERT_EQ(conn.h2->num_streams, 1);

    /* RST_STREAM */
    mock.callbacks.on_stream_reset(mock.cb_user_data, 1, 8);  /* CANCEL */
    ASSERT_EQ(conn.h2->num_streams, 0);
    ASSERT_EQ(g_test_br.error_count, 1);
    ASSERT_EQ(g_test_br.destroy_count, 1);

    kl_http2_server_cleanup(&conn);
    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();
}

UTEST(h2, cb_send_wraps_conn_write) {
    test_setup();
    MockH2Session mock;
    mock_init(&mock);
    g_mock_session = &mock;

    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);

    KlHttpConn conn;
    memset(&conn, 0, sizeof(conn));
    conn.stream.fd = pfd[1];
    conn.stream.alloc = &test_alloc;

    kl_http2_server_upgrade(&conn, &test_router, &test_h2_cfg, NULL, 0);

    /* Send callback should write to the socket fd */
    const char *data = "HTTP/2 frame data";
    kl_ssize_t r = mock.callbacks.send(mock.cb_user_data, data, 17);
    ASSERT_EQ(r, (kl_ssize_t)17);

    /* Read from pipe to verify */
    char buf[64];
    kl_ssize_t nr = kl_test_sockread(pfd[0], buf, sizeof(buf));
    ASSERT_EQ(nr, (kl_ssize_t)17);
    ASSERT_EQ(memcmp(buf, "HTTP/2 frame data", 17), 0);

    kl_http2_server_cleanup(&conn);
    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();
}

/* A plaintext send that would block is "nothing sent yet" (0), never an error (-1). The session
 * maps -1 to a fatal callback failure, so a response larger than the free send buffer killed the
 * whole connection and every stream on it. TLS already reports WANT_WRITE as 0. */
UTEST(h2, cb_send_would_block_is_not_an_error) {
    test_setup();
    MockH2Session mock;
    mock_init(&mock);
    g_mock_session = &mock;

    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);
    ASSERT_EQ(kl_test_set_nonblock((KlSocketHandle)pfd[1]), 0);

    KlHttpConn conn;
    memset(&conn, 0, sizeof(conn));
    conn.stream.fd = pfd[1];
    conn.stream.alloc = &test_alloc;

    kl_http2_server_upgrade(&conn, &test_router, &test_h2_cfg, NULL, 0);

    static char chunk[64 * 1024];
    memset(chunk, 'f', sizeof chunk);
    kl_ssize_t r = 1;
    for (int i = 0; i < 4096 && r > 0; i++)          /* nobody reads pfd[0]: the buffer fills */
        r = mock.callbacks.send(mock.cb_user_data, chunk, sizeof chunk);

    kl_http2_server_cleanup(&conn);
    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();
    ASSERT_EQ(r, (kl_ssize_t)0);                     /* was: -1, fatal to the session */
}

UTEST(h2, preface_detection_full) {
    test_setup();
    MockH2Session mock;
    mock_init(&mock);
    g_mock_session = &mock;

    KlHttpConn conn;
    memset(&conn, 0, sizeof(conn));
    conn.stream.alloc = &test_alloc;
    conn.h2_config = &test_h2_cfg;
    conn.router = &test_router;

    /* Provide a stack buffer since read_buf is now a pointer */
    char h2_buf[KL_HTTP_CONN_READ_BUF_SIZE];
    conn.stream.read_buf = h2_buf;
    conn.stream.read_cap = sizeof(h2_buf);

    /* Simulate 24-byte HTTP/2 preface in read_buf */
    static const char preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    memcpy(conn.stream.read_buf, preface, 24);
    conn.stream.read_len = 24;

    /* We can't call kl_http_conn_on_readable directly (needs fd),
     * so test the preface detection logic via direct buffer check */
    ASSERT_EQ(conn.stream.read_len, (size_t)24);
    ASSERT_EQ(memcmp(conn.stream.read_buf, preface, 24), 0);

    /* Verify the preface constant matches RFC 7540 */
    ASSERT_EQ((size_t)24, strlen(preface));

    test_teardown();
}

UTEST(h2, preface_detection_partial) {
    /* A partial preface match should not trigger upgrade */
    static const char preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    static const char partial[] = "PRI * HTTP/";

    /* First 11 bytes match the preface prefix */
    ASSERT_EQ(memcmp(partial, preface, 11), 0);

    /* But GET / does not match */
    ASSERT_NE(memcmp("GET / HTTP/1.1", preface, 14), 0);
}

UTEST(h2, preface_detection_non_match) {
    /* A normal HTTP/1.1 request should not match the preface */
    static const char preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    const char *get_req = "GET / HTTP/1.1\r\n";

    ASSERT_NE(memcmp(get_req, preface, 3), 0);
}

UTEST(h2, alpn_h2) {
    /* When TLS ALPN negotiates "h2", upgrade should produce KL_HTTP_CONN_HTTP2 */
    test_setup();
    MockH2Session mock;
    mock_init(&mock);
    g_mock_session = &mock;

    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);

    KlHttpConn conn;
    memset(&conn, 0, sizeof(conn));
    conn.stream.fd = pfd[1];
    conn.stream.alloc = &test_alloc;
    conn.h2_config = &test_h2_cfg;
    conn.router = &test_router;

    /* Direct upgrade test (simulating ALPN h2) */
    int r = kl_http2_server_upgrade(&conn, &test_router, &test_h2_cfg, NULL, 0);
    ASSERT_EQ(r, (int)KL_HTTP_CONN_HTTP2);
    ASSERT_TRUE(conn.h2 != NULL);

    kl_http2_server_cleanup(&conn);
    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();
}

UTEST(h2, alpn_null_fallback) {
    /* When alpn_protocol is NULL, no HTTP/2 upgrade should happen */
    /* This is tested implicitly: without alpn, conn stays in READING */
    KlHttpConn conn;
    memset(&conn, 0, sizeof(conn));

    /* No TLS at all means no ALPN */
    ASSERT_TRUE(conn.tls == NULL);
    /* h2_config set but no TLS: preface detection still works */
    ASSERT_TRUE(conn.h2 == NULL);
}

UTEST(h2, alpn_http11_fallback) {
    /* When ALPN returns "http/1.1", stay on HTTP/1.1 */
    /* Verified by checking the ALPN comparison logic */
    const char *proto = "http/1.1";
    int is_h2 = (proto[0] == 'h' && proto[1] == '2' && proto[2] == '\0');
    ASSERT_EQ(is_h2, 0);
}

UTEST(h2, multi_stream) {
    test_setup();
    MockH2Session mock;
    mock_init(&mock);
    g_mock_session = &mock;

    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);

    KlHttpConn conn;
    memset(&conn, 0, sizeof(conn));
    conn.stream.fd = pfd[1];
    conn.stream.alloc = &test_alloc;
    kl_http_router_add(&test_router, "GET", "/a", test_handler, NULL, NULL);
    kl_http_router_add(&test_router, "GET", "/b", test_handler, NULL, NULL);
    kl_http_router_add(&test_router, "GET", "/c", test_handler, NULL, NULL);

    kl_http2_server_upgrade(&conn, &test_router, &test_h2_cfg, NULL, 0);

    /* Create 3 streams */
    mock.callbacks.on_request(mock.cb_user_data, 1,
                               "GET", 3, "/a", 2,
                               NULL, 0, NULL, NULL, NULL, NULL, 0);
    mock.callbacks.on_request(mock.cb_user_data, 3,
                               "GET", 3, "/b", 2,
                               NULL, 0, NULL, NULL, NULL, NULL, 0);
    mock.callbacks.on_request(mock.cb_user_data, 5,
                               "GET", 3, "/c", 2,
                               NULL, 0, NULL, NULL, NULL, NULL, 0);
    ASSERT_EQ(conn.h2->num_streams, 3);

    /* End stream 3 first (out of order) */
    mock.callbacks.on_stream_end(mock.cb_user_data, 3);
    ASSERT_EQ(conn.h2->num_streams, 2);
    ASSERT_EQ(handler_called, 1);

    /* End stream 1 */
    mock.callbacks.on_stream_end(mock.cb_user_data, 1);
    ASSERT_EQ(conn.h2->num_streams, 1);
    ASSERT_EQ(handler_called, 2);

    /* End stream 5 */
    mock.callbacks.on_stream_end(mock.cb_user_data, 5);
    ASSERT_EQ(conn.h2->num_streams, 0);
    ASSERT_EQ(handler_called, 3);
    ASSERT_EQ(mock.submit_count, 3);

    kl_http2_server_cleanup(&conn);
    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();
}

UTEST(h2, goaway_shutdown) {
    test_setup();
    MockH2Session mock;
    mock_init(&mock);
    g_mock_session = &mock;

    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);

    KlHttpConn conn;
    memset(&conn, 0, sizeof(conn));
    conn.stream.fd = pfd[1];
    conn.stream.alloc = &test_alloc;

    kl_http2_server_upgrade(&conn, &test_router, &test_h2_cfg, NULL, 0);

    kl_http2_server_drain_shutdown(&conn);
    ASSERT_EQ(mock.shutdown_count, 1);
    ASSERT_EQ(conn.h2->goaway_sent, 1);

    /* Calling again should be a no-op */
    kl_http2_server_drain_shutdown(&conn);
    ASSERT_EQ(mock.shutdown_count, 1);

    kl_http2_server_cleanup(&conn);
    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();
}

/* A graceful shutdown whose last response goes out on write readiness: once the session wants
 * neither read nor write it is done, and the connection ends there, as it does after a read. It
 * stayed open until the idle sweep or the drain deadline. (This connection has no drain bounds, as
 * with reject_drain_disable, so it closes; with them it lingers: see the tests below.) */
UTEST(h2, on_writable_closes_a_finished_session) {
    test_setup();
    MockH2Session mock;
    mock_init(&mock);
    mock.with_want_read = 1;
    mock.want_read_return = 1;
    g_mock_session = &mock;

    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);

    KlHttpConn conn;
    memset(&conn, 0, sizeof(conn));
    conn.stream.fd = pfd[1];
    conn.stream.alloc = &test_alloc;

    kl_http2_server_upgrade(&conn, &test_router, &test_h2_cfg, NULL, 0);
    int open_st = kl_http2_server_on_writable(&conn);      /* still wants to read */
    mock.want_read_return = 0;                             /* GOAWAY out, no stream left */
    mock.want_write_return = 0;
    int done_st = kl_http2_server_on_writable(&conn);

    kl_http2_server_cleanup(&conn);
    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();

    ASSERT_EQ(open_st, KL_HTTP_CONN_HTTP2);
    ASSERT_EQ(done_st, KL_HTTP_CONN_CLOSED);               /* was KL_HTTP_CONN_HTTP2 */
}

/* Response DATA leaving on write readiness is activity: the connection's idle clock restarts, so a
 * long download is not timed out (KEEL forgets a stream once its response is submitted, so such a
 * connection otherwise looks idle as soon as the session has nothing left to write). */
UTEST(h2, a_flush_that_moves_bytes_is_activity) {
    test_setup();
    MockH2Session mock;
    mock_init(&mock);
    g_mock_session = &mock;

    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);

    KlHttpConn conn;
    memset(&conn, 0, sizeof(conn));
    conn.stream.fd = pfd[1];
    conn.stream.alloc = &test_alloc;

    kl_http2_server_upgrade(&conn, &test_router, &test_h2_cfg, NULL, 0);
    conn.last_active_ms = 0;
    (void)kl_http2_server_on_writable(&conn);              /* moves nothing */
    uint64_t after_empty = conn.last_active_ms;
    mock.flush_send_len = 32;                              /* moves a DATA frame's worth */
    (void)kl_http2_server_on_writable(&conn);
    uint64_t after_data = conn.last_active_ms;

    kl_http2_server_cleanup(&conn);
    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();

    ASSERT_EQ(after_empty, (uint64_t)0);
    ASSERT_GT(after_data, (uint64_t)0);                    /* was 0: only reads counted */
}

/* ── The idle sweep over an HTTP/2 connection ─────────────────────────────────────────────────── */

static KlHttpServer h2_sweep_srv;

/* A server whose pool holds one HTTP/2 connection over pfd[1], driven by the mock session. The
 * sweep releases the connection (closing pfd[1]) or kl_http_server_free does. */
static KlHttpConn *h2_sweep_setup(int pfd[2], int *completion_loop) {
    KlHttpServerConfig cfg = { .port = 0, .bind_addr = "127.0.0.1", .h2 = &test_h2_cfg,
                               .read_timeout_ms = 100, .max_connections = 2 };
    if (kl_http_server_init(&h2_sweep_srv, &cfg) < 0) return NULL;
    *completion_loop = (kl_event_caps(&h2_sweep_srv.ev.loop) & KL_EVENT_CAP_COMPLETION) != 0;
    KlHttpConn *c = kl_http_conn_acquire(&h2_sweep_srv.pool, (KlSocketHandle)pfd[1]);
    if (!c) return NULL;
    if (kl_http2_server_upgrade(c, &test_router, &test_h2_cfg, NULL, 0) != KL_HTTP_CONN_HTTP2)
        return NULL;
    c->state = KL_HTTP_CONN_HTTP2;
    return c;
}

static size_t linger_read_to_eof(int fd, char *out, size_t cap, int *eof);

/* An idle HTTP/2 connection the sweep times out is sent a GOAWAY first (RFC 9113 6.8), so the client
 * knows no request it sent was processed rather than seeing the connection simply drop. And it ends
 * gracefully, as a finished session does: the half-close after the GOAWAY, then the linger, so a
 * client frame that crosses the GOAWAY does not turn the close into a reset that drops it. */
UTEST(h2, idle_sweep_sends_goaway_then_lingers) {
    test_setup();
    MockH2Session mock;
    mock_init(&mock);
    mock.with_want_read = 1;
    mock.want_read_return = 1;
    mock.goaway_len = 17;                                  /* the GOAWAY frame */
    g_mock_session = &mock;
    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);
    int comp = 0;
    KlHttpConn *c = h2_sweep_setup(pfd, &comp);
    ASSERT_TRUE(c != NULL);
    ASSERT_EQ(0, kl_test_set_nonblock((KlSocketHandle)pfd[1]));
    if (!comp)
        ASSERT_EQ(0, kl_event_add(&h2_sweep_srv.ev.loop, (KlSocketHandle)pfd[1], KL_EVENT_READ,
                                  &c->stream));
    uint64_t now = kl_monotonic_ms();
    c->last_active_ms = now - 1000;                        /* idle past read_timeout_ms */
    kl_http_server_sweep_conn_timeouts(&h2_sweep_srv, now, comp);
    int shutdowns = mock.shutdown_count, destroyed = mock.destroy_count, st = (int)c->state;
    int64_t idle_linger_ms = (int64_t)c->drain_deadline_ms - (int64_t)now;
    char got[64];
    int eof = 0;
    size_t n = 0;
    int destroyed_after_fin = -1;
    if (!comp && st == (int)KL_HTTP_CONN_DRAINING) {
        n = linger_read_to_eof(pfd[0], got, sizeof got, &eof);
        (void)kl_test_sockwrite(pfd[0], "\0\0\x08\x06\0\0\0\0\0PINGPING", 17);   /* crosses it */
        kl_http_server_sweep_conn_timeouts(&h2_sweep_srv, kl_monotonic_ms(), comp);
        shutdown(pfd[0], T_SHUT_WR);                       /* the client's FIN */
        kl_http_server_sweep_conn_timeouts(&h2_sweep_srv, kl_monotonic_ms(), comp);
        destroyed_after_fin = mock.destroy_count;
    }

    kl_http_server_free(&h2_sweep_srv);
    kl_test_closesock(pfd[0]);
    test_teardown();
    ASSERT_EQ(shutdowns, 1);                               /* the GOAWAY first */
    if (comp) {                         /* not completion-driven here: closed as before */
        ASSERT_EQ(destroyed, 1);
        return;
    }
    ASSERT_EQ(st, (int)KL_HTTP_CONN_DRAINING);             /* was CLOSED: an abortive close */
    ASSERT_EQ(destroyed, 0);
    /* A timeout's end lingers with the drain's short bound (reject_drain_timeout_ms, 500 ms by
     * default), not another read_timeout_ms: here 100 ms, so it must be the 500. */
    ASSERT_TRUE(idle_linger_ms >= 450 && idle_linger_ms <= 550);
    ASSERT_EQ(n, (size_t)17);                              /* the GOAWAY, whole */
    ASSERT_EQ(eof, 1);                                     /* then the FIN */
    ASSERT_EQ(destroyed_after_fin, 1);                     /* the client's FIN ends it */
}

/* After a graceful GOAWAY a session with no stream left wants neither read nor write: it is done,
 * and the sweep ends the connection at once (it was left to the idle timeout or the drain deadline,
 * so a draining server waited out its whole deadline). It ends through the linger, not a close: the
 * send side is half-closed (the client sees its FIN at once) and what the client still sends is
 * discarded until its own FIN, which closes it. */
UTEST(h2, sweep_ends_a_finished_session_through_the_linger) {
    test_setup();
    MockH2Session mock;
    mock_init(&mock);
    mock.with_want_read = 1;
    mock.want_read_return = 0;
    g_mock_session = &mock;
    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);
    int comp = 0;
    KlHttpConn *c = h2_sweep_setup(pfd, &comp);
    ASSERT_TRUE(c != NULL);
    ASSERT_EQ(0, kl_test_set_nonblock((KlSocketHandle)pfd[1]));
    if (!comp)
        ASSERT_EQ(0, kl_event_add(&h2_sweep_srv.ev.loop, (KlSocketHandle)pfd[1], KL_EVENT_READ,
                                  &c->stream));

    uint64_t now = kl_monotonic_ms();
    c->last_active_ms = now;                               /* not idle long: only "done" ends it */
    kl_http_server_sweep_conn_timeouts(&h2_sweep_srv, now, comp);
    int st = (int)c->state, destroyed = mock.destroy_count;
    int64_t done_linger_ms = (int64_t)c->drain_deadline_ms - (int64_t)now;
    char b[8];
    long fin = -2, after_fin = -2;
    int destroyed_after_fin = -1;
    if (!comp && st == (int)KL_HTTP_CONN_DRAINING) {   /* else the peer is closed: no writes to it */
        fin = kl_test_sockread(pfd[0], b, sizeof b);                       /* the half-close */
        (void)kl_test_sockwrite(pfd[0], "\0\0\x08\x06\0\0\0\0\0PINGPING", 17);   /* crosses it */
        kl_http_server_sweep_conn_timeouts(&h2_sweep_srv, kl_monotonic_ms(), comp);
        after_fin = (long)c->state;
        shutdown(pfd[0], T_SHUT_WR);                       /* the client's FIN */
        kl_http_server_sweep_conn_timeouts(&h2_sweep_srv, kl_monotonic_ms(), comp);
        destroyed_after_fin = mock.destroy_count;
    }

    kl_http_server_free(&h2_sweep_srv);
    kl_test_closesock(pfd[0]);
    test_teardown();
    if (comp) {    /* not completion-driven here: closed as before (a_done_session_lingers_on_the_servers_loop
                    * covers the completion driver's linger) */
        ASSERT_EQ(destroyed, 1);
        return;
    }
    ASSERT_EQ(st, (int)KL_HTTP_CONN_DRAINING);             /* was CLOSED: a close over unread input */
    ASSERT_EQ(destroyed, 0);
    /* A done session's output is owed to the client: the answer's bound (read_timeout_ms, here
     * 100 ms), not the drain's 500 ms. */
    ASSERT_TRUE(done_linger_ms >= 50 && done_linger_ms <= 150);
    ASSERT_EQ(fin, 0L);                                    /* the client sees the end at once */
    ASSERT_EQ(after_fin, (long)KL_HTTP_CONN_DRAINING);     /* the PING is discarded, no reset */
    ASSERT_EQ(destroyed_after_fin, 1);                     /* the client's FIN ends it */
}

/* ── The lingering end of a finished session, on the connection itself ────────────────────────── */

/* A connection over fd with the drain's bounds, as kl_http_server_init sets them per slot. */
static void linger_conn_init(KlHttpConn *conn, int fd, char *buf, size_t cap) {
    memset(conn, 0, sizeof(*conn));
    conn->stream.fd = fd;
    conn->stream.alloc = &test_alloc;
    conn->stream.read_buf = buf;
    conn->stream.read_cap = cap;
    conn->reject_drain_max_bytes = KL_HTTP_SERVER_DEFAULT_REJECT_DRAIN_BYTES;
    conn->reject_drain_timeout_ms = KL_HTTP_SERVER_DEFAULT_REJECT_DRAIN_MS;
    conn->answer_linger_ms = 5000;                         /* read_timeout_ms */
}

/* Read what pfd[0] holds until the end of stream (or nothing more for a moment). */
static size_t linger_read_to_eof(int fd, char *out, size_t cap, int *eof) {
    size_t n = 0;
    *eof = 0;
    while (n < cap && kl_test_poll1(fd, 0, 200) > 0) {
        long r = kl_test_sockread(fd, out + n, cap - n);
        if (r == 0) { *eof = 1; break; }
        if (r < 0) break;
        n += (size_t)r;
    }
    return n;
}

static void linger_session_done(MockH2Session *m) {
    m->want_read_return = 0;                               /* GOAWAY out, no stream left */
    m->want_write_return = 0;
}

/* The last response of a graceful shutdown goes out on write readiness and the session is done:
 * the connection half-closes after that output and lingers (DRAINING), so the client's frames that
 * cross the GOAWAY are read and discarded instead of turning the close into a reset that drops the
 * output still queued. The client's FIN then ends it. */
UTEST(h2, a_finished_session_lingers_until_the_clients_fin) {
    test_setup();
    MockH2Session mock;
    mock_init(&mock);
    mock.with_want_read = 1;
    mock.want_read_return = 1;
    g_mock_session = &mock;
    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);
    ASSERT_EQ(0, kl_test_set_nonblock((KlSocketHandle)pfd[1]));
    char rbuf[256];
    KlHttpConn conn;
    linger_conn_init(&conn, pfd[1], rbuf, sizeof rbuf);
    kl_http2_server_upgrade(&conn, &test_router, &test_h2_cfg, NULL, 0);
    conn.state = KL_HTTP_CONN_HTTP2;

    mock.flush_send_len = 32;                              /* the last DATA frame and the GOAWAY */
    conn.request_body_complete = 1;                        /* left by an h2c upgrade's request */
    linger_session_done(&mock);
    int st = kl_http2_server_on_writable(&conn);
    char got[128];
    int eof = 0;
    size_t n = linger_read_to_eof(pfd[0], got, sizeof got, &eof);

    (void)kl_test_sockwrite(pfd[0], "\0\0\x08\x06\0\0\0\0\0PINGPING", 17);   /* a PING crosses */
    (void)kl_test_sockwrite(pfd[0], "\0\0\x04\x08\0\0\0\0\0\0\0\0\x01", 13); /* a WINDOW_UPDATE */
    kl_test_sleep_ms(20);
    int st2 = (int)kl_http_conn_drain_step(&conn, kl_monotonic_ms());
    uint64_t discarded = conn.drain_discarded;
    int st_idle = (int)kl_http_conn_drain_step(&conn, kl_monotonic_ms());   /* nothing to read */
    int destroyed = mock.destroy_count;
    shutdown(pfd[0], T_SHUT_WR);                           /* the client's FIN */
    kl_test_sleep_ms(20);
    int st3 = (int)kl_http_conn_drain_step(&conn, kl_monotonic_ms());

    kl_http2_server_cleanup(&conn);
    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();
    ASSERT_EQ(st, (int)KL_HTTP_CONN_DRAINING);             /* was CLOSED */
    ASSERT_EQ(n, (size_t)32);                              /* the output, whole */
    ASSERT_EQ(eof, 1);                                     /* then the FIN */
    ASSERT_EQ(st2, (int)KL_HTTP_CONN_DRAINING);            /* the frames discarded, still open */
    ASSERT_EQ(discarded, (uint64_t)30);
    ASSERT_EQ(st_idle, (int)KL_HTTP_CONN_DRAINING);        /* no framing ends it: only the FIN */
    ASSERT_EQ(destroyed, 0);
    ASSERT_EQ(st3, (int)KL_HTTP_CONN_CLOSED);              /* the client's FIN ends it */
}

/* The same end when the session finishes after a read (the client's frame that completed it). */
UTEST(h2, a_session_finished_by_a_read_lingers) {
    test_setup();
    MockH2Session mock;
    mock_init(&mock);
    mock.with_want_read = 1;
    mock.want_read_return = 1;
    g_mock_session = &mock;
    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);
    char rbuf[256];
    KlHttpConn conn;
    linger_conn_init(&conn, pfd[1], rbuf, sizeof rbuf);
    kl_http2_server_upgrade(&conn, &test_router, &test_h2_cfg, NULL, 0);
    conn.state = KL_HTTP_CONN_HTTP2;
    int open_st = (int)kl_http2_server_feed(&conn, "x", 1);
    linger_session_done(&mock);
    int done_st = (int)kl_http2_server_feed(&conn, "x", 1);
    int eof = 0;
    char got[16];
    (void)linger_read_to_eof(pfd[0], got, sizeof got, &eof);

    kl_http2_server_cleanup(&conn);
    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();
    ASSERT_EQ(open_st, (int)KL_HTTP_CONN_HTTP2);
    ASSERT_EQ(done_st, (int)KL_HTTP_CONN_DRAINING);        /* was CLOSED */
    ASSERT_EQ(eof, 1);                                     /* half-closed */
}

/* The linger is bounded both ways. Past its byte cap it reads no more (and is still not closed over
 * the unread rest); a client that never sends its FIN (it stopped reading, or went away) is closed
 * at the deadline, read_timeout_ms after the linger began, so the slot is not held for good. */
UTEST(h2, a_lingering_connection_is_bounded_by_its_cap_and_deadline) {
    test_setup();
    MockH2Session mock;
    mock_init(&mock);
    mock.with_want_read = 1;
    mock.want_read_return = 1;
    g_mock_session = &mock;
    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);
    ASSERT_EQ(0, kl_test_set_nonblock((KlSocketHandle)pfd[1]));
    char rbuf[256];
    KlHttpConn conn;
    linger_conn_init(&conn, pfd[1], rbuf, sizeof rbuf);
    conn.reject_drain_max_bytes = 16;                      /* the cap: max(this, max_body_size 0) */
    kl_http2_server_upgrade(&conn, &test_router, &test_h2_cfg, NULL, 0);
    conn.state = KL_HTTP_CONN_HTTP2;
    linger_session_done(&mock);
    uint64_t t0 = kl_monotonic_ms();
    int st = kl_http2_server_on_writable(&conn);

    char junk[64];
    memset(junk, 'p', sizeof junk);
    (void)kl_test_sockwrite(pfd[0], junk, sizeof junk);    /* more than the cap */
    kl_test_sleep_ms(20);
    int st_cap = (int)kl_http_conn_drain_step(&conn, kl_monotonic_ms());
    int stopped = conn.answer_linger_stopped;
    uint64_t discarded = conn.drain_discarded;
    int st_more = (int)kl_http_conn_drain_step(&conn, kl_monotonic_ms());   /* reads no more */
    uint64_t discarded_more = conn.drain_discarded;
    uint64_t deadline = conn.drain_deadline_ms;
    int st_before = (int)kl_http_conn_drain_step(&conn, deadline - 1);
    int st_deadline = (int)kl_http_conn_drain_step(&conn, deadline);

    kl_http2_server_cleanup(&conn);
    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();
    ASSERT_EQ(st, (int)KL_HTTP_CONN_DRAINING);
    ASSERT_EQ(st_cap, (int)KL_HTTP_CONN_DRAINING);         /* past the cap: not a close over input */
    ASSERT_EQ(stopped, 1);
    ASSERT_EQ(discarded, (uint64_t)16);
    ASSERT_EQ(st_more, (int)KL_HTTP_CONN_DRAINING);
    ASSERT_EQ(discarded_more, (uint64_t)16);               /* the cap holds */
    ASSERT_TRUE(deadline >= t0 + 5000);                    /* read_timeout_ms after it began */
    ASSERT_TRUE(deadline <= kl_monotonic_ms() + 5000);
    ASSERT_EQ(st_before, (int)KL_HTTP_CONN_DRAINING);
    ASSERT_EQ(st_deadline, (int)KL_HTTP_CONN_CLOSED);      /* the abandoned client's slot comes back */
}

/* reject_drain_disable turns the linger off too (both bounds zero on the slot): a finished session
 * closes at once, as before the linger existed. */
UTEST(h2, a_finished_session_closes_when_the_drain_is_disabled) {
    test_setup();
    MockH2Session mock;
    mock_init(&mock);
    mock.with_want_read = 1;
    mock.want_read_return = 1;
    g_mock_session = &mock;
    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);
    char rbuf[64];
    KlHttpConn conn;
    linger_conn_init(&conn, pfd[1], rbuf, sizeof rbuf);
    conn.reject_drain_max_bytes = 0;
    conn.reject_drain_timeout_ms = 0;
    kl_http2_server_upgrade(&conn, &test_router, &test_h2_cfg, NULL, 0);
    linger_session_done(&mock);
    int st = kl_http2_server_on_writable(&conn);
    kl_http2_server_cleanup(&conn);
    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();
    ASSERT_EQ(st, (int)KL_HTTP_CONN_CLOSED);
}

/* A TLS session that writes through to the socket and marks its close_notify, to see the order. */
typedef struct {
    KlTls base;
    int shutdowns;
} LingerTls;
static kl_ssize_t ltls_write(KlTls *self, KlSocketHandle fd, const void *buf, size_t len) {
    (void)self;
    return (kl_ssize_t)kl_test_sockwrite(fd, buf, len);
}
static KlTlsResult ltls_handshake(KlTls *self, KlSocketHandle fd) {
    (void)self; (void)fd; return KL_TLS_OK;
}
static kl_ssize_t ltls_read(KlTls *self, KlSocketHandle fd, void *buf, size_t len) {
    (void)self; (void)fd; (void)buf; (void)len; return 0;
}
static size_t ltls_pending(KlTls *self) { (void)self; return 0; }
static void ltls_reset(KlTls *self) { (void)self; }
static void ltls_destroy(KlTls *self) { (void)self; }
static KlTlsResult ltls_shutdown(KlTls *self, KlSocketHandle fd) {
    ((LingerTls *)self)->shutdowns++;
    (void)kl_test_sockwrite(fd, "<close_notify>", 14);
    return KL_TLS_OK;
}

/* Over TLS the linger sends close_notify after the last record and before the FIN, so the client
 * sees a clean TLS end; a FIN alone (the half-close came first) would read as a truncation. */
UTEST(h2, a_lingering_tls_connection_sends_close_notify_before_its_fin) {
    test_setup();
    MockH2Session mock;
    mock_init(&mock);
    mock.with_want_read = 1;
    mock.want_read_return = 1;
    g_mock_session = &mock;
    LingerTls tls;
    memset(&tls, 0, sizeof tls);
    tls.base.handshake = ltls_handshake;
    tls.base.read      = ltls_read;
    tls.base.write     = ltls_write;
    tls.base.shutdown  = ltls_shutdown;
    tls.base.pending   = ltls_pending;
    tls.base.reset     = ltls_reset;
    tls.base.destroy   = ltls_destroy;
    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);
    char rbuf[64];
    KlHttpConn conn;
    linger_conn_init(&conn, pfd[1], rbuf, sizeof rbuf);
    conn.tls = &tls.base;
    kl_http2_server_upgrade(&conn, &test_router, &test_h2_cfg, NULL, 0);
    conn.state = KL_HTTP_CONN_HTTP2;
    mock.flush_send_len = 9;                               /* the GOAWAY */
    linger_session_done(&mock);
    int st = kl_http2_server_on_writable(&conn);
    char got[64];
    int eof = 0;
    size_t n = linger_read_to_eof(pfd[0], got, sizeof got, &eof);

    kl_http2_server_cleanup(&conn);
    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();
    ASSERT_EQ(st, (int)KL_HTTP_CONN_DRAINING);
    ASSERT_EQ(tls.shutdowns, 1);
    ASSERT_EQ(n, (size_t)(9 + 14));                        /* the record, then close_notify */
    ASSERT_EQ(0, memcmp(got + 9, "<close_notify>", 14));
    ASSERT_EQ(eof, 1);                                     /* then the FIN */
}

/* An engine whose close_notify a full socket refuses (WANT_WRITE) a given number of times, then
 * writes it through; it counts the shutdown calls. */
typedef struct {
    KlTls base;
    int refuse, calls;
} RefuseTls;
static KlTlsResult rtls_shutdown(KlTls *self, KlSocketHandle fd) {
    RefuseTls *t = (RefuseTls *)self;
    t->calls++;
    if (t->refuse > 0) { t->refuse--; return KL_TLS_WANT_WRITE; }
    (void)kl_test_sockwrite(fd, "<close_notify>", 14);
    return KL_TLS_OK;
}
static void rtls_init(RefuseTls *t, int refuse) {
    memset(t, 0, sizeof *t);
    t->refuse = refuse;
    t->base.handshake = ltls_handshake;
    t->base.read      = ltls_read;
    t->base.write     = ltls_write;
    t->base.shutdown  = rtls_shutdown;
    t->base.pending   = ltls_pending;
    t->base.reset     = ltls_reset;
    t->base.destroy   = ltls_destroy;
}
/* A rejected request whose framing completed with bytes over-sent past it: the drain starts with
 * its input already complete (nothing is left to wait for once a read would block). */
static void drain_conn_over_sent(KlHttpConn *conn, int fd, char *buf, size_t cap, KlTls *tls) {
    linger_conn_init(conn, fd, buf, cap);
    conn->tls = tls;
    conn->req.content_length = 4;
    conn->request_body_received = 8;                       /* over-sent past its framing */
    conn->request_body_complete = 1;                       /* the framing's verdict */
}

/* A drain whose input is already complete closes at the first read that would block, but not while
 * its close_notify waits for a writable socket: that close dropped the alert. It stays open for the
 * alert's writable event, then the half-close, then it closes. */
UTEST(h1_tls_drain, a_complete_drain_waits_for_its_refused_close_notify) {
    test_setup();
    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);
    ASSERT_EQ(0, kl_test_set_nonblock((KlSocketHandle)pfd[1]));
    char rbuf[64];
    RefuseTls tls;
    rtls_init(&tls, 1);
    KlHttpConn conn;
    drain_conn_over_sent(&conn, pfd[1], rbuf, sizeof rbuf, &tls.base);
    int st = (int)kl_http_conn_begin_drain(&conn);
    int pending = conn.tls_shutdown_pending;
    int st_idle = (int)kl_http_conn_drain_step(&conn, kl_monotonic_ms());   /* would block */
    (void)kl_http_conn_drain_on_writable(&conn);           /* the alert, then the half-close */
    char got[64];
    int eof = 0;
    size_t n = linger_read_to_eof(pfd[0], got, sizeof got, &eof);
    int st_after = (int)kl_http_conn_drain_step(&conn, kl_monotonic_ms());
    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();
    ASSERT_EQ(st, (int)KL_HTTP_CONN_DRAINING);
    ASSERT_EQ(pending, 1);
    ASSERT_EQ(st_idle, (int)KL_HTTP_CONN_DRAINING);        /* was CLOSED, the alert dropped */
    ASSERT_EQ(n, (size_t)14);
    ASSERT_EQ(0, memcmp(got, "<close_notify>", 14));
    ASSERT_EQ(eof, 1);
    ASSERT_EQ(st_after, (int)KL_HTTP_CONN_CLOSED);         /* then it closes, as before */
    ASSERT_EQ(conn.tls_shutdown_abandoned, 0);             /* delivered: a clean TLS end */
}

/* An engine that never takes the alert, though the socket is writable, must not keep the drain
 * spinning on writability until the deadline: after a bounded number of writable events it gives up
 * on the alert and half-closes. */
UTEST(h1_tls_drain, a_close_notify_refused_for_good_is_given_up_after_bounded_tries) {
    test_setup();
    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);
    ASSERT_EQ(0, kl_test_set_nonblock((KlSocketHandle)pfd[1]));
    char rbuf[64];
    RefuseTls tls;
    rtls_init(&tls, 1000000);
    KlHttpConn conn;
    drain_conn_over_sent(&conn, pfd[1], rbuf, sizeof rbuf, &tls.base);
    (void)kl_http_conn_begin_drain(&conn);
    for (int i = 0; i < 100; i++) (void)kl_http_conn_drain_on_writable(&conn);
    int calls = tls.calls, pending = conn.tls_shutdown_pending;
    int abandoned = conn.tls_shutdown_abandoned;
    KlEventMask m = kl_http_conn_drain_interest(&conn, 0);
    char got[64];
    int eof = 0;
    size_t n = linger_read_to_eof(pfd[0], got, sizeof got, &eof);
    int st = (int)conn.state;
    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();
    ASSERT_LE(calls, 10);                                  /* was one per writable event */
    ASSERT_EQ(pending, 0);
    ASSERT_EQ(abandoned, 1);                               /* recorded as given up, not as clean */
    ASSERT_FALSE(m & KL_EVENT_WRITE);                      /* writability no longer watched */
    ASSERT_EQ(n, (size_t)0);
    ASSERT_EQ(eof, 1);                                     /* the FIN, without the alert */
    ASSERT_EQ(st, (int)KL_HTTP_CONN_DRAINING);             /* the drain and its deadline go on */
}

/* Giving up on the alert never costs output already admitted: the alert is only attempted once
 * the session's output is out (all of it handed to the socket on readiness), so a refusal for good
 * cuts nothing but the alert itself. Here a done HTTP/2 session's last output, then an engine that
 * never takes the alert: every output byte, then the FIN, the end recorded as abandoned. */
UTEST(h1_tls_drain, giving_up_on_the_alert_keeps_the_output_before_it) {
    test_setup();
    MockH2Session mock;
    mock_init(&mock);
    mock.with_want_read = 1;
    mock.want_read_return = 1;
    g_mock_session = &mock;
    RefuseTls tls;
    rtls_init(&tls, 1000000);
    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);
    ASSERT_EQ(0, kl_test_set_nonblock((KlSocketHandle)pfd[1]));
    char rbuf[64];
    KlHttpConn conn;
    linger_conn_init(&conn, pfd[1], rbuf, sizeof rbuf);
    conn.tls = &tls.base;
    kl_http2_server_upgrade(&conn, &test_router, &test_h2_cfg, NULL, 0);
    conn.state = KL_HTTP_CONN_HTTP2;
    mock.flush_send_len = 48;                              /* the last frames */
    linger_session_done(&mock);
    int st = kl_http2_server_on_writable(&conn);
    for (int i = 0; i < 50; i++) (void)kl_http_conn_drain_on_writable(&conn);
    char got[128];
    int eof = 0;
    size_t n = linger_read_to_eof(pfd[0], got, sizeof got, &eof);
    int calls = tls.calls, abandoned = conn.tls_shutdown_abandoned;
    kl_http2_server_cleanup(&conn);
    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();
    ASSERT_EQ(st, (int)KL_HTTP_CONN_DRAINING);
    ASSERT_EQ(n, (size_t)48);                              /* every output byte */
    ASSERT_EQ(eof, 1);                                     /* then the FIN, no alert */
    ASSERT_LE(calls, 10);
    ASSERT_EQ(abandoned, 1);
}

/* On a completion loop the alert goes into the engine's ring behind all output and is never
 * retried: an engine whose ring refuses it does not stall the half-close (it follows once the
 * queue is out), and that end is recorded as abandoned, not as a clean TLS shutdown. */
UTEST(h1_tls_drain, a_completion_drain_records_a_refused_alert_as_abandoned) {
    test_setup();
    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);
    char rbuf[64];
    RefuseTls tls;
    rtls_init(&tls, 1000000);
    KlHttpConn conn;
    drain_conn_over_sent(&conn, pfd[1], rbuf, sizeof rbuf, &tls.base);
    conn.comp_driven = 1;
    int st = (int)kl_http_conn_begin_drain(&conn);
    int pending = conn.tls_shutdown_pending, then_shutwr = conn.comp_tlsq_then_shutwr;
    int abandoned = conn.tls_shutdown_abandoned, calls = tls.calls;
    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();
    ASSERT_EQ(st, (int)KL_HTTP_CONN_DRAINING);
    ASSERT_EQ(calls, 1);                                   /* tried once, behind the output */
    ASSERT_EQ(pending, 0);                                 /* no writability to wait for */
    ASSERT_EQ(then_shutwr, 1);                             /* the FIN once the queue is out */
    ASSERT_EQ(abandoned, 1);
}

/* A session that becomes done outside its connection's events (the drain's GOAWAY, the settle
 * pass's flush) must not wait for the next event to be ended: the due check reports it, so the
 * loop's wait does not block and the sweep after it ends the connection. On a readiness loop it
 * was ended one whole wait (about a second) late. A completion connection already closing (its
 * receive still to retire) is not due: counting it would keep the loop from waiting at all. */
UTEST(h2, a_session_done_outside_its_events_is_due) {
    test_setup();
    MockH2Session mock;
    mock_init(&mock);
    mock.with_want_read = 1;
    mock.want_read_return = 1;
    g_mock_session = &mock;
    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);
    int comp = 0;
    KlHttpConn *c = h2_sweep_setup(pfd, &comp);
    ASSERT_TRUE(c != NULL);
    int open_due = kl_http_server_settle_h2_due(&h2_sweep_srv);
    linger_session_done(&mock);
    int done_due = kl_http_server_settle_h2_due(&h2_sweep_srv);
    c->comp_closing = 1;
    int closing_due = kl_http_server_settle_h2_due(&h2_sweep_srv);
    c->comp_closing = 0;

    kl_http_server_free(&h2_sweep_srv);
    kl_test_closesock(pfd[0]);
    test_teardown();
    ASSERT_EQ(open_due, 0);
    ASSERT_EQ(done_due, 1);                                /* was 0: left to the next wait */
    ASSERT_EQ(closing_due, 0);
}

/* Completion: response DATA a posted send keeps moving is activity for an HTTP/2 connection too.
 * KEEL forgets a stream once its response is submitted, so the connection looked idle while its
 * download was still going out, and was closed under it. */
UTEST(h2, completion_sweep_counts_send_progress) {
    test_setup();
    MockH2Session mock;
    mock_init(&mock);
    g_mock_session = &mock;
    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);
    int comp = 0;
    KlHttpConn *c = h2_sweep_setup(pfd, &comp);
    ASSERT_TRUE(c != NULL);
    if (!comp) {
        kl_http_server_free(&h2_sweep_srv);
        kl_test_closesock(pfd[0]);
        test_teardown();
        UTEST_SKIP("a readiness loop: send progress is a completion engine's");
    }

    uint64_t now = kl_monotonic_ms();
    c->last_active_ms = now - 1000;                        /* the last read was long ago */
    c->stream.send_progress += 256 * 1024;                 /* ...but the download is moving */
    kl_http_server_sweep_conn_timeouts(&h2_sweep_srv, now, comp);
    int destroyed = mock.destroy_count;
    uint64_t active = c->last_active_ms;

    kl_http_server_free(&h2_sweep_srv);
    kl_test_closesock(pfd[0]);
    test_teardown();
    ASSERT_EQ(destroyed, 0);                               /* was 1: timed out mid-download */
    ASSERT_EQ(active, now);
}

UTEST(h2, cleanup_frees_all) {
    test_setup();
    MockH2Session mock;
    mock_init(&mock);
    g_mock_session = &mock;

    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);

    KlHttpConn conn;
    memset(&conn, 0, sizeof(conn));
    conn.stream.fd = pfd[1];
    conn.stream.alloc = &test_alloc;
    kl_http_router_add(&test_router, "GET", "/x", test_handler, NULL, NULL);

    kl_http2_server_upgrade(&conn, &test_router, &test_h2_cfg, NULL, 0);

    /* Create 3 streams */
    mock.callbacks.on_request(mock.cb_user_data, 1,
                               "GET", 3, "/x", 2,
                               NULL, 0, NULL, NULL, NULL, NULL, 0);
    mock.callbacks.on_request(mock.cb_user_data, 3,
                               "GET", 3, "/x", 2,
                               NULL, 0, NULL, NULL, NULL, NULL, 0);
    mock.callbacks.on_request(mock.cb_user_data, 5,
                               "GET", 3, "/x", 2,
                               NULL, 0, NULL, NULL, NULL, NULL, 0);
    ASSERT_EQ(conn.h2->num_streams, 3);

    /* Cleanup should destroy all streams + session */
    kl_http2_server_cleanup(&conn);
    ASSERT_TRUE(conn.h2 == NULL);
    ASSERT_EQ(mock.destroy_count, 1);

    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();
}

UTEST(h2, h2c_disabled_when_null) {
    /* h2_config=NULL means preface check is skipped */
    KlHttpConn conn;
    memset(&conn, 0, sizeof(conn));
    conn.h2_config = NULL;

    /* With h2_config NULL, the preface detection block is skipped entirely */
    ASSERT_TRUE(conn.h2_config == NULL);
    ASSERT_TRUE(conn.h2 == NULL);
}

UTEST(h2, response_header_extraction) {
    test_setup();
    MockH2Session mock;
    mock_init(&mock);
    g_mock_session = &mock;

    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);

    KlHttpConn conn;
    memset(&conn, 0, sizeof(conn));
    conn.stream.fd = pfd[1];
    conn.stream.alloc = &test_alloc;
    kl_http_router_add(&test_router, "GET", "/json", test_handler, NULL, NULL);

    kl_http2_server_upgrade(&conn, &test_router, &test_h2_cfg, NULL, 0);

    mock.callbacks.on_request(mock.cb_user_data, 1,
                               "GET", 3, "/json", 5,
                               NULL, 0, NULL, NULL, NULL, NULL, 0);
    mock.callbacks.on_stream_end(mock.cb_user_data, 1);

    /* Handler called kl_http_response_json → submit_response should have
     * Content-Type header and correct body */
    ASSERT_EQ(mock.submit_count, 1);
    ASSERT_EQ(mock.last_status, 200);

    /* Check that content-type header was extracted */
    int found_ct = 0;
    for (int i = 0; i < mock.last_num_headers; i++) {
        if (strcasecmp(mock.last_hdr_names[i], "Content-Type") == 0) {
            found_ct = 1;
            ASSERT_STREQ(mock.last_hdr_values[i], "application/json");
        }
    }
    ASSERT_EQ(found_ct, 1);

    kl_http2_server_cleanup(&conn);
    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();
}

UTEST(h2, handler_same_api) {
    test_setup();
    MockH2Session mock;
    mock_init(&mock);
    g_mock_session = &mock;

    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);

    KlHttpConn conn;
    memset(&conn, 0, sizeof(conn));
    conn.stream.fd = pfd[1];
    conn.stream.alloc = &test_alloc;

    /* Custom handler that sets a custom header */
    handler_status = 201;
    handler_body = "{\"id\":1}";
    handler_body_len = 8;
    kl_http_router_add(&test_router, "POST", "/create", test_handler, NULL, NULL);

    kl_http2_server_upgrade(&conn, &test_router, &test_h2_cfg, NULL, 0);

    mock.callbacks.on_request(mock.cb_user_data, 1,
                               "POST", 4, "/create", 7,
                               NULL, 0, NULL, NULL, NULL, NULL, 0);
    mock.callbacks.on_stream_end(mock.cb_user_data, 1);

    ASSERT_EQ(handler_called, 1);
    ASSERT_EQ(mock.last_status, 201);
    ASSERT_EQ(mock.last_body_len, (size_t)8);
    ASSERT_EQ(memcmp(mock.last_body, "{\"id\":1}", 8), 0);

    kl_http2_server_cleanup(&conn);
    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();
}

UTEST(h2, query_string_parsing) {
    test_setup();
    MockH2Session mock;
    mock_init(&mock);
    g_mock_session = &mock;

    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);

    KlHttpConn conn;
    memset(&conn, 0, sizeof(conn));
    conn.stream.fd = pfd[1];
    conn.stream.alloc = &test_alloc;
    kl_http_router_add(&test_router, "GET", "/search", test_handler, NULL, NULL);

    kl_http2_server_upgrade(&conn, &test_router, &test_h2_cfg, NULL, 0);

    mock.callbacks.on_request(mock.cb_user_data, 1,
                               "GET", 3, "/search?q=test&page=1", 21,
                               NULL, 0, NULL, NULL, NULL, NULL, 0);

    KlHttp2ServerStream *s = &conn.h2->streams[0];
    /* Path should be split at '?' */
    ASSERT_EQ(s->req.path_len, (size_t)7);
    ASSERT_EQ(memcmp(s->req.path, "/search", 7), 0);
    ASSERT_TRUE(s->req.query != NULL);
    ASSERT_EQ(s->req.query_len, (size_t)13);
    ASSERT_EQ(memcmp(s->req.query, "q=test&page=1", 13), 0);

    kl_http2_server_cleanup(&conn);
    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();
}

UTEST(h2, cleanup_null_is_noop) {
    /* kl_http2_server_cleanup with NULL h2 should be safe */
    KlHttpConn conn;
    memset(&conn, 0, sizeof(conn));
    conn.h2 = NULL;
    kl_http2_server_cleanup(&conn);
    ASSERT_TRUE(conn.h2 == NULL);
}

/* ── h2c upgrade from HTTP/1 (kl_http2_server_upgrade_from_h1) ──────────────── */
/* RFC 7540 3.2: the upgrading request is answered on stream 1 of the new HTTP/2 connection. The
 * server used to send 101 and start a fresh session, and the request was never answered. When an
 * upgrade cannot be done properly it is declined (KL_HTTP2_UPGRADE_DECLINED) before anything is
 * written, and the caller answers over HTTP/1.1 (RFC 9113 lets a server ignore the Upgrade). */

/* A GET /test carrying `Upgrade: h2c` (and HTTP2-Settings unless settings is NULL). */
static void h2c_req(KlHttpConn *conn, const char *settings, size_t content_length) {
    KlHttpRequest *r = &conn->req;
    r->method = "GET"; r->method_len = 3;
    r->path = "/test"; r->path_len = 5;
    r->query = "a=1"; r->query_len = 3;
    int n = 0;
    r->headers[n].name = "Host"; r->headers[n].name_len = 4;
    r->headers[n].value = "example"; r->headers[n].value_len = 7; n++;
    r->headers[n].name = "Upgrade"; r->headers[n].name_len = 7;
    r->headers[n].value = "h2c"; r->headers[n].value_len = 3; n++;
    if (settings) {
        r->headers[n].name = "HTTP2-Settings"; r->headers[n].name_len = 14;
        r->headers[n].value = settings; r->headers[n].value_len = strlen(settings); n++;
    }
    r->num_headers = n;
    r->content_length = content_length;
}

static int g_seen_query;
static void query_handler(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)ud;
    handler_called++;
    g_seen_query = req->query && req->query_len == 3 && memcmp(req->query, "a=1", 3) == 0 &&
                   kl_http_request_header(req, "HTTP2-Settings") == NULL &&
                   kl_http_request_header(req, "Upgrade") == NULL;
    kl_http_response_json(res, handler_status, handler_body, handler_body_len);
}

/* The 101 goes out, the session takes the settings, and the request is answered on stream 1. */
UTEST(h2, upgrade_from_h1_answers_the_request_on_stream_1) {
    test_setup();
    kl_http_router_add(&test_router, "GET", "/test", query_handler, NULL, NULL);
    MockH2Session mock; mock_init(&mock); mock.with_upgrade = 1; g_mock_session = &mock;
    int pfd[2]; ASSERT_EQ(kl_test_socketpair(pfd), 0);
    KlHttpConn conn; memset(&conn, 0, sizeof(conn));
    conn.stream.fd = pfd[1]; conn.stream.alloc = &test_alloc;
    h2c_req(&conn, "AAMAAABk", 0);
    g_seen_query = 0;

    int r = kl_http2_server_upgrade_from_h1(&conn, &test_router, &test_h2_cfg, NULL, 0);
    ASSERT_EQ(r, (int)KL_HTTP_CONN_HTTP2);
    ASSERT_TRUE(conn.h2 != NULL);
    ASSERT_EQ(mock.upgrade_count, 1);
    ASSERT_EQ(strcmp(mock.upgrade_settings, "AAMAAABk"), 0);
    ASSERT_EQ(mock.upgrade_head, 0);
    ASSERT_EQ(handler_called, 1);
    ASSERT_EQ(g_seen_query, 1);                   /* query kept; hop-by-hop headers dropped */
    ASSERT_EQ(mock.submit_count, 1);
    ASSERT_EQ(mock.last_stream_id, 1u);           /* answered on stream 1 */
    ASSERT_EQ(mock.last_status, 200);

    char buf[128];
    ASSERT_GT(kl_test_poll1(pfd[0], 0, 1000), 0);
    long n = kl_test_sockread(pfd[0], buf, sizeof(buf) - 1);
    ASSERT_GT(n, (long)0);
    buf[n] = '\0';
    ASSERT_EQ(strncmp(buf, "HTTP/1.1 101 Switching Protocols", 32), 0);

    kl_http2_server_cleanup(&conn);
    ASSERT_EQ(mock.destroy_count, 1);
    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();
}

/* Stream 1 does not run the pre-body middleware again (it ran in the HTTP/1.1 phase), so it must
 * carry what that middleware left: req->ctx (the documented way to hand data to the handler) and the
 * response headers it added (CORS, say). Stream 1 was built from a fresh request and response, so the
 * handler saw ctx NULL and the HTTP/2 response lacked the headers. */
static int ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char x = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char y = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (x != y) return 0;
    }
    return *a == *b;
}
static void *g_seen_ctx;
static void ctx_handler(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)ud;
    handler_called++;
    g_seen_ctx = req->ctx;
    kl_http_response_json(res, 200, "{}", 2);
}

UTEST(h2, upgrade_keeps_the_http1_middleware_state) {
    test_setup();
    kl_http_router_add(&test_router, "GET", "/test", ctx_handler, NULL, NULL);
    MockH2Session mock; mock_init(&mock); mock.with_upgrade = 1; g_mock_session = &mock;
    int pfd[2]; ASSERT_EQ(kl_test_socketpair(pfd), 0);
    KlHttpConn conn; memset(&conn, 0, sizeof(conn));
    conn.stream.fd = pfd[1]; conn.stream.alloc = &test_alloc;
    h2c_req(&conn, "AAMAAABk", 0);
    static int marker;
    conn.req.ctx = &marker;                       /* as an auth middleware would set it */
    ASSERT_EQ(kl_http_response_init(&conn.res, &test_alloc), 0);
    ASSERT_EQ(kl_http_response_header(&conn.res, "X-Mw", "ran"), 0);   /* as CORS would */
    g_seen_ctx = NULL;

    int r = kl_http2_server_upgrade_from_h1(&conn, &test_router, &test_h2_cfg, NULL, 0);
    int found = 0;
    for (int i = 0; i < mock.last_num_headers && i < 16; i++)
        if (ieq(mock.last_hdr_names[i], "x-mw") &&
            strcmp(mock.last_hdr_values[i], "ran") == 0) found = 1;
    void *seen = g_seen_ctx;
    kl_http2_server_cleanup(&conn);
    kl_http_response_free(&conn.res);
    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();
    ASSERT_EQ(r, (int)KL_HTTP_CONN_HTTP2);
    ASSERT_TRUE(seen == &marker);                 /* was NULL */
    ASSERT_EQ(found, 1);                          /* was 0: the header was dropped */
}

/* Declined before anything is written: no HTTP2-Settings, a request body, or a session that
 * cannot take an upgrade. */
static void declined_case(int *utest_result, const char *settings, size_t body, int with_upgrade) {
    test_setup();
    MockH2Session mock; mock_init(&mock); mock.with_upgrade = with_upgrade; g_mock_session = &mock;
    int pfd[2]; ASSERT_EQ(kl_test_socketpair(pfd), 0);
    KlHttpConn conn; memset(&conn, 0, sizeof(conn));
    conn.stream.fd = pfd[1]; conn.stream.alloc = &test_alloc;
    h2c_req(&conn, settings, body);

    int r = kl_http2_server_upgrade_from_h1(&conn, &test_router, &test_h2_cfg, NULL, 0);
    ASSERT_EQ(r, KL_HTTP2_UPGRADE_DECLINED);
    ASSERT_TRUE(conn.h2 == NULL);
    ASSERT_EQ(mock.upgrade_count, 0);
    ASSERT_EQ(mock.submit_count, 0);
    ASSERT_EQ(kl_test_poll1(pfd[0], 0, 50), 0);   /* no 101 on the wire */
    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();
}
UTEST(h2, upgrade_from_h1_declined_without_settings)   { declined_case(utest_result, NULL, 0, 1); }
UTEST(h2, upgrade_from_h1_declined_with_a_body)        { declined_case(utest_result, "AAMAAABk", 5, 1); }
UTEST(h2, upgrade_from_h1_declined_without_upgrade_op) { declined_case(utest_result, "AAMAAABk", 0, 0); }

/* An HTTP/2 response never carries a connection-specific header (RFC 9113 8.2.2): a client resets
 * the stream as malformed. The response filter dropped Connection, Transfer-Encoding and Keep-Alive
 * only; Upgrade, Proxy-Connection and TE went out. Stream 1 of an h2c upgrade now also carries what
 * the HTTP/1.1-phase middleware set, and a middleware advertising h2c sets Upgrade there. */
static void hop_headers_handler(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)req; (void)ud;
    kl_http_response_status(res, 200);
    kl_http_response_header(res, "Upgrade", "h2c");
    kl_http_response_header(res, "Proxy-Connection", "keep-alive");
    kl_http_response_header(res, "TE", "trailers");
    kl_http_response_header(res, "X-Kept", "1");
    (void)kl_http_response_body_copy(res, "ok", 2);
}
UTEST(h2, connection_specific_response_headers_are_not_sent) {
    test_setup();
    MockH2Session mock; mock_init(&mock); g_mock_session = &mock;
    int pfd[2]; ASSERT_EQ(kl_test_socketpair(pfd), 0);
    KlHttpConn conn; memset(&conn, 0, sizeof(conn));
    conn.stream.fd = pfd[1]; conn.stream.alloc = &test_alloc;
    kl_http_router_add(&test_router, "GET", "/hop", hop_headers_handler, NULL, NULL);
    kl_http2_server_upgrade(&conn, &test_router, &test_h2_cfg, NULL, 0);
    mock.callbacks.on_request(mock.cb_user_data, 1, "GET", 3, "/hop", 4, NULL, 0, NULL, NULL, NULL, NULL, 0);
    (void)mock.callbacks.on_stream_end(mock.cb_user_data, 1);
    int forbidden = 0, kept = 0;
    for (int i = 0; i < mock.last_num_headers && i < 16; i++) {
        const char *n = mock.last_hdr_names[i];
        if (ieq(n, "upgrade") || ieq(n, "proxy-connection") || ieq(n, "te")) forbidden++;
        if (ieq(n, "x-kept")) kept++;
    }
    kl_http2_server_cleanup(&conn);
    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();
    ASSERT_EQ(forbidden, 0);                      /* was 3 */
    ASSERT_EQ(kept, 1);
}

/* A session that refuses the settings (nghttp2 rejects an out-of-range value, or more settings than
 * the adapter takes) is asked before the 101, so the request is still answered over HTTP/1.1. The
 * 101 went out first, and the refusal then closed the connection with no response at all. */
UTEST(h2, upgrade_from_h1_declined_when_the_session_refuses_the_settings) {
    test_setup();
    MockH2Session mock; mock_init(&mock); mock.with_upgrade = 1; mock.upgrade_fail = 1;
    g_mock_session = &mock;
    int pfd[2]; ASSERT_EQ(kl_test_socketpair(pfd), 0);
    KlHttpConn conn; memset(&conn, 0, sizeof(conn));
    conn.stream.fd = pfd[1]; conn.stream.alloc = &test_alloc;
    h2c_req(&conn, "AAIAAAAC", 0);                /* ENABLE_PUSH = 2: well-formed, invalid */
    int r = kl_http2_server_upgrade_from_h1(&conn, &test_router, &test_h2_cfg, NULL, 0);
    int quiet = kl_test_poll1(pfd[0], 0, 50) == 0;
    int h2_null = conn.h2 == NULL;
    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();
    ASSERT_EQ(r, KL_HTTP2_UPGRADE_DECLINED);      /* was KL_HTTP_CONN_CLOSED */
    ASSERT_TRUE(h2_null);
    ASSERT_TRUE(quiet);                           /* was: a 101, then nothing */
}

/* The upgrading request already went through the pre-body middleware in its HTTP/1.1 phase (the only
 * caller of upgrade_from_h1 runs it first); stream 1 must not run it again, or a rate limiter or an
 * audit log counts the request twice. */
UTEST(h2, upgrade_does_not_run_pre_body_middleware_again) {
    test_setup();
    kl_http_router_add(&test_router, "GET", "/test", query_handler, NULL, NULL);
    kl_http_router_use(&test_router, "*", "/*", test_middleware, NULL);
    MockH2Session mock; mock_init(&mock); mock.with_upgrade = 1; g_mock_session = &mock;
    int pfd[2]; ASSERT_EQ(kl_test_socketpair(pfd), 0);
    KlHttpConn conn; memset(&conn, 0, sizeof(conn));
    conn.stream.fd = pfd[1]; conn.stream.alloc = &test_alloc;
    h2c_req(&conn, "AAMAAABk", 0);
    middleware_called = 0;
    int r = kl_http2_server_upgrade_from_h1(&conn, &test_router, &test_h2_cfg, NULL, 0);
    int calls = middleware_called;
    kl_http2_server_cleanup(&conn);
    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();
    ASSERT_EQ(r, (int)KL_HTTP_CONN_HTTP2);
    ASSERT_EQ(calls, 0);                          /* was 1: a second run for stream 1 */
}

/* A HEAD response carries no body over HTTP/2 either (the HTTP/1.1 path drops it); a strict client
 * resets a stream whose HEAD response has DATA. */
UTEST(h2, head_response_has_no_body) {
    test_setup();
    MockH2Session mock; mock_init(&mock); g_mock_session = &mock;
    int pfd[2]; ASSERT_EQ(kl_test_socketpair(pfd), 0);
    KlHttpConn conn; memset(&conn, 0, sizeof(conn));
    conn.stream.fd = pfd[1]; conn.stream.alloc = &test_alloc;
    kl_http_router_add(&test_router, "HEAD", "/h", test_handler, NULL, NULL);
    kl_http2_server_upgrade(&conn, &test_router, &test_h2_cfg, NULL, 0);
    mock.callbacks.on_request(mock.cb_user_data, 1, "HEAD", 4, "/h", 2, NULL, 0, NULL, NULL, NULL, NULL, 0);
    (void)mock.callbacks.on_stream_end(mock.cb_user_data, 1);
    int status = mock.last_status;
    size_t blen = mock.last_body_len;
    kl_http2_server_cleanup(&conn);
    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();
    ASSERT_EQ(status, 200);
    ASSERT_EQ(blen, (size_t)0);                   /* was: the handler's body */
}

/* The early 500 fallbacks (a streaming body HTTP/2 does not support, an oversized or unreadable
 * file) keep the HEAD rule too: headers only. They submitted their error text as a body. */
static void stream_handler(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)req; (void)ud;
    KlHttpResponseWriteFn w = NULL;
    void *wc = NULL;
    if (kl_http_response_begin_stream(res, 200, &w, &wc) < 0) return;
    w(wc, "x", 1);
    kl_http_response_end_stream(res);
}
UTEST(h2, head_fallback_500_has_no_body) {
    test_setup();
    MockH2Session mock; mock_init(&mock); g_mock_session = &mock;
    int pfd[2]; ASSERT_EQ(kl_test_socketpair(pfd), 0);
    KlHttpConn conn; memset(&conn, 0, sizeof(conn));
    conn.stream.fd = pfd[1]; conn.stream.alloc = &test_alloc;
    kl_http_router_add(&test_router, "HEAD", "/s", stream_handler, NULL, NULL);
    kl_http2_server_upgrade(&conn, &test_router, &test_h2_cfg, NULL, 0);
    mock.callbacks.on_request(mock.cb_user_data, 1, "HEAD", 4, "/s", 2, NULL, 0, NULL, NULL, NULL, NULL, 0);
    (void)mock.callbacks.on_stream_end(mock.cb_user_data, 1);
    int status = mock.last_status;
    size_t blen = mock.last_body_len;
    kl_http2_server_cleanup(&conn);
    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();
    ASSERT_EQ(status, 500);
    ASSERT_EQ(blen, (size_t)0);                   /* was: "Streaming responses not supported ..." */
}

/* ═══════════════════════════════════════════════════════════════════
 * A streamed response must never put HTTP/1.1 framing on this connection.
 *
 * kl_http_response_begin_stream() writes eagerly: the status line, the headers and every chunk go
 * out through the response's own write target. An HTTP/2 stream has no such target: the session
 * owns the connection and frames what goes on it. The stream response was nevertheless initialized
 * with the connection's live socket and TLS session, so a handler that streamed (a chunked body,
 * or any kl_http_sse_* route) put raw "HTTP/1.1 200 OK ... Transfer-Encoding: chunked" bytes on
 * the HTTP/2 connection, and h2_submit_response() then submitted its own 500 HEADERS on top: two
 * answers to one request, the first of them not an HTTP/2 frame at all.
 *
 * The test above this one drives the same handler and asserts on the session's recorded status,
 * which is exactly why the framing went unnoticed: the status was always the 500. These tests read
 * the socket. They assert the transport saw nothing, and separately that the session still got its
 * 500, so the refusal cannot be paid for by losing the error response.
 * ═══════════════════════════════════════════════════════════════════ */

/* What the handler's own calls returned, so the refusal is asserted where the handler sees it. */
static int g_begin_rc;
static int g_write_rc;
static int g_end_rc;

static void wire_streaming_handler(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)req; (void)ud;
    KlHttpResponseWriteFn w = NULL;
    void *wc = NULL;
    g_begin_rc = kl_http_response_begin_stream(res, 200, &w, &wc);
    if (g_begin_rc < 0) return;
    g_write_rc = w(wc, "HELLO-STREAM", 12);
    g_end_rc = kl_http_response_end_stream(res);
}

/* Everything the server wrote to the connection end of the pair since the last read. The writes
 * under test are synchronous inside the nghttp2 callbacks, so by the time the callbacks have
 * returned the whole of it is in the socket buffer: poll once with a real timeout to catch it,
 * then drain with no wait left. */
static size_t wire_read_all(int fd, char *buf, size_t cap) {
    size_t total = 0;
    int first = 1;
    while (total < cap - 1) {
        int r = kl_test_poll1(fd, 0, first ? 200 : 0);
        first = 0;
        if (r <= 0) break;
        long n = kl_test_sockread(fd, buf + total, cap - 1 - total);
        if (n <= 0) break;
        total += (size_t)n;
    }
    buf[total] = '\0';
    return total;
}

static int wire_contains(const char *buf, size_t len, const char *needle) {
    return len > 0 && strstr(buf, needle) != NULL;
}

UTEST(h2, a_streaming_handler_emits_no_http1_framing_on_the_wire) {
    test_setup();
    MockH2Session mock; mock_init(&mock); g_mock_session = &mock;
    int pfd[2]; ASSERT_EQ(kl_test_socketpair(pfd), 0);
    KlHttpConn conn; memset(&conn, 0, sizeof(conn));
    conn.stream.fd = pfd[1]; conn.stream.alloc = &test_alloc;
    kl_http_router_add(&test_router, "GET", "/stream", wire_streaming_handler, NULL, NULL);
    kl_http2_server_upgrade(&conn, &test_router, &test_h2_cfg, NULL, 0);

    /* Nothing the upgrade itself produced should be sitting on the socket: start clean. */
    char pre[8];
    ASSERT_EQ(wire_read_all(pfd[0], pre, sizeof pre), (size_t)0);

    g_begin_rc = g_write_rc = g_end_rc = 0;
    mock.callbacks.on_request(mock.cb_user_data, 1, "GET", 3, "/stream", 7, NULL, 0, NULL, NULL, NULL, NULL, 0);
    (void)mock.callbacks.on_stream_end(mock.cb_user_data, 1);

    char wire[4096];
    size_t n = wire_read_all(pfd[0], wire, sizeof wire);

    /* The defect: raw HTTP/1.1 wire format on an HTTP/2 connection. */
    ASSERT_FALSE(wire_contains(wire, n, "HTTP/1.1"));
    ASSERT_FALSE(wire_contains(wire, n, "Transfer-Encoding"));
    ASSERT_FALSE(wire_contains(wire, n, "chunked"));
    ASSERT_FALSE(wire_contains(wire, n, "HELLO-STREAM"));
    ASSERT_EQ(n, (size_t)0);                       /* the transport saw nothing at all */

    /* The refusal reaches the handler before any side effect, and it is a refusal, not a silent
     * success: nothing was written, so the write and end calls never happened. */
    ASSERT_EQ(g_begin_rc, -1);
    ASSERT_EQ(g_write_rc, 0);
    ASSERT_EQ(g_end_rc, 0);

    /* And the HTTP/2 error response still stands: the session is still the only writer, and it
     * still answers the stream with the existing 500. */
    ASSERT_EQ(mock.last_status, 500);

    /* No transport close: the connection is still the one we handed it, and still writable. */
    ASSERT_EQ(conn.stream.fd, (KlSocketHandle)pfd[1]);
    ASSERT_TRUE(kl_test_poll1(pfd[1], 1, 0) > 0);

    kl_http2_server_cleanup(&conn);
    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();
}

/* A recording TLS session, to check the other half of the same leak: the stream response was also
 * handed the connection's TLS session, so a streaming handler wrote its framing into the session
 * that carries the HTTP/2 frames (encrypted, so invisible to the socket above). The optional vtable
 * entries stay NULL: this test only needs to know whether anything was handed to write(). */
typedef struct {
    KlTls base;
    size_t write_calls;
    size_t write_bytes;
    char   first[256];
} RecTls;

static KlTlsResult rec_handshake(KlTls *self, KlSocketHandle fd) {
    (void)self; (void)fd; return KL_TLS_OK;
}
static kl_ssize_t rec_read(KlTls *self, KlSocketHandle fd, void *buf, size_t len) {
    (void)self; (void)fd; (void)buf; (void)len; return 0;
}
static kl_ssize_t rec_write(KlTls *self, KlSocketHandle fd, const void *buf, size_t len) {
    (void)fd;
    RecTls *t = (RecTls *)self;
    t->write_calls++;
    t->write_bytes += len;
    size_t have = strlen(t->first);
    if (have + len < sizeof t->first) memcpy(t->first + have, buf, len);
    return (kl_ssize_t)len;
}
static KlTlsResult rec_shutdown(KlTls *self, KlSocketHandle fd) {
    (void)self; (void)fd; return KL_TLS_OK;
}
static size_t rec_pending(KlTls *self) { (void)self; return 0; }
static void rec_reset(KlTls *self) { (void)self; }
static void rec_destroy(KlTls *self) { (void)self; }

/* Same handler, same connection, but the h2 connection is TLS-protected (an ALPN h2 negotiation
 * reaches exactly this shape). The bytes must reach neither the socket nor the TLS session. */
UTEST(h2, a_streaming_handler_writes_nothing_into_the_connections_tls_session) {
    test_setup();
    MockH2Session mock; mock_init(&mock); g_mock_session = &mock;
    RecTls tls; memset(&tls, 0, sizeof tls);
    tls.base.handshake = rec_handshake;
    tls.base.read      = rec_read;
    tls.base.write     = rec_write;
    tls.base.shutdown  = rec_shutdown;
    tls.base.pending   = rec_pending;
    tls.base.reset     = rec_reset;
    tls.base.destroy   = rec_destroy;

    int pfd[2]; ASSERT_EQ(kl_test_socketpair(pfd), 0);
    KlHttpConn conn; memset(&conn, 0, sizeof(conn));
    conn.stream.fd = pfd[1]; conn.stream.alloc = &test_alloc;
    conn.tls = &tls.base;                       /* the h2 connection runs over TLS */
    kl_http_router_add(&test_router, "GET", "/stream", wire_streaming_handler, NULL, NULL);
    kl_http2_server_upgrade(&conn, &test_router, &test_h2_cfg, NULL, 0);

    g_begin_rc = g_write_rc = g_end_rc = 0;
    mock.callbacks.on_request(mock.cb_user_data, 1, "GET", 3, "/stream", 7, NULL, 0, NULL, NULL, NULL, NULL, 0);
    (void)mock.callbacks.on_stream_end(mock.cb_user_data, 1);

    char wire[4096];
    size_t n = wire_read_all(pfd[0], wire, sizeof wire);

    ASSERT_EQ(tls.write_calls, (size_t)0);      /* nothing was handed to the session to encrypt */
    ASSERT_FALSE(wire_contains(tls.first, strlen(tls.first), "HTTP/1.1"));
    ASSERT_FALSE(wire_contains(wire, n, "HTTP/1.1"));
    ASSERT_FALSE(wire_contains(wire, n, "HELLO-STREAM"));
    ASSERT_EQ(n, (size_t)0);

    ASSERT_EQ(g_begin_rc, -1);
    ASSERT_EQ(mock.last_status, 500);           /* answered over the session, as before */

    kl_http2_server_cleanup(&conn);
    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();
}

/* The same refusal must hold for the other eager public entry point on a response that has no
 * HTTP/1.1 write target: kl_http_response_send() would otherwise put a whole HTTP/1.1 response
 * (status line, headers, body) on the HTTP/2 connection without a stream in sight. */
static void wire_send_handler(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)req; (void)ud;
    kl_http_response_body_copy(res, "HELLO-BUFFER", 12);
    g_begin_rc = kl_http_response_send(res);
}

UTEST(h2, a_direct_response_send_emits_no_http1_framing_on_the_wire) {
    test_setup();
    MockH2Session mock; mock_init(&mock); g_mock_session = &mock;
    int pfd[2]; ASSERT_EQ(kl_test_socketpair(pfd), 0);
    KlHttpConn conn; memset(&conn, 0, sizeof(conn));
    conn.stream.fd = pfd[1]; conn.stream.alloc = &test_alloc;
    kl_http_router_add(&test_router, "GET", "/send", wire_send_handler, NULL, NULL);
    kl_http2_server_upgrade(&conn, &test_router, &test_h2_cfg, NULL, 0);

    char pre[8];
    ASSERT_EQ(wire_read_all(pfd[0], pre, sizeof pre), (size_t)0);

    g_begin_rc = 0;
    mock.callbacks.on_request(mock.cb_user_data, 1, "GET", 3, "/send", 5, NULL, 0, NULL, NULL, NULL, NULL, 0);
    (void)mock.callbacks.on_stream_end(mock.cb_user_data, 1);

    char wire[4096];
    size_t n = wire_read_all(pfd[0], wire, sizeof wire);
    ASSERT_FALSE(wire_contains(wire, n, "HTTP/1.1"));
    ASSERT_FALSE(wire_contains(wire, n, "HELLO-BUFFER"));
    ASSERT_EQ(n, (size_t)0);
    ASSERT_EQ(g_begin_rc, -1);                  /* refused, not sent */

    kl_http2_server_cleanup(&conn);
    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();
}

/* A file response whose file is shorter than its declared size (it shrank after the handler sized
 * it) must not go out as a complete 200: HTTP/2 sends no content-length here, so the client could
 * not tell the body was cut. HTTP/1 closes the connection in the same case (E13). */
#if defined(_WIN32)
#include <io.h>
#include <fcntl.h>
#define T_OPEN_RD(p) _open((p), _O_RDONLY | _O_BINARY)
#else
#include <fcntl.h>
#include <unistd.h>
#define T_OPEN_RD(p) open((p), O_RDONLY)
#endif
static char g_short_path[64];
static void short_file_handler(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)req; (void)ud;
    int fd = T_OPEN_RD(g_short_path);
    if (fd < 0) { kl_http_response_error(res, 404, "missing"); return; }
    kl_http_response_status(res, 200);
    kl_http_response_file(res, (KlSocketHandle)fd, 10 + 100);   /* 100 bytes the file lacks */
}
UTEST(h2, file_shorter_than_its_size_is_not_a_complete_200) {
    snprintf(g_short_path, sizeof g_short_path, "keel_h2_short_%d.tmp", (int)rand());
    FILE *f = fopen(g_short_path, "wb");
    ASSERT_TRUE(f != NULL);
    fwrite("0123456789", 1, 10, f);
    fclose(f);
    test_setup();
    MockH2Session mock; mock_init(&mock); g_mock_session = &mock;
    int pfd[2]; ASSERT_EQ(kl_test_socketpair(pfd), 0);
    KlHttpConn conn; memset(&conn, 0, sizeof(conn));
    conn.stream.fd = pfd[1]; conn.stream.alloc = &test_alloc;
    kl_http_router_add(&test_router, "GET", "/f", short_file_handler, NULL, NULL);
    kl_http2_server_upgrade(&conn, &test_router, &test_h2_cfg, NULL, 0);
    mock.callbacks.on_request(mock.cb_user_data, 1, "GET", 3, "/f", 2, NULL, 0, NULL, NULL, NULL, NULL, 0);
    (void)mock.callbacks.on_stream_end(mock.cb_user_data, 1);
    int status = mock.last_status;
    kl_http2_server_cleanup(&conn);
    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();
    remove(g_short_path);
    ASSERT_NE(status, 200);                       /* was 200 with the 10 bytes it had */
}

/* A request that ends with its HEADERS (no body) is handled like a bodiless HTTP/1.1 request: no body
 * reader is created for it. The reader factory ran anyway, and one that needs a body (a multipart
 * reader without a Content-Type) answered 415. */
static KlHttpBodyReader *refusing_factory(KlAllocator *alloc, const KlHttpRequest *req, void *ud) {
    (void)alloc; (void)req; (void)ud;
    return NULL;
}
UTEST(h2, bodiless_request_runs_its_handler_without_a_reader) {
    test_setup();
    MockH2Session mock; mock_init(&mock); g_mock_session = &mock;
    int pfd[2]; ASSERT_EQ(kl_test_socketpair(pfd), 0);
    KlHttpConn conn; memset(&conn, 0, sizeof(conn));
    conn.stream.fd = pfd[1]; conn.stream.alloc = &test_alloc;
    kl_http_router_add(&test_router, "POST", "/form", test_handler, NULL, refusing_factory);
    kl_http2_server_upgrade(&conn, &test_router, &test_h2_cfg, NULL, 0);
    handler_called = 0;
    mock.callbacks.on_request(mock.cb_user_data, 1, "POST", 4, "/form", 5, NULL, 0, NULL, NULL, NULL, NULL, 0);
    (void)mock.callbacks.on_stream_end(mock.cb_user_data, 1);   /* END_STREAM on the HEADERS */
    int status = mock.last_status, calls = handler_called;
    kl_http2_server_cleanup(&conn);
    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();
    ASSERT_EQ(calls, 1);                          /* was 0: 415 before the handler */
    ASSERT_EQ(status, 200);
}

/* Malformed session vtable: the session is destroyed (ownership cleanup), nothing is written, and
 * conn.h2 is left NULL (no dangling connection state); the request stays on HTTP/1.1. */
UTEST(h2, upgrade_from_h1_malformed_rejected) {
    test_setup();
    MockH2Session mock; mock_init(&mock); g_mock_session = &mock;
    int pfd[2]; ASSERT_EQ(kl_test_socketpair(pfd), 0);
    KlHttpConn conn; memset(&conn, 0, sizeof(conn));
    conn.stream.fd = pfd[1]; conn.stream.alloc = &test_alloc;
    h2c_req(&conn, "AAMAAABk", 0);
    mock.skip_vtable_init = 1;
    mock.base.recv = NULL;                       /* missing required op */
    mock.base.submit_response = mock_submit_response;
    mock.base.want_write = mock_want_write;
    mock.base.flush = mock_flush;
    mock.base.shutdown = mock_shutdown;
    mock.base.destroy = mock_destroy;
    mock.base.upgrade = mock_upgrade;

    int r = kl_http2_server_upgrade_from_h1(&conn, &test_router, &test_h2_cfg, NULL, 0);
    ASSERT_EQ(r, KL_HTTP2_UPGRADE_DECLINED);
    ASSERT_EQ(mock.destroy_count, 1);            /* the rejected session was destroyed */
    ASSERT_TRUE(conn.h2 == NULL);                /* no dangling h2 connection */
    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();
}

/* ── Prior knowledge through a real server ─────────────────────────────────────────────────
 * A client with prior knowledge opens with the 24-byte preface magic. Every entry path hands the
 * session the whole preface, magic included, and the nghttp2 adapter lets nghttp2 consume it. The
 * completion path (io_uring, IOCP, pollcomp) stripped the magic first, so nghttp2 saw SETTINGS where
 * it expected the magic and rejected the connection. */
static KlHttpServer g_pk_srv;
static MockH2Session g_pk_mock;
static void pk_server_thread(void *arg) { (void)arg; kl_http_server_run(&g_pk_srv); }

UTEST(h2, prior_knowledge_preface_reaches_the_session_whole) {
    test_setup();
    mock_init(&g_pk_mock);
    g_mock_session = &g_pk_mock;
    KlHttpServerConfig cfg = { .port = 0, .h2 = &test_h2_cfg };
    ASSERT_EQ(kl_http_server_init(&g_pk_srv, &cfg), 0);
    KlPlatThread t;
    kl_plat_thread_create(&t, pk_server_thread, NULL);
    for (int i = 0; i < 200 && kl_http_server_bound_port(&g_pk_srv) == 0; i++) kl_test_sleep_ms(10);

    int fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)kl_http_server_bound_port(&g_pk_srv));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    int ok = fd >= 0 && connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0;
    static const char preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    static const char settings[9] = { 0, 0, 0, 4, 0, 0, 0, 0, 0 };   /* empty SETTINGS frame */
    if (ok) {
        (void)kl_test_sockwrite(fd, preface, 24);
        (void)kl_test_sockwrite(fd, settings, sizeof settings);
    }
    for (int i = 0; ok && i < 200 && T_LOAD(g_pk_mock.recv_count) == 0; i++) kl_test_sleep_ms(10);
    if (fd >= 0) kl_test_closesock(fd);
    kl_http_server_stop(&g_pk_srv);
    kl_plat_thread_join(&t);
    size_t got = g_pk_mock.first_recv_len;         /* the session's state, read after the join */
    int whole = got >= 24 && memcmp(g_pk_mock.first_recv, preface, 24) == 0;
    kl_http_server_free(&g_pk_srv);
    test_teardown();
    ASSERT_TRUE(ok);
    ASSERT_GT(got, (size_t)0);
    ASSERT_TRUE(whole);                          /* was: the session got SETTINGS, magic stripped */
}

/* ── A done session lingers on the server's own loop (readiness or completion) ─────────────── */
/* The same linger, driven by the real loop of whichever backend this suite runs on: on a completion
 * loop (pollcomp, io_uring, IOCP) the output goes through the connection's queue, the half-close
 * waits for it (comp_tlsq_then_shutwr), and a posted receive discards what the client sends. Two
 * connections, one after the other:
 *   A: the session finishes inside a read (its output fits the queue at once): every byte arrives,
 *      then the FIN; a frame the client sends after it is discarded, the connection stays; the
 *      client's FIN then closes it at once.
 *   B: the session finishes inside a send (its output is past the queue's producer bound, so it
 *      finishes from a send completion and the sweep ends it): every byte, then the FIN; the client
 *      never closes, and the linger's deadline (read_timeout_ms) closes it.
 *   C: an idle connection times out: its GOAWAY, then the FIN; a frame after it is discarded; the
 *      client's FIN closes it. */
static KlHttpServer g_lg_srv;
static MockH2Session g_lg_mock;
static void lg_server_thread(void *arg) { (void)arg; kl_http_server_run(&g_lg_srv); }

static int lg_connect(int port) {
    int fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) { kl_test_closesock(fd); return -1; }
    static const char preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    (void)kl_test_sockwrite(fd, preface, 24);
    kl_test_sleep_ms(50);                                  /* the upgrade first, then the request */
    (void)kl_test_sockwrite(fd, "DONE!", 5);
    return fd;
}

/* Read to the end of stream, checking the pattern. *eof: 1 clean end, -1 an error (a reset). */
static size_t lg_read_all(int fd, int *eof, int *bad) {
    size_t n = 0;
    *eof = 0;
    *bad = 0;
    char buf[16384];
    while (kl_test_poll1(fd, 0, 3000) > 0) {
        long r = kl_test_sockread(fd, buf, sizeof buf);
        if (r == 0) { *eof = 1; break; }
        if (r < 0) { *eof = -1; break; }
        for (long i = 0; i < r; i++)
            if ((unsigned char)buf[i] != live_pattern(n + (size_t)i)) *bad = 1;
        n += (size_t)r;
    }
    return n;
}

/* Milliseconds until `want` sessions were destroyed (their connections released), or -1. */
static long lg_wait_destroys(int want, int max_ms) {
    uint64_t t0 = kl_monotonic_ms();
    while (kl_atomic_load_int(&g_mock_destroys) < want) {
        if (kl_monotonic_ms() - t0 > (uint64_t)max_ms) return -1;
        kl_test_sleep_ms(5);
    }
    return (long)(kl_monotonic_ms() - t0);
}

UTEST(h2, a_done_session_lingers_on_the_servers_loop) {
    test_setup();
    mock_init(&g_lg_mock);
    g_lg_mock.with_want_read = 1;
    g_lg_mock.want_read_return = 1;
    g_lg_mock.done_marker = "DONE!";
    g_lg_mock.live_out[0] = 256u * 1024u;                  /* within the completion queue's bound */
    g_lg_mock.live_out[1] = 3u * 1024u * 1024u;            /* past it */
    g_lg_mock.goaway_len = 17;                             /* C's idle-timeout GOAWAY */
    g_mock_session = &g_lg_mock;
    kl_atomic_store_int(&g_mock_destroys, 0);
    KlHttpServerConfig cfg = { .port = 0, .bind_addr = "127.0.0.1", .h2 = &test_h2_cfg,
                               .read_timeout_ms = 1500, .max_connections = 4 };
    ASSERT_EQ(kl_http_server_init(&g_lg_srv, &cfg), 0);
    KlPlatThread t;
    kl_plat_thread_create(&t, lg_server_thread, NULL);
    for (int i = 0; i < 200 && g_lg_srv.bound_port == 0; i++) kl_test_sleep_ms(10);

    /* A: finished by a read, ended by the client's FIN. */
    int eof_a = 0, bad_a = 0;
    int fd = lg_connect(g_lg_srv.bound_port);
    size_t got_a = fd >= 0 ? lg_read_all(fd, &eof_a, &bad_a) : 0;
    if (fd >= 0) (void)kl_test_sockwrite(fd, "\0\0\x08\x06\0\0\0\0\0PINGPING", 17);   /* crosses it */
    kl_test_sleep_ms(200);
    int lingering_a = kl_atomic_load_int(&g_mock_destroys) == 0;
    if (fd >= 0) shutdown(fd, T_SHUT_WR);                  /* the client's FIN */
    long fin_close_ms = lg_wait_destroys(1, 3000);
    if (fd >= 0) kl_test_closesock(fd);

    /* B: finished by a send, never closed by the client: the deadline ends it. */
    int eof_b = 0, bad_b = 0;
    fd = lg_connect(g_lg_srv.bound_port);
    size_t got_b = fd >= 0 ? lg_read_all(fd, &eof_b, &bad_b) : 0;
    kl_test_sleep_ms(300);
    int lingering_b = kl_atomic_load_int(&g_mock_destroys) == 1;
    long deadline_close_ms = lg_wait_destroys(2, 5000);
    if (fd >= 0) kl_test_closesock(fd);

    /* C: idle past read_timeout_ms: the GOAWAY, the FIN, a crossing frame discarded, the client's FIN. */
    int eof_c = 0;
    size_t got_c = 0;
    int lingering_c = 0;
    long idle_fin_close_ms = -1;
    fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof addr);
        addr.sin_family = AF_INET;
        addr.sin_port = htons((uint16_t)g_lg_srv.bound_port);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (fd >= 0 && connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0) {
            (void)kl_test_sockwrite(fd, "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n", 24);
            char gb[64];
            while (kl_test_poll1(fd, 0, 4000) > 0) {       /* the idle timeout's GOAWAY, then EOF */
                long r = kl_test_sockread(fd, gb, sizeof gb);
                if (r == 0) { eof_c = 1; break; }
                if (r < 0) { eof_c = -1; break; }
                got_c += (size_t)r;
            }
            (void)kl_test_sockwrite(fd, "\0\0\x08\x06\0\0\0\0\0PINGPING", 17);
            kl_test_sleep_ms(200);
            lingering_c = kl_atomic_load_int(&g_mock_destroys) == 2;
            shutdown(fd, T_SHUT_WR);
            idle_fin_close_ms = lg_wait_destroys(3, 3000);
        }
        if (fd >= 0) kl_test_closesock(fd);
    }

    kl_http_server_stop(&g_lg_srv);
    kl_plat_thread_join(&t);
    kl_http_server_free(&g_lg_srv);
    test_teardown();
    ASSERT_EQ(got_a, (size_t)(256u * 1024u));              /* every byte */
    ASSERT_EQ(bad_a, 0);
    ASSERT_EQ(eof_a, 1);                                   /* then the FIN, not a reset */
    ASSERT_TRUE(lingering_a);                              /* was closed: the frame would reset it */
    ASSERT_TRUE(fin_close_ms >= 0 && fin_close_ms < 800);  /* the FIN, not the deadline, ended it */
    ASSERT_EQ(got_b, (size_t)(3u * 1024u * 1024u));
    ASSERT_EQ(bad_b, 0);
    ASSERT_EQ(eof_b, 1);
    ASSERT_TRUE(lingering_b);
    ASSERT_TRUE(deadline_close_ms >= 700);                 /* held to its deadline (1500 ms) ... */
    ASSERT_TRUE(deadline_close_ms >= 0 && deadline_close_ms < 4000);   /* ... and closed by it */
    ASSERT_EQ(got_c, (size_t)17);                          /* the GOAWAY */
    ASSERT_EQ(eof_c, 1);                                   /* then the FIN, not a reset */
    ASSERT_TRUE(lingering_c);                              /* was closed at once */
    ASSERT_TRUE(idle_fin_close_ms >= 0 && idle_fin_close_ms < 800);
}

/* The same over TLS. A clear-text stand-in engine: records pass through unchanged, and its
 * close_notify is the 4 bytes "<CN>", which its read reports as a clean end (at_eof) when the client
 * sends them. It does both transports: the socket on a readiness loop, the feed_input/drain_output
 * buffers on a completion loop. */
#define LTLS_CN "<CN>"
typedef struct {
    KlTls base;
    int comp, eof, cn_sent;
    char *in;  size_t in_len, in_cap;
    char *out; size_t out_len, out_cap;
} LiveTls;
static KlTlsResult livetls_handshake(KlTls *self, KlSocketHandle fd) { (void)self; (void)fd; return KL_TLS_OK; }
static const char *livetls_alpn(KlTls *self) { (void)self; return "h2"; }
static kl_ssize_t livetls_read(KlTls *self, KlSocketHandle fd, void *buf, size_t len) {
    LiveTls *t = (LiveTls *)self;
    if (t->eof) return -1;
    if (!t->comp) {
        long r = kl_test_sockread(fd, buf, len);
        if (r == 0) { t->eof = 1; return -1; }
        if (r < 0) return 0;                               /* would block (a readiness read) */
        if (r >= 4 && memcmp(buf, LTLS_CN, 4) == 0) { t->eof = 1; return -1; }
        return (kl_ssize_t)r;
    }
    if (t->in_len == 0) return 0;                          /* WANT_READ */
    if (t->in_len >= 4 && memcmp(t->in, LTLS_CN, 4) == 0) { t->eof = 1; return -1; }
    size_t n = t->in_len < len ? t->in_len : len;
    memcpy(buf, t->in, n);
    memmove(t->in, t->in + n, t->in_len - n);
    t->in_len -= n;
    return (kl_ssize_t)n;
}
static kl_ssize_t livetls_put(LiveTls *t, KlSocketHandle fd, const void *buf, size_t len) {
    if (!t->comp) {
        long w = kl_test_sockwrite(fd, buf, len);
        return w < 0 ? 0 : (kl_ssize_t)w;                  /* a full socket: retried */
    }
    if (len > t->out_cap - t->out_len) return 0;
    memcpy(t->out + t->out_len, buf, len);
    t->out_len += len;
    return (kl_ssize_t)len;
}
static kl_ssize_t livetls_write(KlTls *self, KlSocketHandle fd, const void *buf, size_t len) {
    return livetls_put((LiveTls *)self, fd, buf, len);
}
static int g_livetls_shutdowns;   /* every shutdown call (the server thread; read after join) */
static int g_livetls_refuse_cn;   /* readiness: the first close_notify meets a full socket */
static int g_livetls_refuse_all;  /* readiness: every close_notify is refused (a broken engine) */
static KlTlsResult livetls_shutdown(KlTls *self, KlSocketHandle fd) {
    LiveTls *t = (LiveTls *)self;
    kl_atomic_store_int(&g_livetls_shutdowns, kl_atomic_load_int(&g_livetls_shutdowns) + 1);
    if (!t->comp && kl_atomic_cas_int(&g_livetls_refuse_cn, 1, 0)) return KL_TLS_WANT_WRITE;
    if (!t->comp && kl_atomic_load_int(&g_livetls_refuse_all)) return KL_TLS_WANT_WRITE;
    if (!t->cn_sent && livetls_put(t, fd, LTLS_CN, 4) == 4) t->cn_sent = 1;   /* sent once */
    return KL_TLS_OK;
}
static size_t livetls_pending(KlTls *self) { (void)self; return 0; }
static void livetls_reset(KlTls *self) {
    LiveTls *t = (LiveTls *)self;
    t->comp = t->eof = t->cn_sent = 0;
    t->in_len = t->out_len = 0;
}
static void livetls_destroy(KlTls *self) {
    LiveTls *t = (LiveTls *)self;
    free(t->in);
    free(t->out);
    free(t);
}
static int livetls_feed(KlTls *self, const void *cipher, size_t len) {
    LiveTls *t = (LiveTls *)self;
    t->comp = 1;
    if (!cipher) return 0;
    if (len > t->in_cap - t->in_len) return -1;
    memcpy(t->in + t->in_len, cipher, len);
    t->in_len += len;
    return 0;
}
static kl_ssize_t livetls_drain(KlTls *self, void *buf, size_t cap) {
    LiveTls *t = (LiveTls *)self;
    size_t n = t->out_len < cap ? t->out_len : cap;
    memcpy(buf, t->out, n);
    memmove(t->out, t->out + n, t->out_len - n);
    t->out_len -= n;
    return (kl_ssize_t)n;
}
static int livetls_at_eof(KlTls *self) { return ((LiveTls *)self)->eof; }
static KlTls *livetls_factory(KlTlsCtx *ctx, KlAllocator *alloc) {
    (void)ctx; (void)alloc;
    LiveTls *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->in_cap = 1u << 20;
    t->out_cap = 1u << 20;
    t->in = malloc(t->in_cap);
    t->out = malloc(t->out_cap);
    if (!t->in || !t->out) { free(t->in); free(t->out); free(t); return NULL; }
    t->base.handshake = livetls_handshake;
    t->base.read = livetls_read;
    t->base.write = livetls_write;
    t->base.shutdown = livetls_shutdown;
    t->base.pending = livetls_pending;
    t->base.reset = livetls_reset;
    t->base.destroy = livetls_destroy;
    t->base.alpn_protocol = livetls_alpn;
    t->base.feed_input = livetls_feed;
    t->base.drain_output = livetls_drain;
    t->base.at_eof = livetls_at_eof;
    return &t->base;
}

/* Read to the end of stream: `want` pattern bytes, then the engine's close_notify. */
static int lg_read_tls(int fd, size_t want, size_t *got, int *eof) {
    size_t n = 0;
    char tail[8];
    size_t tl = 0;
    int bad = 0;
    *eof = 0;
    char buf[16384];
    while (kl_test_poll1(fd, 0, 3000) > 0) {
        long r = kl_test_sockread(fd, buf, sizeof buf);
        if (r == 0) { *eof = 1; break; }
        if (r < 0) { *eof = -1; break; }
        for (long i = 0; i < r; i++, n++) {
            if (n < want) { if ((unsigned char)buf[i] != live_pattern(n)) bad = 1; }
            else if (tl < sizeof tail) tail[tl++] = buf[i];
        }
    }
    *got = n < want ? n : want;
    return !bad && tl == 4 && memcmp(tail, LTLS_CN, 4) == 0;   /* then close_notify, alone */
}

/* Over TLS on the server's own loop: the output, then close_notify, then the FIN. And on a completion
 * loop, a client close_notify that arrives while the end of the answer is still queued (the client
 * stopped reading for a moment) must not cancel that output: it is the client's FIN in TLS, and the
 * connection closes once what is queued has gone, as for a FIN. (A readiness loop's linger reads
 * raw bytes past its half-close, so it never decodes the client's close_notify.) */
static KlHttpServer g_lt_srv;
static void lt_server_thread(void *arg) { (void)arg; kl_http_server_run(&g_lt_srv); }
UTEST(h2, a_done_tls_session_lingers_on_the_servers_loop) {
    test_setup();
    mock_init(&g_lg_mock);
    g_lg_mock.with_want_read = 1;
    g_lg_mock.want_read_return = 1;
    g_lg_mock.done_marker = "DONE!";
    g_lg_mock.live_out[0] = 256u * 1024u;
    g_lg_mock.live_out[1] = 900u * 1024u;                  /* more than the sockets hold, within
                                                             * the completion queue's bound */
    g_mock_session = &g_lg_mock;
    kl_atomic_store_int(&g_mock_destroys, 0);
    g_livetls_shutdowns = 0;
    static int dummy_ctx;
    KlTlsConfig tls = { .ctx = (KlTlsCtx *)&dummy_ctx, .factory = livetls_factory };
    KlHttpServerConfig cfg = { .port = 0, .bind_addr = "127.0.0.1", .h2 = &test_h2_cfg, .tls = &tls,
                               .read_timeout_ms = 1500, .max_connections = 2 };
    ASSERT_EQ(kl_http_server_init(&g_lt_srv, &cfg), 0);
    int comp = (kl_event_caps(&g_lt_srv.ev.loop) & KL_EVENT_CAP_COMPLETION) != 0;
    KlPlatThread t;
    kl_plat_thread_create(&t, lt_server_thread, NULL);
    for (int i = 0; i < 200 && g_lt_srv.bound_port == 0; i++) kl_test_sleep_ms(10);

    /* A: the output, close_notify, the FIN. */
    size_t got_a = 0;
    int eof_a = 0;
    int fd = lg_connect(g_lt_srv.bound_port);
    int whole_a = fd >= 0 && lg_read_tls(fd, 256u * 1024u, &got_a, &eof_a);
    if (fd >= 0) kl_test_closesock(fd);
    long closed_a = lg_wait_destroys(1, 3000);

    /* B (completion): the client's close_notify crosses the queued end of the answer. */
    size_t got_b = 0;
    int eof_b = 0, whole_b = 1;
    long closed_b = 0;
    if (comp) {
        fd = lg_connect(g_lt_srv.bound_port);
        kl_test_sleep_ms(300);                             /* done; the end still queued */
        if (fd >= 0) (void)kl_test_sockwrite(fd, LTLS_CN, 4);
        kl_test_sleep_ms(100);
        whole_b = fd >= 0 && lg_read_tls(fd, 900u * 1024u, &got_b, &eof_b);
        if (fd >= 0) kl_test_closesock(fd);
        closed_b = lg_wait_destroys(2, 3000);
    }

    kl_http_server_stop(&g_lt_srv);
    kl_plat_thread_join(&t);
    kl_http_server_free(&g_lt_srv);
    test_teardown();
    ASSERT_EQ(got_a, (size_t)(256u * 1024u));
    ASSERT_TRUE(whole_a);                                  /* every byte, then close_notify */
    ASSERT_EQ(eof_a, 1);                                   /* then the FIN */
    ASSERT_TRUE(closed_a >= 0);
    ASSERT_EQ(g_livetls_shutdowns, comp ? 2 : 1);          /* once per connection, not again at release */
    if (comp) {
        ASSERT_EQ(got_b, (size_t)(900u * 1024u));          /* was cut off by the client's close_notify */
        ASSERT_TRUE(whole_b);
        ASSERT_EQ(eof_b, 1);
        ASSERT_TRUE(closed_b >= 0);
    }
}

/* The same transport end for HTTP/1.1 (the TLS stand-in above; no HTTP/2 involved): a request
 * rejected before its body (413, Content-Length past max_body_size) is answered, then close_notify,
 * then the FIN, while the body still arriving is discarded; on both models. On a readiness loop the
 * first close_notify can meet a full socket (WANT_WRITE): the half-close then waits for it, and it
 * goes out on writability. It used to half-close first, so close_notify never reached the wire. */
static KlHttpServer g_h1t_srv;
static void h1t_server_thread(void *arg) { (void)arg; kl_http_server_run(&g_h1t_srv); }
static void h1t_handler(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)req; (void)ud;
    kl_http_response_body_copy(res, "unreached", 9);
}

/* One rejected upload; 1 if the 413, then close_notify alone, then a clean end of stream arrived. */
static int h1t_reject_once(int port) {
    int fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    int ok = 0;
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0) {
        static const char head[] = "POST /up HTTP/1.1\r\nHost: x\r\nContent-Length: 100000\r\n\r\n";
        char body[4096];
        memset(body, 'b', sizeof body);
        (void)kl_test_sockwrite(fd, head, sizeof head - 1);
        (void)kl_test_sockwrite(fd, body, sizeof body);  /* the body the drain discards */
        char got[4096];
        size_t n = 0;
        int eof = 0;
        while (n < sizeof got && kl_test_poll1(fd, 0, 3000) > 0) {
            long rr = kl_test_sockread(fd, got + n, sizeof got - n);
            if (rr == 0) { eof = 1; break; }
            if (rr < 0) { eof = -1; break; }
            n += (size_t)rr;
        }
        ok = eof == 1 && n > 16 && memcmp(got, "HTTP/1.1 413", 12) == 0 &&
             memcmp(got + n - 4, LTLS_CN, 4) == 0;
    }
    kl_test_closesock(fd);
    return ok;
}

UTEST(h1_tls_drain, a_rejected_request_gets_its_answer_then_close_notify_then_the_fin) {
    test_setup();
    static int dummy_ctx;
    KlTlsConfig tls = { .ctx = (KlTlsCtx *)&dummy_ctx, .factory = livetls_factory };
    KlHttpServerConfig cfg = { .port = 0, .bind_addr = "127.0.0.1", .tls = &tls,
                               .max_body_size = 1024, .read_timeout_ms = 1500,
                               .max_connections = 2 };
    ASSERT_EQ(kl_http_server_init(&g_h1t_srv, &cfg), 0);
    int comp = (kl_event_caps(&g_h1t_srv.ev.loop) & KL_EVENT_CAP_COMPLETION) != 0;
    kl_http_server_route(&g_h1t_srv, "POST", "/up", h1t_handler, NULL, NULL);
    g_livetls_shutdowns = 0;
    kl_atomic_store_int(&g_livetls_refuse_cn, 0);
    KlPlatThread t;
    kl_plat_thread_create(&t, h1t_server_thread, NULL);
    for (int i = 0; i < 200 && g_h1t_srv.bound_port == 0; i++) kl_test_sleep_ms(10);

    int first = h1t_reject_once(g_h1t_srv.bound_port);
    kl_atomic_store_int(&g_livetls_refuse_cn, !comp);   /* readiness: the next one is refused once */
    kl_test_sleep_ms(50);
    int refused = h1t_reject_once(g_h1t_srv.bound_port);

    kl_http_server_stop(&g_h1t_srv);
    kl_plat_thread_join(&t);
    kl_http_server_free(&g_h1t_srv);
    test_teardown();
    ASSERT_TRUE(first);                                    /* was: the 413, then a FIN alone */
    ASSERT_TRUE(refused);                                  /* the half-close waited for it */
}

/* An answer given after an async resume, with input still unread (a pipelined request read with
 * this one: the connection is not reused and ends through the drain), also ends with close_notify
 * then the FIN; and when a full socket refuses that close_notify (WANT_WRITE), the
 * resumed connection must be watched for writability, so the alert and the FIN follow at once
 * rather than at the drain's deadline (here 3 s). The same for the sweep's 408 on a head that
 * stalled: the drain it starts must be watched for writability too. */
static KlAsyncOp g_ar_op;
static void ar_resume(KlAsyncOp *op, void *ud) {
    (void)ud;
    KlHttpResponse *res = kl_http_conn_response(op->conn);
    kl_http_response_status(res, 200);
    kl_http_response_body_copy(res, "late", 4);
}
static void ar_cancel(KlAsyncOp *op, void *ud) { (void)op; (void)ud; }
static void ar_timer(void *ud) { (void)ud; kl_async_complete(&g_h1t_srv, &g_ar_op); }
static void ar_handler(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)res; (void)ud;
    memset(&g_ar_op, 0, sizeof g_ar_op);
    g_ar_op.on_resume = ar_resume;
    g_ar_op.on_cancel = ar_cancel;
    if (kl_async_suspend(&g_h1t_srv, kl_http_request_conn(req), &g_ar_op) == 0)
        (void)kl_timer_add(kl_http_server_event_ctx(&g_h1t_srv), 50, ar_timer, NULL);
}

UTEST(h1_tls_drain, an_async_answer_with_a_refused_close_notify_ends_when_writable) {
    test_setup();
    static int dummy_ctx;
    KlTlsConfig tls = { .ctx = (KlTlsCtx *)&dummy_ctx, .factory = livetls_factory };
    KlHttpServerConfig cfg = { .port = 0, .bind_addr = "127.0.0.1", .tls = &tls,
                               .read_timeout_ms = 5000, .reject_drain_timeout_ms = 3000,
                               .header_timeout_ms = 300, .max_connections = 2 };
    ASSERT_EQ(kl_http_server_init(&g_h1t_srv, &cfg), 0);
    int comp = (kl_event_caps(&g_h1t_srv.ev.loop) & KL_EVENT_CAP_COMPLETION) != 0;
    kl_http_server_route(&g_h1t_srv, "POST", "/as", ar_handler, NULL, NULL);
    kl_atomic_store_int(&g_livetls_refuse_cn, !comp);     /* readiness: refused once */
    KlPlatThread t;
    kl_plat_thread_create(&t, h1t_server_thread, NULL);
    for (int i = 0; i < 200 && g_h1t_srv.bound_port == 0; i++) kl_test_sleep_ms(10);

    int fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)g_h1t_srv.bound_port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    int ok = 0, refused_used = 0, released = 0;
    long took = -1;
    if (fd >= 0 && connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0) {
        /* The whole request and a pipelined one in one write: the server reads both at once, so
         * nothing more becomes readable to rescue a drain that is not watched for writability. */
        static const char req2[] = "POST /as HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\n\r\nbbbb"
                                   "GET /next HTTP/1.1\r\nHost: x\r\n\r\n";
        uint64_t t0 = kl_monotonic_ms();
        (void)kl_test_sockwrite(fd, req2, sizeof req2 - 1);
        char got[2048];
        size_t n = 0;
        int eof = 0;
        while (n < sizeof got && kl_test_poll1(fd, 0, 5000) > 0) {
            long rr = kl_test_sockread(fd, got + n, sizeof got - n);
            if (rr == 0) { eof = 1; break; }
            if (rr < 0) { eof = -1; break; }
            n += (size_t)rr;
        }
        took = (long)(kl_monotonic_ms() - t0);
        ok = eof == 1 && n > 16 && memcmp(got, "HTTP/1.1 200", 12) == 0 &&
             memcmp(got + n - 4, LTLS_CN, 4) == 0;
        refused_used = kl_atomic_load_int(&g_livetls_refuse_cn) == 0;
        /* Readiness: its input was complete, so once the alert is out nothing is left, and the
         * server closes its end itself (the sweep's drain step, the same iteration), without waiting
         * for this client to close or for the drain's 3 s. (A completion drain reads only what a
         * posted receive brings, so it waits for the client or the deadline, as before.) */
        for (int i = 0; !comp && i < 100 && !released; i++) {
            KlHttpServerStats st;
            kl_http_server_stats(&g_h1t_srv, &st);
            released = st.active_connections == 0;
            if (!released) kl_test_sleep_ms(10);
        }
    }
    if (fd >= 0) kl_test_closesock(fd);

    /* The sweep's 408 on a stalled head. */
    kl_atomic_store_int(&g_livetls_refuse_cn, !comp);
    int ok408 = 0, refused408 = 0;
    long took408 = -1;
    fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0 && connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0) {
        uint64_t t0 = kl_monotonic_ms();
        (void)kl_test_sockwrite(fd, "POST /as HTTP/1.1\r\nHo", 21);   /* and nothing more */
        char got[2048];
        size_t n = 0;
        int eof = 0;
        while (n < sizeof got && kl_test_poll1(fd, 0, 6000) > 0) {
            long rr = kl_test_sockread(fd, got + n, sizeof got - n);
            if (rr == 0) { eof = 1; break; }
            if (rr < 0) { eof = -1; break; }
            n += (size_t)rr;
        }
        took408 = (long)(kl_monotonic_ms() - t0);
        ok408 = eof == 1 && n > 16 && memcmp(got, "HTTP/1.1 408", 12) == 0 &&
                memcmp(got + n - 4, LTLS_CN, 4) == 0;
        refused408 = kl_atomic_load_int(&g_livetls_refuse_cn) == 0;
    }
    if (fd >= 0) kl_test_closesock(fd);

    /* The same 408 with an engine that never takes the alert: the drain gives up on it after a
     * bounded number of writable events and sends the FIN, rather than spinning to the deadline. */
    long took_all = -1;
    int fin_all = 0, calls_all = -1;
    if (!comp) {
        kl_atomic_store_int(&g_livetls_refuse_all, 1);
        int before = kl_atomic_load_int(&g_livetls_shutdowns);
        fd = (int)socket(AF_INET, SOCK_STREAM, 0);
        if (fd >= 0 && connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0) {
            uint64_t t0 = kl_monotonic_ms();
            (void)kl_test_sockwrite(fd, "POST /as HTTP/1.1\r\nHo", 21);
            char got[2048];
            size_t n = 0;
            while (n < sizeof got && kl_test_poll1(fd, 0, 6000) > 0) {
                long rr = kl_test_sockread(fd, got + n, sizeof got - n);
                if (rr <= 0) { fin_all = rr == 0; break; }
                n += (size_t)rr;
            }
            took_all = (long)(kl_monotonic_ms() - t0);
        }
        if (fd >= 0) kl_test_closesock(fd);
        kl_test_sleep_ms(100);
        kl_atomic_store_int(&g_livetls_refuse_all, 0);
        calls_all = kl_atomic_load_int(&g_livetls_shutdowns) - before;
    }
    kl_http_server_stop(&g_h1t_srv);
    kl_plat_thread_join(&t);
    kl_http_server_free(&g_h1t_srv);
    test_teardown();
    ASSERT_TRUE(ok);                                       /* the answer, close_notify, the FIN */
    ASSERT_TRUE(refused_used);
    ASSERT_TRUE(took >= 0 && took < 1500);                 /* was ~3 s: WRITE was not watched */
    if (!comp) ASSERT_TRUE(released);                      /* within 1 s, the client still open */
    ASSERT_TRUE(ok408);
    ASSERT_TRUE(refused408);
    /* The header deadline (300 ms) plus up to one sweep tick (~1 s), not the drain's 3 s. */
    ASSERT_TRUE(took408 >= 0 && took408 < 2600);
    if (!comp) {
        ASSERT_TRUE(fin_all);                              /* the FIN came, without the alert */
        ASSERT_TRUE(took_all >= 0 && took_all < 2600);     /* not the drain's 3 s deadline */
        ASSERT_TRUE(calls_all >= 1 && calls_all <= 12);    /* was millions (a spin) */
    }
}

/* ── A session whose flush reports the stream it just finished as closed ───────────────────── */
/* A real session (nghttp2) closes a stream inside its send once the response's END_STREAM goes out
 * on a stream the client has already half-closed, and reports that close (on_stream_reset) from
 * inside the flush. Keel flushed while it still held the stream and destroyed it afterwards, so the
 * close destroyed it first and Keel then destroyed the same slot again: num_streams went to -1, and
 * the next stream was created at streams[-1], before the table. Stream 1 of an h2c upgrade is
 * answered outside recv, where the nghttp2 adapter really does send on flush. */

/* A GET on stream 3 after the first exchange: answered with 200, and leaves no stream behind.
 * Only run when the table is sane (num_streams 0): on a corrupt table it would write before it. */
static void second_get(MockH2Session *mock, int *status, uint32_t *sid, int *streams_after,
                       const KlHttpConn *conn) {
    *status = -1; *sid = 0; *streams_after = -99;
    if (conn->h2->num_streams != 0) return;
    mock->last_status = 0;
    if (mock->callbacks.on_request(mock->cb_user_data, 3, "GET", 3, "/test", 5,
                                   NULL, 0, NULL, NULL, NULL, NULL, 0) != 0) return;
    if (mock->callbacks.on_stream_end(mock->cb_user_data, 3) != 0) return;
    *status = mock->last_status;
    *sid = mock->last_stream_id;
    *streams_after = conn->h2->num_streams;
}

UTEST(h2, upgrade_stream_1_closed_by_its_own_flush_is_destroyed_once) {
    test_setup();
    kl_http_router_add(&test_router, "GET", "/test", test_handler, NULL, NULL);
    static MockH2Session mock;
    mock_init(&mock); mock.with_upgrade = 1; mock.want_write_return = 1; mock.close_on_flush = 1;
    g_mock_session = &mock;
    int pfd[2]; ASSERT_EQ(kl_test_socketpair(pfd), 0);
    static KlHttpConn conn; memset(&conn, 0, sizeof(conn));
    conn.stream.fd = pfd[1]; conn.stream.alloc = &test_alloc;
    h2c_req(&conn, "AAMAAABk", 0);

    int r = kl_http2_server_upgrade_from_h1(&conn, &test_router, &test_h2_cfg, NULL, 0);
    int streams = conn.h2 ? conn.h2->num_streams : -99;
    int status1 = mock.last_status, closes = mock.closes_reported;
    int status3 = -1, streams3 = -99; uint32_t sid3 = 0;
    if (conn.h2) second_get(&mock, &status3, &sid3, &streams3, &conn);
    if (conn.h2 && conn.h2->num_streams < 0) conn.h2->num_streams = 0;   /* let cleanup run */
    kl_http2_server_cleanup(&conn);
    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();
    ASSERT_EQ(r, (int)KL_HTTP_CONN_HTTP2);
    ASSERT_EQ(status1, 200);
    ASSERT_GE(closes, 1);                         /* the flush did report stream 1 closed */
    ASSERT_EQ(streams, 0);                        /* was -1: destroyed twice */
    ASSERT_EQ(status3, 200);                      /* the next request is served */
    ASSERT_EQ(sid3, 3u);
    ASSERT_EQ(streams3, 0);
}

/* The same order on the ordinary answer path (a stream answered after END_STREAM), when the
 * session's flush is not deferred. */
UTEST(h2, answered_stream_closed_by_the_flush_is_destroyed_once) {
    test_setup();
    kl_http_router_add(&test_router, "GET", "/test", test_handler, NULL, NULL);
    static MockH2Session mock;
    mock_init(&mock); mock.want_write_return = 1; mock.close_on_flush = 1;
    g_mock_session = &mock;
    int pfd[2]; ASSERT_EQ(kl_test_socketpair(pfd), 0);
    static KlHttpConn conn; memset(&conn, 0, sizeof(conn));
    conn.stream.fd = pfd[1]; conn.stream.alloc = &test_alloc;
    kl_http2_server_upgrade(&conn, &test_router, &test_h2_cfg, NULL, 0);

    int ro = mock.callbacks.on_request(mock.cb_user_data, 1, "GET", 3, "/test", 5,
                                       NULL, 0, NULL, NULL, NULL, NULL, 0);
    int re = mock.callbacks.on_stream_end(mock.cb_user_data, 1);
    int streams = conn.h2->num_streams;
    int status3 = -1, streams3 = -99; uint32_t sid3 = 0;
    second_get(&mock, &status3, &sid3, &streams3, &conn);
    if (conn.h2->num_streams < 0) conn.h2->num_streams = 0;
    kl_http2_server_cleanup(&conn);
    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();
    ASSERT_EQ(ro, 0);
    ASSERT_EQ(re, 0);
    ASSERT_EQ(streams, 0);                        /* was -1 */
    ASSERT_EQ(status3, 200);
    ASSERT_EQ(streams3, 0);
}

/* The early answers: a 413 for an over-limit body, and a pre-body middleware short-circuit. */
UTEST(h2, early_answers_closed_by_the_flush_are_destroyed_once) {
    test_setup();
    kl_http_router_add(&test_router, "POST", "/data", test_handler, NULL, test_br_factory);
    kl_http_router_add(&test_router, "GET", "/test", test_handler, NULL, NULL);
    static MockH2Session mock;
    mock_init(&mock); mock.want_write_return = 1; mock.close_on_flush = 1;
    g_mock_session = &mock;
    int pfd[2]; ASSERT_EQ(kl_test_socketpair(pfd), 0);
    static KlHttpConn conn; memset(&conn, 0, sizeof(conn));
    conn.stream.fd = pfd[1]; conn.stream.alloc = &test_alloc;
    conn.max_body_size = 4;
    kl_http2_server_upgrade(&conn, &test_router, &test_h2_cfg, NULL, 0);

    /* 413 on stream 1 */
    mock.callbacks.on_request(mock.cb_user_data, 1, "POST", 4, "/data", 5,
                              NULL, 0, NULL, NULL, NULL, NULL, 0);
    int rd = mock.callbacks.on_data(mock.cb_user_data, 1, "0123456789", 10);
    int status413 = mock.last_status;
    int streams413 = conn.h2->num_streams;
    if (conn.h2->num_streams < 0) conn.h2->num_streams = 0;

    /* 403 from pre-body middleware on stream 5 */
    kl_http_router_use(&test_router, "*", "/*", test_middleware, NULL);
    middleware_return = 1;
    int rm = mock.callbacks.on_request(mock.cb_user_data, 5, "GET", 3, "/test", 5,
                                       NULL, 0, NULL, NULL, NULL, NULL, 0);
    int status403 = mock.last_status;
    int streams403 = conn.h2->num_streams;
    if (conn.h2->num_streams < 0) conn.h2->num_streams = 0;

    kl_http2_server_cleanup(&conn);
    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();
    ASSERT_EQ(rd, 0);
    ASSERT_EQ(status413, 413);
    ASSERT_EQ(streams413, 0);                     /* was -1 */
    ASSERT_EQ(rm, 0);
    ASSERT_EQ(status403, 403);
    ASSERT_EQ(streams403, 0);                     /* was -1 */
}

/* ═══════════════════════════════════════════════════════════════════
 * Streamed responses: answered early, body-reader lifecycle, abort
 * ═══════════════════════════════════════════════════════════════════ */

static KlHttpResponse *g_sres[8];
static int g_nsres;
static int g_sclose_calls[8], g_sclose_completed[8];
static void sclose(void *ctx, int completed) {
    int i = (int)(intptr_t)ctx;
    g_sclose_calls[i]++;
    g_sclose_completed[i] = completed;
}

/* Begin a stream on res, write "hi" into it, record it; `end` ends the body too. */
static int begin_recorded(KlHttpResponse *res, int end) {
    KlHttpResponseWriteFn w; void *wc;
    if (kl_http_response_begin_stream(res, 200, &w, &wc) < 0) return -1;
    int i = g_nsres++;
    g_sres[i] = res;
    g_sclose_calls[i] = 0;
    g_sclose_completed[i] = -1;
    kl_http_response_stream_on_close(res, sclose, (void *)(intptr_t)i);
    if (kl_http_response_stream_try_write(res, "hi", 2) != KL_HTTP_STREAM_ACCEPTED) return -1;
    if (end && kl_http_response_end_stream(res) < 0) return -1;
    return 0;
}

static int mw_streams_and_stops(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)req; (void)ud;
    middleware_called++;
    return begin_recorded(res, 1) == 0 ? 1 : 0;
}

static int mw_streams_and_continues(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)req; (void)ud;
    middleware_called++;
    (void)begin_recorded(res, 0);
    return 0;
}

static void streaming_handler(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)req; (void)ud;
    handler_called++;
    (void)begin_recorded(res, 0);
}

typedef struct { int pfd[2]; KlHttpConn conn; MockH2Session mock; } StreamRig;

static void rig_open(StreamRig *r) {
    test_setup();
    g_nsres = 0;
    mock_init(&r->mock);
    r->mock.with_stream = 1;
    g_mock_session = &r->mock;
    kl_test_socketpair(r->pfd);
    memset(&r->conn, 0, sizeof(r->conn));
    r->conn.stream.fd = r->pfd[1];
    r->conn.stream.alloc = &test_alloc;
}

static void rig_close(StreamRig *r) {
    kl_http2_server_cleanup(&r->conn);
    kl_test_closesock(r->pfd[0]);
    kl_test_closesock(r->pfd[1]);
    test_teardown();
}

static kl_ssize_t rig_pull(StreamRig *r, uint32_t id, char *buf, size_t cap, int *eof) {
    return r->mock.callbacks.read_body(r->mock.cb_user_data, id, buf, cap, eof);
}

/* Pre-body middleware that begins a stream and short-circuits answers the request: the stream
 * finishes exactly once, and neither the handler nor the 404 fallback runs at the request's end
 * (they used to, and the 404's buffered body replaced the open stream, which then never ended). */
static void middleware_stream_case(int *utest_result, const char *path, size_t path_len) {
    StreamRig r;
    rig_open(&r);
    kl_http_router_add(&test_router, "POST", "/s", test_handler, NULL, NULL);
    kl_http_router_use(&test_router, "*", "/*", mw_streams_and_stops, NULL);
    kl_http2_server_upgrade(&r.conn, &test_router, &test_h2_cfg, NULL, 0);
    r.conn.max_body_size = 2;            /* the body below is over it: ignored, not refused */

    ASSERT_EQ(0, r.mock.callbacks.on_request(r.mock.cb_user_data, 1, "POST", 4, path, path_len,
                                             NULL, 0, NULL, NULL, NULL, NULL, 0));
    ASSERT_EQ(1, middleware_called);
    ASSERT_EQ(1, r.mock.stream_submit_count);
    ASSERT_EQ(1, r.conn.h2->num_streams);
    ASSERT_EQ(0, r.mock.callbacks.on_data(r.mock.cb_user_data, 1, "body", 4));   /* ignored */
    ASSERT_EQ(0, r.mock.reset_count);
    ASSERT_EQ(0, r.mock.callbacks.on_stream_end(r.mock.cb_user_data, 1));
    ASSERT_EQ(0, handler_called);
    ASSERT_EQ(0, r.mock.submit_count);              /* no 404, no buffered answer */
    ASSERT_EQ(0, r.mock.reset_count);

    char buf[16];
    int eof = 0;
    ASSERT_EQ((kl_ssize_t)2, rig_pull(&r, 1, buf, sizeof(buf), &eof));
    ASSERT_EQ(0, memcmp(buf, "hi", 2));
    ASSERT_EQ(1, eof);                              /* the body ended: END_STREAM goes out */
    r.mock.callbacks.on_stream_reset(r.mock.cb_user_data, 1, 0);   /* the session closes it */
    ASSERT_EQ(1, g_sclose_calls[0]);
    ASSERT_EQ(1, g_sclose_completed[0]);
    ASSERT_EQ(0, r.conn.h2->num_streams);
    rig_close(&r);
}

UTEST(h2, a_stream_begun_by_pre_body_middleware_finishes_without_the_handler) {
    middleware_stream_case(utest_result, "/s", 2);
}

UTEST(h2, a_stream_begun_by_pre_body_middleware_finishes_without_the_404) {
    middleware_stream_case(utest_result, "/none", 5);
}

/* A request body that completed gets on_complete once, and never on_error: not when its streamed
 * response closes normally, and not when the producer aborts it. */
UTEST(h2, a_completed_body_gets_no_on_error_when_its_stream_closes_or_aborts) {
    StreamRig r;
    rig_open(&r);
    kl_http_router_add(&test_router, "POST", "/up", streaming_handler, NULL, test_br_factory);
    kl_http2_server_upgrade(&r.conn, &test_router, &test_h2_cfg, NULL, 0);
    const char *hn[] = {"content-length"};
    const char *hv[] = {"3"};
    size_t hnl[] = {14}, hvl[] = {1};

    for (int round = 0; round < 2; round++) {         /* 0: a normal close, 1: an abort */
        uint32_t id = round == 0 ? 1u : 3u;
        ASSERT_EQ(0, r.mock.callbacks.on_request(r.mock.cb_user_data, id, "POST", 4, "/up", 3,
                                                 NULL, 0, hn, hv, hnl, hvl, 1));
        ASSERT_EQ(0, r.mock.callbacks.on_data(r.mock.cb_user_data, id, "abc", 3));
        ASSERT_EQ(0, r.mock.callbacks.on_stream_end(r.mock.cb_user_data, id));
        ASSERT_EQ(round + 1, handler_called);
        ASSERT_EQ(1, g_test_br.complete_count);
        ASSERT_EQ(0, g_test_br.error_count);
        if (round == 0) {
            ASSERT_EQ(0, kl_http_response_end_stream(g_sres[0]));
            char buf[8];
            int eof = 0;
            ASSERT_EQ((kl_ssize_t)2, rig_pull(&r, id, buf, sizeof(buf), &eof));
            ASSERT_EQ(1, eof);
            r.mock.callbacks.on_stream_reset(r.mock.cb_user_data, id, 0);
            ASSERT_EQ(1, g_sclose_completed[0]);
        } else {
            ASSERT_EQ(0, kl_http_response_stream_abort(g_sres[1]));
            ASSERT_EQ(0, kl_http2_server_on_writable(&r.conn) == KL_HTTP_CONN_CLOSED);
            ASSERT_EQ(1, r.mock.reset_count);
            ASSERT_EQ(8u, r.mock.last_reset_code);   /* CANCEL */
            r.mock.callbacks.on_stream_reset(r.mock.cb_user_data, id, 8);
            ASSERT_EQ(0, g_sclose_completed[1]);
        }
        ASSERT_EQ(1, g_test_br.complete_count);
        ASSERT_EQ(0, g_test_br.error_count);          /* the close is not the body's error */
        ASSERT_EQ(1, g_test_br.destroy_count);
    }
    rig_close(&r);
}

/* Abort needs no writable edge: the buffer is dropped at once and the next flush resets only that
 * stream (CANCEL); its producer hears completed=0 once, from the session's close, never inside the
 * call. Its sibling keeps its data and stays open. */
UTEST(h2, abort_resets_only_its_stream_without_a_writable_edge) {
    StreamRig r;
    rig_open(&r);
    kl_http_router_add(&test_router, "GET", "/s", streaming_handler, NULL, NULL);
    kl_http2_server_upgrade(&r.conn, &test_router, &test_h2_cfg, NULL, 0);
    for (uint32_t id = 1; id <= 3; id += 2) {
        ASSERT_EQ(0, r.mock.callbacks.on_request(r.mock.cb_user_data, id, "GET", 3, "/s", 2,
                                                 NULL, 0, NULL, NULL, NULL, NULL, 0));
        ASSERT_EQ(0, r.mock.callbacks.on_stream_end(r.mock.cb_user_data, id));
    }
    ASSERT_EQ(2, g_nsres);
    ASSERT_EQ((size_t)2, kl_http_response_stream_pending(g_sres[0]));

    ASSERT_EQ(0, kl_http_response_stream_abort(g_sres[0]));
    ASSERT_EQ((size_t)0, kl_http_response_stream_pending(g_sres[0]));
    ASSERT_EQ(0, g_sclose_calls[0]);
    ASSERT_EQ(-1, kl_http_response_stream_abort(g_sres[0]));
    ASSERT_EQ(KL_HTTP_STREAM_ERROR, kl_http_response_stream_try_write(g_sres[0], "x", 1));
    ASSERT_EQ(-1, kl_http_response_end_stream(g_sres[0]));

    char buf[16];
    int eof = 0;
    ASSERT_TRUE(rig_pull(&r, 1, buf, sizeof(buf), &eof) < 0);   /* nothing more to pull */
    ASSERT_NE(KL_HTTP_CONN_CLOSED, kl_http2_server_on_writable(&r.conn));
    ASSERT_EQ(1, r.mock.reset_count);
    ASSERT_EQ(1u, r.mock.last_reset_id);
    ASSERT_EQ(8u, r.mock.last_reset_code);
    ASSERT_NE(KL_HTTP_CONN_CLOSED, kl_http2_server_on_writable(&r.conn));
    ASSERT_EQ(1, r.mock.reset_count);                /* reset once */

    r.mock.callbacks.on_stream_reset(r.mock.cb_user_data, 1, 8);
    ASSERT_EQ(1, g_sclose_calls[0]);
    ASSERT_EQ(0, g_sclose_completed[0]);
    ASSERT_EQ(0, g_sclose_calls[1]);
    ASSERT_EQ(1, r.conn.h2->num_streams);
    eof = 0;
    ASSERT_EQ((kl_ssize_t)2, rig_pull(&r, 3, buf, sizeof(buf), &eof));   /* the sibling's data */
    ASSERT_EQ(0, eof);
    ASSERT_EQ(KL_HTTP_STREAM_ACCEPTED, kl_http_response_stream_try_write(g_sres[1], "more", 4));
    rig_close(&r);
    ASSERT_EQ(1, g_sclose_calls[1]);                 /* cut short by the connection's close */
    ASSERT_EQ(0, g_sclose_completed[1]);
    ASSERT_EQ(1, g_sclose_calls[0]);
}

/* A body refused after the response began streaming (over the limit): a 413 can no longer be sent,
 * so the stream is reset; the reader gets on_error once, and the request's end runs nothing. */
UTEST(h2, a_body_refused_on_a_begun_stream_resets_it) {
    StreamRig r;
    rig_open(&r);
    kl_http_router_add(&test_router, "POST", "/up", test_handler, NULL, test_br_factory);
    kl_http_router_use(&test_router, "*", "/*", mw_streams_and_continues, NULL);
    kl_http2_server_upgrade(&r.conn, &test_router, &test_h2_cfg, NULL, 0);
    r.conn.max_body_size = 2;
    const char *hn[] = {"content-length"};
    const char *hv[] = {"3"};
    size_t hnl[] = {14}, hvl[] = {1};
    ASSERT_EQ(0, r.mock.callbacks.on_request(r.mock.cb_user_data, 1, "POST", 4, "/up", 3,
                                             NULL, 0, hn, hv, hnl, hvl, 1));
    ASSERT_EQ(1, g_nsres);
    ASSERT_EQ(0, r.mock.callbacks.on_data(r.mock.cb_user_data, 1, "abc", 3));
    ASSERT_EQ(1, g_test_br.error_count);
    ASSERT_EQ(1, r.mock.reset_count);
    ASSERT_EQ(2u, r.mock.last_reset_code);           /* INTERNAL_ERROR */
    ASSERT_EQ(0, r.mock.submit_count);               /* no 413 over the begun stream */
    ASSERT_EQ(0, r.mock.callbacks.on_stream_end(r.mock.cb_user_data, 1));
    ASSERT_EQ(0, handler_called);
    ASSERT_EQ(0, g_test_br.complete_count);
    r.mock.callbacks.on_stream_reset(r.mock.cb_user_data, 1, 2);
    ASSERT_EQ(1, g_test_br.error_count);             /* once */
    ASSERT_EQ(1, g_sclose_calls[0]);
    ASSERT_EQ(0, g_sclose_completed[0]);
    rig_close(&r);
}

/* ── A flush from outside the connection's events: no timer, or a failure ─────────────────────── */

static void *nomem_malloc(void *ctx, size_t n) { (void)ctx; (void)n; return NULL; }
static void *nomem_realloc(void *ctx, void *p, size_t o, size_t n) {
    (void)ctx; (void)p; (void)o; (void)n;
    return NULL;
}
static void nomem_free(void *ctx, void *p, size_t n) { (void)ctx; (void)p; (void)n; }
static KlAllocator nomem_alloc = { .malloc = nomem_malloc, .realloc = nomem_realloc,
                                   .free = nomem_free, .ctx = NULL };

/* While refused, kl_timer_add on ev fails as it does out of memory (the heap cannot grow). */
typedef struct { KlAllocator *alloc; int cap; } TimerRefusal;
static TimerRefusal timers_refuse(KlEventCtx *ev) {
    TimerRefusal t = { ev->alloc, ev->timer_cap };
    ev->alloc = &nomem_alloc;
    ev->timer_cap = ev->timer_count;
    return t;
}
static void timers_allow(KlEventCtx *ev, TimerRefusal t) {
    ev->alloc = t.alloc;
    ev->timer_cap = t.cap;
}

/* Open stream `id` on the sweep server's connection, handled by streaming_handler. */
static int sweep_open_stream(MockH2Session *m, uint32_t id) {
    if (m->callbacks.on_request(m->cb_user_data, id, "GET", 3, "/s", 2,
                                NULL, 0, NULL, NULL, NULL, NULL, 0) != 0) return -1;
    return m->callbacks.on_stream_end(m->cb_user_data, id);
}

static int g_closes_in_flush;
static void record_closes_in_flush(void) { g_closes_in_flush = g_sclose_calls[0] + g_sclose_calls[1]; }

static int g_writes_in_flush;
static void write_in_flush(void) {
    if (kl_http_response_stream_try_write(g_sres[0], "again", 5) == KL_HTTP_STREAM_ACCEPTED)
        g_writes_in_flush++;
}

/* A producer writes from outside the connection's events (a timer, a watcher). Its output used to
 * be sent by a zero-delay timer, and when no timer could be armed the flush was owed to whichever
 * event came next. The server's pass before each wait now sends it, and needs no timer: one cannot
 * be refused. */
UTEST(h2, output_written_outside_the_connection_events_is_sent_by_the_settle_pass) {
    test_setup();
    g_nsres = 0;
    MockH2Session mock;
    mock_init(&mock);
    mock.with_stream = 1;
    mock.want_write_return = 1;
    g_mock_session = &mock;
    kl_http_router_add(&test_router, "GET", "/s", streaming_handler, NULL, NULL);
    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);
    int comp = 0;
    KlHttpConn *c = h2_sweep_setup(pfd, &comp);
    ASSERT_TRUE(c != NULL);
    ASSERT_EQ(0, sweep_open_stream(&mock, 1));
    ASSERT_EQ(1, g_nsres);
    kl_http_server_settle_h2(&h2_sweep_srv);               /* the begin's flush */

    int timers = h2_sweep_srv.ev.timer_count;
    TimerRefusal t = timers_refuse(&h2_sweep_srv.ev);
    int wr = kl_http_response_stream_try_write(g_sres[0], "more", 4);
    int due = c->h2->flush_due;
    int flushes = mock.flush_count;
    kl_http_server_settle_h2(&h2_sweep_srv);
    int again = kl_http_server_settle_h2_due(&h2_sweep_srv);
    timers_allow(&h2_sweep_srv.ev, t);
    int settled = mock.flush_count, due_after = c->h2 ? c->h2->flush_due : -1;
    int timers_after = h2_sweep_srv.ev.timer_count;
    kl_http_server_settle_h2(&h2_sweep_srv);
    int idle = mock.flush_count == settled;
    int closes = g_sclose_calls[0], destroyed = mock.destroy_count;

    kl_http_server_free(&h2_sweep_srv);
    kl_test_closesock(pfd[0]);
    test_teardown();
    ASSERT_EQ(KL_HTTP_STREAM_ACCEPTED, wr);
    ASSERT_EQ(1, due);
    ASSERT_EQ(flushes + 1, settled);                       /* sent by the pass, no timer needed */
    ASSERT_EQ(0, due_after);
    ASSERT_EQ(0, again);                                   /* nothing new: the loop may wait */
    ASSERT_EQ(timers, timers_after);                       /* no timer armed */
    ASSERT_TRUE(idle);                                     /* nothing due: no further flush */
    ASSERT_EQ(0, closes);                                  /* the stream goes on */
    ASSERT_EQ(0, destroyed);
}

/* A flush the settle pass runs, outside the connection's events, fails (the session cannot send):
 * on a completion loop nothing closed the connection until the client's next frame, so with a
 * silent client every stream on it stalled and no producer heard. The failure now marks the
 * session failed and the same pass ends its streams (every open stream's producer is told, once,
 * that it was cut short, after the flush has returned); the sweep then closes the connection, and
 * no later flush reaches the failed session. */
UTEST(h2, a_failed_flush_outside_the_connection_events_closes_it) {
    test_setup();
    g_nsres = 0;
    MockH2Session mock;
    mock_init(&mock);
    mock.with_stream = 1;
    mock.want_write_return = 1;
    g_mock_session = &mock;
    kl_http_router_add(&test_router, "GET", "/s", streaming_handler, NULL, NULL);
    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);
    int comp = 0;
    KlHttpConn *c = h2_sweep_setup(pfd, &comp);
    ASSERT_TRUE(c != NULL);
    ASSERT_EQ(0, sweep_open_stream(&mock, 1));
    ASSERT_EQ(0, sweep_open_stream(&mock, 3));
    ASSERT_EQ(2, g_nsres);
    kl_http_server_settle_h2(&h2_sweep_srv);

    mock.flush_return = -1;
    mock.on_flush = record_closes_in_flush;
    g_closes_in_flush = -1;
    int wr = kl_http_response_stream_try_write(g_sres[0], "more", 4);
    kl_http_server_settle_h2(&h2_sweep_srv);               /* the flush fails here */
    mock.on_flush = NULL;
    int failed_flushes = mock.flush_count;
    int closes_at_settle = g_sclose_calls[0] + g_sclose_calls[1];
    int streams_left = c->h2 ? c->h2->num_streams : -1;
    int destroyed_at_settle = mock.destroy_count;
    int refused = kl_http2_server_comp_sent(c) == KL_HTTP_CONN_CLOSED &&
                  kl_http2_server_on_writable(c) == KL_HTTP_CONN_CLOSED;
    kl_http_server_settle_h2(&h2_sweep_srv);
    int no_retry = mock.flush_count == failed_flushes;     /* the failed session is not flushed */

    uint64_t now = kl_monotonic_ms();
    c->last_active_ms = now;                               /* not idle: only the failure closes it */
    kl_http_server_sweep_conn_timeouts(&h2_sweep_srv, now, comp);
    int destroyed = mock.destroy_count;
    int closes0 = g_sclose_calls[0], closes1 = g_sclose_calls[1];
    int done0 = g_sclose_completed[0], done1 = g_sclose_completed[1];

    kl_http_server_free(&h2_sweep_srv);
    kl_test_closesock(pfd[0]);
    test_teardown();
    ASSERT_EQ(KL_HTTP_STREAM_ACCEPTED, wr);
    ASSERT_EQ(0, g_closes_in_flush);                       /* never from inside the flush */
    ASSERT_EQ(2, closes_at_settle);                        /* was 0: told only at the release */
    ASSERT_EQ(0, streams_left);
    ASSERT_EQ(0, destroyed_at_settle);                     /* the connection itself is closed next */
    ASSERT_TRUE(refused);
    ASSERT_TRUE(no_retry);
    ASSERT_EQ(1, destroyed);                               /* was 0: left open, every stream stalled */
    ASSERT_EQ(1, closes0);
    ASSERT_EQ(1, closes1);                                 /* the sibling is told too */
    ASSERT_EQ(0, done0);
    ASSERT_EQ(0, done1);
    ASSERT_EQ(1, g_sclose_calls[0]);                       /* once each: not again at the free */
    ASSERT_EQ(1, g_sclose_calls[1]);
}

/* A producer that writes while the settle pass runs (as one does from the writable edge the pass's
 * flush fires) makes the check before the wait report it, so the loop does not block on output it
 * holds: it goes out on the next iteration, and one flush per iteration is all such a producer costs. */
UTEST(h2, output_written_during_the_settle_pass_makes_the_wait_zero) {
    test_setup();
    g_nsres = 0;
    MockH2Session mock;
    mock_init(&mock);
    mock.with_stream = 1;
    mock.want_write_return = 1;
    g_mock_session = &mock;
    kl_http_router_add(&test_router, "GET", "/s", streaming_handler, NULL, NULL);
    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);
    int comp = 0;
    KlHttpConn *c = h2_sweep_setup(pfd, &comp);
    ASSERT_TRUE(c != NULL);
    ASSERT_EQ(0, sweep_open_stream(&mock, 1));
    kl_http_server_settle_h2(&h2_sweep_srv);
    int quiet = kl_http_server_settle_h2_due(&h2_sweep_srv);

    g_writes_in_flush = 0;
    mock.on_flush = write_in_flush;
    int wr = kl_http_response_stream_try_write(g_sres[0], "more", 4);
    int flushes = mock.flush_count;
    kl_http_server_settle_h2(&h2_sweep_srv);               /* flushes once; the producer writes */
    mock.on_flush = NULL;
    int once = mock.flush_count - flushes;
    int due = kl_http_server_settle_h2_due(&h2_sweep_srv);
    kl_http_server_settle_h2(&h2_sweep_srv);               /* the next iteration sends it */
    int next = mock.flush_count - flushes;
    int due_after = kl_http_server_settle_h2_due(&h2_sweep_srv);

    kl_http_server_free(&h2_sweep_srv);
    kl_test_closesock(pfd[0]);
    test_teardown();
    ASSERT_EQ(0, quiet);
    ASSERT_EQ(KL_HTTP_STREAM_ACCEPTED, wr);
    ASSERT_EQ(1, g_writes_in_flush);
    ASSERT_EQ(1, once);                                    /* one flush per connection per pass */
    ASSERT_EQ(1, due);                                     /* the wait must not block */
    ASSERT_EQ(2, next);
    ASSERT_EQ(0, due_after);
}

/* A body resume that fails (no memory) leaves the stream deferred, and the settle pass retries it on
 * every iteration until it succeeds, without a zero wait: a resume that keeps failing is retried at
 * the loop's pace, not in a loop that never blocks. */
UTEST(h2, a_failed_body_resume_is_retried_by_the_settle_pass) {
    test_setup();
    g_nsres = 0;
    MockH2Session mock;
    mock_init(&mock);
    mock.with_stream = 1;
    mock.want_write_return = 1;
    g_mock_session = &mock;
    kl_http_router_add(&test_router, "GET", "/s", streaming_handler, NULL, NULL);
    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);
    int comp = 0;
    KlHttpConn *c = h2_sweep_setup(pfd, &comp);
    ASSERT_TRUE(c != NULL);
    ASSERT_EQ(0, sweep_open_stream(&mock, 1));
    kl_http_server_settle_h2(&h2_sweep_srv);
    char buf[64];
    int eof = 0;
    kl_ssize_t got = mock.callbacks.read_body(mock.cb_user_data, 1, buf, sizeof buf, &eof);
    kl_ssize_t dry = mock.callbacks.read_body(mock.cb_user_data, 1, buf, sizeof buf, &eof);

    mock.resume_fail = 2;                                  /* the write's resume and one retry */
    int resumes = mock.resume_count;
    int wr = kl_http_response_stream_try_write(g_sres[0], "x", 1);
    kl_http_server_settle_h2(&h2_sweep_srv);               /* retried: fails again */
    int after_one = mock.resume_count - resumes;
    int spin = kl_http_server_settle_h2_due(&h2_sweep_srv);
    kl_http_server_settle_h2(&h2_sweep_srv);               /* retried: succeeds */
    int after_two = mock.resume_count - resumes;
    kl_http_server_settle_h2(&h2_sweep_srv);
    int after_three = mock.resume_count - resumes;
    kl_ssize_t more = mock.callbacks.read_body(mock.cb_user_data, 1, buf, sizeof buf, &eof);

    kl_http_server_free(&h2_sweep_srv);
    kl_test_closesock(pfd[0]);
    test_teardown();
    ASSERT_EQ(2, (int)got);                                /* "hi" */
    ASSERT_EQ(KL_HTTP2_BODY_DEFERRED, dry);
    ASSERT_EQ(KL_HTTP_STREAM_ACCEPTED, wr);
    ASSERT_EQ(2, after_one);                               /* the write's, then the pass's */
    ASSERT_EQ(0, spin);                                    /* no zero wait for a failing resume */
    ASSERT_EQ(3, after_two);                               /* was never retried without a flush */
    ASSERT_EQ(3, after_three);                             /* resumed: no further retry */
    ASSERT_EQ(1, (int)more);
}

/* A flush inside the connection's own event that fails marks the session failed before the settle
 * pass ends its streams. A write in that window (another stream's handler in the same batch) is
 * refused, rather than accepted into a session that sends nothing more. */
UTEST(h2, a_write_after_a_failed_flush_is_refused) {
    test_setup();
    g_nsres = 0;
    MockH2Session mock;
    mock_init(&mock);
    mock.with_stream = 1;
    mock.want_write_return = 1;
    g_mock_session = &mock;
    kl_http_router_add(&test_router, "GET", "/s", streaming_handler, NULL, NULL);
    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);
    int comp = 0;
    KlHttpConn *c = h2_sweep_setup(pfd, &comp);
    ASSERT_TRUE(c != NULL);
    ASSERT_EQ(0, sweep_open_stream(&mock, 1));
    kl_http_server_settle_h2(&h2_sweep_srv);
    mock.flush_return = -1;
    int closed = kl_http2_server_on_writable(c) == KL_HTTP_CONN_CLOSED;
    int streams = c->h2 ? c->h2->num_streams : -1;
    int refused = kl_http_response_stream_try_write(g_sres[0], "x", 1) == KL_HTTP_STREAM_ERROR;
    int due = kl_http_server_settle_h2_due(&h2_sweep_srv);
    kl_http_server_settle_h2(&h2_sweep_srv);
    int told = g_sclose_calls[0];
    int settled = kl_http_server_settle_h2_due(&h2_sweep_srv);   /* awaiting only the sweep's close */

    kl_http_server_free(&h2_sweep_srv);
    kl_test_closesock(pfd[0]);
    test_teardown();
    ASSERT_TRUE(closed);
    ASSERT_EQ(1, streams);                                 /* not yet ended */
    ASSERT_TRUE(refused);                                  /* was ACCEPTED, into a dead session */
    ASSERT_EQ(1, due);                                     /* the pass is to end it promptly */
    ASSERT_EQ(1, told);
    ASSERT_EQ(0, settled);                                 /* then no zero wait while it closes */
}

/* Output that is pending but cannot move (the session holds it for a full socket, a spent window or
 * a full completion queue) is not output that is due: the settle check must not turn it into a
 * zero wait, or the loop would spin until the client reads. Only a producer's write makes output
 * due; what is held moves on the connection's own writable event or send completion. */
UTEST(h2, held_output_does_not_make_the_wait_zero) {
    test_setup();
    g_nsres = 0;
    MockH2Session mock;
    mock_init(&mock);
    mock.with_stream = 1;
    mock.want_write_return = 1;                            /* the session keeps holding output */
    g_mock_session = &mock;
    kl_http_router_add(&test_router, "GET", "/s", streaming_handler, NULL, NULL);
    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);
    int comp = 0;
    KlHttpConn *c = h2_sweep_setup(pfd, &comp);
    ASSERT_TRUE(c != NULL);
    ASSERT_EQ(0, sweep_open_stream(&mock, 1));
    char buf[64];
    int eof = 0;
    (void)mock.callbacks.read_body(mock.cb_user_data, 1, buf, sizeof buf, &eof);
    kl_ssize_t dry = mock.callbacks.read_body(mock.cb_user_data, 1, buf, sizeof buf, &eof);
    kl_http_server_settle_h2(&h2_sweep_srv);
    int flushes = mock.flush_count;
    int held = 0;
    for (int i = 0; i < 5; i++) {                          /* five idle iterations */
        kl_http_server_settle_h2(&h2_sweep_srv);
        held += kl_http_server_settle_h2_due(&h2_sweep_srv);
    }
    int idle_flushes = mock.flush_count - flushes;
    c->h2->out_refused = 1;                                /* a completion queue at its bound */
    kl_http_server_settle_h2(&h2_sweep_srv);
    held += kl_http_server_settle_h2_due(&h2_sweep_srv);

    kl_http_server_free(&h2_sweep_srv);
    kl_test_closesock(pfd[0]);
    test_teardown();
    ASSERT_EQ(KL_HTTP2_BODY_DEFERRED, dry);                /* the stream has nothing to send */
    ASSERT_EQ(0, held);                                    /* never a zero wait for held output */
    ASSERT_EQ(0, idle_flushes);                            /* and no flush retried for it */
}

/* ── The drain's flush, a failed session's input, the request-write seam ─────────────────────── */

/* Is WRITE reported for c's stream on the sweep server's (readiness) loop? */
static int sweep_write_armed(KlHttpConn *c) {
    KlEvent evs[8];
    int n = kl_event_wait(&h2_sweep_srv.ev.loop, evs, 8, 0);
    for (int i = 0; i < n; i++)
        if (evs[i].udata == &c->stream && (evs[i].ready & KL_EVENT_WRITE)) return 1;
    return 0;
}

/* A producer writes from outside the connection's events (a timer, a thread pool), and in the same
 * iteration a graceful stop begins: the drain's GOAWAY flush takes the producer's output too, until
 * the socket is full. That flush cleared what the settle pass looks for, so on a readiness loop
 * nothing asked for WRITE: the rest waited for the client's next frame, and a client that only
 * reads saw the in-flight stream cut off at the drain deadline. The drain's flush now asks for
 * WRITE itself, so the held output moves on the connection's writable event with no client
 * traffic at all. */
UTEST(h2, output_held_by_the_drain_flush_is_sent_without_client_traffic) {
    test_setup();
    g_nsres = 0;
    MockH2Session mock;
    mock_init(&mock);
    mock.with_stream = 1;
    g_mock_session = &mock;
    kl_http_router_add(&test_router, "GET", "/s", streaming_handler, NULL, NULL);
    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);
    int comp = 0;
    KlHttpConn *c = h2_sweep_setup(pfd, &comp);
    ASSERT_TRUE(c != NULL);
    if (comp) {                                            /* a completion loop sends from its queue */
        kl_http_server_free(&h2_sweep_srv);
        kl_test_closesock(pfd[0]);
        test_teardown();
        UTEST_SKIP("readiness only");
    }
    ASSERT_EQ(0, sweep_open_stream(&mock, 1));
    kl_http_server_settle_h2(&h2_sweep_srv);
    /* The connection's own last event left READ only: nothing was held then. */
    ASSERT_EQ(0, kl_event_add(&h2_sweep_srv.ev.loop, (KlSocketHandle)pfd[1], KL_EVENT_READ,
                              &c->stream));
    (void)sweep_write_armed(c);
    int recvs = mock.recv_count;

    mock.want_write_return = 1;                            /* from here the socket is full */
    int wr = kl_http_response_stream_try_write(g_sres[0], "more", 4);
    kl_http2_server_drain_shutdown(c);                     /* the drain's progress step */
    int shutdowns = mock.shutdown_count;
    kl_http_server_settle_h2(&h2_sweep_srv);               /* the next iteration's pass */
    int due = kl_http_server_settle_h2_due(&h2_sweep_srv);
    int armed = sweep_write_armed(c);

    mock.want_write_return = 0;                            /* the client read: the socket drains */
    int flushes = mock.flush_count;
    int st = kl_http2_server_on_writable(c);
    char buf[16];
    int eof = 0;
    kl_ssize_t got = mock.callbacks.read_body(mock.cb_user_data, 1, buf, sizeof buf, &eof);
    int sent_flushes = mock.flush_count - flushes;
    int recvs_after = mock.recv_count;

    kl_event_del(&h2_sweep_srv.ev.loop, (KlSocketHandle)pfd[1]);
    kl_http_server_free(&h2_sweep_srv);
    kl_test_closesock(pfd[0]);
    test_teardown();
    ASSERT_EQ(KL_HTTP_STREAM_ACCEPTED, wr);
    ASSERT_EQ(1, shutdowns);                               /* the GOAWAY */
    ASSERT_EQ(0, due);                                     /* held, not due: no zero wait */
    ASSERT_EQ(1, armed);                                   /* was 0: nothing watched for WRITE */
    ASSERT_EQ(KL_HTTP_CONN_HTTP2, st);
    ASSERT_EQ(1, sent_flushes);                            /* the writable event flushed */
    ASSERT_EQ(6, (int)got);                                /* the producer's bytes reach the session */
    ASSERT_EQ(0, memcmp(buf, "himore", 6));
    ASSERT_EQ(recvs, recvs_after);                         /* without a frame from the client */
}

/* A session whose flush failed can answer nothing more. Its input was still parsed, so a request
 * that arrived before the close ran its handler, side effects and all, for an answer that could
 * only be the close. The connection's input is now refused once the session has failed. */
UTEST(h2, a_failed_session_reads_no_more_input) {
    test_setup();
    g_nsres = 0;
    MockH2Session mock;
    mock_init(&mock);
    mock.with_stream = 1;
    g_mock_session = &mock;
    kl_http_router_add(&test_router, "GET", "/s", streaming_handler, NULL, NULL);
    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);
    int comp = 0;
    KlHttpConn *c = h2_sweep_setup(pfd, &comp);
    ASSERT_TRUE(c != NULL);
    ASSERT_EQ(0, sweep_open_stream(&mock, 1));
    kl_http_server_settle_h2(&h2_sweep_srv);
    mock.want_write_return = 1;
    mock.flush_return = -1;                                /* the session cannot send */
    (void)kl_http_response_stream_try_write(g_sres[0], "x", 1);
    kl_http_server_settle_h2(&h2_sweep_srv);               /* the failure, outside the events */
    int failed = c->h2->failed;
    int recvs = mock.recv_count;
    KlHttpConnState st = kl_http2_server_feed(c, "frame", 5);
    int recvs_after = mock.recv_count;

    kl_http_server_free(&h2_sweep_srv);
    kl_test_closesock(pfd[0]);
    test_teardown();
    ASSERT_EQ(1, failed);
    ASSERT_EQ(KL_HTTP_CONN_CLOSED, st);
    ASSERT_EQ(recvs, recvs_after);                         /* was +1: the session parsed it */
}

/* A flush that fails inside the connection's own event (its input's flush) closes it. On a
 * completion loop the close waits for queued output, which a client that stopped reading never
 * takes, and the settle pass does not visit a closing connection: the producers heard nothing until
 * the slot was released. The failing event now ends the streams itself, after its flush returned. */
UTEST(h2, a_failed_flush_in_the_connection_event_ends_its_streams) {
    test_setup();
    g_nsres = 0;
    MockH2Session mock;
    mock_init(&mock);
    mock.with_stream = 1;
    g_mock_session = &mock;
    kl_http_router_add(&test_router, "GET", "/s", streaming_handler, NULL, NULL);
    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);
    int comp = 0;
    KlHttpConn *c = h2_sweep_setup(pfd, &comp);
    ASSERT_TRUE(c != NULL);
    ASSERT_EQ(0, sweep_open_stream(&mock, 1));
    kl_http_server_settle_h2(&h2_sweep_srv);
    g_sclose_calls[1] = 0;                                 /* one stream: the other slot is unused */
    g_closes_in_flush = -1;
    mock.on_flush = record_closes_in_flush;
    mock.want_write_return = 1;
    mock.flush_return = -1;
    KlHttpConnState st = kl_http2_server_feed(c, "frame", 5);
    int closes = g_sclose_calls[0], completed = g_sclose_completed[0];
    int left = c->h2 ? c->h2->num_streams : -1;

    kl_http_server_free(&h2_sweep_srv);
    kl_test_closesock(pfd[0]);
    test_teardown();
    ASSERT_EQ(KL_HTTP_CONN_CLOSED, st);
    ASSERT_EQ(0, g_closes_in_flush);                       /* never from inside the flush */
    ASSERT_EQ(1, closes);                                  /* was 0 until the slot's release */
    ASSERT_EQ(0, completed);
    ASSERT_EQ(0, left);
}

/* A fatal session error on the client's input (recv fails) closes the connection in that event too,
 * and so ends its streams there, for the same reason. */
UTEST(h2, a_fatal_session_error_ends_its_streams) {
    test_setup();
    g_nsres = 0;
    MockH2Session mock;
    mock_init(&mock);
    mock.with_stream = 1;
    g_mock_session = &mock;
    kl_http_router_add(&test_router, "GET", "/s", streaming_handler, NULL, NULL);
    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);
    int comp = 0;
    KlHttpConn *c = h2_sweep_setup(pfd, &comp);
    ASSERT_TRUE(c != NULL);
    ASSERT_EQ(0, sweep_open_stream(&mock, 1));
    kl_http_server_settle_h2(&h2_sweep_srv);
    mock.recv_return = -1;
    KlHttpConnState st = kl_http2_server_feed(c, "bad", 3);
    int closes = g_sclose_calls[0], completed = g_sclose_completed[0];
    int left = c->h2 ? c->h2->num_streams : -1;

    kl_http_server_free(&h2_sweep_srv);
    kl_test_closesock(pfd[0]);
    test_teardown();
    ASSERT_EQ(KL_HTTP_CONN_CLOSED, st);
    ASSERT_EQ(1, closes);
    ASSERT_EQ(0, completed);
    ASSERT_EQ(0, left);
}

/* The same for the flush a completion loop's send completion runs to resume output the queue had
 * refused: it is the connection's own event too. */
UTEST(h2, a_failed_flush_in_a_send_completion_ends_its_streams) {
    test_setup();
    g_nsres = 0;
    MockH2Session mock;
    mock_init(&mock);
    mock.with_stream = 1;
    g_mock_session = &mock;
    kl_http_router_add(&test_router, "GET", "/s", streaming_handler, NULL, NULL);
    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);
    int comp = 0;
    KlHttpConn *c = h2_sweep_setup(pfd, &comp);
    ASSERT_TRUE(c != NULL);
    ASSERT_EQ(0, sweep_open_stream(&mock, 1));
    kl_http_server_settle_h2(&h2_sweep_srv);
    g_sclose_calls[1] = 0;
    g_closes_in_flush = -1;
    mock.on_flush = record_closes_in_flush;
    c->h2->out_refused = 1;                                /* the queue had refused output */
    mock.want_write_return = 1;
    mock.flush_return = -1;
    KlHttpConnState st = kl_http2_server_comp_sent(c);
    int closes = g_sclose_calls[0], completed = g_sclose_completed[0];
    int left = c->h2 ? c->h2->num_streams : -1;

    kl_http_server_free(&h2_sweep_srv);
    kl_test_closesock(pfd[0]);
    test_teardown();
    ASSERT_EQ(KL_HTTP_CONN_CLOSED, st);
    ASSERT_EQ(0, g_closes_in_flush);
    ASSERT_EQ(1, closes);
    ASSERT_EQ(0, completed);
    ASSERT_EQ(0, left);
}

/* kl_http_conn_request_write arms READ|WRITE, the mask of the WebSocket and HTTP/2 states only: any
 * other state's registration is left as it is. */
UTEST(h2, request_write_leaves_other_states_alone) {
    test_setup();
    MockH2Session mock;
    mock_init(&mock);
    g_mock_session = &mock;
    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);
    int comp = 0;
    KlHttpConn *c = h2_sweep_setup(pfd, &comp);
    ASSERT_TRUE(c != NULL);
    if (comp) {
        kl_http_server_free(&h2_sweep_srv);
        kl_test_closesock(pfd[0]);
        test_teardown();
        UTEST_SKIP("readiness only");
    }
    ASSERT_EQ(0, kl_event_add(&h2_sweep_srv.ev.loop, (KlSocketHandle)pfd[1], KL_EVENT_READ,
                              &c->stream));
    (void)sweep_write_armed(c);
    c->state = KL_HTTP_CONN_READING;
    kl_http_conn_request_write(c);
    int other = sweep_write_armed(c);
    c->state = KL_HTTP_CONN_HTTP2;
    kl_http_conn_request_write(c);
    int h2 = sweep_write_armed(c);

    kl_event_del(&h2_sweep_srv.ev.loop, (KlSocketHandle)pfd[1]);
    kl_http_server_free(&h2_sweep_srv);
    kl_test_closesock(pfd[0]);
    test_teardown();
    ASSERT_EQ(0, other);
    ASSERT_EQ(1, h2);
}

UTEST_MAIN();
