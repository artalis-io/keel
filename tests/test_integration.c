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

#define TEST_PORT 18080

static void handle_hello(KlRequest *req, KlResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_response_json(res, 200, "{\"msg\":\"hello\"}", 15);
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
    char body[1024];
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

UTEST_MAIN();
