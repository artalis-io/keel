/*
 * test_completion_output.c: server output on a completion loop never blocks the loop, and goes out in
 * order and in full.
 *
 * On a completion loop the TLS output already leaves through one ordered, overlapped per-connection
 * queue. Plaintext output did not: a streamed response, WebSocket frames and HTTP/2 frames were sent
 * with a synchronous send on the loop thread, and io_uring and IOCP accepted sockets are blocking, so
 * a client that stops reading stalled every connection on the loop. Readiness loops use WRITE
 * interest and pass these tests either way; they exist for the completion lanes.
 */
#include "utest.h"
#include <keel/keel.h>
#include <keel/tls.h>
#include "net_compat.h"
#include "mock_tls.h"
#include <string.h>
#include "platform_thread.h"

static void handle_hello(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_http_response_json(res, 200, "{\"ok\":true}", 11);
}

static void wait_for_bind(KlHttpServer *s) {
    for (int i = 0; i < 200 && s->bound_port == 0; i++) kl_test_sleep_ms(10);
}

static void server_thread_fn(void *arg) {
    kl_http_server_run((KlHttpServer *)arg);
}

/* A 4 KiB receive buffer set BEFORE connecting, so a client that then never reads stops the
 * server's sends quickly. rcvbuf 0 leaves the default. */
static int connect_rcvbuf(int port, int rcvbuf) {
    int fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (rcvbuf > 0)
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (const char *)&rcvbuf, sizeof rcvbuf);
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

/* Read until the peer closes, `timeout_ms` passes with nothing to read, or the buffer is full.
 * *closed is set when the read ended on the peer's close. */
static kl_ssize_t read_until_close(int fd, char *buf, size_t buflen, int timeout_ms, int *closed) {
    kl_ssize_t total = 0;
    *closed = 0;
    while (total < (kl_ssize_t)buflen - 1) {
        int pr = kl_test_poll1(fd, 0, timeout_ms);
        if (pr <= 0) break;
        kl_ssize_t n = kl_test_sockread(fd, buf + total, buflen - (size_t)total - 1);
        if (n == 0) { *closed = 1; break; }
        if (n < 0) break;
        total += n;
    }
    buf[total] = '\0';
    return total;
}

/* One /hello request on a fresh connection; 1 if it was answered within 2 s. */
static int hello_answered(int port) {
    char buf[1024];
    int closed = 0;
    buf[0] = '\0';
    int b = connect_rcvbuf(port, 0);
    if (b < 0) return 0;
    const char *rq = "GET /hello HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    (void)kl_test_sockwrite(b, rq, strlen(rq));
    (void)read_until_close(b, buf, sizeof buf, 2000, &closed);
    kl_test_closesock(b);
    return strstr(buf, "200 OK") != NULL;
}

/* ── A plaintext stream to a client that stops reading ────────────────────────────────────────── */

#define BIG_CHUNK  (64 * 1024)
#define BIG_CHUNKS 1024                                    /* 64 MiB: more than any socket buffers */
static char g_big_chunk[BIG_CHUNK];

static void handle_bigstream(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)ctx;
    KlHttpResponseWriteFn w = NULL;
    void *wc = NULL;
    if (kl_http_response_begin_stream(res, 200, &w, &wc) < 0) return;
    memset(g_big_chunk, 'P', sizeof g_big_chunk);
    for (int i = 0; i < BIG_CHUNKS; i++) w(wc, g_big_chunk, sizeof g_big_chunk);
    kl_http_response_end_stream(res);
}

static KlHttpServer st_srv;

UTEST(completion_output, a_stalled_stream_reader_does_not_block_other_connections) {
    KlHttpServerConfig cfg = { .port = 0, .max_connections = 8 };
    ASSERT_EQ(0, kl_http_server_init(&st_srv, &cfg));
    kl_http_server_route(&st_srv, "GET", "/big", handle_bigstream, NULL, NULL);
    kl_http_server_route(&st_srv, "GET", "/hello", handle_hello, NULL, NULL);
    KlPlatThread tid;
    kl_plat_thread_create(&tid, server_thread_fn, &st_srv);
    wait_for_bind(&st_srv);
    int port = st_srv.bound_port;

    int a = connect_rcvbuf(port, 4096);                    /* asks for 64 MiB, then reads nothing */
    if (a >= 0) {
        const char *rq = "GET /big HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
        (void)kl_test_sockwrite(a, rq, strlen(rq));
    }
    kl_test_sleep_ms(300);                                 /* the server is now stuck on A, or not */
    int answered = hello_answered(port);
    if (a >= 0) kl_test_closesock(a);                      /* unblocks a loop stuck on A */
    kl_http_server_stop(&st_srv);
    kl_plat_thread_join(&tid);
    kl_http_server_free(&st_srv);
    ASSERT_TRUE(answered);                                 /* was (completion): no answer in 2 s */
}

