/* test_reject_drain.c: the early-response teardown invariant (#278).
 *
 * INVARIANT UNDER TEST: once Keel has committed to a final response, connection teardown must not
 * invalidate that response merely because unread request bytes remain.
 *
 * A server that rejects a request early leaves the rest of the body unread. Closing a socket with
 * unread received data makes TCP send RST, and the peer then discards the response it had already
 * buffered, so the client sees a reset instead of the status explaining why. The server therefore
 * half-closes its send side and drains inbound bytes, bounded by BOTH a byte cap and a deadline.
 *
 * These assert the SEMANTIC outcome, not timing: the client must be able to read the complete final
 * response. How long the drain took, and which bound ended it, are deliberately not asserted. The
 * suite is backend-agnostic and is enrolled on both the readiness and completion axes, because
 * "readiness and completion produce the same observable result" is part of the contract.
 */
#include "utest.h"
#include <keel/keel.h>
#include "net_compat.h"
#include <string.h>
#include <stdio.h>
#include <pthread.h>

#define CRLF "\r\n"

static KlHttpServer rd_server;
static pthread_t    rd_tid;
static int          rd_port;
static int          rd_live;

static void rd_echo(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)ctx;
    KlHttpBufReader *br = (KlHttpBufReader *)req->body_reader;
    kl_http_response_json(res, 200, "{\"ok\":1}", 8);
    (void)br;
}


/* A body reader that produces the FINAL response from inside on_data, while the declared body is
 * still outstanding. This is the other way into the drain: not a rejection, but a handler deciding
 * mid-stream that it has seen enough. It reaches teardown through kl_http_conn_send_complete()
 * rather than through the rejection helper, which is a genuinely different code path on the
 * completion axis (a write completion, not a dispatch return). #270 was exactly this path being
 * closed instead of drained. */
typedef struct { KlHttpBodyReader base; KlAllocator *alloc; const KlHttpRequest *req; KlHttpResponse *res; int done; } RdEarly;

static int rd_early_on_data(KlHttpBodyReader *self, const char *data, size_t len) {
    RdEarly *r = (RdEarly *)self;
    (void)data; (void)len;
    if (r->done || !r->req || !r->res) return 0;
    kl_http_response_status(r->res, 200);
    kl_http_response_header(r->res, "Content-Type", "text/plain");
    kl_http_response_body_borrow(r->res, "seen enough", 11);
    kl_http_request_send_response(r->req);   /* -> SENDING; the rest of the body is never read */
    r->done = 1;
    return 0;
}
static void rd_early_noop(KlHttpBodyReader *self) { (void)self; }
static void rd_early_destroy(KlHttpBodyReader *self) {
    RdEarly *r = (RdEarly *)self;
    kl_free(r->alloc, r, sizeof(*r));
}
static KlHttpBodyReader *rd_early_factory(KlAllocator *alloc, const KlHttpRequest *req, void *ud) {
    (void)ud; (void)req;
    RdEarly *r = kl_malloc(alloc, sizeof(*r));
    if (!r) return NULL;
    memset(r, 0, sizeof(*r));
    r->base.on_data = rd_early_on_data;
    r->base.on_complete = rd_early_noop;
    r->base.on_error = rd_early_noop;
    r->base.destroy = rd_early_destroy;
    r->alloc = alloc;
    return &r->base;   /* req + res are stashed by the streaming handler below */
}

/* Streaming route handler: runs at dispatch, BEFORE the body is complete, hands the reader the req
 * and res, then yields so the loop keeps pumping on_data. */
static void rd_early_handler(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)ctx;
    RdEarly *r = (RdEarly *)req->body_reader;
    if (!r) { kl_http_response_error(res, 500, "no reader"); return; }
    r->req = req;
    r->res = res;
    kl_http_request_await_body(req);
}

static void *rd_thread(void *a) { (void)a; kl_http_server_run(&rd_server); return NULL; }

