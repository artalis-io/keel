#include "utest.h"
#include <keel/keel.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <signal.h>

#define TEST_PORT 18080

static void handle_hello(KlRequest *req, KlResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_response_json(res, 200, "{\"msg\":\"hello\"}", 15);
}

static void handle_param(KlRequest *req, KlResponse *res, void *ctx) {
    (void)ctx;
    size_t id_len;
    const char *id = kl_request_param(req, "id", &id_len);
    static char body[256]; /* static: body must outlive handler for async writev */
    int n;
    if (id) {
        n = snprintf(body, sizeof(body),
                     "{\"id\":\"%.*s\",\"num_params\":%d}",
                     (int)id_len, id, req->num_params);
    } else {
        n = snprintf(body, sizeof(body), "{\"id\":null,\"num_params\":0}");
    }
    if (n < 0) n = 0;
    kl_response_json(res, 200, body, (size_t)n);
}

static void handle_echo(KlRequest *req, KlResponse *res, void *ctx) {
    (void)ctx;
    KlBufReader *br = (KlBufReader *)req->body_reader;
    kl_response_status(res, 200);
    kl_response_header(res, "Content-Type", "text/plain");
    if (br && br->len > 0) {
        kl_response_body(res, br->data, br->len);
    } else {
        kl_response_body(res, "no body", 7);
    }
}

/* Handler that reports multipart parts as JSON */
static void handle_upload(KlRequest *req, KlResponse *res, void *ctx) {
    (void)ctx;
    KlMultipartReader *mr = (KlMultipartReader *)req->body_reader;
    if (!mr || mr->num_parts == 0) {
        kl_response_error(res, 400, "No parts");
        return;
    }

    /* Simple response: "parts=N name0=... len0=..." */
    static char body[1024];  /* static: body must outlive handler for async writev */
    int off = snprintf(body, sizeof(body), "parts=%d", mr->num_parts);
    for (int i = 0; i < mr->num_parts && off < (int)sizeof(body) - 64; i++) {
        off += snprintf(body + off, sizeof(body) - (size_t)off,
                        " name%d=%s len%d=%zu",
                        i, mr->parts[i].name, i, mr->parts[i].data_len);
    }

    kl_response_status(res, 200);
    kl_response_header(res, "Content-Type", "text/plain");
    kl_response_body(res, body, (size_t)off);
}

static KlServer test_server;

static void *server_thread(void *arg) {
    (void)arg;
    kl_server_run(&test_server);
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
        close(fd);
        return -1;
    }
    return fd;
}

/* Read full response (until EOF or read returns 0) */
static ssize_t read_response(int fd, char *buf, size_t buflen) {
    ssize_t total = 0;
    ssize_t n;
    while ((n = read(fd, buf + total,
                     buflen - (size_t)total - 1)) > 0) {
        total += n;
    }
    buf[total] = '\0';
    return total;
}

/* ── Server lifecycle ───────────────────────────────────────────────── */

static pthread_t server_tid;

static void start_server(void) {
    KlConfig cfg = {.port = TEST_PORT};
    kl_server_init(&test_server, &cfg);
    kl_server_route(&test_server, "GET", "/hello", handle_hello, NULL, NULL);
    kl_server_route(&test_server, "POST", "/echo", handle_echo,
                    (void *)(size_t)(64 * 1024), /* 64KB max */
                    kl_body_reader_buffer);
    kl_server_route(&test_server, "POST", "/upload", handle_upload,
                    NULL, kl_body_reader_multipart);
    kl_server_route(&test_server, "GET", "/users/:id", handle_param,
                    NULL, NULL);
    pthread_create(&server_tid, NULL, server_thread, NULL);
    usleep(100000);
}

static void stop_server(void) {
    kl_server_stop(&test_server);
    pthread_join(server_tid, NULL);
    kl_server_free(&test_server);
}

/* ── Tests ──────────────────────────────────────────────────────────── */

UTEST(integration, hello_world) {
    start_server();

    int fd = connect_to(TEST_PORT);
    ASSERT_TRUE(fd >= 0);

    const char *req = "GET /hello HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Connection: close\r\n"
                      "\r\n";
    (void)write(fd, req, strlen(req));

    char buf[4096];
    read_response(fd, buf, sizeof(buf));
    close(fd);

    ASSERT_TRUE(strstr(buf, "HTTP/1.1 200 OK") != NULL);
    ASSERT_TRUE(strstr(buf, "{\"msg\":\"hello\"}") != NULL);

    stop_server();
}

