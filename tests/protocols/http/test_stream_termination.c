/*
 * test_stream_termination.c: once a streamed response's termination has been requested, nothing is
 * appended to it, and its connection is reused only after the whole response framing went out.
 *
 * Every case reads the wire as a client does: a first response on a keep-alive connection, parsed
 * to its end (the chunked terminator), then a second request on the same connection, whose response
 * must begin on the very next byte. A body cut short, a chunk after the terminator, or framing left
 * behind for the next response all break that boundary. Testing only the first body is not enough:
 * the defects these pin (a tail lost to a keep-alive reset, a chunk after the terminating chunk)
 * show up where the second response starts.
 *
 * The producers end their stream from each place termination can be requested: the drain's
 * on_drain after the op completed, the bounded writable edge, an async completion after buffered
 * output, and the two compressed finishes. Pipelined requests (not supported) must get one complete
 * response and a close, never a hang or a reused connection. Runs on every loop: readiness here,
 * completion on the pollcomp / io_uring / IOCP lanes.
 */
#include "utest.h"
#include <keel/keel.h>
#include <keel/tls.h>
#include <keel/http_compress.h>
#include "net_compat.h"
#include "mock_tls.h"
#include "http_conn_internal.h"   /* white-box: the server-side socket, to shrink its send buffer */
#include "platform_thread.h"
#include "event_caps.h"       /* kl_event_caps: which loop the server runs */
#include "event_builtin.h"    /* the built-in backend, wrapped to shape one batch */
#include "kl_atomic.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <keel/clock.h>
#if !defined(_WIN32)
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#endif

/* ── Server plumbing ─────────────────────────────────────────────────────────────────────────── */

static KlHttpServer g_srv;
static KlPlatThread g_tid;

static void server_thread_fn(void *arg) { kl_http_server_run((KlHttpServer *)arg); }

static void handle_hello(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_http_response_json(res, 200, "{\"ok\":true}", 11);
}

static int connect_rcvbuf(int port, int rcvbuf) {
    int fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (rcvbuf > 0)
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (const char *)&rcvbuf, sizeof rcvbuf);
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        kl_test_closesock(fd);
        return -1;
    }
    return fd;
}

/* ── A client-side HTTP/1.1 response parser ──────────────────────────────────────────────────── */

static const char *find(const char *buf, size_t n, const char *pat) {
    size_t m = strlen(pat);
    for (size_t i = 0; i + m <= n; i++)
        if (memcmp(buf + i, pat, m) == 0) return buf + i;
    return NULL;
}

static int has_header(const char *head, size_t n, const char *line) {
    size_t m = strlen(line);
    for (size_t i = 0; i + m <= n; i++) {
        size_t k = 0;
        while (k < m) {
            char a = head[i + k], b = line[k];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) break;
            k++;
        }
        if (k == m) return 1;
    }
    return 0;
}

/* Set while the request being answered is a HEAD: the response has a Content-Length but no body. */
static int g_head;

/* One response at buf[0..n): its total length when complete, 0 if more is needed, -1 if the bytes
 * are not a well-formed response. The decoded body goes to body (up to cap), its length to *blen. */
static long parse_response(const char *buf, size_t n, char *body, size_t cap, size_t *blen,
                           int *conn_close) {
    *blen = 0;
    if (n < 9) return 0;
    if (memcmp(buf, "HTTP/1.1 ", n < 9 ? n : 9) != 0) return -1;
    const char *e = find(buf, n, "\r\n\r\n");
    if (!e) return 0;
    size_t hlen = (size_t)(e - buf) + 4;
    if (conn_close) *conn_close = has_header(buf, hlen, "\r\nConnection: close\r\n");
    if (has_header(buf, hlen, "\r\nTransfer-Encoding: chunked\r\n")) {
        size_t pos = hlen;
        for (;;) {
            const char *le = find(buf + pos, n - pos, "\r\n");
            if (!le) return 0;
            size_t sz = 0;
            const char *p = buf + pos;
            if (p == le) return -1;
            for (; p < le; p++) {
                int d;
                if (*p >= '0' && *p <= '9') d = *p - '0';
                else if (*p >= 'a' && *p <= 'f') d = *p - 'a' + 10;
                else if (*p >= 'A' && *p <= 'F') d = *p - 'A' + 10;
                else return -1;
                sz = sz * 16 + (size_t)d;
            }
            pos = (size_t)(le - buf) + 2;
            if (sz == 0) {
                if (n - pos < 2) return 0;
                if (memcmp(buf + pos, "\r\n", 2) != 0) return -1;
                return (long)(pos + 2);
            }
            if (n - pos < sz + 2) return 0;
            if (memcmp(buf + pos + sz, "\r\n", 2) != 0) return -1;
            if (body && *blen + sz <= cap) memcpy(body + *blen, buf + pos, sz);
            *blen += sz;
            pos += sz + 2;
        }
    }
    const char *cl = find(buf, hlen, "Content-Length: ");
    if (!cl) return -1;
    size_t len = g_head ? 0 : (size_t)strtoul(cl + 16, NULL, 10);
    if (n - hlen < len) return 0;
    if (body && len <= cap) memcpy(body, buf + hlen, len);
    *blen = len;
    return (long)(hlen + len);
}

#define WIRE_CAP (32u * 1024u * 1024u)
#define BODY_CAP (20u * 1024u * 1024u)

typedef struct {
    char  *wire;
    size_t wire_len;
    long   first_len;      /* the first response's length: > 0 complete, 0 incomplete, -1 malformed */
    char  *body;
    size_t body_len;
    int    first_close;    /* it said Connection: close */
    long   second_len;     /* the second response's, parsed from the byte after the first */
    int    closed;         /* the server closed the connection: an orderly FIN */
    int    reset;          /* a read failed (a reset): never acceptable after a complete response */
} Pair;

static void pair_free(Pair *p) {
    free(p->wire);
    free(p->body);
}

/* Read until the response at wire[off..] is complete (or malformed), the peer closes, or nothing
 * arrives for `idle_ms`. */
static long read_response(int fd, Pair *p, size_t off, char *body, size_t *blen, int *conn_close,
                          int idle_ms) {
    long r = parse_response(p->wire + off, p->wire_len - off, body, BODY_CAP, blen, conn_close);
    while (r == 0 && !p->closed && p->wire_len < WIRE_CAP) {
        if (kl_test_poll1(fd, 0, idle_ms) <= 0) break;
        kl_ssize_t n = kl_test_sockread(fd, p->wire + p->wire_len, WIRE_CAP - p->wire_len);
        if (n == 0) { p->closed = 1; break; }
        if (n < 0) { p->reset = 1; break; }
        p->wire_len += (size_t)n;
        /* Re-parse a long response only once it may have ended (its last bytes are a terminating
         * chunk): parsing megabytes on every read is quadratic, and slow under the sanitizers. */
        size_t have = p->wire_len - off;
        if (have < 65536 || (have >= 5 && memcmp(p->wire + p->wire_len - 5, "0\r\n\r\n", 5) == 0))
            r = parse_response(p->wire + off, have, body, BODY_CAP, blen, conn_close);
    }
    if (r == 0)
        r = parse_response(p->wire + off, p->wire_len - off, body, BODY_CAP, blen, conn_close);
    return r;
}

/* After a response: did the peer close (with nothing more on the wire) within `ms`? */
static void read_to_close(int fd, Pair *p, int ms) {
    while (!p->closed && p->wire_len < WIRE_CAP) {
        if (kl_test_poll1(fd, 0, ms) <= 0) break;
        kl_ssize_t n = kl_test_sockread(fd, p->wire + p->wire_len, WIRE_CAP - p->wire_len);
        if (n == 0) { p->closed = 1; break; }
        if (n < 0) { p->reset = 1; break; }
        p->wire_len += (size_t)n;
    }
}

/* Time between the first response and the second request (0 = none). */
static int g_pair_gap_ms;

/* GET `path` on a keep-alive connection, wait `stall_ms` without reading (the server's output backs
 * up), read the response; then, unless the server closed, GET /hello on the same connection. */
static void run_pair(int port, const char *path, int stall_ms, Pair *p) {
    memset(p, 0, sizeof *p);
    p->wire = malloc(WIRE_CAP);
    p->body = malloc(BODY_CAP);
    if (!p->wire || !p->body) return;
    int fd = connect_rcvbuf(port, 8192);
    if (fd < 0) return;
    char rq[256];
    snprintf(rq, sizeof rq, "GET %s HTTP/1.1\r\nHost: x\r\n\r\n", path);
    (void)kl_test_sockwrite(fd, rq, strlen(rq));
    if (stall_ms) kl_test_sleep_ms(stall_ms);
    p->first_len = read_response(fd, p, 0, p->body, &p->body_len, &p->first_close, 3000);
    if (p->first_len > 0 && !p->closed) {
        if (g_pair_gap_ms) kl_test_sleep_ms(g_pair_gap_ms);
        const char *rq2 = "GET /hello HTTP/1.1\r\nHost: x\r\n\r\n";
        (void)kl_test_sockwrite(fd, rq2, strlen(rq2));
        size_t blen2 = 0;
        p->second_len = read_response(fd, p, (size_t)p->first_len, NULL, &blen2, NULL, 3000);
        if (p->second_len > 0) {
            /* Nothing may follow the second response either: a stray chunk after the first one's
             * terminator would show up here at the latest. */
            if (kl_test_poll1(fd, 0, 200) > 0 && !p->closed) {
                kl_ssize_t n = kl_test_sockread(fd, p->wire + p->wire_len, WIRE_CAP - p->wire_len);
                if (n > 0) p->wire_len += (size_t)n;
                else if (n == 0) p->closed = 1;
                else p->reset = 1;
            }
        }
    }
    kl_test_closesock(fd);
}

/* The body every streaming producer below sends: `a` 64 KiB chunks of 'A', `b` of 'B', then `tail`. */
#define CHUNK (64 * 1024)
static char g_a[CHUNK];
static char g_b[CHUNK];

static int body_is(const Pair *p, int a, int b, const char *tail) {
    size_t tl = strlen(tail);
    size_t want = (size_t)(a + b) * CHUNK + tl;
    if (p->body_len != want) return 0;
    for (size_t i = 0; i < (size_t)a * CHUNK; i++)
        if (p->body[i] != 'A') return 0;
    for (size_t i = (size_t)a * CHUNK; i < (size_t)(a + b) * CHUNK; i++)
        if (p->body[i] != 'B') return 0;
    return memcmp(p->body + (size_t)(a + b) * CHUNK, tail, tl) == 0;
}

/* The boundary: the first response is complete and well-formed, the second one starts on the next
 * byte and is the /hello answer, and nothing else is on the wire. */
#define ASSERT_TWO_RESPONSES(p) do {                                                              \
        ASSERT_FALSE((p).reset);                                                                  \
        ASSERT_GT((p).first_len, 0L);                                                             \
        ASSERT_GT((p).second_len, 0L);                                                            \
        ASSERT_EQ((p).wire_len, (size_t)((p).first_len + (p).second_len));                       \
        ASSERT_TRUE(find((p).wire + (p).first_len, (size_t)(p).second_len, "{\"ok\":true}") != NULL); \
    } while (0)

/* ── The producers ───────────────────────────────────────────────────────────────────────────── */

enum { M_ON_DRAIN, M_WRITABLE, M_ASYNC, M_COMPRESS_LEGACY, M_COMPRESS_BOUNDED };

#define INIT_CHUNKS 32    /* 2 MiB: more than any socket buffers, and past a completion queue's bound */
#define EXTRA_CHUNKS 48   /* what on_drain adds before it ends the stream: past the point where what
                            * was queued at completion is all sent */
#define BOUNDED_CHUNKS 32

typedef struct {
    int mode;
    KlAsyncOp op;
    KlHttpResponse *res;
    KlHttpResponseWriteFn w;
    void *wc;
    KlHttpCompressStream cs;
    int refills;               /* on_drain calls */
    int extra;                 /* chunks on_drain wrote */
    int written;               /* chunks accepted (bounded) */
    int ended;
    int completed;
    int pending_at_complete;   /* the op completed with the response's drain holding bytes */
    int end_rc;
    int late_rc;               /* the legacy writer after the end: must be -1 */
    KlHttpStreamStatus late_try;    /* a bounded write after the end: must be CLOSED */
    int compress_end_rc;       /* kl_http_compress_stream_end after end_stream: must be -1 */
    int failed;
} Prod;
static Prod g_p;