/* ── WebSocket frames to a client that stops reading ──────────────────────────────────────────── */

static void ws_open_big(KlWsServerConn *ws, void *ud) {
    (void)ud;
    memset(g_big_chunk, 'W', sizeof g_big_chunk);
    for (int i = 0; i < BIG_CHUNKS; i++)
        if (kl_ws_server_send_binary(ws, g_big_chunk, sizeof g_big_chunk) < 0) break;
}

static KlHttpServer ws_srv;

UTEST(completion_output, a_stalled_websocket_reader_does_not_block_other_connections) {
    KlHttpServerConfig cfg = { .port = 0, .max_connections = 8 };
    ASSERT_EQ(0, kl_http_server_init(&ws_srv, &cfg));
    KlWsServerConfig wcfg;
    kl_ws_server_config_init(&wcfg);
    wcfg.callbacks.on_open = ws_open_big;
    ASSERT_EQ(0, kl_http_server_ws_upgrade(&ws_srv, "/ws", &wcfg));
    kl_http_server_route(&ws_srv, "GET", "/hello", handle_hello, NULL, NULL);
    KlPlatThread tid;
    kl_plat_thread_create(&tid, server_thread_fn, &ws_srv);
    wait_for_bind(&ws_srv);
    int port = ws_srv.bound_port;

    int a = connect_rcvbuf(port, 4096);                    /* upgrades, is sent 64 MiB, reads nothing */
    if (a >= 0) {
        const char *rq = "GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                         "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                         "Sec-WebSocket-Version: 13\r\n\r\n";
        (void)kl_test_sockwrite(a, rq, strlen(rq));
    }
    kl_test_sleep_ms(300);
    int answered = hello_answered(port);
    if (a >= 0) kl_test_closesock(a);
    kl_http_server_stop(&ws_srv);
    kl_plat_thread_join(&tid);
    kl_http_server_free(&ws_srv);
    ASSERT_TRUE(answered);                                 /* was (completion): no answer in 2 s */
}

/* ── 100 Continue over TLS ────────────────────────────────────────────────────────────────────── */

static void handle_upload(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_http_response_json(res, 200, "{\"ok\":true}", 11);
}

static KlHttpServer ct_srv;

/* On a completion loop the 100 Continue went into the TLS engine's output and stayed there until the
 * final response: a client that waits for it before sending the body (as curl does, for a second)
 * waited for nothing. */
UTEST(completion_output, tls_100_continue_is_sent_before_the_body) {
    KlTlsConfig tls_cfg = { .ctx = NULL, .factory = mock_tls_create };
    KlHttpServerConfig cfg = { .port = 0, .tls = &tls_cfg, .max_connections = 8 };
    ASSERT_EQ(0, kl_http_server_init(&ct_srv, &cfg));
    kl_http_server_route(&ct_srv, "POST", "/up", handle_upload, NULL, kl_http_body_reader_buffer);
    KlPlatThread tid;
    kl_plat_thread_create(&tid, server_thread_fn, &ct_srv);
    wait_for_bind(&ct_srv);

    char interim[256], final[1024];
    int closed = 0;
    interim[0] = final[0] = '\0';
    int fd = connect_rcvbuf(ct_srv.bound_port, 0);
    if (fd >= 0) {
        const char *rq = "POST /up HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n"
                         "Expect: 100-continue\r\nConnection: close\r\n\r\n";
        (void)kl_test_sockwrite(fd, rq, strlen(rq));
        kl_ssize_t n = 0;                                  /* the interim response, before any body */
        if (kl_test_poll1(fd, 0, 1000) > 0)
            n = kl_test_sockread(fd, interim, sizeof interim - 1);
        interim[n > 0 ? n : 0] = '\0';
        (void)kl_test_sockwrite(fd, "hello", 5);
        (void)read_until_close(fd, final, sizeof final, 2000, &closed);
        kl_test_closesock(fd);
    }
    kl_http_server_stop(&ct_srv);
    kl_plat_thread_join(&tid);
    kl_http_server_free(&ct_srv);
    ASSERT_TRUE(strstr(interim, "100 Continue") != NULL);  /* was (completion): nothing in 1 s */
    ASSERT_TRUE(strstr(interim, "200 OK") != NULL || strstr(final, "200 OK") != NULL);
}

UTEST_MAIN();