static int rd_start(size_t drain_bytes, uint32_t drain_ms) {
    KlHttpServerConfig cfg = {
        .port = 0, .bind_addr = "127.0.0.1", .max_body_size = 4096,
        .reject_drain_max_bytes = drain_bytes,
        .reject_drain_timeout_ms = drain_ms,
    };
    if (kl_http_server_init(&rd_server, &cfg) != 0) return -1;
    kl_http_server_route(&rd_server, "POST", "/echo", rd_echo,
                         (void *)(size_t)(64 * 1024), kl_http_body_reader_buffer);
    kl_http_server_route(&rd_server, "POST", "/deny", rd_echo, NULL, NULL);   /* no reader: discard path */
    kl_http_server_route_streaming(&rd_server, "POST", "/early", rd_early_handler, NULL,
                                   rd_early_factory);
    if (pthread_create(&rd_tid, NULL, rd_thread, NULL) != 0) return -1;
    rd_live = 1;
    for (int i = 0; i < 400 && rd_server.bound_port == 0; i++) usleep(5000);
    rd_port = rd_server.bound_port;
    return (rd_port > 0) ? 0 : -1;
}

static void rd_stop(void) {
    if (!rd_live) return;
    kl_http_server_stop(&rd_server);
    pthread_join(rd_tid, NULL);
    rd_live = 0;
    kl_http_server_free(&rd_server);
}

static int rd_connect(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)rd_port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { kl_test_closesock(fd); return -1; }
    kl_test_set_rcvtimeo(fd, 4000);   /* bound the read so a regression fails rather than hangs */
    return fd;
}

/* Read until the peer finishes or the receive timeout fires. Returns bytes read. Distinguishing a
 * clean EOF from an error is deliberately NOT done here: the assertions are about whether the
 * response text arrived, which is the property that matters to a client. */
static long rd_read_all(int fd, char *buf, size_t cap) {
    long total = 0;
    for (;;) {
        long n = (long)kl_test_sockread(fd, buf + total, cap - (size_t)total - 1);
        if (n <= 0) break;
        total += n;
        if ((size_t)total >= cap - 1) break;
    }
    buf[total] = '\0';
    return total;
}

/* ── The invariant, across the ways a request can end early ─────────────────── */

/* 413 while the client keeps uploading: the canonical case. The client sends well past the route's
 * cap and does not stop, so the server rejects with a large amount still in flight. */
UTEST(reject_drain, oversized_body_while_client_keeps_sending) {
    ASSERT_EQ(0, rd_start(0, 0));   /* defaults: 64 KiB / 500 ms */
    int fd = rd_connect();
    ASSERT_TRUE(fd >= 0);

    const char *hdr = "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 400000\r\n"
                      "Connection: close\r\n\r\n";
    ASSERT_TRUE(kl_test_sockwrite(fd, hdr, strlen(hdr)) > 0);
    char chunk[8192];
    memset(chunk, 'A', sizeof(chunk));
    for (int i = 0; i < 12; i++)   /* 96 KiB: past the 64 KiB route cap, outstanding within the drain budget */
        if (kl_test_sockwrite(fd, chunk, sizeof(chunk)) < 0) break;   /* peer may half-close; fine */

    char buf[4096];
    (void)rd_read_all(fd, buf, sizeof(buf));
    kl_test_closesock(fd);
    ASSERT_TRUE(strstr(buf, "413") != NULL);   /* the response survived teardown */
    rd_stop();
}

/* Content-Length larger than the bytes actually supplied: the client under-delivers and then just
 * waits. The drain must not sit for the missing bytes; the deadline ends it and the response stands. */