UTEST(integration, post_large_body) {
    start_server();

    int fd = connect_to(TEST_PORT);
    ASSERT_TRUE(fd >= 0);

    /* Build a body > 8KB to exercise the READING_BODY sliding window */
    size_t body_len = 16384;
    char *body = malloc(body_len);
    ASSERT_TRUE(body != NULL);
    memset(body, 'X', body_len);

    char hdr[256];
    snprintf(hdr, sizeof(hdr),
             "POST /echo HTTP/1.1\r\n"
             "Host: localhost\r\n"
             "Content-Length: %zu\r\n"
             "Connection: close\r\n"
             "\r\n", body_len);
    (void)write(fd, hdr, strlen(hdr));
    (void)write(fd, body, body_len);

    char buf[32768];
    ssize_t total = read_response(fd, buf, sizeof(buf));
    close(fd);

    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);

    /* Find body start (after \r\n\r\n) */
    char *body_start = strstr(buf, "\r\n\r\n");
    ASSERT_TRUE(body_start != NULL);
    body_start += 4;
    size_t resp_body_len = (size_t)(total - (body_start - buf));
    ASSERT_EQ(resp_body_len, body_len);
    ASSERT_TRUE(memcmp(body_start, body, body_len) == 0);

    free(body);
    stop_server();
}

UTEST(integration, post_413) {
    start_server();

    int fd = connect_to(TEST_PORT);
    ASSERT_TRUE(fd >= 0);

    /* Send POST exceeding 64KB limit */
    const char *hdr = "POST /echo HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Content-Length: 100000\r\n"
                      "Connection: close\r\n"
                      "\r\n";
    (void)write(fd, hdr, strlen(hdr));

    /* Send some data to trigger the body reader */
    char chunk[8192];
    memset(chunk, 'A', sizeof(chunk));
    for (int i = 0; i < 15; i++)
        (void)write(fd, chunk, sizeof(chunk));

    char buf[4096];
    read_response(fd, buf, sizeof(buf));
    close(fd);

    ASSERT_TRUE(strstr(buf, "413") != NULL);

    stop_server();
}

UTEST(integration, keepalive_post) {
    start_server();

    int fd = connect_to(TEST_PORT);
    ASSERT_TRUE(fd >= 0);

    /* First POST on keep-alive connection */
    const char *req1 = "POST /echo HTTP/1.1\r\n"
                       "Host: localhost\r\n"
                       "Content-Length: 5\r\n"
                       "\r\n"
                       "first";
    (void)write(fd, req1, strlen(req1));

    /* Read first response — need to parse Content-Length to know when done */
    char buf[8192];
    ssize_t total = 0;
    ssize_t n;
    /* Read until we have the full first response */
    while ((n = read(fd, buf + total, sizeof(buf) - (size_t)total - 1)) > 0) {
        total += n;
        buf[total] = '\0';
        /* Check if we have a complete response */
        char *body_start = strstr(buf, "\r\n\r\n");
        if (body_start) {
            body_start += 4;
            char *cl = strstr(buf, "Content-Length:");
            if (cl) {
                size_t cl_val = (size_t)strtol(cl + 15, NULL, 10);
                size_t body_received = (size_t)(total - (body_start - buf));
                if (body_received >= cl_val) break;
            }
        }
    }

    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);
    ASSERT_TRUE(strstr(buf, "first") != NULL);

    /* Second POST on same connection */
    const char *req2 = "POST /echo HTTP/1.1\r\n"
                       "Host: localhost\r\n"
                       "Content-Length: 6\r\n"
                       "Connection: close\r\n"
                       "\r\n"
                       "second";
    (void)write(fd, req2, strlen(req2));

    char buf2[8192];
    read_response(fd, buf2, sizeof(buf2));
    close(fd);

    ASSERT_TRUE(strstr(buf2, "200 OK") != NULL);
    ASSERT_TRUE(strstr(buf2, "second") != NULL);

    stop_server();
}

UTEST(integration, empty_body) {
    start_server();

    int fd = connect_to(TEST_PORT);
    ASSERT_TRUE(fd >= 0);

    const char *req = "POST /echo HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Content-Length: 0\r\n"
                      "Connection: close\r\n"
                      "\r\n";
    (void)write(fd, req, strlen(req));

    char buf[4096];
    read_response(fd, buf, sizeof(buf));
    close(fd);

    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);
    ASSERT_TRUE(strstr(buf, "no body") != NULL);

    stop_server();
}