/* ── An identity codec: output = input; the finish emits "TRAILER!" ── */
static size_t id_bound(KlCompress *self, size_t len, int final) {
    (void)self;
    return len + (final ? 8 : 0);
}
static int id_feed(KlCompress *self, const char *data, size_t len, int flush,
                   int (*emit)(void *ctx, const char *data, size_t len), void *emit_ctx) {
    (void)self;
    if (data && len && emit(emit_ctx, data, len) < 0) return -1;
    if (flush == 1 && emit(emit_ctx, "TRAILER!", 8) < 0) return -1;
    return 0;
}
static int id_compress(KlCompress *self, const char *in, size_t in_len, char **out, size_t *out_len,
                       KlAllocator *alloc) {
    (void)self; (void)in; (void)in_len; (void)out; (void)out_len; (void)alloc;
    return -1;
}
static const char *id_encoding(KlCompress *self) { (void)self; return "identity-mock"; }
static void id_destroy(KlCompress *self) { (void)self; }
static KlCompress g_id_codec;
static KlCompress *id_factory(KlCompressCtx *ctx, KlAllocator *alloc) {
    (void)ctx; (void)alloc;
    g_id_codec = (KlCompress){ .compress = id_compress, .feed = id_feed, .encoding = id_encoding,
                               .destroy = id_destroy, .flush_bound = id_bound };
    return &g_id_codec;
}

static void prod_complete(void) {
    g_p.pending_at_complete = g_p.res->drain_enabled && kl_drain_pending(&g_p.res->drain);
    g_p.completed = 1;
    kl_async_complete(&g_srv, &g_p.op);
}

/* After the end: one write through each writer, both of which must be refused without a byte. */
static void prod_late_writes(void) {
    g_p.late_rc = g_p.w(g_p.wc, "late", 4);
    g_p.late_try = kl_http_response_stream_try_write(g_p.res, "late", 4);
}

static void legacy_write(const char *data, size_t len) {
    if (g_p.w(g_p.wc, data, len) < 0) g_p.failed = 1;
}

/* The drain emptied after the op completed: refill it, each call until a write stays buffered (so
 * the drain empties again and calls back: the legacy producer's pattern), then write a last chunk
 * larger than the socket takes at once, the tail and the terminator: those land in the drain the
 * callback was called for having emptied. */
static void on_drain_refill(void *ctx) {
    (void)ctx;
    if (g_p.ended) return;
    g_p.refills++;
    while (g_p.extra < EXTRA_CHUNKS) {
        legacy_write(g_a, CHUNK);
        g_p.extra++;
        if (kl_drain_pending(&g_p.res->drain)) return;   /* backpressure: the next call goes on */
    }
    legacy_write(g_b, CHUNK);
    legacy_write("TAIL", 4);
    g_p.end_rc = kl_http_response_end_stream(g_p.res);
    g_p.ended = 1;
    prod_late_writes();
}

/* Bounded: write until the capacity refuses, continue from the writable edge, then end there. */
static void bounded_pump(void *ctx) {
    (void)ctx;
    if (g_p.ended) return;
    while (g_p.written < BOUNDED_CHUNKS) {
        KlHttpStreamStatus st = kl_http_response_stream_try_write(g_p.res, g_a, CHUNK);
        if (st == KL_HTTP_STREAM_WOULD_BLOCK) return;          /* the edge fires once room frees */
        if (st != KL_HTTP_STREAM_ACCEPTED) { g_p.failed = 1; return; }
        g_p.written++;
    }
    KlHttpStreamStatus st = kl_http_response_stream_try_write(g_p.res, "TAIL", 4);
    if (st == KL_HTTP_STREAM_WOULD_BLOCK) return;
    if (st != KL_HTTP_STREAM_ACCEPTED) { g_p.failed = 1; return; }
    g_p.end_rc = kl_http_response_end_stream(g_p.res);
    g_p.ended = 1;
    prod_late_writes();
    prod_complete();                                           /* from inside the writable edge */
}

static void prod_start(void *ud) {
    (void)ud;
    switch (g_p.mode) {
    case M_ON_DRAIN:
        for (int i = 0; i < INIT_CHUNKS; i++) legacy_write(g_a, CHUNK);
        kl_drain_on_drain(&g_p.res->drain, on_drain_refill, NULL);
        prod_complete();
        break;
    case M_WRITABLE:
        (void)kl_http_response_stream_on_writable(g_p.res, bounded_pump, NULL);
        bounded_pump(NULL);
        break;
    case M_ASYNC:
        for (int i = 0; i < INIT_CHUNKS; i++) legacy_write(g_a, CHUNK);
        legacy_write("TAIL", 4);
        g_p.end_rc = kl_http_response_end_stream(g_p.res);
        g_p.ended = 1;
        prod_late_writes();
        prod_complete();
        break;
    case M_COMPRESS_LEGACY:
        for (int i = 0; i < INIT_CHUNKS; i++)
            if (kl_http_compress_stream_write(&g_p.cs, g_a, CHUNK) < 0) g_p.failed = 1;
        if (kl_http_compress_stream_write(&g_p.cs, "TAIL", 4) < 0) g_p.failed = 1;
        /* The response is ended directly, then the compressed stream: its finish must not follow
         * the terminating chunk. */
        g_p.end_rc = kl_http_response_end_stream(g_p.res);
        g_p.compress_end_rc = kl_http_compress_stream_end(&g_p.cs);
        g_p.ended = 1;
        prod_late_writes();
        prod_complete();
        break;
    case M_COMPRESS_BOUNDED:
        for (int i = 0; i < INIT_CHUNKS; i++)
            if (kl_http_compress_stream_try_write(&g_p.cs, g_a, CHUNK) != KL_HTTP_STREAM_ACCEPTED)
                g_p.failed = 1;
        g_p.end_rc = kl_http_compress_stream_try_end(&g_p.cs) == KL_HTTP_STREAM_ACCEPTED ? 0 : -1;
        g_p.ended = 1;
        prod_late_writes();
        if (kl_http_compress_stream_try_write(&g_p.cs, "late", 4) != KL_HTTP_STREAM_CLOSED) g_p.failed = 1;
        prod_complete();
        break;
    }
}

/* The server's output backs up quickly. The handle is a Windows SOCKET there (pointer-width). */
static void shrink_sndbuf(KlSocketHandle h) {
    int sb = 4096;
#if defined(_WIN32)
    (void)setsockopt((SOCKET)h, SOL_SOCKET, SO_SNDBUF, (const char *)&sb, sizeof sb);
#else
    (void)setsockopt((int)h, SOL_SOCKET, SO_SNDBUF, (const char *)&sb, sizeof sb);
#endif
}

static void handle_stream(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)ctx;
    shrink_sndbuf(kl_http_request_conn(req)->stream.fd);
    g_p.res = res;
    if (g_p.mode == M_COMPRESS_LEGACY || g_p.mode == M_COMPRESS_BOUNDED) {
        KlCompressConfig cc = { .factory = id_factory };
        if (kl_http_compress_stream_begin(res, &cc, 200, &g_p.cs) < 0) { g_p.failed = 1; return; }
        g_p.w = g_p.cs.write_fn;
        g_p.wc = g_p.cs.write_ctx;
    } else if (kl_http_response_begin_stream(res, 200, &g_p.w, &g_p.wc) < 0) {
        g_p.failed = 1;
        return;
    }
    if (kl_async_suspend(&g_srv, kl_http_request_conn(req), &g_p.op) < 0) { g_p.failed = 1; return; }
    (void)kl_timer_add(kl_http_server_event_ctx(&g_srv), 20, prod_start, NULL);
}

/* A stream written and ended inside the handler. */
static void handle_inline(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)ctx;
    KlHttpResponseWriteFn w;
    void *wc;
    static char body[4096];
    memset(body, 'I', sizeof body);
    if (kl_http_response_begin_stream(res, 200, &w, &wc) < 0) return;
    (void)w(wc, body, sizeof body);
    (void)kl_http_response_end_stream(res);
}

/* A streaming-async handler that answers (a whole stream) before the body, then parks on it: the
 * body then fails its reader (an 8-byte cap). */
static void handle_answer_then_await(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)ctx;
    KlHttpResponseWriteFn w;
    void *wc;
    if (kl_http_response_begin_stream(res, 200, &w, &wc) < 0) return;
    (void)w(wc, "x", 1);
    (void)kl_http_response_end_stream(res);
    kl_http_request_await_body(req);
}

/* A buffered answer whose response pointer a stale producer keeps past it. */
static KlHttpResponse *g_stale_res;
static int g_stale_rc;
static void stale_begin(void *ud) {
    (void)ud;
    KlHttpResponseWriteFn w;
    void *wc;
    g_stale_rc = kl_http_response_begin_stream(g_stale_res, 200, &w, &wc);
}
static void handle_stale(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)ctx;
    g_stale_res = res;
    kl_http_response_json(res, 200, "{\"ok\":true}", 11);
    (void)kl_timer_add(kl_http_server_event_ctx(&g_srv), 100, stale_begin, NULL);
}

/* A streaming-async handler that ended a stream larger than the socket takes, then parks on the
 * body, which then fails its reader: the response is sent in full before the connection ends (the
 * half-close used to cut off what the outbound buffer still held, on readiness loops). */
static char g_big[1024 * 1024];
static KlHttpConn *g_answer_conn;
/* The same with a 16 MiB answer and the default send buffer: much of the answer is in the kernel
 * once it is handed over, while the client is still reading it. */
static void handle_large_then_await(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)ctx;
    KlHttpResponseWriteFn w;
    void *wc;
    if (kl_http_response_begin_stream(res, 200, &w, &wc) < 0) return;
    memset(g_big, 'Z', sizeof g_big);
    for (int i = 0; i < 256; i++) (void)w(wc, g_big, 65536);
    (void)kl_http_response_end_stream(res);
    g_answer_conn = kl_http_request_conn(req);
    kl_http_request_await_body(req);
}
static void handle_big_then_await(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)ctx;
    shrink_sndbuf(kl_http_request_conn(req)->stream.fd);
    KlHttpResponseWriteFn w;
    void *wc;
    if (kl_http_response_begin_stream(res, 200, &w, &wc) < 0) return;
    memset(g_big, 'Z', sizeof g_big);
    for (int i = 0; i < 16; i++) (void)w(wc, g_big, 65536);
    (void)kl_http_response_end_stream(res);
    g_p.pending_at_complete = res->drain_enabled && kl_drain_pending(&res->drain);
    g_answer_conn = kl_http_request_conn(req);       /* white-box: its discard counter, read later */
    kl_http_request_await_body(req);
}
/* A streaming-async handler that sends a buffered (or file-backed) answer larger than the socket
 * takes directly, then parks on the body, which then fails (its reader's 8-byte cap, or the body
 * deadline). Once a response head has been committed, a subsequent request-body rejection must not
 * inject another response or discard pending response output merely because the response is
 * buffered, file-backed or streamed: the whole answer goes out, then the connection closes. A
 * completion loop refuses the direct send (one output driver per response), so nothing is committed
 * there and the rejection is answered as usual. */
#define COMMITTED_LEN (4u * 1024u * 1024u)
static char g_committed[COMMITTED_LEN];
static int  g_committed_rc;
static size_t g_committed_len = COMMITTED_LEN;   /* the buffered answer's length */
static void committed_init(void) {
    for (size_t i = 0; i < COMMITTED_LEN; i++) g_committed[i] = (char)('a' + (i * 7u) % 26u);
}
static void handle_buffered_then_await(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)ctx;
    shrink_sndbuf(kl_http_request_conn(req)->stream.fd);
    kl_http_response_status(res, 200);
    kl_http_response_body_borrow(res, g_committed, g_committed_len);
    g_committed_rc = kl_http_response_send(res);
    kl_http_request_await_body(req);
}
#if !defined(_WIN32)   /* a temp file through POSIX open; the buffered cases cover Windows */
static char g_committed_path[64];
/* The default send buffer, not a 4 KiB one: a file response corks the socket (macOS TCP_NOPUSH),
 * and a corked loopback socket holding less than a segment does not send it until it is uncorked.
 * 4 MiB is still far more than the socket buffers take. */