UTEST(reject_drain, content_length_larger_than_supplied) {
    ASSERT_EQ(0, rd_start(0, 0));
    int fd = rd_connect();
    ASSERT_TRUE(fd >= 0);

    const char *hdr = "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 300000\r\n"
                      "Connection: close\r\n\r\n";
    ASSERT_TRUE(kl_test_sockwrite(fd, hdr, strlen(hdr)) > 0);
    char chunk[8192];
    memset(chunk, 'B', sizeof(chunk));
    for (int i = 0; i < 10; i++)
        if (kl_test_sockwrite(fd, chunk, sizeof(chunk)) < 0) break;
    /* deliberately send no more, and never close: the stalled-client shape */

    char buf[4096];
    (void)rd_read_all(fd, buf, sizeof(buf));
    kl_test_closesock(fd);
    ASSERT_TRUE(strstr(buf, "413") != NULL);
    rd_stop();
}

/* Chunked body rejected mid-stream: framing is unknown, so only the byte cap bounds the drain. */
UTEST(reject_drain, chunked_rejected_mid_stream) {
    ASSERT_EQ(0, rd_start(0, 0));
    int fd = rd_connect();
    ASSERT_TRUE(fd >= 0);

    const char *hdr = "POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n"
                      "Connection: close\r\n\r\n";
    ASSERT_TRUE(kl_test_sockwrite(fd, hdr, strlen(hdr)) > 0);
    char body[4096];
    memset(body, 'C', sizeof(body));
    for (int i = 0; i < 20; i++) {   /* 80 KiB of chunks: rejected mid-stream, outstanding within budget */
        char sz[32];
        int n = snprintf(sz, sizeof(sz), "%zx\r\n", sizeof(body));
        if (n <= 0 || kl_test_sockwrite(fd, sz, (size_t)n) < 0) break;
        if (kl_test_sockwrite(fd, body, sizeof(body)) < 0) break;
        if (kl_test_sockwrite(fd, "\r\n", 2) < 0) break;
    }

    char buf[4096];
    (void)rd_read_all(fd, buf, sizeof(buf));
    kl_test_closesock(fd);
    ASSERT_TRUE(strstr(buf, "413") != NULL);
    rd_stop();
}

/* A route with NO body reader and an oversized declared Content-Length. This is the DISCARD-path
 * early reject (a different code site from the body-reader cap above), and it fires before the
 * handler runs at all, so the 403 the handler would have sent never happens: the server answers 413.
 * Covered separately because it is the rejection that happens earliest, with the whole declared body
 * still outstanding. */
UTEST(reject_drain, no_reader_oversized_declared_body) {
    ASSERT_EQ(0, rd_start(0, 0));
    int fd = rd_connect();
    ASSERT_TRUE(fd >= 0);

    const char *hdr = "POST /deny HTTP/1.1\r\nHost: x\r\nContent-Length: 200000\r\n"
                      "Connection: close\r\n\r\n";
    ASSERT_TRUE(kl_test_sockwrite(fd, hdr, strlen(hdr)) > 0);
    char chunk[8192];
    memset(chunk, 'D', sizeof(chunk));
    for (int i = 0; i < 4; i++)
        if (kl_test_sockwrite(fd, chunk, sizeof(chunk)) < 0) break;

    char buf[4096];
    (void)rd_read_all(fd, buf, sizeof(buf));
    kl_test_closesock(fd);
    ASSERT_TRUE(strstr(buf, "413") != NULL);   /* discard-path reject, not the handler */
    rd_stop();
}

/* Byte-cap expiry, and the BOUNDARY of the guarantee. A 1 KiB budget cannot cover an outstanding
 * body far larger than it, so the drain ends on the cap with data still unread and the close can
 * still RST the response away. That is the deliberate trade the cap exists to make: bounded work
 * beats guaranteed delivery to a client that keeps sending past the bound. So this asserts what IS
 * promised at the boundary, that the server retires the connection and keeps serving, and not that
 * the response arrives. Delivery is covered by the cases above, where the outstanding body fits.
 *
 * DO NOT "fix" this test by removing the cap or raising it until delivery succeeds. It is pinning a
 * security boundary, not tolerating an incomplete assertion: an unlimited drain hands any peer an
 * unbounded hold on the single-threaded loop. See docs/contracts/early_rejection_drain.md. */
