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
#include <stdio.h>
#include <stdlib.h>
#include "platform_thread.h"
#include <stdio.h>

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

/* ── Auto-ping while a slow reader takes a WebSocket backlog ──────────────────────────────────── */

#define SLOW_CHUNKS 256                                    /* 16 MiB: seconds at the reader's pace */

static void ws_open_backlog(KlWsServerConn *ws, void *ud) {
    (void)ud;
    (void)kl_ws_server_enable_drain(ws, 0);                /* readiness: buffer it all (unlimited) */
    memset(g_big_chunk, 'S', sizeof g_big_chunk);
    for (int i = 0; i < SLOW_CHUNKS; i++)
        if (kl_ws_server_send_binary(ws, g_big_chunk, sizeof g_big_chunk) < 0) break;
}

static uint64_t g_sw_closed_ms;                            /* when the server let the connection go */
static int g_sw_close_code;                                /* and with which close code */
static void ws_close_backlog(KlWsServerConn *ws, uint16_t code, const char *reason, size_t len,
                             void *ud) {
    (void)ws; (void)reason; (void)len; (void)ud;
    if (g_sw_closed_ms == 0) { g_sw_closed_ms = kl_monotonic_ms(); g_sw_close_code = code; }
}

/* The client's view of the server's frames: headers parsed incrementally, payloads skipped, PINGs
 * answered with a masked PONG, a CLOSE noted. */
typedef struct {
    uint8_t  hdr[14];
    size_t   hdr_len, hdr_need;
    uint64_t remain;
    int      opcode;
    uint8_t  ctrl[125];
    size_t   ctrl_len;
    int      pings, close_seen;
} SlowWsReader;

static void slow_ws_feed(SlowWsReader *r, int fd, const uint8_t *p, size_t n) {
    while (n > 0) {
        if (r->remain == 0 && r->hdr_need == 0) { r->hdr_len = 0; r->hdr_need = 2; }
        if (r->hdr_len < r->hdr_need) {                    /* frame header */
            r->hdr[r->hdr_len++] = *p++;
            n--;
            if (r->hdr_len == 2) {
                unsigned l7 = r->hdr[1] & 0x7F;
                r->hdr_need = 2 + (l7 == 126 ? 2 : l7 == 127 ? 8 : 0);
            }
            if (r->hdr_len < r->hdr_need) continue;
            unsigned l7 = r->hdr[1] & 0x7F;
            uint64_t len = l7;
            if (l7 == 126) len = ((uint64_t)r->hdr[2] << 8) | r->hdr[3];
            if (l7 == 127) { len = 0; for (int i = 2; i < 10; i++) len = (len << 8) | r->hdr[i]; }
            r->opcode = r->hdr[0] & 0x0F;
            r->remain = len;
            r->ctrl_len = 0;
            r->hdr_need = 0;
            if (r->opcode == 0x8) r->close_seen = 1;
        } else {                                           /* payload */
            size_t take = r->remain < n ? (size_t)r->remain : n;
            if (r->opcode >= 0x8 && r->ctrl_len + take <= sizeof r->ctrl) {
                memcpy(r->ctrl + r->ctrl_len, p, take);
                r->ctrl_len += take;
            }
            r->remain -= take;
            p += take;
            n -= take;
        }
        if (r->hdr_need == 0 && r->remain == 0 && r->opcode == 0x9) {   /* a whole PING: PONG it */
            uint8_t pong[2 + 4 + 125];
            static const uint8_t mask[4] = {1, 2, 3, 4};
            pong[0] = 0x8A;
            pong[1] = (uint8_t)(0x80 | r->ctrl_len);
            memcpy(pong + 2, mask, 4);
            for (size_t i = 0; i < r->ctrl_len; i++) pong[6 + i] = r->ctrl[i] ^ mask[i & 3];
            (void)kl_test_sockwrite(fd, (const char *)pong, 6 + r->ctrl_len);
            r->pings++;
            r->opcode = 0;
        }
    }
}

static KlHttpServer sw_srv;

/* A client that reads a large backlog slowly, answering every ping it gets to, is alive. Its PONG
 * cannot arrive while the ping waits behind the backlog, and on a completion loop no receive is
 * even posted while the connection's output is queued: the connection was failed with Close 1001 at
 * the second ping interval and closed under the reader. The backlog moving is the answer.
 *
 * Not on Windows: its loopback takes the whole backlog into the kernel at once (one overlapped send
 * that completes at once on IOCP, non-blocking sends that never fill on WSAPoll), so the server
 * holds no backlog and sees an unanswered ping like any other (the ping is behind data it can no
 * longer see). Readiness elsewhere (the backlog waits in the drain), io_uring and pollcomp (bounded
 * socket buffers, the send moves in parts) are where it applies. */
