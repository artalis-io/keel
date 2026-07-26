#include "utest.h"
#include <keel/keel.h>
#include "net_compat.h"
#include <string.h>
#include <pthread.h>

/* ═══════════════════════════════════════════════════════════════════
 * Request header/param API tests
 *
 * Uses real server + raw sockets to verify header and param APIs
 * from inside handlers.
 * ═══════════════════════════════════════════════════════════════════ */

/* Wait for server to bind (max 2s) */
static void wait_for_bind(KlServer *s) {
    for (int i = 0; i < 200 && s->bound_port == 0; i++) usleep(10000);
}

/* Shared state between handler and test */
static struct {
    /* header tests */
    const char *found_host;
    size_t found_host_len;
    const char *found_missing;
    const char *found_empty;
    size_t found_empty_len;
    /* param tests */
    const char *found_id;
    size_t found_id_len;
    const char *found_uid;
    size_t found_uid_len;
    const char *found_pid;
    size_t found_pid_len;
    const char *found_missing_param;
    size_t found_missing_param_len;
    /* query tests */
    const char *query;
    size_t query_len;
    int query_null;
    /* null-termination tests */
    const char *nt_method;
    const char *nt_path;
    const char *nt_query;
    const char *nt_host;
} test_state;

static KlServer req_server;

static void *server_thread(void *arg) {
    (void)arg;
    kl_server_run(&req_server);
    return NULL;
}

static int connect_to(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)port),
    };
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        kl_test_closesock(fd);
        return -1;
    }
    return fd;
}

static ssize_t read_response(int fd, char *buf, size_t buflen) {
    int r = kl_test_poll1(fd, 0, 2000);
    if (r <= 0) return r;
    return kl_test_sockread(fd, buf, buflen);
}

/* ── Handlers ─────────────────────────────────────────────────────── */

static void handle_headers(KlRequest *req, KlResponse *res, void *ctx) {
    (void)ctx;
    memset(&test_state, 0, sizeof(test_state));

    /* Case-insensitive header lookup */
    test_state.found_host = kl_request_header_len(req, "Host",
                                                   &test_state.found_host_len);

    /* Missing header */
    test_state.found_missing = kl_request_header(req, "X-Nonexistent");

    /* Empty-value header */
    test_state.found_empty = kl_request_header_len(req, "X-Empty",
                                                    &test_state.found_empty_len);

    kl_response_json(res, 200, "{\"ok\":true}", 11);
}

static void handle_params(KlRequest *req, KlResponse *res, void *ctx) {
    (void)ctx;
    memset(&test_state, 0, sizeof(test_state));

    test_state.found_uid = kl_request_param(req, "uid", &test_state.found_uid_len);
    test_state.found_pid = kl_request_param(req, "pid", &test_state.found_pid_len);
    test_state.found_missing_param = kl_request_param(req, "nope",
                                                       &test_state.found_missing_param_len);

    kl_response_json(res, 200, "{\"ok\":true}", 11);
}

static void handle_single_param(KlRequest *req, KlResponse *res, void *ctx) {
    (void)ctx;
    memset(&test_state, 0, sizeof(test_state));

    test_state.found_id = kl_request_param(req, "id", &test_state.found_id_len);

    kl_response_json(res, 200, "{\"ok\":true}", 11);
}

static void handle_query(KlRequest *req, KlResponse *res, void *ctx) {
    (void)ctx;
    memset(&test_state, 0, sizeof(test_state));

    test_state.query = req->query;
    test_state.query_len = req->query_len;
    test_state.query_null = (req->query == NULL) ? 1 : 0;

    kl_response_json(res, 200, "{\"ok\":true}", 11);
}

static void handle_nulterm(KlRequest *req, KlResponse *res, void *ctx) {
    (void)ctx;
    memset(&test_state, 0, sizeof(test_state));

    test_state.nt_method = req->method;
    test_state.nt_path = req->path;
    test_state.nt_query = req->query;
    test_state.nt_host = kl_request_header(req, "Host");

    kl_response_json(res, 200, "{\"ok\":true}", 11);
}