UTEST(reject_drain, byte_cap_ends_the_drain_without_promising_delivery) {
    ASSERT_EQ(0, rd_start(1024, 5000));
    int fd = rd_connect();
    ASSERT_TRUE(fd >= 0);

    const char *hdr = "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 400000\r\n"
                      "Connection: close\r\n\r\n";
    ASSERT_TRUE(kl_test_sockwrite(fd, hdr, strlen(hdr)) > 0);
    char chunk[8192];
    memset(chunk, 'E', sizeof(chunk));
    for (int i = 0; i < 40; i++)
        if (kl_test_sockwrite(fd, chunk, sizeof(chunk)) < 0) break;

    char buf[4096];
    (void)rd_read_all(fd, buf, sizeof(buf));
    kl_test_closesock(fd);

    /* The server survived the capped drain and still answers. */
    int fd2 = rd_connect();
    ASSERT_TRUE(fd2 >= 0);
    const char *ok = "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\n"
                     "Connection: close\r\n\r\nhi";
    ASSERT_TRUE(kl_test_sockwrite(fd2, ok, strlen(ok)) > 0);
    char buf2[2048];
    (void)rd_read_all(fd2, buf2, sizeof(buf2));
    kl_test_closesock(fd2);
    ASSERT_TRUE(strstr(buf2, "200 OK") != NULL);
    rd_stop();
}

/* Deadline expiry: a large budget with a very short deadline against a client that under-delivers,
 * so the deadline is necessarily what ends the drain. */
UTEST(reject_drain, deadline_ends_the_drain) {
    ASSERT_EQ(0, rd_start(1024 * 1024, 50));
    int fd = rd_connect();
    ASSERT_TRUE(fd >= 0);

    const char *hdr = "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 500000\r\n"
                      "Connection: close\r\n\r\n";
    ASSERT_TRUE(kl_test_sockwrite(fd, hdr, strlen(hdr)) > 0);
    char chunk[8192];
    memset(chunk, 'F', sizeof(chunk));
    for (int i = 0; i < 10; i++)
        if (kl_test_sockwrite(fd, chunk, sizeof(chunk)) < 0) break;

    char buf[4096];
    (void)rd_read_all(fd, buf, sizeof(buf));
    kl_test_closesock(fd);
    ASSERT_TRUE(strstr(buf, "413") != NULL);
    rd_stop();
}

/* Peer resets during the drain: the server must retire the connection cleanly and, crucially, must
 * still be serving afterwards. Asserts the server's liveness rather than the dead client's view. */
UTEST(reject_drain, peer_reset_during_drain_leaves_server_healthy) {
    ASSERT_EQ(0, rd_start(0, 0));
    int fd = rd_connect();
    ASSERT_TRUE(fd >= 0);

    const char *hdr = "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 400000\r\n"
                      "Connection: close\r\n\r\n";
    ASSERT_TRUE(kl_test_sockwrite(fd, hdr, strlen(hdr)) > 0);
    char chunk[8192];
    memset(chunk, 'G', sizeof(chunk));
    for (int i = 0; i < 20; i++)
        if (kl_test_sockwrite(fd, chunk, sizeof(chunk)) < 0) break;

    /* Hard reset: SO_LINGER {on, 0} makes close send RST rather than FIN. */
    struct linger lg; lg.l_onoff = 1; lg.l_linger = 0;
    (void)setsockopt(fd, SOL_SOCKET, SO_LINGER, (const char *)&lg, sizeof(lg));
    kl_test_closesock(fd);

    /* The server survived the reset mid-drain and still answers. */
    int fd2 = rd_connect();
    ASSERT_TRUE(fd2 >= 0);
    const char *ok = "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\n"
                     "Connection: close\r\n\r\nhi";
    ASSERT_TRUE(kl_test_sockwrite(fd2, ok, strlen(ok)) > 0);
    char buf[4096];
    (void)rd_read_all(fd2, buf, sizeof(buf));
    kl_test_closesock(fd2);
    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);
    rd_stop();
}

