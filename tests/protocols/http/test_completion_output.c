/*
 * test_completion_output.c: server output on a completion loop never blocks the loop, and goes out in
 * order and in full.
 *
 * On a completion loop the TLS output already leaves through one ordered, overlapped per-connection
 * queue. Plaintext output did not: a streamed response, WebSocket frames and HTTP/2 frames were sent
 * with a synchronous send on the loop thread, and io_uring and IOCP accepted sockets are blocking, so
 * a client that stops reading stalled every connection on the loop. Readiness loops use WRITE
 * interest and pass most of these tests either way; those exist for the completion lanes. The last
 * two are readiness cases: output buffered while the connection had no WRITE interest.
 */
#include "utest.h"
#include <keel/keel.h>
#include <keel/tls.h>
#include "net_compat.h"
#include "mock_tls.h"
#include "event_caps.h"
#include "http_conn_internal.h"          /* white-box: the server-side socket, to shrink its send buffer */
#include "websocket_server_internal.h"   /* white-box: the WebSocket's connection */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "platform_thread.h"
#include "event_builtin.h"   /* the compiled-in backend, wrapped as a runtime provider */
#include "completion.h"      /* kl_comp_ops_builtin, KlCompletionOps.send_max */
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

/* The bound applies to the proposed write, not only to bytes already queued. With an empty queue a
 * single payload larger than 1 MiB used to be accepted in full before later writes saw backpressure. */
#define OVERSIZED_FRAME_BYTES ((1024u * 1024u) + 1u)
static char g_oversized_frame[OVERSIZED_FRAME_BYTES];
static int g_oversized_frame_rc;

static void ws_open_oversized_frame(KlWsServerConn *ws, void *ud) {
    (void)ud;
    memset(g_oversized_frame, 'O', sizeof g_oversized_frame);
    g_oversized_frame_rc = kl_ws_server_send_binary(ws, g_oversized_frame,
                                                     sizeof g_oversized_frame);
}

static KlHttpServer wso_srv;

UTEST(completion_output, one_oversized_websocket_write_cannot_cross_the_queue_bound) {
    KlHttpServerConfig cfg = { .port = 0, .max_connections = 2 };
    ASSERT_EQ(0, kl_http_server_init(&wso_srv, &cfg));
    if (!(kl_event_caps(&wso_srv.ev.loop) & KL_EVENT_CAP_COMPLETION)) {
        kl_http_server_free(&wso_srv);
        UTEST_SKIP("The transport queue allowance applies only to completion backends");
    }
    KlWsServerConfig wcfg;
    kl_ws_server_config_init(&wcfg);
    wcfg.callbacks.on_open = ws_open_oversized_frame;
    ASSERT_EQ(0, kl_http_server_ws_upgrade(&wso_srv, "/ws", &wcfg));
    KlPlatThread tid;
    kl_plat_thread_create(&tid, server_thread_fn, &wso_srv);
    wait_for_bind(&wso_srv);
    g_oversized_frame_rc = 1;
    int fd = connect_rcvbuf(wso_srv.bound_port, 4096);
    if (fd >= 0) {
        const char *rq = "GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                         "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                         "Sec-WebSocket-Version: 13\r\n\r\n";
        (void)kl_test_sockwrite(fd, rq, strlen(rq));
    }
    for (int i = 0; i < 300 && g_oversized_frame_rc == 1; i++) kl_test_sleep_ms(10);
    if (fd >= 0) kl_test_closesock(fd);
    kl_http_server_stop(&wso_srv);
    kl_plat_thread_join(&tid);
    kl_http_server_free(&wso_srv);
    ASSERT_EQ(-1, g_oversized_frame_rc);
}

static int g_large_drain_many, g_large_drain_rc;
static void ws_open_large_drain(KlWsServerConn *ws, void *ud) {
    (void)ud;
    g_large_drain_rc = kl_ws_server_enable_drain(ws, 4u * 1024u * 1024u);
    memset(g_oversized_frame, 'O', sizeof g_oversized_frame);
    int count = g_large_drain_many ? 40 : 1;
    size_t len = g_large_drain_many ? BIG_CHUNK : sizeof g_oversized_frame;
    for (int i = 0; i < count && g_large_drain_rc == 0; i++)
        g_large_drain_rc = kl_ws_server_send_binary(ws, g_oversized_frame, len);
    if (g_large_drain_rc == 0) g_large_drain_rc = kl_ws_server_close(ws, 1000, NULL, 0);
}

static void large_ws_drain_case(int *utest_result, int many) {
    KlHttpServer server;
    KlHttpServerConfig cfg = { .port = 0, .max_connections = 2 };
    ASSERT_EQ(0, kl_http_server_init(&server, &cfg));
    KlWsServerConfig wcfg;
    kl_ws_server_config_init(&wcfg);
    wcfg.callbacks.on_open = ws_open_large_drain;
    ASSERT_EQ(0, kl_http_server_ws_upgrade(&server, "/ws", &wcfg));
    g_large_drain_many = many;
    g_large_drain_rc = -1;
    size_t cap = 3u * 1024u * 1024u;
    char *buf = malloc(cap);
    ASSERT_TRUE(buf != NULL);
    buf[0] = '\0';
    KlPlatThread thread;
    ASSERT_EQ(0, kl_plat_thread_create(&thread, server_thread_fn, &server));
    wait_for_bind(&server);
    int fd = connect_rcvbuf(server.bound_port, 0);
    size_t got = 0;
    if (fd >= 0) {
        const char *rq = "GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                         "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                         "Sec-WebSocket-Version: 13\r\n\r\n";
        (void)kl_test_sockwrite(fd, rq, strlen(rq));
        int closed;
        got = (size_t)read_until_close(fd, buf, cap, 2000, &closed);
        kl_test_closesock(fd);
    }
    kl_http_server_stop(&server);
    kl_plat_thread_join(&thread);
    kl_http_server_free(&server);
    int rc = g_large_drain_rc;
    char *head_end = strstr(buf, "\r\n\r\n");
    size_t at = head_end ? (size_t)(head_end + 4 - buf) : got;
    int intact = head_end != NULL;
    int count = many ? 40 : 1;
    size_t len = many ? BIG_CHUNK : sizeof g_oversized_frame;
    for (int i = 0; i < count && intact; i++) {
        if (got - at < 10 || (unsigned char)buf[at] != 0x82 ||
            (unsigned char)buf[at + 1] != 127) { intact = 0; break; }
        uint64_t frame_len = 0;
        for (int j = 2; j < 10; j++) frame_len = (frame_len << 8) | (unsigned char)buf[at + j];
        at += 10;
        if (frame_len != len || got - at < len) { intact = 0; break; }
        for (size_t j = 0; j < len; j++)
            if (buf[at + j] != 'O') { intact = 0; break; }
        at += len;
    }
    free(buf);
    ASSERT_EQ(0, rc);
    ASSERT_TRUE(intact);
}