UTEST(integration, expect_100_continue) {
    start_server();

    int fd = connect_to(TEST_PORT);
    ASSERT_TRUE(fd >= 0);

    /* Send headers with Expect: 100-continue */
    const char *hdr = "POST /echo HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Content-Length: 11\r\n"
                      "Expect: 100-continue\r\n"
                      "Connection: close\r\n"
                      "\r\n";
    (void)write(fd, hdr, strlen(hdr));

    /* Read the 100 Continue interim response */
    char buf[4096];
    ssize_t n = 0;
    /* Wait for 100 Continue */
    for (int i = 0; i < 20 && n == 0; i++) {
        usleep(50000);
        ssize_t r = read(fd, buf + n, sizeof(buf) - (size_t)n - 1);
        if (r > 0) n += r;
    }
    buf[n] = '\0';
    ASSERT_TRUE(strstr(buf, "100 Continue") != NULL);

    /* Now send the body */
    (void)write(fd, "hello world", 11);

    /* Read the final response */
    char buf2[4096];
    ssize_t total = read_response(fd, buf2, sizeof(buf2));
    close(fd);
    (void)total;

    ASSERT_TRUE(strstr(buf2, "200 OK") != NULL);
    ASSERT_TRUE(strstr(buf2, "hello world") != NULL);

    stop_server();
}

UTEST(integration, head_request) {
    start_server();

    int fd = connect_to(TEST_PORT);
    ASSERT_TRUE(fd >= 0);

    const char *req = "HEAD /hello HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Connection: close\r\n"
                      "\r\n";
    (void)write(fd, req, strlen(req));

    char buf[4096];
    read_response(fd, buf, sizeof(buf));
    close(fd);

    /* Should get 200 with Content-Length but no body */
    ASSERT_TRUE(strstr(buf, "HTTP/1.1 200 OK") != NULL);
    ASSERT_TRUE(strstr(buf, "Content-Length: 15") != NULL);
    ASSERT_TRUE(strstr(buf, "application/json") != NULL);

    /* No body after headers */
    char *body_start = strstr(buf, "\r\n\r\n");
    ASSERT_TRUE(body_start != NULL);
    body_start += 4;
    ASSERT_EQ(strlen(body_start), (size_t)0);

    stop_server();
}

/* ── Access log test ────────────────────────────────────────────────── */

typedef struct {
    char method[16];
    char path[64];
    int status;
    size_t body_bytes;
    double duration_ms;
    int call_count;
} AccessLogCapture;

static void test_access_log_fn(const KlRequest *req, int status,
                                size_t body_bytes, double duration_ms,
                                void *user_data) {
    AccessLogCapture *cap = user_data;
    size_t mlen = req->method_len < sizeof(cap->method) - 1
                  ? req->method_len : sizeof(cap->method) - 1;
    memcpy(cap->method, req->method, mlen);
    cap->method[mlen] = '\0';
    size_t plen = req->path_len < sizeof(cap->path) - 1
                  ? req->path_len : sizeof(cap->path) - 1;
    memcpy(cap->path, req->path, plen);
    cap->path[plen] = '\0';
    cap->status = status;
    cap->body_bytes = body_bytes;
    cap->duration_ms = duration_ms;
    cap->call_count++;
}

static KlServer log_server;
static AccessLogCapture log_capture;

static void *log_server_thread(void *arg) {
    (void)arg;
    kl_server_run(&log_server);
    return NULL;
}

#define LOG_TEST_PORT 18081

UTEST(integration, access_log) {
    memset(&log_capture, 0, sizeof(log_capture));
    KlConfig cfg = {
        .port = LOG_TEST_PORT,
        .access_log = test_access_log_fn,
        .access_log_data = &log_capture,
    };
    kl_server_init(&log_server, &cfg);
    kl_server_route(&log_server, "GET", "/hello", handle_hello, NULL, NULL);

    pthread_t tid;
    pthread_create(&tid, NULL, log_server_thread, NULL);
    usleep(100000);

    int fd = connect_to(LOG_TEST_PORT);
    ASSERT_TRUE(fd >= 0);

    const char *req = "GET /hello HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Connection: close\r\n"
                      "\r\n";
    (void)write(fd, req, strlen(req));

    char buf[4096];
    read_response(fd, buf, sizeof(buf));
    close(fd);

    /* Wait briefly for the log callback to fire (it fires after send completes) */
    usleep(50000);

    kl_server_stop(&log_server);
    pthread_join(tid, NULL);
    kl_server_free(&log_server);

    ASSERT_EQ(1, log_capture.call_count);
    ASSERT_STREQ("GET", log_capture.method);
    ASSERT_STREQ("/hello", log_capture.path);
    ASSERT_EQ(200, log_capture.status);
    ASSERT_EQ((size_t)15, log_capture.body_bytes);
    ASSERT_TRUE(log_capture.duration_ms >= 0.0);
}