UTEST(completion_output, a_slow_websocket_reader_answering_pings_is_kept_alive) {
#if defined(_WIN32)
    UTEST_SKIP("Windows loopback takes the whole backlog into the kernel at once");
#endif
    KlHttpServerConfig cfg = { .port = 0, .max_connections = 8 };
    ASSERT_EQ(0, kl_http_server_init(&sw_srv, &cfg));
    KlWsServerConfig wcfg;
    kl_ws_server_config_init(&wcfg);
    wcfg.callbacks.on_open = ws_open_backlog;
    wcfg.callbacks.on_close = ws_close_backlog;
    wcfg.ping_interval_ms = 100;
    g_sw_closed_ms = 0;
    g_sw_close_code = 0;
    uint64_t sw_start_ms = kl_monotonic_ms();
    ASSERT_EQ(0, kl_http_server_ws_upgrade(&sw_srv, "/ws", &wcfg));
    KlPlatThread tid;
    kl_plat_thread_create(&tid, server_thread_fn, &sw_srv);
    wait_for_bind(&sw_srv);

    SlowWsReader rd;
    memset(&rd, 0, sizeof rd);
    int closed = 0, upgraded = 0;
    size_t got = 0;
    int fd = connect_rcvbuf(sw_srv.bound_port, 4096);
    if (fd >= 0) {
        const char *rq = "GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                         "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                         "Sec-WebSocket-Version: 13\r\n\r\n";
        (void)kl_test_sockwrite(fd, rq, strlen(rq));
        static uint8_t buf[16 * 1024];
        char head[1024];
        size_t head_len = 0;
        uint64_t start = kl_monotonic_ms();
        while (kl_monotonic_ms() - start < 4000) {         /* a completion loop sweeps ~1/s */
            if (kl_test_poll1(fd, 0, 50) <= 0) continue;
            size_t want = upgraded ? sizeof buf : 1;       /* the 101 a byte at a time */
            kl_ssize_t n = kl_test_sockread(fd, buf, want);
            if (n <= 0) { closed = 1; break; }
            if (!upgraded) {
                if (head_len < sizeof head - 1) head[head_len++] = (char)buf[0];
                head[head_len] = '\0';
                if (strstr(head, "\r\n\r\n")) upgraded = strstr(head, " 101 ") != NULL;
                continue;
            }
            got += (size_t)n;
            slow_ws_feed(&rd, fd, buf, (size_t)n);
            if (rd.close_seen) break;
            kl_test_sleep_ms(10);                          /* a slow reader */
        }
    }
    uint64_t client_done_ms = kl_monotonic_ms();
    if (fd >= 0) kl_test_closesock(fd);
    kl_http_server_stop(&sw_srv);
    kl_plat_thread_join(&tid);
    kl_http_server_free(&sw_srv);
    int released_early = g_sw_closed_ms != 0 && g_sw_closed_ms < client_done_ms;
    printf("  slow reader: %zu bytes in 4 s, pings answered %d, close %d, eof %d, released early %d"
           " (server close code %d at %d ms)\n",
           got, rd.pings, rd.close_seen, closed, released_early, g_sw_close_code,
           g_sw_closed_ms ? (int)(g_sw_closed_ms - sw_start_ms) : -1);
    ASSERT_TRUE(upgraded);
    ASSERT_GT(got, (size_t)0);
    ASSERT_LT(got, (size_t)SLOW_CHUNKS * BIG_CHUNK);       /* still going: the test covered it */
    ASSERT_FALSE(released_early);                          /* was: failed at the second interval */
    ASSERT_FALSE(rd.close_seen);                           /* was: Close 1001 */
    ASSERT_FALSE(closed);                                  /* was: the connection closed */
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

/* ── A streamed response that does not belong to a pooled connection ─────────────────────────────
 * On a completion loop a streamed response hands its bytes to its connection's output queue,
 * found from the response's address inside the connection. An HTTP/2 stream's response is not
 * inside a connection: the lookup read and wrote unrelated memory. A response that is not a pooled
 * connection's own must be refused. Built on the heap with nothing around it, so a stray lookup
 * reads out of bounds (caught on the sanitizer lanes). */
static KlHttpServer own_srv;

UTEST(completion_output, a_streamed_response_outside_a_pooled_connection_is_refused) {
    KlHttpServerConfig cfg = { .port = 0, .max_connections = 4 };
    ASSERT_EQ(0, kl_http_server_init(&own_srv, &cfg));
    KlAllocator alloc = kl_allocator_default();
    KlHttpResponse *res = (KlHttpResponse *)malloc(sizeof *res);
    ASSERT_TRUE(res != NULL);
    ASSERT_EQ(0, kl_http_response_init(res, &alloc));
    res->ctx = kl_http_server_event_ctx(&own_srv);
    res->conn_fd = KL_INVALID_SOCKET;
    KlHttpResponseWriteFn w = NULL;
    void *wc = NULL;
    int rc = kl_http_response_begin_stream(res, 200, &w, &wc);
    if (rc == 0) rc = w(wc, "x", 1);
    kl_http_response_free(res);
    free(res);
    kl_http_server_free(&own_srv);
    ASSERT_EQ(-1, rc);
}

/* ── Long transfers that keep moving are not idle ─────────────────────────────────────────────── */

/* Read everything until the peer closes, slowly but steadily (64 KiB, then a pause). Returns the
 * byte count; *closed when the read ended on the peer's close. */
static size_t read_paced(int fd, int pause_ms, int *closed) {
    static char chunk[64 * 1024];
    size_t total = 0;
    *closed = 0;
    for (;;) {
        if (kl_test_poll1(fd, 0, 3000) <= 0) break;
        kl_ssize_t n = kl_test_sockread(fd, chunk, sizeof chunk);
        if (n == 0) { *closed = 1; break; }
        if (n < 0) break;
        total += (size_t)n;
        kl_test_sleep_ms(pause_ms);
    }
    return total;
}

#define LONG_BYTES (16u * 1024u * 1024u)

/* A streamed response produced in the handler is one output queue on a completion loop, closed once
 * it is out. Its send moved bytes the whole time, but the sweep's check for a connection closing
 * after its output ran before send progress was counted, so it was released at read_timeout_ms.
 * 960 KiB fits the stream's outbound buffer, so readiness takes it too; the client reads it slowly
 * enough to outlast a 200 ms read timeout. */
#define LONG_STREAM_CHUNKS 15
static void handle_long_stream(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)ctx;
    KlHttpResponseWriteFn w = NULL;
    void *wc = NULL;
    if (kl_http_response_begin_stream(res, 200, &w, &wc) < 0) return;
    memset(g_big_chunk, 'S', sizeof g_big_chunk);
    for (int i = 0; i < LONG_STREAM_CHUNKS; i++) w(wc, g_big_chunk, sizeof g_big_chunk);
    kl_http_response_end_stream(res);
}