UTEST(completion_output, an_oversized_frame_in_a_large_drain_makes_progress) {
    large_ws_drain_case(utest_result, 0);
}

UTEST(completion_output, accumulated_small_frames_in_a_large_drain_make_progress) {
    large_ws_drain_case(utest_result, 1);
}

/* ── TLS output written outside a drive goes out ────────────────────────────────────────────────
 * On a completion loop TLS writes land in the engine's output ring, which only a drive moved onto
 * the output queue. Output written from elsewhere (the sweep's auto-ping, a frame sent from a
 * timer, the drain's Close or GOAWAY) waited until the client sent something; a client answering
 * pings never got one, so it looked dead and was closed. The ping must reach an idle client that
 * answers it, and the connection must stay open. */
static KlHttpServer tp_srv;

UTEST(completion_output, a_tls_websocket_auto_ping_reaches_an_idle_client) {
    KlTlsConfig tls_cfg = { .ctx = NULL, .factory = mock_tls_create };
    KlHttpServerConfig cfg = { .port = 0, .tls = &tls_cfg, .max_connections = 4 };
    ASSERT_EQ(0, kl_http_server_init(&tp_srv, &cfg));
    KlWsServerConfig wcfg;
    kl_ws_server_config_init(&wcfg);
    wcfg.ping_interval_ms = 150;
    ASSERT_EQ(0, kl_http_server_ws_upgrade(&tp_srv, "/ws", &wcfg));
    KlPlatThread tid;
    kl_plat_thread_create(&tid, server_thread_fn, &tp_srv);
    wait_for_bind(&tp_srv);

    int pings = 0, close_seen = 0, eof = 0;
    int fd = connect_rcvbuf(tp_srv.bound_port, 0);
    if (fd >= 0) {
        const char *rq = "GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                         "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                         "Sec-WebSocket-Version: 13\r\n\r\n";
        (void)kl_test_sockwrite(fd, rq, strlen(rq));
        unsigned char b[512];
        size_t have = 0;
        int upgraded = 0;
        uint64_t start = kl_monotonic_ms();
        while (kl_monotonic_ms() - start < 3000 && !close_seen && !eof) {   /* sweeps run ~1/s */
            if (kl_test_poll1(fd, 0, 50) <= 0) continue;
            kl_ssize_t n = kl_test_sockread(fd, b + have, sizeof b - have);
            if (n <= 0) { eof = 1; break; }
            have += (size_t)n;
            if (!upgraded) {                               /* skip the 101 head */
                unsigned char *e = NULL;
                for (size_t i = 0; i + 3 < have; i++)
                    if (b[i] == '\r' && b[i + 1] == '\n' && b[i + 2] == '\r' && b[i + 3] == '\n') {
                        e = b + i + 4;
                        break;
                    }
                if (!e) continue;
                upgraded = 1;
                have -= (size_t)(e - b);
                memmove(b, e, have);
            }
            while (have >= 2) {                            /* server frames: unmasked, small */
                size_t plen = b[1] & 0x7F;
                if (plen > 125 || have < 2 + plen) break;
                unsigned op = b[0] & 0x0F;
                if (op == 0x9) {                           /* PING: answer with a masked PONG */
                    pings++;
                    unsigned char pong[2 + 4 + 125];
                    pong[0] = 0x8A;
                    pong[1] = (unsigned char)(0x80 | plen);
                    memset(pong + 2, 0, 4);                /* zero mask: payload unchanged */
                    memcpy(pong + 6, b + 2, plen);
                    (void)kl_test_sockwrite(fd, pong, 6 + plen);
                } else if (op == 0x8) {
                    close_seen = 1;
                }
                have -= 2 + plen;
                memmove(b, b + 2 + plen, have);
            }
        }
        kl_test_closesock(fd);
    }
    kl_http_server_stop(&tp_srv);
    kl_plat_thread_join(&tid);
    kl_http_server_free(&tp_srv);
    ASSERT_GT(pings, 0);
    ASSERT_FALSE(close_seen);                              /* was (completion): Close 1001 */
    ASSERT_FALSE(eof);
}

/* ── A response reset by its handler still streams ──────────────────────────────────────────────
 * kl_http_response_reset cleared the mark that the response is a pooled connection's own, so a
 * handler that reset its response and then streamed got a failed stream on a completion loop. */
static void handle_reset_then_stream(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_http_response_header(res, "X-Discarded", "1");
    kl_http_response_reset(res);
    KlHttpResponseWriteFn w = NULL;
    void *wc = NULL;
    if (kl_http_response_begin_stream(res, 200, &w, &wc) < 0) return;
    w(wc, "streamed-after-reset", 20);
    kl_http_response_end_stream(res);
}

static KlHttpServer rs_srv;