UTEST(integration, multipart_upload) {
    start_server();

    int fd = connect_to(TEST_PORT);
    ASSERT_TRUE(fd >= 0);

    const char *body =
        "--TESTBND\r\n"
        "Content-Disposition: form-data; name=\"field1\"\r\n"
        "\r\n"
        "value1"
        "\r\n--TESTBND\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"test.txt\"\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "file data here"
        "\r\n--TESTBND--\r\n";
    size_t body_len = strlen(body);

    char hdr[512];
    snprintf(hdr, sizeof(hdr),
             "POST /upload HTTP/1.1\r\n"
             "Host: localhost\r\n"
             "Content-Type: multipart/form-data; boundary=TESTBND\r\n"
             "Content-Length: %zu\r\n"
             "Connection: close\r\n"
             "\r\n", body_len);
    (void)write(fd, hdr, strlen(hdr));
    (void)write(fd, body, body_len);

    char buf[4096];
    read_response(fd, buf, sizeof(buf));
    close(fd);

    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);
    ASSERT_TRUE(strstr(buf, "parts=2") != NULL);
    ASSERT_TRUE(strstr(buf, "name0=field1") != NULL);
    ASSERT_TRUE(strstr(buf, "name1=file") != NULL);
    ASSERT_TRUE(strstr(buf, "len1=14") != NULL);

    stop_server();
}

/* ── Chunked transfer-encoding tests ────────────────────────────────── */

UTEST(integration, chunked_post) {
    start_server();

    int fd = connect_to(TEST_PORT);
    ASSERT_TRUE(fd >= 0);

    const char *req = "POST /echo HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Transfer-Encoding: chunked\r\n"
                      "Connection: close\r\n"
                      "\r\n"
                      "5\r\nhello\r\n0\r\n\r\n";
    (void)write(fd, req, strlen(req));

    char buf[4096];
    read_response(fd, buf, sizeof(buf));
    close(fd);

    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);
    ASSERT_TRUE(strstr(buf, "hello") != NULL);

    stop_server();
}

UTEST(integration, chunked_multi_chunk) {
    start_server();

    int fd = connect_to(TEST_PORT);
    ASSERT_TRUE(fd >= 0);

    const char *req = "POST /echo HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Transfer-Encoding: chunked\r\n"
                      "Connection: close\r\n"
                      "\r\n"
                      "5\r\nhello\r\n"
                      "1\r\n \r\n"
                      "5\r\nworld\r\n"
                      "0\r\n\r\n";
    (void)write(fd, req, strlen(req));

    char buf[4096];
    read_response(fd, buf, sizeof(buf));
    close(fd);

    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);
    ASSERT_TRUE(strstr(buf, "hello world") != NULL);

    stop_server();
}

UTEST(integration, chunked_keepalive) {
    start_server();

    int fd = connect_to(TEST_PORT);
    ASSERT_TRUE(fd >= 0);

    /* First request: chunked POST */
    const char *req1 = "POST /echo HTTP/1.1\r\n"
                       "Host: localhost\r\n"
                       "Transfer-Encoding: chunked\r\n"
                       "\r\n"
                       "5\r\nfirst\r\n0\r\n\r\n";
    (void)write(fd, req1, strlen(req1));

    /* Read first response */
    char buf[8192];
    ssize_t total = 0;
    ssize_t n;
    while ((n = read(fd, buf + total, sizeof(buf) - (size_t)total - 1)) > 0) {
        total += n;
        buf[total] = '\0';
        char *body_start = strstr(buf, "\r\n\r\n");
        if (body_start) {
            body_start += 4;
            char *cl = strstr(buf, "Content-Length:");
            if (cl) {
                size_t cl_val = (size_t)strtol(cl + 15, NULL, 10);
                size_t body_received = (size_t)(total - (body_start - buf));
                if (body_received >= cl_val) break;
            }
        }
    }

    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);
    ASSERT_TRUE(strstr(buf, "first") != NULL);

    /* Second request: plain GET on same connection */
    const char *req2 = "GET /hello HTTP/1.1\r\n"
                       "Host: localhost\r\n"
                       "Connection: close\r\n"
                       "\r\n";
    (void)write(fd, req2, strlen(req2));

    char buf2[4096];
    read_response(fd, buf2, sizeof(buf2));
    close(fd);

    ASSERT_TRUE(strstr(buf2, "200 OK") != NULL);
    ASSERT_TRUE(strstr(buf2, "{\"msg\":\"hello\"}") != NULL);

    stop_server();
}

UTEST(integration, chunked_100_continue) {
    start_server();

    int fd = connect_to(TEST_PORT);
    ASSERT_TRUE(fd >= 0);

    /* Send headers with Expect: 100-continue and chunked TE */
    const char *hdr = "POST /echo HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Transfer-Encoding: chunked\r\n"
                      "Expect: 100-continue\r\n"
                      "Connection: close\r\n"
                      "\r\n";
    (void)write(fd, hdr, strlen(hdr));

    /* Wait for 100 Continue */
    char buf[4096];
    ssize_t rn = 0;
    for (int i = 0; i < 20 && rn == 0; i++) {
        usleep(50000);
        ssize_t r = read(fd, buf + rn, sizeof(buf) - (size_t)rn - 1);
        if (r > 0) rn += r;
    }
    buf[rn] = '\0';
    ASSERT_TRUE(strstr(buf, "100 Continue") != NULL);

    /* Send chunked body */
    const char *body = "b\r\nhello world\r\n0\r\n\r\n";
    (void)write(fd, body, strlen(body));

    char buf2[4096];
    read_response(fd, buf2, sizeof(buf2));
    close(fd);

    ASSERT_TRUE(strstr(buf2, "200 OK") != NULL);
    ASSERT_TRUE(strstr(buf2, "hello world") != NULL);

    stop_server();
}