static KlHttpServer ls_srv;

UTEST(completion_output, a_long_streamed_download_that_moves_is_not_timed_out) {
    KlHttpServerConfig cfg = { .port = 0, .max_connections = 4, .read_timeout_ms = 200 };
    ASSERT_EQ(0, kl_http_server_init(&ls_srv, &cfg));
    kl_http_server_route(&ls_srv, "GET", "/long", handle_long_stream, NULL, NULL);
    KlPlatThread tid;
    kl_plat_thread_create(&tid, server_thread_fn, &ls_srv);
    wait_for_bind(&ls_srv);
    size_t got = 0;
    int closed = 0;
    int fd = connect_rcvbuf(ls_srv.bound_port, 16 * 1024);
    if (fd >= 0) {
        const char *rq = "GET /long HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
        (void)kl_test_sockwrite(fd, rq, strlen(rq));
        got = read_paced(fd, 40, &closed);
        kl_test_closesock(fd);
    }
    kl_http_server_stop(&ls_srv);
    kl_plat_thread_join(&tid);
    kl_http_server_free(&ls_srv);
    ASSERT_TRUE(got > (size_t)LONG_STREAM_CHUNKS * BIG_CHUNK);   /* was (completion): cut at ~200 ms */
    ASSERT_TRUE(closed);
}

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
/* A file body goes out zero-copy as one send op. On pollcomp the file bytes it moved were not
 * counted as progress (only the head), so a long download was cut at read_timeout_ms. */
static char g_long_file[64];
static void handle_long_file(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)ctx;
    int fd = open(g_long_file, O_RDONLY);
    if (fd < 0) { kl_http_response_json(res, 500, "{}", 2); return; }
    kl_http_response_file(res, fd, LONG_BYTES);
}