UTEST(completion_output, a_stream_after_a_response_reset_still_goes_out) {
    KlHttpServerConfig cfg = { .port = 0, .max_connections = 4 };
    ASSERT_EQ(0, kl_http_server_init(&rs_srv, &cfg));
    kl_http_server_route(&rs_srv, "GET", "/rs", handle_reset_then_stream, NULL, NULL);
    KlPlatThread tid;
    kl_plat_thread_create(&tid, server_thread_fn, &rs_srv);
    wait_for_bind(&rs_srv);
    char buf[1024];
    int closed = 0;
    buf[0] = '\0';
    int fd = connect_rcvbuf(rs_srv.bound_port, 0);
    if (fd >= 0) {
        const char *rq = "GET /rs HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
        (void)kl_test_sockwrite(fd, rq, strlen(rq));
        (void)read_until_close(fd, buf, sizeof buf, 2000, &closed);
        kl_test_closesock(fd);
    }
    kl_http_server_stop(&rs_srv);
    kl_plat_thread_join(&tid);
    kl_http_server_free(&rs_srv);
    ASSERT_TRUE(strstr(buf, "streamed-after-reset") != NULL);   /* was (completion): no body */
}

/* ── A frame sent from on_close(1006) is refused ────────────────────────────────────────────────
 * A WebSocket that dies without a Close (the client just goes) is released, and its on_close(1006)
 * runs inside the release. A frame the application sent from there went to a connection being torn
 * down: on a completion loop (since TLS output is queued at once) it was posted as a send whose
 * completion then arrived for a slot already back in the pool, uncounted, and a failed one released
 * that slot a second time. The connection is gone, so such a send now fails on every loop, and the
 * pool stays consistent: no connection left counted, every slot still serving. */
static int g_bye_sent, g_bye_closes;
static void ws_bye_on_close(KlWsServerConn *ws, uint16_t code, const char *reason, size_t len,
                            void *ud) {
    (void)code; (void)reason; (void)len; (void)ud;
    if (kl_ws_server_send_text(ws, "bye", 3) == 0) g_bye_sent++;
    g_bye_closes++;
}

static KlHttpServer bc_srv;

UTEST(completion_output, a_send_from_on_close_after_an_abnormal_closure_is_refused) {
    KlTlsConfig tls_cfg = { .ctx = NULL, .factory = mock_tls_create };
    KlHttpServerConfig cfg = { .port = 0, .tls = &tls_cfg, .max_connections = 4 };
    ASSERT_EQ(0, kl_http_server_init(&bc_srv, &cfg));
    KlWsServerConfig wcfg;
    kl_ws_server_config_init(&wcfg);
    wcfg.callbacks.on_close = ws_bye_on_close;
    ASSERT_EQ(0, kl_http_server_ws_upgrade(&bc_srv, "/ws", &wcfg));
    kl_http_server_route(&bc_srv, "GET", "/hello", handle_hello, NULL, NULL);
    KlPlatThread tid;
    kl_plat_thread_create(&tid, server_thread_fn, &bc_srv);
    wait_for_bind(&bc_srv);
    int port = bc_srv.bound_port;
    g_bye_sent = g_bye_closes = 0;

    for (int round = 0; round < 4; round++) {             /* each one a dead client's release */
        int fd = connect_rcvbuf(port, 0);
        if (fd < 0) continue;
        const char *rq = "GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                         "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                         "Sec-WebSocket-Version: 13\r\n\r\n";
        (void)kl_test_sockwrite(fd, rq, strlen(rq));
        char head[512];
        if (kl_test_poll1(fd, 0, 1000) > 0) (void)kl_test_sockread(fd, head, sizeof head);
        kl_test_closesock(fd);                             /* gone, no Close */
        kl_test_sleep_ms(100);
    }
    kl_test_sleep_ms(300);
    KlHttpServerStats st;
    kl_http_server_stats(&bc_srv, &st);
    int served = 0;
    for (int i = 0; i < 4; i++) served += hello_answered(port);
    kl_http_server_stop(&bc_srv);
    kl_plat_thread_join(&tid);
    kl_http_server_free(&bc_srv);
    ASSERT_EQ(g_bye_closes, 4);                            /* every dead client's on_close ran */
    ASSERT_EQ(g_bye_sent, 0);                              /* was 4: the frame was taken */
    ASSERT_EQ(st.active_connections, 0);
    ASSERT_EQ(served, 4);
}

/* ── A TLS stream written while suspended goes out ──────────────────────────────────────────────
 * A handler that starts a stream, suspends, and pushes chunks from a timer (an event feed): over TLS
 * on a completion loop the chunks only reached the engine's ring, and nothing moved them onto the
 * output queue until the connection resumed. Readiness and plaintext send them as written. */
typedef struct {
    KlAsyncOp op;
    KlHttpResponse *res;
    KlHttpResponseWriteFn w;
    void *wc;
} SuspStream;
static SuspStream g_ss;
static KlHttpServer ss_srv;

static void ss_resume(KlAsyncOp *op, void *ud) {
    (void)op; (void)ud;
    g_ss.w(g_ss.wc, "end;", 4);
    kl_http_response_end_stream(g_ss.res);
}
static void ss_push(void *ud) {
    (void)ud;
    g_ss.w(g_ss.wc, "pushed-while-suspended;", 23);
}
static void ss_finish(void *ud) {
    (void)ud;
    kl_async_complete(&ss_srv, &g_ss.op);
}
static void handle_suspended_stream(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)ctx;
    memset(&g_ss, 0, sizeof g_ss);
    g_ss.res = res;
    if (kl_http_response_begin_stream(res, 200, &g_ss.w, &g_ss.wc) < 0) return;
    g_ss.w(g_ss.wc, "first;", 6);
    g_ss.op.on_resume = ss_resume;
    if (kl_async_suspend(&ss_srv, kl_http_request_conn(req), &g_ss.op) < 0) return;
    (void)kl_timer_add(kl_http_server_event_ctx(&ss_srv), 50, ss_push, NULL);
    (void)kl_timer_add(kl_http_server_event_ctx(&ss_srv), 2000, ss_finish, NULL);
}