/* ── Helper: send request, wait for 200. Returns 0 on success, -1 on failure. */

static int send_and_wait(int port, const char *raw_req) {
    int fd = connect_to(port);
    if (fd < 0) return -1;
    (void)kl_test_sockwrite(fd, raw_req, strlen(raw_req));
    char buf[4096];
    ssize_t n = read_response(fd, buf, sizeof(buf) - 1);
    kl_test_closesock(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    return strstr(buf, "200") ? 0 : -1;
}

/* ═══════════════════════════════════════════════════════════════════
 * Header tests
 * ═══════════════════════════════════════════════════════════════════ */

UTEST(request, header_case_insensitive) {
    KlConfig cfg = {.port = 0};
    ASSERT_EQ(kl_server_init(&req_server, &cfg), 0);
    kl_server_route(&req_server, "GET", "/hdr", handle_headers, NULL, NULL);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, NULL);
    wait_for_bind(&req_server);
    int port = req_server.bound_port;

    send_and_wait(port,
        "GET /hdr HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n"
        "\r\n");

    ASSERT_TRUE(test_state.found_host != NULL);
    ASSERT_EQ(test_state.found_host_len, (size_t)9);
    ASSERT_TRUE(memcmp(test_state.found_host, "localhost", 9) == 0);

    kl_server_stop(&req_server);
    pthread_join(tid, NULL);
    kl_server_free(&req_server);
}

UTEST(request, header_missing_returns_null) {
    KlConfig cfg = {.port = 0};
    ASSERT_EQ(kl_server_init(&req_server, &cfg), 0);
    kl_server_route(&req_server, "GET", "/hdr", handle_headers, NULL, NULL);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, NULL);
    wait_for_bind(&req_server);
    int port = req_server.bound_port;

    send_and_wait(port,
        "GET /hdr HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n"
        "\r\n");

    ASSERT_TRUE(test_state.found_missing == NULL);

    kl_server_stop(&req_server);
    pthread_join(tid, NULL);
    kl_server_free(&req_server);
}

UTEST(request, header_with_len) {
    KlConfig cfg = {.port = 0};
    ASSERT_EQ(kl_server_init(&req_server, &cfg), 0);
    kl_server_route(&req_server, "GET", "/hdr", handle_headers, NULL, NULL);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, NULL);
    wait_for_bind(&req_server);
    int port = req_server.bound_port;

    send_and_wait(port,
        "GET /hdr HTTP/1.1\r\n"
        "Host: my-server.local\r\n"
        "Connection: close\r\n"
        "\r\n");

    ASSERT_TRUE(test_state.found_host != NULL);
    ASSERT_EQ(test_state.found_host_len, (size_t)15);

    kl_server_stop(&req_server);
    pthread_join(tid, NULL);
    kl_server_free(&req_server);
}

UTEST(request, header_empty_value) {
    KlConfig cfg = {.port = 0};
    ASSERT_EQ(kl_server_init(&req_server, &cfg), 0);
    kl_server_route(&req_server, "GET", "/hdr", handle_headers, NULL, NULL);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, NULL);
    wait_for_bind(&req_server);
    int port = req_server.bound_port;

    send_and_wait(port,
        "GET /hdr HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "X-Empty: \r\n"
        "Connection: close\r\n"
        "\r\n");

    ASSERT_TRUE(test_state.found_empty != NULL);
    ASSERT_EQ(test_state.found_empty_len, (size_t)0);

    kl_server_stop(&req_server);
    pthread_join(tid, NULL);
    kl_server_free(&req_server);
}

/* ═══════════════════════════════════════════════════════════════════
 * Param tests
 * ═══════════════════════════════════════════════════════════════════ */