UTEST(integration, chunked_empty_body) {
    start_server();

    int fd = connect_to(TEST_PORT);
    ASSERT_TRUE(fd >= 0);

    const char *req = "POST /echo HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Transfer-Encoding: chunked\r\n"
                      "Connection: close\r\n"
                      "\r\n"
                      "0\r\n\r\n";
    (void)write(fd, req, strlen(req));

    char buf[4096];
    read_response(fd, buf, sizeof(buf));
    close(fd);

    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);
    ASSERT_TRUE(strstr(buf, "no body") != NULL);

    stop_server();
}

UTEST(integration, chunked_large_body) {
    start_server();

    int fd = connect_to(TEST_PORT);
    ASSERT_TRUE(fd >= 0);

    /* Send headers */
    const char *hdr = "POST /echo HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Transfer-Encoding: chunked\r\n"
                      "Connection: close\r\n"
                      "\r\n";
    (void)write(fd, hdr, strlen(hdr));

    /* Send 16KB in 1KB chunks */
    char chunk_data[1024];
    memset(chunk_data, 'Z', sizeof(chunk_data));

    for (int i = 0; i < 16; i++) {
        char chunk_hdr[16];
        int hlen = snprintf(chunk_hdr, sizeof(chunk_hdr), "400\r\n");
        (void)write(fd, chunk_hdr, (size_t)hlen);
        (void)write(fd, chunk_data, sizeof(chunk_data));
        (void)write(fd, "\r\n", 2);
    }
    (void)write(fd, "0\r\n\r\n", 5);

    char buf[32768];
    ssize_t total = read_response(fd, buf, sizeof(buf));
    close(fd);

    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);

    /* Find body start */
    char *body_start = strstr(buf, "\r\n\r\n");
    ASSERT_TRUE(body_start != NULL);
    body_start += 4;
    size_t resp_body_len = (size_t)(total - (body_start - buf));
    ASSERT_EQ(resp_body_len, (size_t)(16 * 1024));

    /* Verify all bytes are 'Z' */
    int all_z = 1;
    for (size_t j = 0; j < resp_body_len; j++) {
        if (body_start[j] != 'Z') { all_z = 0; break; }
    }
    ASSERT_TRUE(all_z);

    stop_server();
}

/* ── Signal handling test ───────────────────────────────────────────── */

static KlServer signal_server;

static void *signal_server_thread(void *arg) {
    (void)arg;
    kl_server_run(&signal_server);
    return NULL;
}

#define SIGNAL_TEST_PORT 18082

UTEST(integration, signal_stop) {
    KlConfig cfg = {
        .port = SIGNAL_TEST_PORT,
        .install_signal_handlers = 1,
    };
    kl_server_init(&signal_server, &cfg);
    kl_server_route(&signal_server, "GET", "/hello", handle_hello, NULL, NULL);

    pthread_t tid;
    pthread_create(&tid, NULL, signal_server_thread, NULL);
    usleep(100000);

    /* Verify server is up */
    int fd = connect_to(SIGNAL_TEST_PORT);
    ASSERT_TRUE(fd >= 0);
    const char *req = "GET /hello HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Connection: close\r\n"
                      "\r\n";
    (void)write(fd, req, strlen(req));
    char buf[4096];
    read_response(fd, buf, sizeof(buf));
    close(fd);
    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);

    /* Send SIGTERM to ourselves — handler should stop the server */
    kill(getpid(), SIGTERM);
    pthread_join(tid, NULL);
    kl_server_free(&signal_server);

    /* If we get here, the server exited cleanly */
    ASSERT_TRUE(1);
}

/* ── Middleware integration tests ────────────────────────────────────── */

static int cors_middleware(KlRequest *req, KlResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_response_header(res, "Access-Control-Allow-Origin", "*");
    return 0;
}

static int auth_middleware(KlRequest *req, KlResponse *res, void *ctx) {
    const char *secret = ctx;
    size_t tok_len;
    const char *tok = kl_request_header_len(req, "Authorization", &tok_len);
    size_t secret_len = strlen(secret);
    if (!tok || tok_len != secret_len || memcmp(tok, secret, secret_len) != 0) {
        kl_response_error(res, 401, "Unauthorized");
        return 1;
    }
    return 0;
}