UTEST(completion_output, a_tls_stream_written_while_suspended_goes_out) {
    KlTlsConfig tls_cfg = { .ctx = NULL, .factory = mock_tls_create };
    KlHttpServerConfig cfg = { .port = 0, .tls = &tls_cfg, .max_connections = 4 };
    ASSERT_EQ(0, kl_http_server_init(&ss_srv, &cfg));
    kl_http_server_route(&ss_srv, "GET", "/feed", handle_suspended_stream, NULL, NULL);
    KlPlatThread tid;
    kl_plat_thread_create(&tid, server_thread_fn, &ss_srv);
    wait_for_bind(&ss_srv);
    char buf[2048];
    size_t have = 0;
    buf[0] = '\0';
    int fd = connect_rcvbuf(ss_srv.bound_port, 0);
    if (fd >= 0) {
        const char *rq = "GET /feed HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
        (void)kl_test_sockwrite(fd, rq, strlen(rq));
        uint64_t start = kl_monotonic_ms();
        while (kl_monotonic_ms() - start < 1000 && !strstr(buf, "pushed-while-suspended")) {
            if (kl_test_poll1(fd, 0, 50) <= 0) continue;
            kl_ssize_t n = kl_test_sockread(fd, buf + have, sizeof buf - 1 - have);
            if (n <= 0) break;
            have += (size_t)n;
            buf[have] = '\0';
        }
        kl_test_closesock(fd);
    }
    kl_http_server_stop(&ss_srv);
    kl_plat_thread_join(&tid);
    kl_http_server_free(&ss_srv);
    ASSERT_TRUE(strstr(buf, "first;") != NULL);
    ASSERT_TRUE(strstr(buf, "pushed-while-suspended") != NULL);   /* was (completion): at resume */
}

/* ── A suspended TLS stream to a client that stops reading is bounded ───────────────────────────
 * The producer of a stream gets backpressure: once what the client has not taken reaches the
 * stream's bounds, a write fails instead of memory growing. A TLS stream written from a timer while
 * its connection is suspended had every chunk moved onto the connection's output queue, which grew
 * without bound for a client that stopped reading. 8 MiB are offered; far less may be taken. */
#define BP_CHUNKS 128
typedef struct {
    KlAsyncOp op;
    KlHttpResponse *res;
    KlHttpResponseWriteFn w;
    void *wc;
    int suspended, pushed, taken;
} BpStream;
static BpStream g_bp;
static KlHttpServer bp_srv;

static void bp_resume(KlAsyncOp *op, void *ud) {
    (void)op; (void)ud;
    kl_http_response_end_stream(g_bp.res);
}
static void bp_push(void *ud) {
    (void)ud;
    g_bp.pushed = 1;
    memset(g_big_chunk, 'P', sizeof g_big_chunk);
    for (int i = 0; i < BP_CHUNKS; i++) {
        if (g_bp.w(g_bp.wc, g_big_chunk, sizeof g_big_chunk) < 0) break;
        g_bp.taken++;
    }
}
static void bp_finish(void *ud) {
    (void)ud;
    kl_async_complete(&bp_srv, &g_bp.op);
}
static void handle_bp_stream(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)ctx;
    memset(&g_bp, 0, sizeof g_bp);
    g_bp.res = res;
    if (kl_http_response_begin_stream(res, 200, &g_bp.w, &g_bp.wc) < 0) return;
    g_bp.op.on_resume = bp_resume;
    if (kl_async_suspend(&bp_srv, kl_http_request_conn(req), &g_bp.op) < 0) return;
    g_bp.suspended = 1;
    (void)kl_timer_add(kl_http_server_event_ctx(&bp_srv), 100, bp_push, NULL);
    (void)kl_timer_add(kl_http_server_event_ctx(&bp_srv), 800, bp_finish, NULL);
}

UTEST(completion_output, a_suspended_tls_stream_to_a_stalled_client_is_bounded) {
    KlTlsConfig tls_cfg = { .ctx = NULL, .factory = mock_tls_create };
    KlHttpServerConfig cfg = { .port = 0, .tls = &tls_cfg, .max_connections = 4 };
    ASSERT_EQ(0, kl_http_server_init(&bp_srv, &cfg));
    kl_http_server_route(&bp_srv, "GET", "/bp", handle_bp_stream, NULL, NULL);
    KlPlatThread tid;
    kl_plat_thread_create(&tid, server_thread_fn, &bp_srv);
    wait_for_bind(&bp_srv);
    int fd = connect_rcvbuf(bp_srv.bound_port, 4096);    /* reads nothing */
    if (fd >= 0) {
        const char *rq = "GET /bp HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
        (void)kl_test_sockwrite(fd, rq, strlen(rq));
        kl_test_sleep_ms(1200);
        kl_test_closesock(fd);
    }
    kl_http_server_stop(&bp_srv);
    kl_plat_thread_join(&tid);
    kl_http_server_free(&bp_srv);
    ASSERT_TRUE(g_bp.suspended);
    ASSERT_TRUE(g_bp.pushed);
    ASSERT_LT(g_bp.taken, BP_CHUNKS / 2);                  /* was (completion): all 8 MiB taken */
}

/* ── A suspended connection that dies is cancelled, not resumed ─────────────────────────────────
 * A suspended streaming connection whose client resets: on a completion loop the failed send of a
 * chunk written meanwhile released the connection but left its async op registered, so the later
 * kl_async_complete resumed a slot already back in the pool (or a new client's). The op must end
 * exactly once: on_cancel when the connection died first (completion), on_resume otherwise
 * (readiness, which finds out at the resume); and the pool must stay consistent. */
typedef struct {
    KlAsyncOp op;
    KlHttpResponse *res;
    KlHttpResponseWriteFn w;
    void *wc;
    int suspended, resumed, cancelled;
} DeadStream;
static DeadStream g_ds;
static KlHttpServer ds_srv;