static void handle_file_then_await(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)ctx;
    int fd = open(g_committed_path, O_RDONLY);
    if (fd < 0) { g_committed_rc = -2; kl_http_request_await_body(req); return; }
    kl_http_response_status(res, 200);
    kl_http_response_file(res, fd, COMMITTED_LEN);
    g_committed_rc = kl_http_response_send(res);
    kl_http_request_await_body(req);
}
#endif
/* A streaming-async handler that sends its answer directly and returns with it partly sent, the
 * request (its body already read with the head) left to the connection: the connection then decides
 * it is the last response on it. Once any response header bytes have been transmitted, the
 * serialized response layout must remain immutable: the rest of the answer is resumed from where it
 * stopped in that same layout, then the connection closes. Modes: a large buffered body (the head
 * out, the body partly), a head larger than the socket takes (the head itself partly out), a file. */
enum { LAYOUT_BODY, LAYOUT_HEAD, LAYOUT_FILE };
#define LAYOUT_PAD (64u * 1024u)
static int    g_layout_mode;
static size_t g_layout_off;        /* send_offset after the handler's send */
static char   g_layout_pad[LAYOUT_PAD + 1];
static void handle_send_then_return(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)ctx;
    if (g_layout_mode != LAYOUT_FILE) shrink_sndbuf(kl_http_request_conn(req)->stream.fd);
    kl_http_response_status(res, 200);
    if (g_layout_mode == LAYOUT_HEAD) {
        memset(g_layout_pad, 'p', LAYOUT_PAD);
        g_layout_pad[LAYOUT_PAD] = '\0';
        (void)kl_http_response_header(res, "X-Pad", g_layout_pad);
    }
#if !defined(_WIN32)
    if (g_layout_mode == LAYOUT_FILE) {
        int fd = open(g_committed_path, O_RDONLY);
        if (fd < 0) { g_committed_rc = -2; return; }
        kl_http_response_file(res, fd, COMMITTED_LEN);
    } else
#endif
    kl_http_response_body_borrow(res, g_committed, COMMITTED_LEN);
    g_committed_rc = kl_http_response_send(res);
    /* TLS writes one segment per call: send on until the body has begun (the head whole), except
     * where the head is to stay cut. */
    while (g_layout_mode == LAYOUT_BODY && g_committed_rc == 1 && res->send_offset < 256)
        g_committed_rc = kl_http_response_send(res);
    g_layout_off = res->send_offset;
}
#if !defined(_WIN32)
/* A file response whose head (64 KiB of padding) is larger than the socket takes: the head write
 * gives up part way (rc -1) with some of the head on the wire. The handler ignores that and parks on
 * the body, which then fails its reader's cap. */
static void handle_file_head_cut(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)ctx;
    shrink_sndbuf(kl_http_request_conn(req)->stream.fd);
    kl_http_response_status(res, 200);
    memset(g_layout_pad, 'p', LAYOUT_PAD);
    g_layout_pad[LAYOUT_PAD] = '\0';
    (void)kl_http_response_header(res, "X-Pad", g_layout_pad);
    int fd = open(g_committed_path, O_RDONLY);
    if (fd < 0) { g_committed_rc = -2; return; }
    kl_http_response_file(res, fd, COMMITTED_LEN);
    g_committed_rc = kl_http_response_send(res);
    kl_http_request_await_body(req);
}
#endif
/* Per-test server settings (0 = the defaults). */
static size_t   g_cfg_drain_bytes;
static uint32_t g_cfg_drain_ms;
static int      g_cfg_read_to, g_cfg_body_to;
static int      g_cfg_max_conns;
static const KlEventProvider *g_cfg_provider;
static void handle_where(KlHttpRequest *req, KlHttpResponse *res, void *ctx);

static int server_start(size_t capacity, int tls) {
    static KlTlsConfig tls_cfg = { .ctx = NULL, .factory = mock_tls_create };
    KlHttpServerConfig cfg = { .port = 0, .max_connections = g_cfg_max_conns ? g_cfg_max_conns : 4,
                               .event_provider = g_cfg_provider, .stream_capacity = capacity,
                               .tls = tls ? &tls_cfg : NULL,
                               .reject_drain_max_bytes = g_cfg_drain_bytes,
                               .reject_drain_timeout_ms = g_cfg_drain_ms,
                               .read_timeout_ms = g_cfg_read_to, .body_timeout_ms = g_cfg_body_to };
    if (kl_http_server_init(&g_srv, &cfg) < 0) return -1;
    kl_http_server_route(&g_srv, "GET", "/stream", handle_stream, NULL, NULL);
    kl_http_server_route(&g_srv, "GET", "/hello", handle_hello, NULL, NULL);
    kl_http_server_route(&g_srv, "POST", "/sink", handle_hello, NULL, NULL);
    kl_http_server_route(&g_srv, "POST", "/buf", handle_hello, NULL, kl_http_body_reader_buffer);
    kl_http_server_route(&g_srv, "GET", "/inline", handle_inline, NULL, NULL);
    kl_http_server_route(&g_srv, "GET", "/stale", handle_stale, NULL, NULL);
    kl_http_server_route(&g_srv, "GET", "/where/:id", handle_where, NULL, NULL);
    kl_http_server_route_streaming_async(&g_srv, "POST", "/await", handle_answer_then_await,
                                         (void *)(size_t)8, kl_http_body_reader_buffer);
    kl_http_server_route_streaming_async(&g_srv, "POST", "/await2", handle_big_then_await,
                                         (void *)(size_t)8, kl_http_body_reader_buffer);
    kl_http_server_route_streaming_async(&g_srv, "POST", "/await3", handle_large_then_await,
                                         (void *)(size_t)8, kl_http_body_reader_buffer);
    kl_http_server_route_streaming_async(&g_srv, "POST", "/await_buf", handle_buffered_then_await,
                                         (void *)(size_t)8, kl_http_body_reader_buffer);
    kl_http_server_route_streaming_async(&g_srv, "POST", "/sendret", handle_send_then_return,
                                         (void *)(size_t)1024, kl_http_body_reader_buffer);
#if !defined(_WIN32)
    kl_http_server_route_streaming_async(&g_srv, "POST", "/fhead", handle_file_head_cut,
                                         (void *)(size_t)8, kl_http_body_reader_buffer);
#endif
#if !defined(_WIN32)
    kl_http_server_route_streaming_async(&g_srv, "POST", "/await_file", handle_file_then_await,
                                         (void *)(size_t)8, kl_http_body_reader_buffer);
#endif
    kl_plat_thread_create(&g_tid, server_thread_fn, &g_srv);
    for (int i = 0; i < 200 && g_srv.bound_port == 0; i++) kl_test_sleep_ms(10);
    return g_srv.bound_port;
}

static void server_stop_join(void) {
    kl_http_server_stop(&g_srv);
    kl_plat_thread_join(&g_tid);
}
static void server_stop(void) {
    server_stop_join();
    kl_http_server_free(&g_srv);
}

static void stream_case(int *utest_result, int mode, int tls, size_t capacity) {
    memset(&g_p, 0, sizeof g_p);
    g_p.mode = mode;
    memset(g_a, 'A', sizeof g_a);
    memset(g_b, 'B', sizeof g_b);
    int port = server_start(capacity, tls);
    ASSERT_GT(port, 0);
    Pair p;
    run_pair(port, "/stream", 300, &p);
    server_stop();
    printf("  wire %zu bytes: first %ld (body %zu), second %ld, closed %d; drain pending at "
           "completion %d, on_drain calls %d\n", p.wire_len, p.first_len, p.body_len, p.second_len,
           p.closed, g_p.pending_at_complete, g_p.refills);

    EXPECT_FALSE(g_p.failed);
    EXPECT_TRUE(g_p.completed);
    EXPECT_TRUE(g_p.ended);
    EXPECT_EQ(g_p.end_rc, 0);
    /* Writes after the end were refused, and none of them reached the wire (the boundary below). */
    EXPECT_EQ(g_p.late_rc, -1);
    EXPECT_EQ(g_p.late_try, KL_HTTP_STREAM_CLOSED);
    if (p.wire && p.body) {
        switch (mode) {
        case M_ON_DRAIN:
            EXPECT_TRUE(g_p.pending_at_complete);     /* the path: completed with output waiting */
            /* on_drain ran (and ended the stream); how many calls it took depends on how much the
             * platform's socket buffers take at once (Windows may take all of it in one). */
            EXPECT_GE(g_p.refills, 1);
            EXPECT_EQ(g_p.extra, EXTRA_CHUNKS);
            EXPECT_TRUE(body_is(&p, INIT_CHUNKS + EXTRA_CHUNKS, 1, "TAIL"));
            break;
        case M_WRITABLE:
            EXPECT_TRUE(body_is(&p, BOUNDED_CHUNKS, 0, "TAIL"));
            break;
        case M_ASYNC:
            EXPECT_TRUE(g_p.pending_at_complete);
            EXPECT_TRUE(body_is(&p, INIT_CHUNKS, 0, "TAIL"));
            break;
        case M_COMPRESS_LEGACY:
            EXPECT_EQ(g_p.compress_end_rc, -1);       /* its finish could not be sent: a failure */
            EXPECT_TRUE(body_is(&p, INIT_CHUNKS, 0, "TAIL"));   /* and no TRAILER! after the end */
            break;
        case M_COMPRESS_BOUNDED:
            EXPECT_TRUE(body_is(&p, INIT_CHUNKS, 0, "TRAILER!"));
            break;
        }
    }
    /* Every stream here ended cleanly, with output still buffered or not (a completion loop's queue
     * may already hold all of it): the connection is reused, on every loop. */
    EXPECT_FALSE(p.first_close);
    ASSERT_TWO_RESPONSES(p);
    pair_free(&p);
}

#define BIG_CAP (8u * 1024u * 1024u)   /* the legacy writer is fatal at its buffer's cap */

UTEST(stream_termination, ended_from_on_drain_after_completion) {
    stream_case(utest_result, M_ON_DRAIN, 0, BIG_CAP);
}
UTEST(stream_termination, ended_from_on_drain_after_completion_tls) {
    stream_case(utest_result, M_ON_DRAIN, 1, BIG_CAP);
}
UTEST(stream_termination, ended_from_the_writable_edge) {
    stream_case(utest_result, M_WRITABLE, 0, 256u * 1024u);
}
UTEST(stream_termination, ended_before_an_async_completion_with_output_buffered) {
    stream_case(utest_result, M_ASYNC, 0, BIG_CAP);
}
UTEST(stream_termination, ended_before_an_async_completion_with_output_buffered_tls) {
    stream_case(utest_result, M_ASYNC, 1, BIG_CAP);
}
UTEST(stream_termination, compressed_legacy_end_after_the_response_ended) {
    stream_case(utest_result, M_COMPRESS_LEGACY, 0, BIG_CAP);
}
UTEST(stream_termination, compressed_bounded_try_end) {
    stream_case(utest_result, M_COMPRESS_BOUNDED, 0, BIG_CAP);
}

/* ── Pipelined requests: not supported, so one complete response and a close ─────────────────── */

/* Send `reqs` in one write; the first response must be complete, say Connection: close, carry
 * `expect` in its body, and be followed by an orderly close (a FIN, never a reset, never a hang)
 * with nothing more on the wire. */