/* The drain must not damage ordinary traffic: a request whose body IS fully consumed keeps its
 * keep-alive behaviour and never enters teardown draining. Two requests on one connection. */
UTEST(reject_drain, successful_keepalive_is_unaffected) {
    ASSERT_EQ(0, rd_start(0, 0));
    int fd = rd_connect();
    ASSERT_TRUE(fd >= 0);

    const char *r1 = "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello";
    ASSERT_TRUE(kl_test_sockwrite(fd, r1, strlen(r1)) > 0);
    char buf[2048];
    long n = (long)kl_test_sockread(fd, buf, sizeof(buf) - 1);
    ASSERT_TRUE(n > 0);
    buf[n] = '\0';
    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);

    const char *r2 = "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n"
                     "Connection: close\r\n\r\nworld";
    ASSERT_TRUE(kl_test_sockwrite(fd, r2, strlen(r2)) > 0);   /* connection was reused */
    char buf2[2048];
    (void)rd_read_all(fd, buf2, sizeof(buf2));
    kl_test_closesock(fd);
    ASSERT_TRUE(strstr(buf2, "200 OK") != NULL);
    rd_stop();
}


/* The framing oracle, specifically. A WELL-FORMED chunked upload rejected on size, where the client
 * then sends a proper terminal chunk. The drain must end because the decoder reached the terminal
 * chunk, NOT because the 500 ms deadline expired, so the test also bounds the elapsed time: a drain
 * that ignored framing and ran to the deadline would take ~500 ms and fail this. That distinction is
 * the reason the drain feeds the real decoder instead of counting bytes.
 *
 * Both OTHER bounds are deliberately put out of reach, because this case is only meaningful if
 * framing is the one that can fire. The budget is 1 MiB against ~80 KiB of upload: at 64 KiB it sat
 * in the same order as the unread remainder, so a faster or slower loop could end the drain on the
 * CAP instead, which legitimately loses the response and made this flake on io_uring. Raising it
 * costs the test nothing, because the elapsed-time bound below is what does the discriminating: a
 * drain that ignored framing would run to the 2000 ms deadline and fail. */
UTEST(reject_drain, chunked_terminal_chunk_ends_drain_before_deadline) {
    ASSERT_EQ(0, rd_start(1024 * 1024, 2000));   /* only framing is in reach; see above */
    int fd = rd_connect();
    ASSERT_TRUE(fd >= 0);

    const char *hdr = "POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n"
                      "Connection: close\r\n\r\n";
    ASSERT_TRUE(kl_test_sockwrite(fd, hdr, strlen(hdr)) > 0);
    char body[4096];
    memset(body, 'H', sizeof(body));
    for (int i = 0; i < 20; i++) {   /* past the 64 KiB route cap, so it is rejected mid-stream */
        char sz[32];
        int n = snprintf(sz, sizeof(sz), "%zx\r\n", sizeof(body));
        if (n <= 0 || kl_test_sockwrite(fd, sz, (size_t)n) < 0) break;
        if (kl_test_sockwrite(fd, body, sizeof(body)) < 0) break;
        if (kl_test_sockwrite(fd, "\r\n", 2) < 0) break;
    }
    (void)kl_test_sockwrite(fd, "0\r\n\r\n", 5);   /* proper terminal chunk */

    uint64_t t0 = kl_monotonic_ms();
    char buf[4096];
    (void)rd_read_all(fd, buf, sizeof(buf));
    uint64_t elapsed = kl_monotonic_ms() - t0;
    kl_test_closesock(fd);

    ASSERT_TRUE(strstr(buf, "413") != NULL);
    ASSERT_TRUE(elapsed < 1500);   /* framing ended it, not the deadline */
    rd_stop();
}