UTEST(request, param_basic) {
    KlConfig cfg = {.port = 0};
    ASSERT_EQ(kl_server_init(&req_server, &cfg), 0);
    kl_server_route(&req_server, "GET", "/items/:id", handle_single_param,
                    NULL, NULL);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, NULL);
    wait_for_bind(&req_server);
    int port = req_server.bound_port;

    send_and_wait(port,
        "GET /items/42 HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n"
        "\r\n");

    ASSERT_TRUE(test_state.found_id != NULL);
    ASSERT_EQ(test_state.found_id_len, (size_t)2);
    ASSERT_TRUE(memcmp(test_state.found_id, "42", 2) == 0);

    kl_server_stop(&req_server);
    pthread_join(tid, NULL);
    kl_server_free(&req_server);
}

UTEST(request, param_multi) {
    KlConfig cfg = {.port = 0};
    ASSERT_EQ(kl_server_init(&req_server, &cfg), 0);
    kl_server_route(&req_server, "GET", "/users/:uid/posts/:pid",
                    handle_params, NULL, NULL);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, NULL);
    wait_for_bind(&req_server);
    int port = req_server.bound_port;

    send_and_wait(port,
        "GET /users/7/posts/99 HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n"
        "\r\n");

    ASSERT_TRUE(test_state.found_uid != NULL);
    ASSERT_EQ(test_state.found_uid_len, (size_t)1);
    ASSERT_TRUE(memcmp(test_state.found_uid, "7", 1) == 0);

    ASSERT_TRUE(test_state.found_pid != NULL);
    ASSERT_EQ(test_state.found_pid_len, (size_t)2);
    ASSERT_TRUE(memcmp(test_state.found_pid, "99", 2) == 0);

    kl_server_stop(&req_server);
    pthread_join(tid, NULL);
    kl_server_free(&req_server);
}

UTEST(request, param_missing) {
    KlConfig cfg = {.port = 0};
    ASSERT_EQ(kl_server_init(&req_server, &cfg), 0);
    kl_server_route(&req_server, "GET", "/users/:uid/posts/:pid",
                    handle_params, NULL, NULL);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, NULL);
    wait_for_bind(&req_server);
    int port = req_server.bound_port;

    send_and_wait(port,
        "GET /users/7/posts/99 HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n"
        "\r\n");

    ASSERT_TRUE(test_state.found_missing_param == NULL);
    ASSERT_EQ(test_state.found_missing_param_len, (size_t)0);

    kl_server_stop(&req_server);
    pthread_join(tid, NULL);
    kl_server_free(&req_server);
}

/* ═══════════════════════════════════════════════════════════════════
 * Query string tests
 * ═══════════════════════════════════════════════════════════════════ */

UTEST(request, query_basic) {
    KlConfig cfg = {.port = 0};
    ASSERT_EQ(kl_server_init(&req_server, &cfg), 0);
    kl_server_route(&req_server, "GET", "/q", handle_query, NULL, NULL);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, NULL);
    wait_for_bind(&req_server);
    int port = req_server.bound_port;

    send_and_wait(port,
        "GET /q?foo=bar HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n"
        "\r\n");

    ASSERT_TRUE(test_state.query != NULL);
    ASSERT_EQ(test_state.query_len, (size_t)7);
    ASSERT_TRUE(memcmp(test_state.query, "foo=bar", 7) == 0);

    kl_server_stop(&req_server);
    pthread_join(tid, NULL);
    kl_server_free(&req_server);
}

UTEST(request, query_empty) {
    KlConfig cfg = {.port = 0};
    ASSERT_EQ(kl_server_init(&req_server, &cfg), 0);
    kl_server_route(&req_server, "GET", "/q", handle_query, NULL, NULL);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, NULL);
    wait_for_bind(&req_server);
    int port = req_server.bound_port;

    send_and_wait(port,
        "GET /q? HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n"
        "\r\n");

    ASSERT_TRUE(test_state.query != NULL);
    ASSERT_EQ(test_state.query_len, (size_t)0);

    kl_server_stop(&req_server);
    pthread_join(tid, NULL);
    kl_server_free(&req_server);
}