static void reqs_case(int *utest_result, const char *reqs, const char *expect, int head,
                      int close_hdr) {
    int port = server_start(0, 0);
    ASSERT_GT(port, 0);
    Pair p;
    memset(&p, 0, sizeof p);
    p.wire = malloc(WIRE_CAP);
    p.body = malloc(BODY_CAP);
    g_head = head;
    int fd = connect_rcvbuf(port, 0);
    if (fd >= 0 && p.wire && p.body) {
        (void)kl_test_sockwrite(fd, reqs, strlen(reqs));   /* all in one write */
        p.first_len = read_response(fd, &p, 0, p.body, &p.body_len, &p.first_close, 3000);
        read_to_close(fd, &p, 3000);                  /* the close must come, not a hang */
        kl_test_closesock(fd);
    }
    g_head = 0;
    server_stop();
    printf("  wire %zu bytes: first %ld, Connection: close %d, then closed %d, reset %d\n",
           p.wire_len, p.first_len, p.first_close, p.closed, p.reset);
    ASSERT_TRUE(fd >= 0);
    ASSERT_GT(p.first_len, 0L);                       /* the first response, complete */
    if (expect) EXPECT_TRUE(find(p.body, p.body_len, expect) != NULL);
    if (close_hdr) EXPECT_TRUE(p.first_close);        /* it says the connection ends */
    EXPECT_FALSE(p.reset);                            /* an orderly close: the response stands */
    EXPECT_TRUE(p.closed);                            /* and it comes: no hang until a timeout */
    EXPECT_EQ(p.wire_len, (size_t)p.first_len);       /* no second response on a reused connection */
    pair_free(&p);
}

static void pipelined_case(int *utest_result, const char *reqs, const char *expect, int head) {
    reqs_case(utest_result, reqs, expect, head, 1);
}

#define GET_HELLO "GET /hello HTTP/1.1\r\nHost: x\r\n\r\n"

UTEST(stream_termination, pipelined_gets_after_a_get_close_after_the_first) {
    pipelined_case(utest_result, GET_HELLO GET_HELLO, "{\"ok\":true}", 0);
}
UTEST(stream_termination, pipelined_after_a_content_length_body_close_after_the_first) {
    pipelined_case(utest_result,
                   "POST /sink HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello" GET_HELLO,
                   "{\"ok\":true}", 0);
}
UTEST(stream_termination, pipelined_after_a_chunked_body_close_after_the_first) {
    pipelined_case(utest_result,
                   "POST /sink HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n"
                   "5\r\nhello\r\n0\r\n\r\n" GET_HELLO, "{\"ok\":true}", 0);
}
UTEST(stream_termination, pipelined_after_a_body_read_by_a_reader_close_after_the_first) {
    pipelined_case(utest_result,
                   "POST /buf HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello" GET_HELLO,
                   "{\"ok\":true}", 0);
}
UTEST(stream_termination, pipelined_after_a_head_close_after_the_first) {
    pipelined_case(utest_result, "HEAD /hello HTTP/1.1\r\nHost: x\r\n\r\n" GET_HELLO, NULL, 1);
}
UTEST(stream_termination, a_partial_next_request_closes_after_the_first) {
    pipelined_case(utest_result, GET_HELLO "GET /hel", "{\"ok\":true}", 0);
}

/* A stream sent in full inside the handler, with a large pipelined tail (more than one read holds):
 * it used to be released with a direct close over the unread tail, which reset the response away. */
UTEST(stream_termination, an_inline_stream_with_a_large_pipelined_tail_closes_in_order) {
    static char reqs[40 * 1024];
    size_t n = (size_t)snprintf(reqs, sizeof reqs, "GET /inline HTTP/1.1\r\nHost: x\r\n\r\n"
                                "GET /hello HTTP/1.1\r\nHost: x\r\nX-Pad: ");
    memset(reqs + n, 'p', 32 * 1024);
    n += 32 * 1024;
    memcpy(reqs + n, "\r\n\r\n", 5);
    for (int i = 0; i < 3; i++) {
        pipelined_case(utest_result, reqs, "IIII", 0);
        if (*utest_result) return;
    }
}

/* A streaming handler that already answered (a whole stream) when its body then fails: no rejection
 * goes out after its terminating chunk; the connection just ends, in order. */
UTEST(stream_termination, a_body_failing_after_a_streamed_answer_adds_no_rejection) {
    reqs_case(utest_result,
                   "POST /await HTTP/1.1\r\nHost: x\r\nContent-Length: 64\r\nConnection: close\r\n\r\n"
                   "0123456789012345678901234567890123456789012345678901234567890123", "x", 0, 0);
}

/* A producer that kept a response pointer past its request: its begin_stream between requests is
 * refused and writes nothing; the next request on the connection is answered at the boundary. */
UTEST(stream_termination, a_stale_begin_between_requests_writes_nothing) {
    g_stale_rc = 0;
    g_stale_res = NULL;
    memset(g_a, 'A', sizeof g_a);
    int port = server_start(0, 0);
    ASSERT_GT(port, 0);
    Pair p;
    /* The stale begin fires 100 ms after the first handler: the connection is then between
     * requests (the first response is out, the second request is sent 300 ms after it). */
    g_pair_gap_ms = 300;
    run_pair(port, "/stale", 0, &p);
    g_pair_gap_ms = 0;
    server_stop();
    EXPECT_EQ(g_stale_rc, -1);
    ASSERT_TWO_RESPONSES(p);
    pair_free(&p);
}

/* Readiness TLS: the next request's plaintext already decrypted in the engine with this one (no
 * socket readiness will report it) is read on after keep-alive, not left waiting. A completion loop
 * gets both in one buffer: it answers the first and closes (pipelining is not supported). */
UTEST(stream_termination, a_tls_request_already_in_the_engine_is_not_left_waiting) {
    static const char reqs[] = GET_HELLO GET_HELLO;
    mock_tls_split_after = 0;
    mock_tls_split_record = sizeof reqs - 1;
    mock_tls_sock_serve_max = sizeof GET_HELLO - 1;   /* one request per read, the rest pending */
    int port = server_start(0, 1);
    ASSERT_GT(port, 0);
    int completion = (kl_event_caps(&g_srv.ev.loop) & KL_EVENT_CAP_COMPLETION) != 0;
    Pair p;
    memset(&p, 0, sizeof p);
    p.wire = malloc(WIRE_CAP);
    p.body = malloc(BODY_CAP);
    int fd = connect_rcvbuf(port, 0);
    if (fd >= 0 && p.wire && p.body) {
        (void)kl_test_sockwrite(fd, reqs, sizeof reqs - 1);
        p.first_len = read_response(fd, &p, 0, p.body, &p.body_len, &p.first_close, 3000);
        if (p.first_len > 0) {
            size_t b2 = 0;
            p.second_len = read_response(fd, &p, (size_t)p.first_len, NULL, &b2, NULL, 2000);
        }
        kl_test_closesock(fd);
    }
    server_stop();
    mock_tls_split_record = 0;
    mock_tls_sock_serve_max = 0;
    printf("  %s: first %ld, second %ld, closed %d, reset %d\n", completion ? "completion" : "readiness",
           p.first_len, p.second_len, p.closed, p.reset);
    ASSERT_GT(p.first_len, 0L);
    EXPECT_FALSE(p.reset);
    if (completion) {
        EXPECT_TRUE(p.first_close);
        EXPECT_TRUE(p.closed);
        EXPECT_EQ(p.wire_len, (size_t)p.first_len);
    } else {
        ASSERT_TWO_RESPONSES(p);                      /* was: the second never came */
    }
    pair_free(&p);
}

UTEST(stream_termination, a_body_failing_after_a_buffered_streamed_answer_sends_it_all) {
    memset(&g_p, 0, sizeof g_p);
    int port = server_start(8u * 1024u * 1024u, 0);
    ASSERT_GT(port, 0);
    Pair p;
    memset(&p, 0, sizeof p);
    p.wire = malloc(WIRE_CAP);
    p.body = malloc(BODY_CAP);
    int fd = connect_rcvbuf(port, 8192);
    if (fd >= 0 && p.wire && p.body) {
        const char *rq = "POST /await2 HTTP/1.1\r\nHost: x\r\nContent-Length: 64\r\n\r\n";
        (void)kl_test_sockwrite(fd, rq, strlen(rq));
        kl_test_sleep_ms(100);                       /* the body comes in a later read */
        (void)kl_test_sockwrite(fd, "0123456789012345678901234567890123456789012345678901234567890123", 64);
        kl_test_sleep_ms(300);                       /* not reading: the answer stays buffered */
        p.first_len = read_response(fd, &p, 0, p.body, &p.body_len, &p.first_close, 3000);
        read_to_close(fd, &p, 3000);
        kl_test_closesock(fd);
    }
    server_stop();
    printf("  wire %zu, first %ld, body %zu, closed %d, reset %d; buffered at the end %d\n",
           p.wire_len, p.first_len, p.body_len, p.closed, p.reset, g_p.pending_at_complete);
    ASSERT_GT(p.first_len, 0L);                      /* was: cut off at the half-close */
    EXPECT_EQ(p.body_len, (size_t)(16 * 65536));
    EXPECT_FALSE(p.reset);
    EXPECT_TRUE(p.closed);
    EXPECT_EQ(p.wire_len, (size_t)p.first_len);      /* no rejection after it */
    pair_free(&p);
}

/* A buffered or file-backed answer whose head (Connection: keep-alive) and part of its body went out
 * before the body failed: the 413 at the reader's cap, or the body deadline's 408. Readiness: the
 * whole declared Content-Length arrives byte-exact, then a clean end of stream (no reset), and no
 * rejection after it, even though the head said keep-alive. (Was: a few KB, then the half-close cut
 * it off.) Completion: the direct send is refused, so the rejection itself is the answer. */
static long first_status(const Pair *p) {
    return (p->wire_len >= 12 && memcmp(p->wire, "HTTP/1.1 ", 9) == 0) ? strtol(p->wire + 9, NULL, 10)
                                                                       : 0;
}
static void committed_case(int *utest_result, const char *path, int body_timeout) {
    memset(&g_p, 0, sizeof g_p);
    committed_init();
    g_committed_rc = 0;
    if (body_timeout) { g_cfg_body_to = 300; g_cfg_read_to = 3000; }
    int port = server_start(0, 0);
    g_cfg_body_to = 0;
    g_cfg_read_to = 0;
    ASSERT_GT(port, 0);
    int completion = (kl_event_caps(&g_srv.ev.loop) & KL_EVENT_CAP_COMPLETION) != 0;
    Pair p;
    memset(&p, 0, sizeof p);
    p.wire = malloc(WIRE_CAP);
    p.body = malloc(BODY_CAP);
    int fd = connect_rcvbuf(port, 8192);
    if (fd >= 0 && p.wire && p.body) {
        char rq[160];
        snprintf(rq, sizeof rq, "POST %s HTTP/1.1\r\nHost: x\r\nContent-Length: 64\r\n\r\n", path);
        (void)kl_test_sockwrite(fd, rq, strlen(rq));
        kl_test_sleep_ms(100);                       /* the answer is begun, awaiting the body */
        if (body_timeout) {
            (void)kl_test_sockwrite(fd, "0123", 4);
            kl_test_sleep_ms(900);                   /* the body deadline passes; not reading */
        } else {
            (void)kl_test_sockwrite(fd, "0123456789012345678901234567890123456789012345678901234567890123", 64);
            kl_test_sleep_ms(300);                   /* the reader refused it; not reading */
        }
        p.first_len = read_response(fd, &p, 0, p.body, &p.body_len, &p.first_close, 3000);
        read_to_close(fd, &p, 3000);
        kl_test_closesock(fd);
    }
    server_stop();
    long status = first_status(&p);
    printf("  %s: send rc %d; wire %zu, status %ld, first %ld, body %zu, closed %d, reset %d\n",
           completion ? "completion" : "readiness", g_committed_rc, p.wire_len, status, p.first_len,
           p.body_len, p.closed, p.reset);
    ASSERT_TRUE(fd >= 0);
    ASSERT_GT(p.first_len, 0L);                      /* was: cut off at the half-close */
    if (completion) {
        EXPECT_EQ(g_committed_rc, -1);               /* refused: the driver owns the output */
        EXPECT_EQ(status, body_timeout ? 408L : 413L);
    } else {
#if defined(_WIN32)
        /* A Windows socket may take the whole send at once, whatever its send buffer: then the
         * answer was complete before the body failed, and nothing is owed. */
        EXPECT_TRUE(g_committed_rc == 1 || g_committed_rc == 0);
#else
        EXPECT_EQ(g_committed_rc, 1);                /* the path: committed, partly sent */
#endif
        EXPECT_EQ(status, 200L);
        EXPECT_TRUE(has_header(p.wire, (size_t)p.first_len, "\r\nConnection: keep-alive\r\n"));
        EXPECT_EQ(p.body_len, (size_t)COMMITTED_LEN);
        if (p.body_len == COMMITTED_LEN)
            EXPECT_EQ(memcmp(p.body, g_committed, COMMITTED_LEN), 0);
    }
    EXPECT_FALSE(p.reset);
    EXPECT_TRUE(p.closed);                           /* never kept alive after it */
    EXPECT_EQ(p.wire_len, (size_t)p.first_len);      /* no rejection after (or inside) it */
    pair_free(&p);
}
UTEST(stream_termination, a_body_failing_after_a_committed_buffered_answer_sends_it_all) {
    committed_case(utest_result, "/await_buf", 0);
}
UTEST(stream_termination, a_body_timeout_after_a_committed_buffered_answer_sends_it_all) {
    committed_case(utest_result, "/await_buf", 1);
}
#if !defined(_WIN32)
static int committed_file_make(void) {
    snprintf(g_committed_path, sizeof g_committed_path, "/tmp/keel_st_commit_XXXXXX");
    int fd = mkstemp(g_committed_path);
    if (fd < 0) return -1;
    committed_init();
    size_t off = 0;
    while (off < COMMITTED_LEN) {
        ssize_t n = write(fd, g_committed + off, COMMITTED_LEN - off);
        if (n <= 0) { close(fd); unlink(g_committed_path); return -1; }
        off += (size_t)n;
    }
    close(fd);
    return 0;
}
UTEST(stream_termination, a_body_failing_after_a_committed_file_answer_sends_it_all) {
    ASSERT_EQ(committed_file_make(), 0);
    committed_case(utest_result, "/await_file", 0);
    unlink(g_committed_path);
}
UTEST(stream_termination, a_body_timeout_after_a_committed_file_answer_sends_it_all) {
    ASSERT_EQ(committed_file_make(), 0);
    committed_case(utest_result, "/await_file", 1);
    unlink(g_committed_path);
}
#endif

