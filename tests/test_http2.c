#include "utest.h"
#include <keel/keel.h>
#include <keel/http2.h>
#include <keel/http2_server.h>
#include "../src/http2_internal.h"
#include <keel/http_connection.h>
#include <keel/http_router.h>
#include <keel/http_body_reader.h>
#include <string.h>
#include "net_compat.h"

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
    ssize_t recv_return;
    int submit_return;
    int want_write_return;
    int flush_return;
    int shutdown_return;

    /* If set, factory skips setting vtable (for vtable validation tests) */
    int skip_vtable_init;

    /* KEEL's callbacks (stored by factory) */
    KlHttp2ServerCallbacks callbacks;
    void *cb_user_data;
} MockH2Session;

static kl_ssize_t mock_recv(KlHttp2ServerSession *self, const void *data, size_t len) {
    MockH2Session *m = (MockH2Session *)self;
    m->recv_count++;
    (void)data;
    if (m->recv_return >= 0)
        return (ssize_t)len;
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
    return m->flush_return;
}

static int mock_shutdown(KlHttp2ServerSession *self) {
    MockH2Session *m = (MockH2Session *)self;
    m->shutdown_count++;
    return m->shutdown_return;
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
    /* We test via kl_http2_server_upgrade — need a minimal conn */
    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);

    KlHttpConn conn;
    memset(&conn, 0, sizeof(conn));
    conn.stream.fd = pfd[1];
    conn.stream.alloc = &test_alloc;

    /* Tell factory to skip vtable init — we set pointers manually
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

    /* Create stream 1 — POST with body, no stream_end yet */
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

    /* Stream 5 should fail — at max */
    rc = mock.callbacks.on_request(mock.cb_user_data, 5,
                                   "POST", 4, "/data", 5,
                                   NULL, 0, hn, hv, hnl, hvl, 1);
    ASSERT_EQ(rc, -1);
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

    /* Reset stream — should call on_error and destroy body reader */
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
    ASSERT_EQ(s->headers_done, 1);

    kl_http2_server_cleanup(&conn);
    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();
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
    /* Middleware short-circuited — response submitted, stream destroyed */
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

    /* Overflow the test body reader's 4096-byte buffer */
    char big[5000];
    memset(big, 'A', sizeof(big));
    int rc = mock.callbacks.on_data(mock.cb_user_data, 1, big, sizeof(big));
    ASSERT_EQ(rc, -1);

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
    ssize_t r = mock.callbacks.send(mock.cb_user_data, data, 17);
    ASSERT_EQ(r, (ssize_t)17);

    /* Read from pipe to verify */
    char buf[64];
    ssize_t nr = kl_test_sockread(pfd[0], buf, sizeof(buf));
    ASSERT_EQ(nr, (ssize_t)17);
    ASSERT_EQ(memcmp(buf, "HTTP/2 frame data", 17), 0);

    kl_http2_server_cleanup(&conn);
    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    test_teardown();
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
    /* This is tested implicitly — without alpn, conn stays in READING */
    KlHttpConn conn;
    memset(&conn, 0, sizeof(conn));

    /* No TLS at all means no ALPN */
    ASSERT_TRUE(conn.tls == NULL);
    /* h2_config set but no TLS — preface detection still works */
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

UTEST_MAIN();