UTEST(request, query_absent) {
    KlConfig cfg = {.port = 0};
    ASSERT_EQ(kl_server_init(&req_server, &cfg), 0);
    kl_server_route(&req_server, "GET", "/q", handle_query, NULL, NULL);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, NULL);
    wait_for_bind(&req_server);
    int port = req_server.bound_port;

    send_and_wait(port,
        "GET /q HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n"
        "\r\n");

    ASSERT_EQ(test_state.query_null, 1);
    ASSERT_EQ(test_state.query_len, (size_t)0);

    kl_server_stop(&req_server);
    pthread_join(tid, NULL);
    kl_server_free(&req_server);
}

/* ═══════════════════════════════════════════════════════════════════
 * Null-termination tests
 * ═══════════════════════════════════════════════════════════════════ */

UTEST(request, header_null_terminated) {
    KlConfig cfg = {.port = 0};
    ASSERT_EQ(kl_server_init(&req_server, &cfg), 0);
    kl_server_route(&req_server, "GET", "/nultest", handle_nulterm, NULL, NULL);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, NULL);
    wait_for_bind(&req_server);
    int port = req_server.bound_port;

    send_and_wait(port,
        "GET /nultest?a=1 HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n"
        "\r\n");

    ASSERT_TRUE(test_state.nt_host != NULL);
    ASSERT_EQ(strcmp(test_state.nt_host, "localhost"), 0);

    kl_server_stop(&req_server);
    pthread_join(tid, NULL);
    kl_server_free(&req_server);
}

UTEST(request, method_null_terminated) {
    KlConfig cfg = {.port = 0};
    ASSERT_EQ(kl_server_init(&req_server, &cfg), 0);
    kl_server_route(&req_server, "GET", "/nultest", handle_nulterm, NULL, NULL);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, NULL);
    wait_for_bind(&req_server);
    int port = req_server.bound_port;

    send_and_wait(port,
        "GET /nultest?a=1 HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n"
        "\r\n");

    ASSERT_TRUE(test_state.nt_method != NULL);
    ASSERT_EQ(strcmp(test_state.nt_method, "GET"), 0);

    kl_server_stop(&req_server);
    pthread_join(tid, NULL);
    kl_server_free(&req_server);
}

UTEST(request, path_null_terminated) {
    KlConfig cfg = {.port = 0};
    ASSERT_EQ(kl_server_init(&req_server, &cfg), 0);
    kl_server_route(&req_server, "GET", "/nultest", handle_nulterm, NULL, NULL);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, NULL);
    wait_for_bind(&req_server);
    int port = req_server.bound_port;

    send_and_wait(port,
        "GET /nultest?a=1 HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n"
        "\r\n");

    ASSERT_TRUE(test_state.nt_path != NULL);
    ASSERT_EQ(strcmp(test_state.nt_path, "/nultest"), 0);

    kl_server_stop(&req_server);
    pthread_join(tid, NULL);
    kl_server_free(&req_server);
}

UTEST(request, query_null_terminated) {
    KlConfig cfg = {.port = 0};
    ASSERT_EQ(kl_server_init(&req_server, &cfg), 0);
    kl_server_route(&req_server, "GET", "/nultest", handle_nulterm, NULL, NULL);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, NULL);
    wait_for_bind(&req_server);
    int port = req_server.bound_port;

    send_and_wait(port,
        "GET /nultest?a=1 HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n"
        "\r\n");

    ASSERT_TRUE(test_state.nt_query != NULL);
    ASSERT_EQ(strcmp(test_state.nt_query, "a=1"), 0);

    kl_server_stop(&req_server);
    pthread_join(tid, NULL);
    kl_server_free(&req_server);
}

UTEST_MAIN();