/* Bounded: a client that never reads the committed answer does not hold the rejection handling
 * open. Readiness: the server keeps trying to finish the answer (it does not close at once under
 * it, as it did), and the idle timeout (read_timeout_ms, 1 s here) ends it. Completion: the 413 is
 * the answer, and the connection ends at once. */
UTEST(stream_termination, a_non_reading_client_of_a_committed_answer_ends_by_the_idle_timeout) {
#if !defined(_WIN32)
    signal(SIGPIPE, SIG_IGN);
#endif
    memset(&g_p, 0, sizeof g_p);
    committed_init();
    g_committed_rc = 0;
    g_cfg_read_to = 1000;
    int port = server_start(0, 0);
    g_cfg_read_to = 0;
    ASSERT_GT(port, 0);
    int completion = (kl_event_caps(&g_srv.ev.loop) & KL_EVENT_CAP_COMPLETION) != 0;
    int fd = connect_rcvbuf(port, 8192);
    ASSERT_TRUE(fd >= 0);
    const char *rq = "POST /await_buf HTTP/1.1\r\nHost: x\r\nContent-Length: 64\r\n\r\n";
    (void)kl_test_sockwrite(fd, rq, strlen(rq));
    kl_test_sleep_ms(100);
    (void)kl_test_sockwrite(fd, "0123456789012345678901234567890123456789012345678901234567890123", 64);
    uint64_t t0 = kl_monotonic_ms();
    kl_test_sleep_ms(400);
    KlHttpServerStats st;
    kl_http_server_stats(&g_srv, &st);
    int held = st.active_connections;
    uint64_t t1 = t0;
    for (;;) {
        kl_http_server_stats(&g_srv, &st);
        t1 = kl_monotonic_ms();
        if (st.active_connections == 0 || t1 - t0 > 10000) break;
        kl_test_sleep_ms(20);
    }
    char head[16] = {0};
    long n = kl_test_sockread(fd, head, sizeof head - 1);
    kl_test_closesock(fd);
    server_stop();
    printf("  %s: send rc %d; held at 400 ms %d, released after %llu ms; first bytes %.12s\n",
           completion ? "completion" : "readiness", g_committed_rc, held,
           (unsigned long long)(t1 - t0), n > 0 ? head : "");
    EXPECT_EQ(st.active_connections, 0);
    EXPECT_LT(t1 - t0, (uint64_t)4000);              /* the idle timeout, plus the sweep's tick */
    if (completion) {
        EXPECT_EQ(memcmp(head, "HTTP/1.1 413", 12), 0);
    } else {
#if defined(_WIN32)
        if (g_committed_rc == 0) {                   /* the whole send taken at once: nothing owed */
            EXPECT_EQ(memcmp(head, "HTTP/1.1 200", 12), 0);
            return;
        }
#endif
        EXPECT_EQ(g_committed_rc, 1);
        EXPECT_EQ(held, 1);                          /* was: closed at once, the answer cut off */
        EXPECT_GE(t1 - t0, (uint64_t)900);           /* held to the idle timeout, not past it */
        EXPECT_EQ(memcmp(head, "HTTP/1.1 200", 12), 0);   /* never a rejection */
    }
}

/* A buffered answer sent in full before the body failed keeps the behaviour it had: nothing is owed,
 * no rejection follows it, and the connection ends with a clean close. */
UTEST(stream_termination, a_body_failing_after_a_completed_buffered_answer_just_ends) {
    committed_init();
    g_committed_rc = 0;
    g_committed_len = 16;
    int port = server_start(0, 0);
    ASSERT_GT(port, 0);
    int completion = (kl_event_caps(&g_srv.ev.loop) & KL_EVENT_CAP_COMPLETION) != 0;
    Pair p;
    memset(&p, 0, sizeof p);
    p.wire = malloc(WIRE_CAP);
    p.body = malloc(BODY_CAP);
    int fd = connect_rcvbuf(port, 0);
    if (fd >= 0 && p.wire && p.body) {
        const char *rq = "POST /await_buf HTTP/1.1\r\nHost: x\r\nContent-Length: 64\r\n\r\n";
        (void)kl_test_sockwrite(fd, rq, strlen(rq));
        kl_test_sleep_ms(100);
        (void)kl_test_sockwrite(fd, "0123456789012345678901234567890123456789012345678901234567890123", 64);
        p.first_len = read_response(fd, &p, 0, p.body, &p.body_len, &p.first_close, 3000);
        read_to_close(fd, &p, 3000);
        kl_test_closesock(fd);
    }
    server_stop();
    g_committed_len = COMMITTED_LEN;
    long status = first_status(&p);
    printf("  %s: send rc %d; wire %zu, status %ld, first %ld, closed %d, reset %d\n",
           completion ? "completion" : "readiness", g_committed_rc, p.wire_len, status, p.first_len,
           p.closed, p.reset);
    ASSERT_GT(p.first_len, 0L);
    if (completion) {
        EXPECT_EQ(g_committed_rc, -1);
        EXPECT_EQ(status, 413L);
    } else {
        EXPECT_EQ(g_committed_rc, 0);                /* complete before the body failed */
        EXPECT_EQ(status, 200L);
        EXPECT_EQ(p.body_len, (size_t)16);
    }
    EXPECT_FALSE(p.reset);
    EXPECT_TRUE(p.closed);
    EXPECT_EQ(p.wire_len, (size_t)p.first_len);      /* no rejection after it */
    pair_free(&p);
}

/* The answer to a request whose connection ends after it, sent partly before the connection decided
 * so: the declared Content-Length arrives byte-exact in the layout the head began with (Connection:
 * keep-alive, which it says although the connection then closes), then a clean close, and nothing
 * after it. (Was: the keep-alive line dropped from the layout mid-send. With the body begun, the
 * resumed bytes were shifted by its 24 bytes: a short, corrupted answer; with the head cut, the line
 * vanished from a head already begun.) TLS writes one segment per call, so its first send leaves
 * the head partly out. A file response's head goes out whole in one write and its body resumes from
 * file_offset, so it has no layout to shift: its case pins that it is finished the same way.
 * Completion loops refuse the direct send: the driver sends the answer whole, then closes. */
static void layout_case(int *utest_result, int mode, int tls) {
    committed_init();
    g_committed_rc = 0;
    g_layout_off = 0;
    g_layout_mode = mode;
    int port = server_start(0, tls);
    ASSERT_GT(port, 0);
    int completion = (kl_event_caps(&g_srv.ev.loop) & KL_EVENT_CAP_COMPLETION) != 0;
    Pair p;
    memset(&p, 0, sizeof p);
    p.wire = malloc(WIRE_CAP);
    p.body = malloc(BODY_CAP);
    int fd = connect_rcvbuf(port, 8192);
    if (fd >= 0 && p.wire && p.body) {
        const char *rq = "POST /sendret HTTP/1.1\r\nHost: x\r\nContent-Length: 64\r\n\r\n"
                         "0123456789012345678901234567890123456789012345678901234567890123";
        (void)kl_test_sockwrite(fd, rq, strlen(rq));   /* head and body together */
        kl_test_sleep_ms(200);                         /* not reading: the answer backs up */
        p.first_len = read_response(fd, &p, 0, p.body, &p.body_len, &p.first_close, 3000);
        read_to_close(fd, &p, 3000);
        kl_test_closesock(fd);
    }
    server_stop();
    g_layout_mode = LAYOUT_BODY;
    long status = first_status(&p);
    const char *hend = p.wire ? find(p.wire, p.wire_len, "\r\n\r\n") : NULL;
    size_t hlen = hend ? (size_t)(hend - p.wire) + 4 : 0;
    printf("  %s%s: send rc %d, send_offset %zu; wire %zu, head %zu, status %ld, first %ld, body %zu, "
           "closed %d, reset %d\n", completion ? "completion" : "readiness", tls ? " tls" : "",
           g_committed_rc, g_layout_off, p.wire_len, hlen, status, p.first_len, p.body_len, p.closed,
           p.reset);
    ASSERT_TRUE(fd >= 0);
    if (completion) {
        EXPECT_EQ(g_committed_rc, -1);               /* refused: the driver owns the output */
    } else {
#if defined(_WIN32)
        /* A Windows socket may take the whole send at once: then nothing was left to resume. */
        EXPECT_TRUE(g_committed_rc == 1 || g_committed_rc == 0);
        if (g_committed_rc == 1)
#else
        EXPECT_EQ(g_committed_rc, 1);                /* the path: committed, partly sent */
#endif
        {
            if (mode == LAYOUT_HEAD)
                EXPECT_LT(g_layout_off, hlen);       /* the head itself was cut */
            if (mode == LAYOUT_BODY)
                EXPECT_GT(g_layout_off, hlen);       /* the head whole, the body cut */
        }
        if (mode != LAYOUT_FILE)
            EXPECT_TRUE(has_header(p.wire, hlen, "\r\nConnection: keep-alive\r\n"));
    }
    ASSERT_GT(p.first_len, 0L);                      /* was: short of its Content-Length */
    EXPECT_EQ(status, 200L);
    EXPECT_EQ(p.body_len, (size_t)COMMITTED_LEN);
    if (p.body_len == COMMITTED_LEN)
        EXPECT_EQ(memcmp(p.body, g_committed, COMMITTED_LEN), 0);
    if (mode == LAYOUT_HEAD) {                       /* the padding header intact, in place */
        const char *pad = find(p.wire, hlen, "\r\nX-Pad: ");
        ASSERT_TRUE(pad != NULL);
        size_t at = (size_t)(pad - p.wire) + 9;
        ASSERT_LE(at + LAYOUT_PAD + 2, hlen);
        EXPECT_EQ(memcmp(p.wire + at, g_layout_pad, LAYOUT_PAD), 0);
        EXPECT_EQ(memcmp(p.wire + at + LAYOUT_PAD, "\r\n", 2), 0);
    }
    EXPECT_FALSE(p.reset);
    EXPECT_TRUE(p.closed);                           /* never reused */
    EXPECT_EQ(p.wire_len, (size_t)p.first_len);      /* nothing after it */
    pair_free(&p);
}
UTEST(stream_termination, a_partly_sent_buffered_body_keeps_its_layout) {
    layout_case(utest_result, LAYOUT_BODY, 0);
}
UTEST(stream_termination, a_partly_sent_head_keeps_its_layout) {
    layout_case(utest_result, LAYOUT_HEAD, 0);
}
UTEST(stream_termination, a_partly_sent_tls_body_keeps_its_layout) {
    layout_case(utest_result, LAYOUT_BODY, 1);
}
UTEST(stream_termination, a_partly_sent_tls_head_keeps_its_layout) {
    layout_case(utest_result, LAYOUT_HEAD, 1);
}
#if !defined(_WIN32)
UTEST(stream_termination, a_partly_sent_file_answer_keeps_its_layout) {
    ASSERT_EQ(committed_file_make(), 0);
    layout_case(utest_result, LAYOUT_FILE, 0);
    unlink(g_committed_path);
}
/* Once any response head byte has been transmitted, nothing may be injected after it and its layout
 * is immutable. A file head cut off part way (the write gave up on a full socket, rc -1) has failed
 * the response: when its body then fails, the connection closes after the part already sent, and
 * never writes the 413 into it. Exactly one status line reaches the client. (Was: two, the 413
 * inside the 200's head.) A completion loop refuses the direct send before anything is written, and
 * answers with the 413 alone. */