/* The send-complete path into the drain, on both axes. The handler answers from inside on_data with
 * most of the declared body still outstanding, so teardown is decided by kl_http_conn_send_complete()
 * after the write is retired. On the completion axis that is a write completion, and treating
 * anything-but-keep-alive as "close now" there closed on top of unread bytes and destroyed the
 * response (#270: the readiness axis delivered all 75 bytes, the completion axis delivered 0 and the
 * client saw WSAECONNABORTED). Asserts only the observable outcome, so it is backend-agnostic.
 *
 * Honest about its own power: this exercises the path (dispatch -> SENDING -> write retired ->
 * send_complete -> DRAINING) but it is NOT a sharp oracle for #270. It passes even with the defect,
 * because the client is already blocked in recv when the reset arrives and so reads the response
 * out of its own receive queue first. Widening that window is what makes the failure appear, which
 * is why the deterministic oracle is integration.streaming_mid_stream_early_exit[_on_error] (whose
 * client polls for the handler before reading), now enrolled on the completion axis. Keep this case
 * for the path coverage on BOTH axes; do not read a pass here as proof the defect is absent. */
UTEST(reject_drain, early_handler_response_survives_on_both_axes) {
    ASSERT_EQ(0, rd_start(1024 * 1024, 500));
    int fd = rd_connect();
    ASSERT_TRUE(fd >= 0);

    /* Declare far more than we send, then stop: the body stays outstanding forever. */
    const char *hdr = "POST /early HTTP/1.1" CRLF "Host: x" CRLF
                      "Content-Length: 200000" CRLF "Connection: close" CRLF CRLF;
    ASSERT_TRUE(kl_test_sockwrite(fd, hdr, strlen(hdr)) > 0);

    /* Push 32 KiB in one burst. The server answers on its first on_data, so the REST is still
     * sitting unread in the receive queue at the moment teardown is decided, which is what makes a
     * close abortive. A client that merely stops sending does not reproduce this: the queue drains
     * to empty on its own and the close is graceful, so the bug hides. 32 KiB also stays well under
     * the 1 MiB budget, so the cap cannot be what ends the drain. */
    char chunk[8192];
    memset(chunk, 'Z', sizeof(chunk));
    for (int i = 0; i < 4; i++)
        if (kl_test_sockwrite(fd, chunk, sizeof(chunk)) < 0) break;

    char buf[2048];
    (void)rd_read_all(fd, buf, sizeof(buf));
    kl_test_closesock(fd);

    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);
    ASSERT_TRUE(strstr(buf, "seen enough") != NULL);
    rd_stop();
}

/* OVER-SEND past the declared Content-Length (#281). A peer may send more than it advertised, and
 * the drain must not stop merely because the DECLARED framing is satisfied: the excess is still in
 * the receive queue, and closing on top of it resets the response away. The budget used to be
 * min(declared remainder, cap), which put those bytes outside the budget by construction; it is now
 * the cap alone.
 *
 * PATH COVERAGE, NOT AN ORACLE. Read this before trusting a pass. I could not build a portable
 * deterministic test for this defect from the client side, and measured three attempts rather than
 * assuming:
 *
 *   body assertion only, chunked writes    passed 10/10 against the very clamp it targets
 *   plus an orderly-FIN assertion          caught it 3 times in 12 on Windows, but FAILED on kqueue,
 *                                          because between two writes the server may legitimately
 *                                          see an empty queue with declared framing complete, close,
 *                                          and race the rest of the body (the #281 residual)
 *   single write, no inter-write gap       portable and stable, but detects nothing: 0/10 both ways
 *
 * The last form is what is here, because a test that is red on one backend is worse than one that is
 * honest about its reach. The server-side defect IS deterministic; what is racy is whether the
 * client notices, since the reset has to overtake data already in its receive queue. The real
 * evidence is the recorded trace: build with -DKEEL_INTERNAL_TRACE and the drain shows
 * `budget-exhausted` at exactly the declared length, framing complete, bytes still unread and the
 * full deadline unused. Do not read a pass here as proof the defect is absent. */