static void ds_resume(KlAsyncOp *op, void *ud) {
    (void)op; (void)ud;
    g_ds.resumed++;
    g_ds.w(g_ds.wc, "end;", 4);
    kl_http_response_end_stream(g_ds.res);
}
static void ds_cancel(KlAsyncOp *op, void *ud) {
    (void)op; (void)ud;
    g_ds.cancelled++;
}
static void ds_push(void *ud) {
    (void)ud;
    if (g_ds.cancelled) return;
    (void)g_ds.w(g_ds.wc, "pushed;", 7);
}
static void ds_finish(void *ud) {
    (void)ud;
    kl_async_complete(&ds_srv, &g_ds.op);                  /* a no-op once cancelled */
}
static void handle_dead_stream(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)ctx;
    memset(&g_ds, 0, sizeof g_ds);
    g_ds.res = res;
    if (kl_http_response_begin_stream(res, 200, &g_ds.w, &g_ds.wc) < 0) return;
    g_ds.w(g_ds.wc, "first;", 6);
    g_ds.op.on_resume = ds_resume;
    g_ds.op.on_cancel = ds_cancel;
    if (kl_async_suspend(&ds_srv, kl_http_request_conn(req), &g_ds.op) < 0) return;
    g_ds.suspended = 1;
    (void)kl_timer_add(kl_http_server_event_ctx(&ds_srv), 300, ds_push, NULL);
    (void)kl_timer_add(kl_http_server_event_ctx(&ds_srv), 400, ds_push, NULL);
    (void)kl_timer_add(kl_http_server_event_ctx(&ds_srv), 900, ds_finish, NULL);
}

UTEST(completion_output, a_suspended_connection_that_dies_is_cancelled_not_resumed) {
    KlTlsConfig tls_cfg = { .ctx = NULL, .factory = mock_tls_create };
    KlHttpServerConfig cfg = { .port = 0, .tls = &tls_cfg, .max_connections = 4 };
    ASSERT_EQ(0, kl_http_server_init(&ds_srv, &cfg));
    kl_http_server_route(&ds_srv, "GET", "/ds", handle_dead_stream, NULL, NULL);
    kl_http_server_route(&ds_srv, "GET", "/hello", handle_hello, NULL, NULL);
    KlPlatThread tid;
    kl_plat_thread_create(&tid, server_thread_fn, &ds_srv);
    wait_for_bind(&ds_srv);
    int port = ds_srv.bound_port;
    int fd = connect_rcvbuf(port, 0);
    if (fd >= 0) {
        const char *rq = "GET /ds HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
        (void)kl_test_sockwrite(fd, rq, strlen(rq));
        char got[1024];
        size_t have = 0;
        got[0] = '\0';
        uint64_t start = kl_monotonic_ms();                /* the head and the first chunk, whole */
        while (kl_monotonic_ms() - start < 1000 && !strstr(got, "first;")) {
            if (kl_test_poll1(fd, 0, 50) <= 0) continue;
            kl_ssize_t n = kl_test_sockread(fd, got + have, sizeof got - 1 - have);
            if (n <= 0) break;
            have += (size_t)n;
            got[have] = '\0';
        }
        struct linger lg;
        memset(&lg, 0, sizeof lg);
        lg.l_onoff = 1;                                    /* then gone, with a reset */
        lg.l_linger = 0;
        (void)setsockopt(fd, SOL_SOCKET, SO_LINGER, (const char *)&lg, sizeof lg);
        kl_test_closesock(fd);
    }
    kl_test_sleep_ms(1300);                                /* the pushes, then the finish */
    KlHttpServerStats st;
    kl_http_server_stats(&ds_srv, &st);
    int served = 0;
    for (int i = 0; i < 4; i++) served += hello_answered(port);
    kl_http_server_stop(&ds_srv);
    kl_plat_thread_join(&tid);
    kl_http_server_free(&ds_srv);
    ASSERT_TRUE(g_ds.suspended);
    ASSERT_EQ(g_ds.resumed + g_ds.cancelled, 1);           /* exactly one end */
    ASSERT_EQ(st.active_connections, 0);                   /* was (completion): released twice */
    ASSERT_EQ(served, 4);
}

/* ── TLS WebSocket output to a client that stops reading is bounded ─────────────────────────────
 * The TLS twins of the two tests above. Each TLS write's ciphertext was moved from the engine's
 * bounded ring onto the uncapped output queue at once, so a TLS WebSocket send never saw a full
 * buffer: a client that stopped reading grew server memory without bound, and one frame over the
 * queue's bound was taken whole. */
static KlHttpServer twsb_srv;

UTEST(completion_output, a_tls_websocket_backlog_to_a_stalled_reader_is_bounded) {
    KlTlsConfig tls_cfg = { .ctx = NULL, .factory = mock_tls_create };
    KlHttpServerConfig cfg = { .port = 0, .tls = &tls_cfg, .max_connections = 4 };
    ASSERT_EQ(0, kl_http_server_init(&twsb_srv, &cfg));
    KlWsServerConfig wcfg;
    kl_ws_server_config_init(&wcfg);
    wcfg.callbacks.on_open = ws_open_flood;
    ASSERT_EQ(0, kl_http_server_ws_upgrade(&twsb_srv, "/ws", &wcfg));
    KlPlatThread tid;
    kl_plat_thread_create(&tid, server_thread_fn, &twsb_srv);
    wait_for_bind(&twsb_srv);
    g_ws_sent_ok = -1;
    int a = connect_rcvbuf(twsb_srv.bound_port, 4096);
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
    kl_http_server_stop(&twsb_srv);
    kl_plat_thread_join(&tid);
    kl_http_server_free(&twsb_srv);
    ASSERT_TRUE(sent_ok >= 0);
    ASSERT_LT(sent_ok, BIG_CHUNKS);                        /* was (completion): all 64 MiB accepted */
}

static KlHttpServer twso_srv;