UTEST(stream_termination, a_body_failing_after_a_cut_file_head_injects_nothing) {
    ASSERT_EQ(committed_file_make(), 0);
    g_committed_rc = 0;
    int port = server_start(0, 0);
    ASSERT_GT(port, 0);
    int completion = (kl_event_caps(&g_srv.ev.loop) & KL_EVENT_CAP_COMPLETION) != 0;
    Pair p;
    memset(&p, 0, sizeof p);
    p.wire = malloc(WIRE_CAP);
    p.body = malloc(BODY_CAP);
    int fd = connect_rcvbuf(port, 8192);
    if (fd >= 0 && p.wire && p.body) {
        const char *body = "0123456789012345678901234567890123456789012345678901234567890123";
        const char *rq = "POST /fhead HTTP/1.1\r\nHost: x\r\nContent-Length: 64\r\n\r\n";
        (void)kl_test_sockwrite(fd, rq, strlen(rq));
        kl_test_sleep_ms(200);                       /* not reading: the head backs up */
        (void)kl_test_sockwrite(fd, body, 64);       /* fails the reader's 8-byte cap */
        kl_test_sleep_ms(200);
        p.first_len = read_response(fd, &p, 0, p.body, &p.body_len, &p.first_close, 3000);
        read_to_close(fd, &p, 3000);
        kl_test_closesock(fd);
    }
    server_stop();
    unlink(g_committed_path);
    size_t lines = 0;
    for (size_t i = 0; p.wire && i + 9 <= p.wire_len; i++)
        if (memcmp(p.wire + i, "HTTP/1.1 ", 9) == 0) lines++;
    printf("  %s: send rc %d; wire %zu, status lines %zu, first %ld, closed %d, reset %d\n",
           completion ? "completion" : "readiness", g_committed_rc, p.wire_len, lines, p.first_len,
           p.closed, p.reset);
    ASSERT_TRUE(fd >= 0);
    EXPECT_EQ(g_committed_rc, -1);                   /* cut off (readiness) or refused (completion) */
    EXPECT_EQ(lines, (size_t)1);                     /* was: 2, the second inside the first head */
    EXPECT_TRUE(p.wire_len >= 12 && memcmp(p.wire, "HTTP/1.1 ", 9) == 0);
    if (!completion) {
        EXPECT_EQ(memcmp(p.wire, "HTTP/1.1 200", 12), 0);
        EXPECT_TRUE(find(p.wire, p.wire_len, "\r\n\r\n") == NULL);   /* only the head's part */
    }
    EXPECT_TRUE(p.closed);                           /* the connection ends: never reused */
    EXPECT_FALSE(p.reset);
    pair_free(&p);
}
#endif

/* A client that writes its whole (rejected) body before it reads the answer: the server keeps
 * discarding the body while it sends the streamed answer, so neither side blocks. Within the drain's
 * budget the whole answer arrives and the connection then closes in order; past the budget the
 * connection ends at once (bounded), rather than both sides stalling until the idle timeout. */
static void upload_then_read_case(int *utest_result, size_t body, size_t budget, int within) {
#if !defined(_WIN32)
    signal(SIGPIPE, SIG_IGN);                        /* a write after the server's close */
#endif
    memset(&g_p, 0, sizeof g_p);
    g_cfg_drain_bytes = budget;
    g_cfg_drain_ms = 10000;
    g_cfg_read_to = within ? 5000 : 1500;
    int port = server_start(8u * 1024u * 1024u, 0);
    g_cfg_drain_bytes = 0;
    g_cfg_drain_ms = 0;
    g_cfg_read_to = 0;
    ASSERT_GT(port, 0);
    Pair p;
    memset(&p, 0, sizeof p);
    p.wire = malloc(WIRE_CAP);
    p.body = malloc(BODY_CAP);
    int fd = connect_rcvbuf(port, 8192);
    if (fd >= 0) {                                   /* its upload backs up unless the server reads */
        int sb = 16384;
        (void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (const char *)&sb, sizeof sb);
    }
    size_t sent = 0;
    int write_failed = 0;
    uint64_t t0 = kl_monotonic_ms(), t1 = t0;
    if (fd >= 0 && p.wire && p.body) {
        char rq[160];
        snprintf(rq, sizeof rq, "POST /await2 HTTP/1.1\r\nHost: x\r\nContent-Length: %zu\r\n\r\n", body);
        (void)kl_test_sockwrite(fd, rq, strlen(rq));
        kl_test_sleep_ms(100);                       /* the answer is begun, buffered, awaiting the body */
        (void)kl_test_set_nonblock(fd);
        static char chunk[65536];
        memset(chunk, 'b', sizeof chunk);
        t0 = kl_monotonic_ms();
        while (sent < body && kl_monotonic_ms() - t0 < 8000) {   /* the whole body, not reading */
            if (kl_test_poll1(fd, 1, 200) <= 0) continue;
            size_t want = body - sent < sizeof chunk ? body - sent : sizeof chunk;
            long n = kl_test_sockwrite(fd, chunk, want);
            if (n <= 0) { write_failed = 1; break; }   /* the server ended the connection */
            sent += (size_t)n;
        }
        t1 = kl_monotonic_ms();
        p.first_len = read_response(fd, &p, 0, p.body, &p.body_len, &p.first_close, 3000);
        read_to_close(fd, &p, 3000);
        kl_test_closesock(fd);
    }
    server_stop();
    printf("  sent %zu of %zu in %llu ms (write failed %d); wire %zu, first %ld, body %zu, closed %d, "
           "reset %d\n", sent, body, (unsigned long long)(t1 - t0), write_failed, p.wire_len,
           p.first_len, p.body_len, p.closed, p.reset);
    ASSERT_TRUE(fd >= 0);
    if (within) {
        EXPECT_EQ(sent, body);                       /* the server read it all: no stall */
        EXPECT_LT(t1 - t0, (uint64_t)4000);          /* well inside the idle timeout */
        ASSERT_GT(p.first_len, 0L);                  /* was: stalled, then reset part way */
        EXPECT_EQ(p.body_len, (size_t)(16 * 65536));
        EXPECT_FALSE(p.reset);
        EXPECT_TRUE(p.closed);
        EXPECT_EQ(p.wire_len, (size_t)p.first_len);
    } else {
        /* Past the budget, a client that will not read until its upload is taken: the server stops
         * reading (the answer is never cut off for the bound itself). Where the socket cannot take
         * the whole answer both sides then wait, as before the discard existed, until the idle
         * timeout (1.5 s here) ends it; where it can, the answer is out and the server lingers,
         * discarding up to its cap and then reading no more, until the same timeout. Either way the
         * upload stops short of the body, it ends, bounded, and what arrives is the answer or a
         * prefix of it, never anything else. */
        EXPECT_LT(sent, body);                       /* bounded: the server stopped reading */
        EXPECT_LT(t1 - t0, (uint64_t)5000);
        EXPECT_TRUE(p.closed || p.reset);
        EXPECT_TRUE(p.first_len == 0 || p.first_len == (long)p.wire_len);
    }
    pair_free(&p);
}
UTEST(stream_termination, an_upload_written_before_reading_gets_the_whole_answer) {
    upload_then_read_case(utest_result, 4u * 1024u * 1024u, 8u * 1024u * 1024u, 1);
}
UTEST(stream_termination, an_upload_past_the_drain_budget_without_reading_ends_by_the_idle_timeout) {
    upload_then_read_case(utest_result, 16u * 1024u * 1024u, 256u * 1024u, 0);
}

/* A full-duplex client (curl-like) with the default drain bounds: it keeps uploading a 16 MiB
 * rejected body, paced, while it reads the 1 MiB streamed answer. The bounds run out long before the
 * body ends; that stops the server's reading, never the answer, which arrives whole. (It used to be
 * reset part way, at the first bound.) */
UTEST(stream_termination, a_client_uploading_while_reading_gets_the_whole_answer_past_the_bounds) {
#if !defined(_WIN32)
    signal(SIGPIPE, SIG_IGN);
#endif
    memset(&g_p, 0, sizeof g_p);
    g_cfg_read_to = 5000;
    int port = server_start(8u * 1024u * 1024u, 0);
    g_cfg_read_to = 0;
    ASSERT_GT(port, 0);
    Pair p;
    memset(&p, 0, sizeof p);
    p.wire = malloc(WIRE_CAP);
    p.body = malloc(BODY_CAP);
    const size_t body = 16u * 1024u * 1024u;
    size_t sent = 0;
    int fd = connect_rcvbuf(port, 8192);              /* the answer takes a while to arrive */
    if (fd >= 0 && p.wire && p.body) {
        char rq[160];
        snprintf(rq, sizeof rq, "POST /await2 HTTP/1.1\r\nHost: x\r\nContent-Length: %zu\r\n\r\n", body);
        (void)kl_test_sockwrite(fd, rq, strlen(rq));
        kl_test_sleep_ms(100);
        (void)kl_test_set_nonblock(fd);
        static char chunk[65536];
        memset(chunk, 'b', sizeof chunk);
        uint64_t t0 = kl_monotonic_ms(), next = t0;
        while (p.first_len == 0 && !p.closed && !p.reset && kl_monotonic_ms() - t0 < 15000) {
            if (p.wire_len >= (size_t)(kl_monotonic_ms() - t0) * 1024u + 4096u) kl_test_sleep_ms(1);
            /* Read slowly, at about 1 KiB per ms whatever the platform's timer resolution: the
             * answer takes longer than the drain's 500 ms deadline to arrive, so the bounds run out
             * while it is still being sent. */
            size_t allow = (size_t)(kl_monotonic_ms() - t0) * 1024u + 4096u;
            if (p.wire_len < allow && kl_test_poll1(fd, 0, 1) > 0) {
                size_t room = allow - p.wire_len < 65536u ? allow - p.wire_len : 65536u;
                long n = kl_test_sockread(fd, p.wire + p.wire_len, room);
                if (n == 0) p.closed = 1;
                else if (n < 0) p.reset = 1;
                else {
                    p.wire_len += (size_t)n;
                    if (p.wire_len >= 5 && memcmp(p.wire + p.wire_len - 5, "0\r\n\r\n", 5) == 0)
                        p.first_len = parse_response(p.wire, p.wire_len, p.body, BODY_CAP,
                                                     &p.body_len, &p.first_close);
                }
            }
            if (sent < body && kl_monotonic_ms() >= next && kl_test_poll1(fd, 1, 1) > 0) {
                size_t want = body - sent < 16384 ? body - sent : 16384;
                long n = kl_test_sockwrite(fd, chunk, want);  /* paced: 16 KiB per 5 ms */
                if (n > 0) { sent += (size_t)n; next = kl_monotonic_ms() + 5; }
            }
        }
        if (p.first_len == 0)
            p.first_len = parse_response(p.wire, p.wire_len, p.body, BODY_CAP, &p.body_len,
                                         &p.first_close);
        kl_test_closesock(fd);
    }
    server_stop();
    printf("  sent %zu of %zu; wire %zu, first %ld, body %zu, closed %d, reset %d\n", sent, body,
           p.wire_len, p.first_len, p.body_len, p.closed, p.reset);
    ASSERT_TRUE(fd >= 0);
    EXPECT_LT(sent, body);                            /* the bounds ran out before the body did */
    ASSERT_GT(p.first_len, 0L);                       /* the whole answer, terminated */
    EXPECT_EQ(p.body_len, (size_t)(16 * 65536));
    EXPECT_EQ(p.wire_len, (size_t)p.first_len);
    pair_free(&p);
}