UTEST(reject_drain, over_send_past_declared_length_is_drained) {
    ASSERT_EQ(0, rd_start(64 * 1024, 500));
    int fd = rd_connect();
    ASSERT_TRUE(fd >= 0);

    const char *hdr = "POST /deny HTTP/1.1" CRLF "Host: x" CRLF
                      "Content-Length: 16384" CRLF "Connection: close" CRLF CRLF;
    ASSERT_TRUE(kl_test_sockwrite(fd, hdr, strlen(hdr)) > 0);

    /* One write: between two, the server may see an empty queue with the declared framing complete
     * and close, which is the residual race above rather than anything this case is testing. */
    static char body[40960];   /* 40960 sent against 16384 declared: 24576 bytes beyond */
    memset(body, 'O', sizeof(body));
    ASSERT_TRUE(kl_test_sockwrite(fd, body, sizeof(body)) > 0);

    char buf[2048];
    (void)rd_read_all(fd, buf, sizeof(buf));
    kl_test_closesock(fd);

    ASSERT_TRUE(strstr(buf, "413") != NULL);
    rd_stop();
}

/* The adjacent gate: framing ALREADY COMPLETE, with excess bytes queued (#281).
 *
 * kl_http_conn_begin_drain() used to skip the drain outright when the declared framing was complete
 * or the declared remainder was zero. Both are derived from Content-Length, which is a statement
 * about the PROTOCOL, not about the socket: a peer that over-sends satisfies the framing while
 * leaving bytes in the receive queue, and the server then closed straight on top of them. The gate
 * now asks the transport with one non-destructive peek before skipping.
 *
 * This arrives through the SUCCESS path rather than a rejection: 4096 declared against a 4096 limit
 * is a legal request, so the handler answers 200 and the connection closes (Connection: close) with
 * 36864 over-sent bytes still queued. The invariant is the same one the whole file is about, and it
 * does not care which status was committed to: once Keel has committed to a final response, teardown
 * must not invalidate it because unread request bytes remain.
 *
 * Unlike the other over-send case in this file, this one is a real oracle: 8 failures in 10 runs
 * before the fix, 0 in 10 after. What makes it work is asserting the TRANSPORT-level outcome, that
 * the connection ends with an orderly FIN, rather than only that the body arrived. On loopback the
 * 200 survived the abortive close either way, so a body-only assertion passes with the defect
 * present and proves nothing; the close being abortive is the defect, and that is observable.
 *
 * Worth stating because I got it wrong first: an earlier draft of this case asserted "413" on the
 * theory that the request was rejected. It is not, 4096 against a 4096 limit is legal, so the 8/8
 * failures that draft produced were the wrong status code, not the defect. */
UTEST(reject_drain, over_sent_bytes_after_a_complete_body_do_not_reset_the_response) {
    ASSERT_EQ(0, rd_start(64 * 1024, 500));
    int fd = rd_connect();
    ASSERT_TRUE(fd >= 0);

    const char *hdr = "POST /deny HTTP/1.1" CRLF "Host: x" CRLF
                      "Content-Length: 4096" CRLF "Connection: close" CRLF CRLF;
    ASSERT_TRUE(kl_test_sockwrite(fd, hdr, strlen(hdr)) > 0);

    static char body[40960];   /* 4096 declared, 36864 bytes beyond it */
    memset(body, 'X', sizeof(body));
    ASSERT_TRUE(kl_test_sockwrite(fd, body, sizeof(body)) > 0);

    char buf[2048];
    size_t got = 0;
    long last;
    for (;;) {
        last = kl_test_sockread(fd, buf + got, (sizeof(buf) - 1) - got);
        if (last <= 0) break;
        got += (size_t)last;
        if (got >= sizeof(buf) - 1) break;
    }
    buf[got] = 0;
    kl_test_closesock(fd);

    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);
    ASSERT_EQ(0L, last);   /* orderly FIN: the close must not be abortive */
    rd_stop();
}

UTEST_MAIN();