static KlHttpServer lf_srv;

UTEST(completion_output, a_long_file_download_that_moves_is_not_timed_out) {
    snprintf(g_long_file, sizeof g_long_file, "/tmp/keel_long_file_%d", (int)getpid());
    int wfd = open(g_long_file, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    ASSERT_TRUE(wfd >= 0);
    memset(g_big_chunk, 'F', sizeof g_big_chunk);
    for (unsigned i = 0; i < LONG_BYTES / BIG_CHUNK; i++)
        ASSERT_EQ((long)BIG_CHUNK, (long)write(wfd, g_big_chunk, BIG_CHUNK));
    close(wfd);
    KlHttpServerConfig cfg = { .port = 0, .max_connections = 4, .read_timeout_ms = 400 };
    ASSERT_EQ(0, kl_http_server_init(&lf_srv, &cfg));
    kl_http_server_route(&lf_srv, "GET", "/file", handle_long_file, NULL, NULL);
    KlPlatThread tid;
    kl_plat_thread_create(&tid, server_thread_fn, &lf_srv);
    wait_for_bind(&lf_srv);
    size_t got = 0;
    int closed = 0;
    int fd = connect_rcvbuf(lf_srv.bound_port, 16 * 1024);
    if (fd >= 0) {
        const char *rq = "GET /file HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
        (void)kl_test_sockwrite(fd, rq, strlen(rq));
        got = read_paced(fd, 10, &closed);
        kl_test_closesock(fd);
    }
    kl_http_server_stop(&lf_srv);
    kl_plat_thread_join(&tid);
    kl_http_server_free(&lf_srv);
    unlink(g_long_file);
    ASSERT_TRUE(got > (size_t)LONG_BYTES);                 /* was (pollcomp): cut at ~400 ms */
    ASSERT_TRUE(closed);
}
#endif

/* ── WebSocket output to a client that stops reading is bounded ───────────────────────────────── */

/* Every frame went onto the output queue, which took everything, so sending to a client that never
 * reads grew server memory without bound (a WebSocket connection is exempt from the idle sweep). A
 * send must start failing once the backlog is large, as it does on readiness once the socket fills. */
static int g_ws_sent_ok;
static void ws_open_flood(KlWsServerConn *ws, void *ud) {
    (void)ud;
    memset(g_big_chunk, 'B', sizeof g_big_chunk);
    g_ws_sent_ok = 0;
    for (int i = 0; i < BIG_CHUNKS; i++) {
        if (kl_ws_server_send_binary(ws, g_big_chunk, sizeof g_big_chunk) < 0) break;
        g_ws_sent_ok++;
    }
}

static KlHttpServer wsb_srv;

UTEST(completion_output, a_websocket_backlog_to_a_stalled_reader_is_bounded) {
    KlHttpServerConfig cfg = { .port = 0, .max_connections = 4 };
    ASSERT_EQ(0, kl_http_server_init(&wsb_srv, &cfg));
    KlWsServerConfig wcfg;
    kl_ws_server_config_init(&wcfg);
    wcfg.callbacks.on_open = ws_open_flood;
    ASSERT_EQ(0, kl_http_server_ws_upgrade(&wsb_srv, "/ws", &wcfg));
    KlPlatThread tid;
    kl_plat_thread_create(&tid, server_thread_fn, &wsb_srv);
    wait_for_bind(&wsb_srv);
    g_ws_sent_ok = -1;
    int a = connect_rcvbuf(wsb_srv.bound_port, 4096);
    if (a >= 0) {
        const char *rq = "GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                         "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                         "Sec-WebSocket-Version: 13\r\n\r\n";
        (void)kl_test_sockwrite(a, rq, strlen(rq));
    }
    for (int i = 0; i < 300 && g_ws_sent_ok < 0; i++) kl_test_sleep_ms(10);
    kl_test_sleep_ms(200);
    int sent_ok = g_ws_sent_ok;
    if (a >= 0) kl_test_closesock(a);
    kl_http_server_stop(&wsb_srv);
    kl_plat_thread_join(&tid);
    kl_http_server_free(&wsb_srv);
    ASSERT_TRUE(sent_ok >= 0);
    ASSERT_LT(sent_ok, BIG_CHUNKS);                        /* was (completion): all 64 MiB accepted */
}

UTEST_MAIN();