/* A client that uploads fast and reads slowly (64 KiB every 10 ms): the answer is in the kernel long
 * before the client has read it, and the drain after it must not close over the unread upload (the
 * reset would drop the queued answer). The whole answer arrives, then a clean end of stream. With
 * `flood_ms` the client keeps flooding for that long after the answer, and the server's discard must
 * stay within its caps: the discard while answering (reject_drain_max_bytes) plus the linger's
 * (max(reject_drain_max_bytes, max_body_size)), plus a receive window each. */
static void fast_upload_slow_read_case(int *utest_result, int flood_ms, size_t body, int half_close) {
#if !defined(_WIN32)
    signal(SIGPIPE, SIG_IGN);
#endif
    memset(&g_p, 0, sizeof g_p);
    g_answer_conn = NULL;
    int port = server_start((half_close ? 32u : 8u) * 1024u * 1024u, 0);
    ASSERT_GT(port, 0);
    Pair p;
    memset(&p, 0, sizeof p);
    p.wire = malloc(WIRE_CAP);
    p.body = malloc(BODY_CAP);
    size_t sent = 0;
    int shut = 0;
    int fd = connect_rcvbuf(port, half_close ? 65536 : 0);
    if (fd >= 0 && p.wire && p.body) {
        char rq[160];
        snprintf(rq, sizeof rq, "POST %s HTTP/1.1\r\nHost: x\r\nContent-Length: %zu\r\n\r\n",
                 half_close ? "/await3" : "/await2", body);
        (void)kl_test_sockwrite(fd, rq, strlen(rq));
        kl_test_sleep_ms(100);
        (void)kl_test_set_nonblock(fd);
        static char chunk[65536];
        memset(chunk, 'b', sizeof chunk);
        uint64_t t0 = kl_monotonic_ms(), next_read = t0, flood_until = 0;
        while (!p.closed && !p.reset && kl_monotonic_ms() - t0 < 20000) {
            uint64_t now = kl_monotonic_ms();
            if (flood_until && now >= flood_until) break;
            if (now >= next_read && kl_test_poll1(fd, 0, 0) > 0) {   /* a slice per 10 ms */
                long n = kl_test_sockread(fd, p.wire + p.wire_len, half_close ? 32768 : 65536);
                if (n == 0) p.closed = 1;
                else if (n < 0) p.reset = 1;
                else p.wire_len += (size_t)n;
                next_read = now + 10;
                if (p.first_len == 0 && p.wire_len >= 5 &&
                    memcmp(p.wire + p.wire_len - 5, "0\r\n\r\n", 5) == 0) {
                    p.first_len = parse_response(p.wire, p.wire_len, p.body, BODY_CAP, &p.body_len,
                                                 &p.first_close);
                    if (flood_ms && p.first_len > 0) flood_until = now + (uint64_t)flood_ms;
                }
            }
            if (sent < body && kl_test_poll1(fd, 1, 1) > 0) {       /* as fast as it is taken */
                size_t want = body - sent < sizeof chunk ? body - sent : sizeof chunk;
                long n = kl_test_sockwrite(fd, chunk, want);
                if (n > 0) sent += (size_t)n;
            }
            if (half_close && sent == body && !shut) {              /* the whole body, then FIN */
#if defined(_WIN32)
                (void)shutdown((SOCKET)fd, SD_SEND);
#else
                (void)shutdown(fd, SHUT_WR);
#endif
                shut = 1;
            }
        }
        if (!flood_ms) read_to_close(fd, &p, 3000);
        if (p.first_len == 0)
            p.first_len = parse_response(p.wire, p.wire_len, p.body, BODY_CAP, &p.body_len,
                                         &p.first_close);
        kl_test_closesock(fd);
        if (flood_ms) kl_test_sleep_ms(500);       /* the server notices the hang-up meanwhile */
    }
    unsigned caps = kl_event_caps(&g_srv.ev.loop);
    server_stop_join();
    uint64_t discarded = g_answer_conn ? g_answer_conn->drain_discarded : 0;
    int released = g_answer_conn && g_answer_conn->state == KL_HTTP_CONN_CLOSED;
    size_t window = g_answer_conn ? g_answer_conn->stream.read_cap : 0;
    kl_http_server_free(&g_srv);
    printf("  sent %zu; wire %zu, first %ld, body %zu, closed %d, reset %d; server discarded %llu\n",
           sent, p.wire_len, p.first_len, p.body_len, p.closed, p.reset,
           (unsigned long long)discarded);
    ASSERT_TRUE(fd >= 0);
    ASSERT_GT(p.first_len, 0L);                       /* was: reset, the queued answer dropped */
    EXPECT_EQ(p.body_len, (size_t)((half_close ? 256 : 16) * 65536));
    EXPECT_FALSE(p.reset);
    if (!flood_ms) {
        EXPECT_TRUE(p.closed);                        /* a clean end of stream after it */
        EXPECT_EQ(p.wire_len, (size_t)p.first_len);
    } else {
        /* Defaults: 64 KiB while answering, max(64 KiB, 1 MB) lingering, a window each. */
        EXPECT_LE(discarded, (uint64_t)(64u * 1024u + 1024u * 1024u + 2u * (window + 65536u)));
        /* Past the cap it reads no more. The flooding client's FIN waits behind the upload the
         * server no longer reads, so no hang-up is seen: the slot is held to read_timeout_ms,
         * bounded (it is still lingering here; nothing asserts when it goes). */
        (void)caps;
        (void)released;
    }
    pair_free(&p);
}
UTEST(stream_termination, a_fast_uploader_reading_slowly_gets_the_whole_answer_and_a_clean_end) {
    fast_upload_slow_read_case(utest_result, 0, 64u * 1024u * 1024u, 0);
}
UTEST(stream_termination, a_flooding_client_is_discarded_only_up_to_the_caps) {
    fast_upload_slow_read_case(utest_result, 1500, 64u * 1024u * 1024u, 0);
}
/* The whole body, a bit past both caps, then a half-close (FIN), reading slowly through a small
 * receive buffer, with a 16 MiB answer: the FIN reaches the lingering server behind the upload it
 * stopped reading, while much of the answer is still queued in the kernel. It must read on to the
 * end of stream, not close over the unread bytes (a reset that dropped the queued answer). With
 * epoll and poll that report even carries KL_EVENT_ERROR, this side having half-closed already. */
UTEST(stream_termination, a_half_closing_slow_reader_past_the_caps_gets_the_whole_answer) {
    /* Three rounds: whether the FIN meets the answer still queued depends on timing, so one round
     * alone does not reliably catch a close there. */
    for (int i = 0; i < 3; i++) {
        fast_upload_slow_read_case(utest_result, 0, 64u * 1024u + 1024u * 1024u + 256u * 1024u, 1);
        if (*utest_result) return;
    }
}

/* The body deadline passes while a streamed answer is buffered (the idle sweep's 408 path): no 408
 * is written after the answer's head; the answer is sent in full, then the connection closes. */
UTEST(stream_termination, a_body_timeout_after_a_streamed_answer_sends_it_all) {
    memset(&g_p, 0, sizeof g_p);
    g_cfg_body_to = 300;                             /* the body deadline hits first ... */
    g_cfg_read_to = 3000;                            /* ... long before the idle timeout */
    int port = server_start(8u * 1024u * 1024u, 0);
    g_cfg_body_to = 0;
    g_cfg_read_to = 0;
    ASSERT_GT(port, 0);
    Pair p;
    memset(&p, 0, sizeof p);
    p.wire = malloc(WIRE_CAP);
    p.body = malloc(BODY_CAP);
    int fd = connect_rcvbuf(port, 8192);
    if (fd >= 0 && p.wire && p.body) {
        const char *rq = "POST /await2 HTTP/1.1\r\nHost: x\r\nContent-Length: 64\r\n\r\n";
        (void)kl_test_sockwrite(fd, rq, strlen(rq));
        kl_test_sleep_ms(100);
        (void)kl_test_sockwrite(fd, "0123", 4);
        kl_test_sleep_ms(900);                       /* the body deadline passes; not reading */
        p.first_len = read_response(fd, &p, 0, p.body, &p.body_len, &p.first_close, 3000);
        read_to_close(fd, &p, 3000);
        kl_test_closesock(fd);
    }
    server_stop();
    printf("  wire %zu, first %ld, body %zu, closed %d, reset %d\n", p.wire_len, p.first_len,
           p.body_len, p.closed, p.reset);
    ASSERT_GT(p.first_len, 0L);                      /* was: closed at once, cut off */
    EXPECT_EQ(p.body_len, (size_t)(16 * 65536));
    EXPECT_FALSE(p.reset);
    EXPECT_TRUE(p.closed);
    EXPECT_EQ(p.wire_len, (size_t)p.first_len);      /* no 408 after it */
    pair_free(&p);
}

/* HTTP/1.0 with Connection: keep-alive: a streamed response's head carries no keep-alive signal, so
 * the client reads it to the end of the connection, which must come (it used to stay open until
 * the idle timeout). A buffered response announces keep-alive and keeps the connection. */
UTEST(stream_termination, an_http10_streamed_response_closes_after_it) {
    int port = server_start(0, 0);
    ASSERT_GT(port, 0);
    Pair p;
    memset(&p, 0, sizeof p);
    p.wire = malloc(WIRE_CAP);
    p.body = malloc(BODY_CAP);
    int fd = connect_rcvbuf(port, 0);
    if (fd >= 0 && p.wire && p.body) {
        const char *rq = "GET /inline HTTP/1.0\r\nHost: x\r\nConnection: keep-alive\r\n\r\n";
        (void)kl_test_sockwrite(fd, rq, strlen(rq));
        p.first_len = read_response(fd, &p, 0, p.body, &p.body_len, &p.first_close, 3000);
        read_to_close(fd, &p, 1500);
        kl_test_closesock(fd);
    }
    server_stop();
    ASSERT_GT(p.first_len, 0L);
    EXPECT_EQ(p.body_len, (size_t)4096);
    EXPECT_FALSE(p.reset);
    EXPECT_TRUE(p.closed);                           /* was: open until the idle timeout */
    EXPECT_EQ(p.wire_len, (size_t)p.first_len);
    pair_free(&p);
}

/* Empty lines after a request are not a request: they do not end keep-alive. */
UTEST(stream_termination, a_crlf_after_a_body_keeps_the_connection) {
    int port = server_start(0, 0);
    ASSERT_GT(port, 0);
    Pair p;
    memset(&p, 0, sizeof p);
    p.wire = malloc(WIRE_CAP);
    p.body = malloc(BODY_CAP);
    int fd = connect_rcvbuf(port, 0);
    if (fd >= 0 && p.wire && p.body) {
        const char *rq = "POST /sink HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello\r\n";
        (void)kl_test_sockwrite(fd, rq, strlen(rq));
        p.first_len = read_response(fd, &p, 0, p.body, &p.body_len, &p.first_close, 3000);
        if (p.first_len > 0 && !p.closed) {
            const char *rq2 = "GET /hello HTTP/1.1\r\nHost: x\r\n\r\n";
            (void)kl_test_sockwrite(fd, rq2, strlen(rq2));
            size_t b2 = 0;
            p.second_len = read_response(fd, &p, (size_t)p.first_len, NULL, &b2, NULL, 3000);
        }
        kl_test_closesock(fd);
    }
    server_stop();
    ASSERT_TRUE(fd >= 0);
    EXPECT_FALSE(p.first_close);
    ASSERT_TWO_RESPONSES(p);
    pair_free(&p);
}

/* ── A batch holding a second event for a connection the first released ─────────────────────────
 * kqueue reports a connection's READ and WRITE as two events of one batch. Handling the first can
 * release the connection; the second must then not act on the slot: not release it again (the pool's
 * count goes negative and its free list loops on itself, so every later accept shares one slot), and
 * not drive a new connection that an accept in the same batch put in that slot. */