static void handle_mw_hello(KlRequest *req, KlResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_response_json(res, 200, "{\"ok\":true}", 11);
}

static void handle_mw_echo(KlRequest *req, KlResponse *res, void *ctx) {
    (void)ctx;
    KlBufReader *br = (KlBufReader *)req->body_reader;
    kl_response_status(res, 200);
    kl_response_header(res, "Content-Type", "text/plain");
    if (br && br->len > 0)
        kl_response_body(res, br->data, br->len);
    else
        kl_response_body(res, "no body", 7);
}

static KlServer mw_server;

static void *mw_server_thread(void *arg) {
    (void)arg;
    kl_server_run(&mw_server);
    return NULL;
}

#define MW_TEST_PORT 18083

static pthread_t mw_server_tid;

static void start_mw_server(void) {
    KlConfig cfg = {.port = MW_TEST_PORT};
    kl_server_init(&mw_server, &cfg);

    /* Register middleware */
    kl_server_use(&mw_server, "*", "/*", cors_middleware, NULL);
    kl_server_use(&mw_server, "GET", "/api/*", auth_middleware, (void *)"secret123");

    /* Register routes */
    kl_server_route(&mw_server, "GET", "/hello", handle_mw_hello, NULL, NULL);
    kl_server_route(&mw_server, "GET", "/api/data", handle_mw_hello, NULL, NULL);
    kl_server_route(&mw_server, "POST", "/api/echo", handle_mw_echo,
                    (void *)(size_t)(64 * 1024), kl_body_reader_buffer);

    pthread_create(&mw_server_tid, NULL, mw_server_thread, NULL);
    usleep(100000);
}

static void stop_mw_server(void) {
    kl_server_stop(&mw_server);
    pthread_join(mw_server_tid, NULL);
    kl_server_free(&mw_server);
}

UTEST(integration, middleware_cors) {
    start_mw_server();

    int fd = connect_to(MW_TEST_PORT);
    ASSERT_TRUE(fd >= 0);

    const char *req = "GET /hello HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Connection: close\r\n"
                      "\r\n";
    (void)write(fd, req, strlen(req));

    char buf[4096];
    read_response(fd, buf, sizeof(buf));
    close(fd);

    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);
    ASSERT_TRUE(strstr(buf, "Access-Control-Allow-Origin: *") != NULL);
    ASSERT_TRUE(strstr(buf, "{\"ok\":true}") != NULL);

    stop_mw_server();
}

UTEST(integration, middleware_auth_reject) {
    start_mw_server();

    int fd = connect_to(MW_TEST_PORT);
    ASSERT_TRUE(fd >= 0);

    /* No Authorization header → should get 401 */
    const char *req = "GET /api/data HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Connection: close\r\n"
                      "\r\n";
    (void)write(fd, req, strlen(req));

    char buf[4096];
    read_response(fd, buf, sizeof(buf));
    close(fd);

    ASSERT_TRUE(strstr(buf, "401") != NULL);
    /* CORS header should still be present (runs before auth) */
    ASSERT_TRUE(strstr(buf, "Access-Control-Allow-Origin: *") != NULL);

    stop_mw_server();
}

UTEST(integration, middleware_auth_pass) {
    start_mw_server();

    int fd = connect_to(MW_TEST_PORT);
    ASSERT_TRUE(fd >= 0);

    const char *req = "GET /api/data HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Authorization: secret123\r\n"
                      "Connection: close\r\n"
                      "\r\n";
    (void)write(fd, req, strlen(req));

    char buf[4096];
    read_response(fd, buf, sizeof(buf));
    close(fd);

    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);
    ASSERT_TRUE(strstr(buf, "Access-Control-Allow-Origin: *") != NULL);
    ASSERT_TRUE(strstr(buf, "{\"ok\":true}") != NULL);

    stop_mw_server();
}

static int mw_add_x_first(KlRequest *req, KlResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_response_header(res, "X-First", "1");
    return 0;
}

static int mw_add_x_second(KlRequest *req, KlResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_response_header(res, "X-Second", "2");
    return 0;
}

static KlServer chain_server;

static void *chain_server_thread(void *arg) {
    (void)arg;
    kl_server_run(&chain_server);
    return NULL;
}

#define CHAIN_TEST_PORT 18084