UTEST(completion_output, one_oversized_tls_websocket_write_cannot_cross_the_queue_bound) {
    KlTlsConfig tls_cfg = { .ctx = NULL, .factory = mock_tls_create };
    KlHttpServerConfig cfg = { .port = 0, .tls = &tls_cfg, .max_connections = 2 };
    ASSERT_EQ(0, kl_http_server_init(&twso_srv, &cfg));
    if (!(kl_event_caps(&twso_srv.ev.loop) & KL_EVENT_CAP_COMPLETION)) {
        kl_http_server_free(&twso_srv);
        UTEST_SKIP("The transport queue allowance applies only to completion backends");
    }
    KlWsServerConfig wcfg;
    kl_ws_server_config_init(&wcfg);
    wcfg.callbacks.on_open = ws_open_oversized_frame;
    ASSERT_EQ(0, kl_http_server_ws_upgrade(&twso_srv, "/ws", &wcfg));
    KlPlatThread tid;
    kl_plat_thread_create(&tid, server_thread_fn, &twso_srv);
    wait_for_bind(&twso_srv);
    g_oversized_frame_rc = 1;
    int fd = connect_rcvbuf(twso_srv.bound_port, 4096);
    if (fd >= 0) {
        const char *rq = "GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                         "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                         "Sec-WebSocket-Version: 13\r\n\r\n";
        (void)kl_test_sockwrite(fd, rq, strlen(rq));
    }
    for (int i = 0; i < 300 && g_oversized_frame_rc == 1; i++) kl_test_sleep_ms(10);
    if (fd >= 0) kl_test_closesock(fd);
    kl_http_server_stop(&twso_srv);
    kl_plat_thread_join(&tid);
    kl_http_server_free(&twso_srv);
    ASSERT_EQ(-1, g_oversized_frame_rc);                   /* was (completion): taken whole */
}

/* A small send buffer on the server's side of an accepted connection, so a client that pauses or
 * reads slowly makes the server's sends would-block (Windows loopback otherwise takes far more). */
static void shrink_sndbuf(KlHttpConn *c) {
    int sb = 4096;
    int sfd = (int)c->stream.fd;
    (void)setsockopt(sfd, SOL_SOCKET, SO_SNDBUF, (const char *)&sb, sizeof sb);
}

/* ── A WebSocket backlog queued from another connection's event goes out ────────────────────────
 * Readiness: connection A's on_message sends a backlog to connection B, whose client reads steadily
 * and sends nothing. The frames that would-block went into B's drain, but WRITE interest was set only
 * after B's own event, and B had none: the backlog sat until B's client sent something. */
#define XW_FRAME  (32 * 1024)                              /* 4-byte frame header each */
#define XW_FRAMES 64                                       /* 2 MiB */
static char g_xw_frame[XW_FRAME];
static KlWsServerConn *g_xw_conns[2];
static int g_xw_opened, g_xw_sent;
static KlHttpServer xw_srv;

static void xw_open(KlWsServerConn *ws, void *ud) {
    (void)ud;
    (void)kl_ws_server_enable_drain(ws, 0);                /* unlimited: the whole backlog is kept */
    shrink_sndbuf(ws->conn);
    if (g_xw_opened < 2) g_xw_conns[g_xw_opened] = ws;
    g_xw_opened++;
}

static void xw_message(KlWsServerConn *ws, const char *data, size_t len, int is_binary, void *ud) {
    (void)data; (void)len; (void)is_binary; (void)ud;
    if (ws != g_xw_conns[1] || !g_xw_conns[0]) return;     /* A asks for B's backlog */
    memset(g_xw_frame, 'X', sizeof g_xw_frame);
    for (int i = 0; i < XW_FRAMES; i++) {
        if (kl_ws_server_send_binary(g_xw_conns[0], g_xw_frame, sizeof g_xw_frame) < 0) break;
        g_xw_sent++;
    }
}

static void xw_close(KlWsServerConn *ws, uint16_t code, const char *reason, size_t len, void *ud) {
    (void)code; (void)reason; (void)len; (void)ud;
    for (int i = 0; i < 2; i++)
        if (g_xw_conns[i] == ws) g_xw_conns[i] = NULL;
}

/* Upgrade on a fresh connection; the fd once the 101 is read (a byte at a time: nothing past it). */
static int ws_connect_upgraded(int port, int rcvbuf) {
    int fd = connect_rcvbuf(port, rcvbuf);
    if (fd < 0) return -1;
    const char *rq = "GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                     "Sec-WebSocket-Version: 13\r\n\r\n";
    (void)kl_test_sockwrite(fd, rq, strlen(rq));
    char head[1024];
    size_t head_len = 0;
    head[0] = '\0';
    uint64_t start = kl_monotonic_ms();
    while (kl_monotonic_ms() - start < 2000 && !strstr(head, "\r\n\r\n")) {
        if (kl_test_poll1(fd, 0, 50) <= 0) continue;
        char ch;
        if (kl_test_sockread(fd, &ch, 1) != 1) break;
        if (head_len < sizeof head - 1) { head[head_len++] = ch; head[head_len] = '\0'; }
    }
    if (!strstr(head, " 101 ")) { kl_test_closesock(fd); return -1; }
    return fd;
}