/* GET /where/:id answers {"id":"<id>"} and records which slot answered, in which batch. */
#define WHERE_MAX 8
static int        g_wait_seq;                /* batches returned by the wrapped wait */
static int        g_where_n;
static KlHttpConn *g_where_conn[WHERE_MAX];
static int        g_where_seq[WHERE_MAX];
static void handle_where(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)ctx;
    size_t il = 0;
    const char *id = kl_http_request_param(req, "id", &il);
    int k = g_where_n++;
    if (k < WHERE_MAX) {
        g_where_conn[k] = kl_http_request_conn(req);
        g_where_seq[k] = kl_atomic_load_int(&g_wait_seq);
    }
    char body[64];
    int bl = snprintf(body, sizeof body, "{\"id\":\"%.*s\"}", (int)(il < 16 ? il : 16), id ? id : "");
    kl_http_response_status(res, 200);
    kl_http_response_body_copy(res, body, (size_t)bl);   /* the json helper borrows */
}

/* The built-in readiness backend, with its wait shaping one batch on request: the first connection
 * event is followed by a second one for the same connection (DUP), or by a listen event and then the
 * second one (REUSE). Listen events are held back while g_hold_listen is set, so a client connected
 * meanwhile waits in the backlog until the REUSE batch accepts it. */
enum { BATCH_PASS, BATCH_DUP, BATCH_REUSE };
static int g_batch_mode, g_hold_listen, g_inject_seq;
static int batch_wait(KlEventLoop *l, KlEvent *out, int max, int timeout_ms) {
    int n = kl_event_wait_builtin(l, out, max, timeout_ms);
    if (n <= 0) return n;
    int seq = kl_atomic_load_int(&g_wait_seq) + 1;
    kl_atomic_store_int(&g_wait_seq, seq);
    int k = 0;
    for (int i = 0; i < n; i++)
        if (out[i].udata || !kl_atomic_load_int(&g_hold_listen)) out[k++] = out[i];
    n = k;
    int mode = kl_atomic_load_int(&g_batch_mode);
    if (mode == BATCH_PASS) return n;
    for (int i = 0; i < n && n + 2 <= max; i++) {
        uintptr_t u = (uintptr_t)out[i].udata;
        if (!u || (u & 1)) continue;                 /* the listener, or a watcher */
        if (mode == BATCH_REUSE) {
            out[n].udata = NULL;
            out[n++].ready = KL_EVENT_READ;
            kl_atomic_store_int(&g_hold_listen, 0);
        }
        out[n].udata = out[i].udata;
        out[n++].ready = KL_EVENT_READ;
        kl_atomic_store_int(&g_inject_seq, seq);
        kl_atomic_store_int(&g_batch_mode, BATCH_PASS);
        break;
    }
    return n;
}
static const KlEventOps batch_ops = {
    kl_event_init_builtin, kl_event_add_builtin, kl_event_mod_builtin, kl_event_del_builtin,
    batch_wait, kl_event_close_builtin, kl_event_caps_builtin, kl_event_native_provider_builtin, NULL,
};
static const KlEventProvider batch_provider = { &batch_ops, "builtin-batch-shaping" };

static int built_in_is_completion(void) {
    KlAllocator alloc = kl_allocator_default();
    KlEventLoop probe = { 0 };
    probe.alloc = &alloc;
    if (kl_event_init(&probe) != 0) return 1;
    unsigned caps = kl_event_caps(&probe);
    kl_event_close(&probe);
    return (caps & KL_EVENT_CAP_COMPLETION) != 0;
}

static int wait_active(int want, int ms) {
    KlHttpServerStats st;
    for (int i = 0; i * 10 < ms; i++) {
        kl_http_server_stats(&g_srv, &st);
        if (st.active_connections == want) return want;
        kl_test_sleep_ms(10);
    }
    kl_http_server_stats(&g_srv, &st);
    return st.active_connections;
}

/* The pool after the connections above it are gone: none active, and a free list that does not
 * loop on its head. */
static void expect_pool_sound(int *utest_result) {
    int active = wait_active(0, 2000);
    KlHttpConn *fl = g_srv.pool.free_list;
    printf("  active %d, free list %p -> %p\n", active, (void *)fl, fl ? (void *)fl->next_free : NULL);
    EXPECT_EQ(active, 0);
    EXPECT_EQ(g_srv.pool.active_count, 0);
    EXPECT_TRUE(fl != NULL && fl->next_free != fl);
}

/* Read one response and check it is the 200 for `id`. */
static void expect_where(int *utest_result, int fd, const char *id) {
    Pair p;
    memset(&p, 0, sizeof p);
    p.wire = malloc(WIRE_CAP);
    p.body = malloc(BODY_CAP);
    ASSERT_TRUE(p.wire && p.body);
    p.first_len = read_response(fd, &p, 0, p.body, &p.body_len, &p.first_close, 2000);
    char want[64];
    int wl = snprintf(want, sizeof want, "{\"id\":\"%s\"}", id);
    printf("  %s: response %ld, body %.*s\n", id, p.first_len, (int)(p.body_len < 40 ? p.body_len : 40),
           p.body);
    EXPECT_GT(p.first_len, 0L);
    EXPECT_EQ(memcmp(p.wire, "HTTP/1.1 200", 12), 0);
    EXPECT_EQ(p.body_len, (size_t)wl);
    if (p.body_len == (size_t)wl) EXPECT_EQ(memcmp(p.body, want, (size_t)wl), 0);
    pair_free(&p);
}

/* Several concurrent clients, interleaved: each gets its own answer, from its own slot. A third
 * client only where the pool holds three. */
static void expect_concurrent_clients(int *utest_result, int port, int three) {
    int a = connect_rcvbuf(port, 0);
    ASSERT_TRUE(a >= 0);
    kl_test_sleep_ms(50);
    (void)kl_test_sockwrite(a, "GET /where/A HTTP/1.1\r\n", 23);   /* half a head */
    kl_test_sleep_ms(50);
    int b = connect_rcvbuf(port, 0);
    ASSERT_TRUE(b >= 0);
    kl_test_sleep_ms(50);
    int c = three ? connect_rcvbuf(port, 0) : -1;
    ASSERT_TRUE(!three || c >= 0);
    kl_test_sleep_ms(50);
    const char *rb = "GET /where/B HTTP/1.1\r\nHost: x\r\n\r\n";
    (void)kl_test_sockwrite(b, rb, strlen(rb));
    kl_test_sleep_ms(100);
    (void)kl_test_sockwrite(a, "Host: x\r\n\r\n", 11);             /* the rest of A's */
    kl_test_sleep_ms(50);
    const char *rc = "GET /where/C HTTP/1.1\r\nHost: x\r\n\r\n";
    if (three) (void)kl_test_sockwrite(c, rc, strlen(rc));
    expect_where(utest_result, a, "A");
    expect_where(utest_result, b, "B");
    if (three) expect_where(utest_result, c, "C");
    kl_test_closesock(a);
    kl_test_closesock(b);
    if (three) kl_test_closesock(c);
}

/* A client resets while its rejected request's streamed answer is still going out, the rest of the
 * body being discarded (READ and WRITE armed). On kqueue the reset arrives as two events of one
 * batch; the first releases the connection. */
UTEST(stream_termination, a_reset_during_a_discarding_answer_releases_its_slot_once) {
#if !defined(_WIN32)
    signal(SIGPIPE, SIG_IGN);
#endif
    memset(&g_p, 0, sizeof g_p);
    g_where_n = 0;
    g_cfg_read_to = 5000;
    int port = server_start(0, 0);
    g_cfg_read_to = 0;
    ASSERT_GT(port, 0);
    int fd = connect_rcvbuf(port, 8192);
    ASSERT_TRUE(fd >= 0);
    const char *rq = "POST /await2 HTTP/1.1\r\nHost: x\r\nContent-Length: 64\r\n\r\n";
    (void)kl_test_sockwrite(fd, rq, strlen(rq));
    kl_test_sleep_ms(100);
    (void)kl_test_sockwrite(fd, "0123456789012345678901234567890123456789012345678901234567890123", 64);
    kl_test_sleep_ms(200);                           /* the answer backs up; the body is refused */
    struct linger lg = { 1, 0 };
    (void)setsockopt(fd, SOL_SOCKET, SO_LINGER, (const char *)&lg, sizeof lg);
    kl_test_closesock(fd);                           /* a reset */
    expect_pool_sound(utest_result);
    expect_concurrent_clients(utest_result, port, 1);
    expect_pool_sound(utest_result);
    server_stop();
}

/* The same, deterministically on every readiness loop: the batch carries a second event for a
 * connection its first event closed (Connection: close). */
UTEST(stream_termination, a_second_event_for_a_released_connection_is_dropped) {
    if (built_in_is_completion()) UTEST_SKIP("the batch-shaping wrapper is readiness-only");
    g_where_n = 0;
    g_cfg_provider = &batch_provider;
    int port = server_start(0, 0);
    g_cfg_provider = NULL;
    ASSERT_GT(port, 0);
    int x = connect_rcvbuf(port, 0);
    ASSERT_TRUE(x >= 0);
    ASSERT_EQ(wait_active(1, 2000), 1);
    kl_atomic_store_int(&g_batch_mode, BATCH_DUP);
    const char *rq = "GET /where/X HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    (void)kl_test_sockwrite(x, rq, strlen(rq));
    expect_where(utest_result, x, "X");
    kl_test_closesock(x);
    EXPECT_EQ(kl_atomic_load_int(&g_batch_mode), BATCH_PASS);   /* the batch was shaped */
    expect_pool_sound(utest_result);
    expect_concurrent_clients(utest_result, port, 1);
    expect_pool_sound(utest_result);
    server_stop();
    kl_atomic_store_int(&g_batch_mode, BATCH_PASS);
}

/* The slot released by the first event is taken by a new connection accepted later in the same
 * batch; the batch's second event, the old connection's, must not drive the new one. The new
 * client's request is already waiting, so a stale event acting on it would answer it in that batch;
 * the new connection's own readiness can only come in a later one. */
UTEST(stream_termination, a_stale_event_does_not_act_on_a_connection_reusing_its_slot) {
    if (built_in_is_completion()) UTEST_SKIP("the batch-shaping wrapper is readiness-only");
    g_where_n = 0;
    g_cfg_provider = &batch_provider;
    g_cfg_max_conns = 2;
    int port = server_start(0, 0);
    g_cfg_provider = NULL;
    g_cfg_max_conns = 0;
    ASSERT_GT(port, 0);
    int x = connect_rcvbuf(port, 0);
    ASSERT_TRUE(x >= 0);
    ASSERT_EQ(wait_active(1, 2000), 1);
    kl_atomic_store_int(&g_hold_listen, 1);
    int y = connect_rcvbuf(port, 0);                 /* waits in the backlog */
    ASSERT_TRUE(y >= 0);
    const char *ry = "GET /where/Y HTTP/1.1\r\nHost: x\r\n\r\n";
    (void)kl_test_sockwrite(y, ry, strlen(ry));
    kl_test_sleep_ms(100);
    EXPECT_EQ(wait_active(1, 100), 1);               /* not accepted yet */
    kl_atomic_store_int(&g_batch_mode, BATCH_REUSE);
    const char *rx = "GET /where/X HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    (void)kl_test_sockwrite(x, rx, strlen(rx));
    expect_where(utest_result, x, "X");
    expect_where(utest_result, y, "Y");
    kl_test_closesock(x);
    kl_atomic_store_int(&g_hold_listen, 0);
    int inject = kl_atomic_load_int(&g_inject_seq);
    printf("  shaped batch %d; X answered in %d by %p, Y in %d by %p\n", inject, g_where_seq[0],
           (void *)g_where_conn[0], g_where_seq[1], (void *)g_where_conn[1]);
    ASSERT_EQ(g_where_n, 2);
    ASSERT_TRUE(g_where_conn[0] == g_where_conn[1]);   /* Y did take X's slot in that batch */
    EXPECT_EQ(g_where_seq[0], inject);
    EXPECT_GT(g_where_seq[1], inject);                 /* answered on its own event, not X's */
    kl_test_closesock(y);
    expect_pool_sound(utest_result);
    expect_concurrent_clients(utest_result, port, 0);
    expect_pool_sound(utest_result);
    server_stop();
    kl_atomic_store_int(&g_batch_mode, BATCH_PASS);
}

UTEST_MAIN();