UTEST(integration, middleware_chain_order) {
    KlConfig cfg = {.port = CHAIN_TEST_PORT};
    kl_server_init(&chain_server, &cfg);
    kl_server_use(&chain_server, "*", "/*", mw_add_x_first, NULL);
    kl_server_use(&chain_server, "*", "/*", mw_add_x_second, NULL);
    kl_server_route(&chain_server, "GET", "/hello", handle_mw_hello, NULL, NULL);

    pthread_t tid;
    pthread_create(&tid, NULL, chain_server_thread, NULL);
    usleep(100000);

    int fd = connect_to(CHAIN_TEST_PORT);
    ASSERT_TRUE(fd >= 0);

    const char *req = "GET /hello HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Connection: close\r\n"
                      "\r\n";
    (void)write(fd, req, strlen(req));

    char buf[4096];
    read_response(fd, buf, sizeof(buf));
    close(fd);

    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);
    ASSERT_TRUE(strstr(buf, "X-First: 1") != NULL);
    ASSERT_TRUE(strstr(buf, "X-Second: 2") != NULL);

    /* Verify order: X-First appears before X-Second in response */
    char *p1 = strstr(buf, "X-First");
    char *p2 = strstr(buf, "X-Second");
    ASSERT_TRUE(p1 != NULL);
    ASSERT_TRUE(p2 != NULL);
    ASSERT_TRUE(p1 < p2);

    kl_server_stop(&chain_server);
    pthread_join(tid, NULL);
    kl_server_free(&chain_server);
}

static KlServer sc_body_server;

static void *sc_body_server_thread(void *arg) {
    (void)arg;
    kl_server_run(&sc_body_server);
    return NULL;
}

#define SC_BODY_TEST_PORT 18085

static int mw_reject_all(KlRequest *req, KlResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_response_error(res, 403, "Forbidden");
    return 1;
}

UTEST(integration, middleware_short_circuit_body) {
    KlConfig cfg = {.port = SC_BODY_TEST_PORT};
    kl_server_init(&sc_body_server, &cfg);
    kl_server_use(&sc_body_server, "*", "/*", mw_reject_all, NULL);
    kl_server_route(&sc_body_server, "POST", "/echo", handle_mw_echo,
                    (void *)(size_t)(64 * 1024), kl_body_reader_buffer);

    pthread_t tid;
    pthread_create(&tid, NULL, sc_body_server_thread, NULL);
    usleep(100000);

    int fd = connect_to(SC_BODY_TEST_PORT);
    ASSERT_TRUE(fd >= 0);

    /* POST with body — middleware should reject before body is read */
    const char *req = "POST /echo HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Content-Length: 5\r\n"
                      "Connection: close\r\n"
                      "\r\n"
                      "hello";
    (void)write(fd, req, strlen(req));

    char buf[4096];
    read_response(fd, buf, sizeof(buf));
    close(fd);

    ASSERT_TRUE(strstr(buf, "403") != NULL);

    kl_server_stop(&sc_body_server);
    pthread_join(tid, NULL);
    kl_server_free(&sc_body_server);
}

UTEST(integration, route_params) {
    start_server();

    int fd = connect_to(TEST_PORT);
    ASSERT_TRUE(fd >= 0);

    const char *req = "GET /users/42 HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Connection: close\r\n"
                      "\r\n";
    (void)write(fd, req, strlen(req));

    char buf[4096];
    read_response(fd, buf, sizeof(buf));
    close(fd);

    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);
    ASSERT_TRUE(strstr(buf, "\"id\":\"42\"") != NULL);
    ASSERT_TRUE(strstr(buf, "\"num_params\":1") != NULL);

    stop_server();
}

/* ── Post-body middleware integration tests ──────────────────────────── */

static int post_mw_check_body(KlRequest *req, KlResponse *res, void *ctx) {
    (void)ctx;
    KlBufReader *br = (KlBufReader *)req->body_reader;
    if (br && br->len >= 6 && memcmp(br->data, "secret", 6) == 0) {
        return 0;  /* continue to handler */
    }
    kl_response_error(res, 403, "Forbidden");
    return 1;
}

static void handle_post_mw_echo(KlRequest *req, KlResponse *res, void *ctx) {
    (void)ctx;
    KlBufReader *br = (KlBufReader *)req->body_reader;
    kl_response_status(res, 200);
    kl_response_header(res, "Content-Type", "text/plain");
    if (br && br->len > 0)
        kl_response_body(res, br->data, br->len);
    else
        kl_response_body(res, "no body", 7);
}

static KlServer post_mw_server;

static void *post_mw_server_thread(void *arg) {
    (void)arg;
    kl_server_run(&post_mw_server);
    return NULL;
}

#define POST_MW_TEST_PORT 18086

