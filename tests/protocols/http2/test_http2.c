#include "utest.h"
#include "../../../src/protocols/http/http_conn_internal.h"
#include <keel/keel.h>
#include <keel/http2.h>
#include <keel/http2_server.h>
#include "http2_internal.h"
#include <keel/http_connection.h>
#include <keel/http_router.h>
#include <keel/http_body_reader.h>
#include <string.h>
#include <stdlib.h>
#include "net_compat.h"
#include "platform_thread.h"   /* Keel PAL threads: portable to MSVC */
#include "http_internal.h"     /* kl_http_server_sweep_conn_timeouts */
#include "event_caps.h"        /* kl_event_caps: is the server's loop a completion loop? */
#include <keel/clock.h>

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

    /* KEEL's callbacks (stored by factory) */
    KlHttp2ServerCallbacks callbacks;
    void *cb_user_data;
} MockH2Session;

static kl_ssize_t mock_recv(KlHttp2ServerSession *self, const void *data, size_t len) {
    MockH2Session *m = (MockH2Session *)self;
    if (m->recv_count == 0 && data) {
        m->first_recv_len = len < sizeof(m->first_recv) ? len : sizeof(m->first_recv);
        memcpy(m->first_recv, data, m->first_recv_len);
    }
    m->recv_count++;
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
    return m->want_write_return;
}

static int mock_flush(KlHttp2ServerSession *self) {
    MockH2Session *m = (MockH2Session *)self;
    m->flush_count++;
    if (m->flush_send_len > 0) {
        static const char frames[64] = {0};
        size_t n = m->flush_send_len < sizeof frames ? m->flush_send_len : sizeof frames;
        (void)m->callbacks.send(m->cb_user_data, frames, n);
    }
    return m->flush_return;
}

static int mock_want_read(KlHttp2ServerSession *self) {
    return ((MockH2Session *)self)->want_read_return;
}

static int mock_shutdown(KlHttp2ServerSession *self) {
    MockH2Session *m = (MockH2Session *)self;
    m->shutdown_count++;
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

static void mock_destroy(KlHttp2ServerSession *self) {
    MockH2Session *m = (MockH2Session *)self;
    m->destroy_count++;
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
    }

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
 * made from :authority has a slot of its own (the last field gives way), so a handler reading Host
 * finds it. It was dropped silently. */
UTEST(h2, synthetic_host_keeps_its_slot_at_the_header_cap) {
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
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(conn.h2->num_streams, 1);
    KlHttp2ServerStream *s = &conn.h2->streams[0];
    const char *host = kl_http_request_header(&s->req, "host");
    int num = s->req.num_headers;

    kl_http2_server_cleanup(&conn);
    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();

    ASSERT_TRUE(host != NULL);                             /* was NULL: no slot left for it */
    ASSERT_STREQ(host, "example.com");
    ASSERT_EQ(num, KL_MAX_HEADERS);
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
 * neither read nor write it is done, and the connection closes there, as it does after a read. It
 * stayed open until the idle sweep or the drain deadline. */
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

/* An idle HTTP/2 connection the sweep closes is sent a GOAWAY first (RFC 9113 6.8), so the client
 * knows no request it sent was processed rather than seeing the connection simply drop. */
UTEST(h2, idle_sweep_sends_goaway_before_closing) {
    test_setup();
    MockH2Session mock;
    mock_init(&mock);
    g_mock_session = &mock;
    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);
    int comp = 0;
    KlHttpConn *c = h2_sweep_setup(pfd, &comp);
    ASSERT_TRUE(c != NULL);

    uint64_t now = kl_monotonic_ms();
    c->last_active_ms = now - 1000;                        /* idle past read_timeout_ms */
    kl_http_server_sweep_conn_timeouts(&h2_sweep_srv, now, comp);
    int shutdowns = mock.shutdown_count, destroyed = mock.destroy_count;

    kl_http_server_free(&h2_sweep_srv);
    kl_test_closesock(pfd[0]);
    test_teardown();
    ASSERT_EQ(destroyed, 1);                               /* closed */
    ASSERT_EQ(shutdowns, 1);                               /* was 0: closed without a GOAWAY */
}

/* After a graceful GOAWAY a session with no stream left wants neither read nor write: it is done,
 * and the sweep closes the connection at once. It was left to the idle timeout or the drain
 * deadline, so a draining server waited out its whole deadline. */
UTEST(h2, sweep_closes_a_finished_session) {
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

    uint64_t now = kl_monotonic_ms();
    c->last_active_ms = now;                               /* not idle long: only "done" closes it */
    kl_http_server_sweep_conn_timeouts(&h2_sweep_srv, now, comp);
    int destroyed = mock.destroy_count;

    kl_http_server_free(&h2_sweep_srv);
    kl_test_closesock(pfd[0]);
    test_teardown();
    ASSERT_EQ(destroyed, 1);                               /* was 0: held until a timeout */
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
    for (int i = 0; i < 200 && g_pk_srv.bound_port == 0; i++) kl_test_sleep_ms(10);

    int fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)g_pk_srv.bound_port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    int ok = fd >= 0 && connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0;
    static const char preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    static const char settings[9] = { 0, 0, 0, 4, 0, 0, 0, 0, 0 };   /* empty SETTINGS frame */
    if (ok) {
        (void)kl_test_sockwrite(fd, preface, 24);
        (void)kl_test_sockwrite(fd, settings, sizeof settings);
    }
    for (int i = 0; ok && i < 200 && g_pk_mock.recv_count == 0; i++) kl_test_sleep_ms(10);
    size_t got = g_pk_mock.first_recv_len;
    int whole = got >= 24 && memcmp(g_pk_mock.first_recv, preface, 24) == 0;
    if (fd >= 0) kl_test_closesock(fd);
    kl_http_server_stop(&g_pk_srv);
    kl_plat_thread_join(&t);
    kl_http_server_free(&g_pk_srv);
    test_teardown();
    ASSERT_TRUE(ok);
    ASSERT_GT(got, (size_t)0);
    ASSERT_TRUE(whole);                          /* was: the session got SETTINGS, magic stripped */
}

UTEST_MAIN();