UTEST(completion_output, a_websocket_backlog_sent_from_another_connection_goes_out) {
    KlHttpServerConfig cfg = { .port = 0, .max_connections = 8 };
    ASSERT_EQ(0, kl_http_server_init(&xw_srv, &cfg));
    KlWsServerConfig wcfg;
    kl_ws_server_config_init(&wcfg);
    wcfg.callbacks.on_open = xw_open;
    wcfg.callbacks.on_message = xw_message;
    wcfg.callbacks.on_close = xw_close;
    ASSERT_EQ(0, kl_http_server_ws_upgrade(&xw_srv, "/ws", &wcfg));
    g_xw_conns[0] = g_xw_conns[1] = NULL;
    g_xw_opened = 0;
    g_xw_sent = 0;
    KlPlatThread tid;
    kl_plat_thread_create(&tid, server_thread_fn, &xw_srv);
    wait_for_bind(&xw_srv);
    int port = xw_srv.bound_port;

    size_t want = (size_t)XW_FRAMES * (XW_FRAME + 4), got = 0;
    int b = ws_connect_upgraded(port, 4096);               /* the subscriber: reads, never sends */
    for (int i = 0; i < 200 && g_xw_opened < 1; i++) kl_test_sleep_ms(10);
    int a = ws_connect_upgraded(port, 0);                  /* the publisher */
    for (int i = 0; i < 200 && g_xw_opened < 2; i++) kl_test_sleep_ms(10);
    if (a >= 0 && b >= 0) {
        static const uint8_t go[] = { 0x81, 0x82, 1, 2, 3, 4, 'g' ^ 1, 'o' ^ 2 };   /* masked "go" */
        (void)kl_test_sockwrite(a, (const char *)go, sizeof go);
        static char buf[16 * 1024];
        uint64_t start = kl_monotonic_ms(), last = start;
        while (got < want && kl_monotonic_ms() - start < 10000 && kl_monotonic_ms() - last < 2000) {
            if (kl_test_poll1(b, 0, 50) <= 0) continue;
            kl_ssize_t n = kl_test_sockread(b, buf, sizeof buf);
            if (n <= 0) break;
            got += (size_t)n;
            last = kl_monotonic_ms();
            kl_test_sleep_ms(1);                           /* slow but steady */
        }
    }
    if (a >= 0) kl_test_closesock(a);
    if (b >= 0) kl_test_closesock(b);
    kl_http_server_stop(&xw_srv);
    kl_plat_thread_join(&tid);
    kl_http_server_free(&xw_srv);
    printf("  cross-connection backlog: %zu of %zu bytes, %d frames queued\n", got, want, g_xw_sent);
    ASSERT_TRUE(a >= 0 && b >= 0);
    ASSERT_EQ(g_xw_sent, XW_FRAMES);
    ASSERT_EQ(got, want);                                  /* was (readiness): stalled in B's drain */
}

/* ── A plaintext stream written while suspended keeps moving ────────────────────────────────────
 * Readiness: a handler starts a stream, suspends, and pushes chunks from a timer (an event feed).
 * The client pauses reading, then reads steadily. Once a write would-block, the drain held bytes,
 * and a later write only appended to it without trying the socket; the suspended connection has no
 * WRITE interest, so nothing more went out until the resume (or the drain's cap failed the stream). */
#define SF_CHUNK  2048
#define SF_CHUNKS 60
typedef struct {
    KlAsyncOp op;
    KlHttpResponse *res;
    KlHttpResponseWriteFn w;
    void *wc;
    int next, done, failed;
    uint64_t resumed_ms;
} SfStream;
static SfStream g_sf;
static KlHttpServer sf_srv;

static void sf_resume(KlAsyncOp *op, void *ud) {
    (void)op; (void)ud;
    g_sf.resumed_ms = kl_monotonic_ms();
    kl_http_response_end_stream(g_sf.res);
}
static void sf_push(void *ud) {
    (void)ud;
    if (g_sf.done || g_sf.failed) return;
    if (g_sf.next < SF_CHUNKS) {                           /* a numbered chunk */
        char chunk[SF_CHUNK];
        memset(chunk, '.', sizeof chunk);
        int hl = snprintf(chunk, sizeof chunk, "<D%03d>", g_sf.next);
        chunk[hl] = '.';
        if (g_sf.w(g_sf.wc, chunk, sizeof chunk) < 0) { g_sf.failed = 1; return; }
        g_sf.next++;
    } else if (g_sf.w(g_sf.wc, "h;", 2) < 0) {             /* then a heartbeat */
        g_sf.failed = 1;
        return;
    }
    (void)kl_timer_add(kl_http_server_event_ctx(&sf_srv), 20, sf_push, NULL);
}
static void sf_finish(void *ud) {
    (void)ud;
    g_sf.done = 1;
    kl_async_complete(&sf_srv, &g_sf.op);
}
static void handle_suspended_feed(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)ctx;
    memset(&g_sf, 0, sizeof g_sf);
    g_sf.res = res;
    KlHttpConn *c = kl_http_request_conn(req);
    shrink_sndbuf(c);
    if (kl_http_response_begin_stream(res, 200, &g_sf.w, &g_sf.wc) < 0) return;
    g_sf.op.on_resume = sf_resume;
    if (kl_async_suspend(&sf_srv, c, &g_sf.op) < 0) return;
    (void)kl_timer_add(kl_http_server_event_ctx(&sf_srv), 20, sf_push, NULL);
    (void)kl_timer_add(kl_http_server_event_ctx(&sf_srv), 6000, sf_finish, NULL);
}

UTEST(completion_output, a_stream_written_while_suspended_keeps_moving) {
    KlHttpServerConfig cfg = { .port = 0, .max_connections = 4 };
    ASSERT_EQ(0, kl_http_server_init(&sf_srv, &cfg));
    kl_http_server_route(&sf_srv, "GET", "/feed", handle_suspended_feed, NULL, NULL);
    KlPlatThread tid;
    kl_plat_thread_create(&tid, server_thread_fn, &sf_srv);
    wait_for_bind(&sf_srv);
    char last[16];
    snprintf(last, sizeof last, "<D%03d>", SF_CHUNKS - 1);
    static char buf[256 * 1024];
    size_t have = 0;
    buf[0] = '\0';
    int seen_last = 0;
    int fd = connect_rcvbuf(sf_srv.bound_port, 4096);
    if (fd >= 0) {
        const char *rq = "GET /feed HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
        (void)kl_test_sockwrite(fd, rq, strlen(rq));
        kl_test_sleep_ms(500);                             /* a pause: the server's sends block */
        uint64_t start = kl_monotonic_ms();                /* then steady reading, well before */
        while (kl_monotonic_ms() - start < 4000) {         /* the resume at 6 s */
            if (kl_test_poll1(fd, 0, 50) <= 0) continue;
            kl_ssize_t n = kl_test_sockread(fd, buf + have, sizeof buf - 1 - have);
            if (n <= 0) break;
            have += (size_t)n;
            buf[have] = '\0';
            if (strstr(buf, last)) { seen_last = 1; break; }
            if (have > sizeof buf - 4096) break;
        }
        kl_test_closesock(fd);
    }
    uint64_t resumed_ms = g_sf.resumed_ms;
    int pushed = g_sf.next, failed = g_sf.failed;
    kl_http_server_stop(&sf_srv);
    kl_plat_thread_join(&tid);
    kl_http_server_free(&sf_srv);
    printf("  suspended feed: %zu bytes read, %d of %d chunks pushed, failed %d\n",
           have, pushed, SF_CHUNKS, failed);
    ASSERT_EQ(resumed_ms, (uint64_t)0);                    /* all of it while still suspended */
    ASSERT_FALSE(failed);
    ASSERT_TRUE(seen_last);                                /* was (readiness): stuck until resume */
}