UTEST(integration, post_middleware_body_access) {
    KlConfig cfg = {.port = POST_MW_TEST_PORT};
    kl_server_init(&post_mw_server, &cfg);
    kl_server_use_post(&post_mw_server, "POST", "/*", post_mw_check_body, NULL);
    kl_server_route(&post_mw_server, "POST", "/submit", handle_post_mw_echo,
                    (void *)(size_t)(64 * 1024), kl_body_reader_buffer);
    kl_server_route(&post_mw_server, "GET", "/hello", handle_hello, NULL, NULL);

    pthread_t tid;
    pthread_create(&tid, NULL, post_mw_server_thread, NULL);
    usleep(100000);

    /* POST with valid body — should pass post-middleware */
    int fd = connect_to(POST_MW_TEST_PORT);
    ASSERT_TRUE(fd >= 0);
    const char *req1 = "POST /submit HTTP/1.1\r\n"
                       "Host: localhost\r\n"
                       "Content-Length: 11\r\n"
                       "Connection: close\r\n"
                       "\r\n"
                       "secret data";
    (void)write(fd, req1, strlen(req1));
    char buf[4096];
    read_response(fd, buf, sizeof(buf));
    close(fd);
    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);
    ASSERT_TRUE(strstr(buf, "secret data") != NULL);

    /* POST with invalid body — should be rejected by post-middleware */
    fd = connect_to(POST_MW_TEST_PORT);
    ASSERT_TRUE(fd >= 0);
    const char *req2 = "POST /submit HTTP/1.1\r\n"
                       "Host: localhost\r\n"
                       "Content-Length: 5\r\n"
                       "Connection: close\r\n"
                       "\r\n"
                       "wrong";
    (void)write(fd, req2, strlen(req2));
    read_response(fd, buf, sizeof(buf));
    close(fd);
    ASSERT_TRUE(strstr(buf, "403") != NULL);

    /* GET skips POST-only post-middleware — should succeed */
    fd = connect_to(POST_MW_TEST_PORT);
    ASSERT_TRUE(fd >= 0);
    const char *req3 = "GET /hello HTTP/1.1\r\n"
                       "Host: localhost\r\n"
                       "Connection: close\r\n"
                       "\r\n";
    (void)write(fd, req3, strlen(req3));
    read_response(fd, buf, sizeof(buf));
    close(fd);
    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);

    kl_server_stop(&post_mw_server);
    pthread_join(tid, NULL);
    kl_server_free(&post_mw_server);
}

static KlServer post_mw_ka_server;

static void *post_mw_ka_server_thread(void *arg) {
    (void)arg;
    kl_server_run(&post_mw_ka_server);
    return NULL;
}

#define POST_MW_KA_PORT 18087

UTEST(integration, post_middleware_keepalive_preserved) {
    KlConfig cfg = {.port = POST_MW_KA_PORT};
    kl_server_init(&post_mw_ka_server, &cfg);
    kl_server_use_post(&post_mw_ka_server, "POST", "/*", post_mw_check_body, NULL);
    kl_server_route(&post_mw_ka_server, "POST", "/submit", handle_post_mw_echo,
                    (void *)(size_t)(64 * 1024), kl_body_reader_buffer);
    kl_server_route(&post_mw_ka_server, "GET", "/hello", handle_hello, NULL, NULL);

    pthread_t tid;
    pthread_create(&tid, NULL, post_mw_ka_server_thread, NULL);
    usleep(100000);

    /* Post-middleware rejection should preserve keep-alive */
    int fd = connect_to(POST_MW_KA_PORT);
    ASSERT_TRUE(fd >= 0);

    /* First: POST rejected by post-middleware (body consumed → keep-alive OK) */
    const char *req1 = "POST /submit HTTP/1.1\r\n"
                       "Host: localhost\r\n"
                       "Content-Length: 5\r\n"
                       "\r\n"
                       "wrong";
    (void)write(fd, req1, strlen(req1));

    /* Read first response */
    char buf[8192];
    ssize_t total = 0;
    ssize_t n;
    while ((n = read(fd, buf + total, sizeof(buf) - (size_t)total - 1)) > 0) {
        total += n;
        buf[total] = '\0';
        char *body_start = strstr(buf, "\r\n\r\n");
        if (body_start) {
            body_start += 4;
            char *cl = strstr(buf, "Content-Length:");
            if (cl) {
                size_t cl_val = (size_t)strtol(cl + 15, NULL, 10);
                size_t body_received = (size_t)(total - (body_start - buf));
                if (body_received >= cl_val) break;
            }
        }
    }
    ASSERT_TRUE(strstr(buf, "403") != NULL);

    /* Second: GET on same connection — should work if keep-alive preserved */
    const char *req2 = "GET /hello HTTP/1.1\r\n"
                       "Host: localhost\r\n"
                       "Connection: close\r\n"
                       "\r\n";
    (void)write(fd, req2, strlen(req2));

    char buf2[4096];
    read_response(fd, buf2, sizeof(buf2));
    close(fd);
    ASSERT_TRUE(strstr(buf2, "200 OK") != NULL);
    ASSERT_TRUE(strstr(buf2, "{\"msg\":\"hello\"}") != NULL);

    kl_server_stop(&post_mw_ka_server);
    pthread_join(tid, NULL);
    kl_server_free(&post_mw_ka_server);
}

UTEST_MAIN();