/* ── A buffered response larger than the backend's send_max ──────────────────────────────────────
 *
 * A completion backend may cap one post_send (KlCompletionOps.send_max): EFI copies a send into a
 * buffer of that size and fails a longer post. The output queue honoured the cap, but a buffered
 * plaintext response was posted whole, so on such a backend any response above the cap failed and
 * the connection closed with nothing sent. The compiled-in completion backend is wrapped here as a
 * runtime provider whose post_send enforces a 4 KiB cap the way EFI does; a 64 KiB buffered response
 * must still arrive whole. Completion backends only. */
#define SMAX_CAP  4096u
#define SMAX_BODY (64 * 1024)
static char g_smax_body[SMAX_BODY];
static int  g_smax_oversized;     /* posts the wrapper refused */
static KlCompletionOps g_smax_comp;
static KlEventOps      g_smax_ops;
static const KlEventProvider g_smax_prov = { &g_smax_ops, "send-max-cap" };

static int  smax_init(KlEventLoop *l) { return kl_event_init_builtin(l); }
static int  smax_add(KlEventLoop *l, KlSocketHandle fd, KlEventMask m, void *u) {
    return kl_event_add_builtin(l, fd, m, u);
}
static int  smax_mod(KlEventLoop *l, KlSocketHandle fd, KlEventMask m, void *u) {
    return kl_event_mod_builtin(l, fd, m, u);
}
static int  smax_del(KlEventLoop *l, KlSocketHandle fd) { return kl_event_del_builtin(l, fd); }
static int  smax_wait(KlEventLoop *l, KlEvent *o, int mx, int to) {
    return kl_event_wait_builtin(l, o, mx, to);
}
static void smax_close(KlEventLoop *l) { kl_event_close_builtin(l); }
static unsigned smax_caps(const KlEventLoop *l) { return kl_event_caps_builtin(l); }
static const struct KlSocketProvider *smax_native(const KlEventLoop *l) {
    return kl_event_native_provider_builtin(l);
}
static int smax_post_send(KlStream *st, const KlIoVec *iov, int n, size_t total) {
    if (total > SMAX_CAP) { g_smax_oversized++; return -1; }   /* as EFI's el_post_send */
    return kl_comp_ops_builtin()->post_send(st, iov, n, total);
}

static void handle_smax(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)ctx;
    memset(g_smax_body, 'S', sizeof g_smax_body);
    kl_http_response_status(res, 200);
    kl_http_response_body_borrow(res, g_smax_body, sizeof g_smax_body);
}

static KlHttpServer smax_srv;

UTEST(completion_output, a_buffered_response_above_send_max_arrives_whole) {
    KlEventLoop probe;
    memset(&probe, 0, sizeof(probe));
    if (!kl_comp_ops_builtin() || !(kl_event_caps_builtin(&probe) & KL_EVENT_CAP_COMPLETION))
        UTEST_SKIP("send_max applies only to completion backends");
    g_smax_comp = *kl_comp_ops_builtin();
    g_smax_comp.post_send = smax_post_send;
    g_smax_comp.send_max = SMAX_CAP;
    memset(&g_smax_ops, 0, sizeof(g_smax_ops));
    g_smax_ops.init = smax_init; g_smax_ops.add = smax_add; g_smax_ops.mod = smax_mod;
    g_smax_ops.del = smax_del; g_smax_ops.wait = smax_wait; g_smax_ops.close = smax_close;
    g_smax_ops.caps = smax_caps; g_smax_ops.native_provider = smax_native;
    g_smax_ops.completion = &g_smax_comp;
    g_smax_oversized = 0;

    KlHttpServerConfig cfg = { .port = 0, .max_connections = 4 };
    cfg.event_provider = &g_smax_prov;
    ASSERT_EQ(0, kl_http_server_init(&smax_srv, &cfg));
    kl_http_server_route(&smax_srv, "GET", "/big", handle_smax, NULL, NULL);
    KlPlatThread tid;
    kl_plat_thread_create(&tid, server_thread_fn, &smax_srv);
    wait_for_bind(&smax_srv);

    static char buf[SMAX_BODY + 1024];
    int closed = 0;
    kl_ssize_t got = 0;
    int fd = connect_rcvbuf(smax_srv.bound_port, 0);
    if (fd >= 0) {
        const char *rq = "GET /big HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
        (void)kl_test_sockwrite(fd, rq, strlen(rq));
        got = read_until_close(fd, buf, sizeof buf, 3000, &closed);
        kl_test_closesock(fd);
    }
    int oversized = g_smax_oversized;
    kl_http_server_stop(&smax_srv);
    kl_plat_thread_join(&tid);
    kl_http_server_free(&smax_srv);

    const char *body = strstr(buf, "\r\n\r\n");
    size_t body_len = body ? (size_t)(got - (body + 4 - buf)) : 0;
    ASSERT_EQ(0, oversized);                         /* was 1: the whole response in one post */
    ASSERT_TRUE(strncmp(buf, "HTTP/1.1 200", 12) == 0);
    ASSERT_EQ((size_t)SMAX_BODY, body_len);
}

UTEST_MAIN();
