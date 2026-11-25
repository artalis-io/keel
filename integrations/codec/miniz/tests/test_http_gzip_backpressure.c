/*
 * Bounded compressed streaming over a live server with the real miniz codec, under backpressure.
 * The shape of tests/protocols/http/test_http_stream_relay.c's upstream producer: a suspended
 * handler writes with kl_http_compress_stream_try_write and resumes on the writable edge; a client
 * with a small receive buffer stalls, then reads everything. Checked: the producer is refused
 * (WOULD_BLOCK) and resumes, pending never exceeds the capacity, the body decodes byte-exact and
 * incrementally, the stream finishes (waiting for room at the finish is covered by
 * tests/protocols/http/test_compress_bounded.c), every allocation is returned,
 * and a client that leaves mid-stall gets the op cancelled and the stream released. POSIX only.
 * Opt-in (bring-your-own miniz): built via `make decompress-miniz-test MINIZ_DIR=...`.
 */
#include "utest.h"
#include <keel/keel.h>
#include <keel/compress.h>
#include <keel/http_compress.h>
#include "compress_miniz.h"
#include <miniz.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define GB_CAP    (64u * 1024u)            /* the server's stream capacity */
#define GB_TOTAL  (12u * 1024u * 1024u)    /* uncompressed body */

static unsigned rnd(unsigned *s) { *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5; return *s; }

/* Every allocation the server makes, counted (live bytes must return to the start). */
typedef struct { size_t live, peak; pthread_mutex_t mu; } CountAlloc;
static CountAlloc g_ca = { 0, 0, PTHREAD_MUTEX_INITIALIZER };
static void ca_add(long d) {
    pthread_mutex_lock(&g_ca.mu);
    g_ca.live = (size_t)((long)g_ca.live + d);
    if (g_ca.live > g_ca.peak) g_ca.peak = g_ca.live;
    pthread_mutex_unlock(&g_ca.mu);
}
static void *ca_malloc(void *ctx, size_t n) { (void)ctx; void *p = malloc(n); if (p) ca_add((long)n); return p; }
static void *ca_realloc(void *ctx, void *ptr, size_t o, size_t n) {
    (void)ctx;
    void *p = realloc(ptr, n);
    if (p) ca_add((long)n - (long)o);
    return p;
}
static void ca_free(void *ctx, void *ptr, size_t n) { (void)ctx; if (ptr) { free(ptr); ca_add(-(long)n); } }
static KlAllocator g_alloc = { ca_malloc, ca_realloc, ca_free, NULL };

/* ── The producer: a suspended handler writing compressed, paced by the writable edge ────────── */
typedef struct {
    KlAsyncOp op;
    KlHttpResponse *res;
    KlHttpCompressStream cs;
    size_t produced;           /* uncompressed bytes accepted */
    size_t max_pending;
    int would_blocks, end_would_blocks, ended, cancelled, errors;
    char block[16384];
    size_t block_len;          /* the next write, kept across a WOULD_BLOCK */
} Producer;
static Producer g_p;
static KlHttpServer g_srv;
static KlCompressCtx *g_cctx;

/* The body is a deterministic function of the offset: random 16 KiB blocks for most (incompressible,
 * so about 9 MB reach the wire: more than any kernel buffers between the two ends, whatever the
 * host's socket-buffer autotuning) and SSE-like text in between. */
static size_t next_block(size_t at, char *b) {
    unsigned s = (unsigned)(at * 2654435761u) | 1u;        /* local: both threads call this */
    if ((at / 16384) % 4 != 3) {
        size_t n = 16384;
        for (size_t i = 0; i < n; i++) b[i] = (char)(rnd(&s) & 0xff);
        return n;
    }
    size_t n = (size_t)snprintf(b, 512, "data: {\"choices\":[{\"delta\":{\"content\":\" t%zu\"}}]}\n\n", at);
    return n;
}

static void pump(void) {
    while (!g_p.ended && !g_p.cancelled) {
        if (g_p.produced >= GB_TOTAL) {
            KlHttpStreamStatus st = kl_http_compress_stream_try_end(&g_p.cs);
            if (st == KL_HTTP_STREAM_WOULD_BLOCK) { g_p.end_would_blocks++; return; }
            if (st != KL_HTTP_STREAM_ACCEPTED) { g_p.errors++; kl_async_cancel(&g_srv, &g_p.op); return; }
            __atomic_store_n(&g_p.ended, 1, __ATOMIC_RELEASE);
            kl_async_complete(&g_srv, &g_p.op);
            return;
        }
        if (g_p.block_len == 0) g_p.block_len = next_block(g_p.produced, g_p.block);
        KlHttpStreamStatus st = kl_http_compress_stream_try_write(&g_p.cs, g_p.block, g_p.block_len);
        if (st == KL_HTTP_STREAM_WOULD_BLOCK) { __atomic_fetch_add(&g_p.would_blocks, 1, __ATOMIC_RELEASE); return; }
        if (st != KL_HTTP_STREAM_ACCEPTED) { g_p.errors++; kl_async_cancel(&g_srv, &g_p.op); return; }
        __atomic_store_n(&g_p.produced, g_p.produced + g_p.block_len, __ATOMIC_RELEASE);
        g_p.block_len = 0;
        size_t pend = kl_http_response_stream_pending(g_p.res);
        if (pend > g_p.max_pending) g_p.max_pending = pend;
    }
}
static void on_writable(void *ud) { (void)ud; pump(); }
static void on_resume(KlAsyncOp *op, void *ud) { (void)op; (void)ud; }
static void on_cancel(KlAsyncOp *op, void *ud) {
    (void)op; (void)ud;
    kl_http_compress_stream_release(&g_p.cs);      /* abandon: write nothing more */
    __atomic_fetch_add(&g_p.cancelled, 1, __ATOMIC_RELEASE);   /* published after the release */
}
static void start(void *ud) { (void)ud; pump(); }
static void handle(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)ctx;                                     /* g_p was zeroed by server_up, before this thread */
    g_p.res = res;
    KlCompressConfig cfg = { .ctx = g_cctx, .factory = kl_compress_miniz_create };
    if (kl_http_compress_stream_begin(res, &cfg, 200, &g_p.cs) < 0) return;
    if (!kl_http_compress_stream_bounded(&g_p.cs)) return;
    (void)kl_http_response_stream_on_writable(res, on_writable, NULL);
    g_p.op.on_resume = on_resume;
    g_p.op.on_cancel = on_cancel;
    if (kl_async_suspend(&g_srv, kl_http_request_conn(req), &g_p.op) < 0) return;
    (void)kl_timer_add(kl_http_server_event_ctx(&g_srv), 0, start, NULL);
}

static void *server_thread(void *arg) { kl_http_server_run(arg); return NULL; }
static int g_ready;
static void mark_ready(void *ud) { (void)ud; __atomic_store_n(&g_ready, 1, __ATOMIC_SEQ_CST); }
static pthread_t g_tid;
static int server_up(void) {
    memset(&g_p, 0, sizeof g_p);                         /* no state left from an earlier case */
    g_cctx = kl_compress_miniz_ctx_create(1, &g_alloc);
    KlHttpServerConfig cfg = { .port = 0, .max_connections = 4, .stream_capacity = GB_CAP,
                               .alloc = &g_alloc };
    if (!g_cctx || kl_http_server_init(&g_srv, &cfg) != 0) return -1;
    kl_http_server_route(&g_srv, "GET", "/gz", handle, NULL, NULL);
    /* Ordered startup: this zero-delay timer runs on the loop's first tick, once the listener is
     * bound and listening; the test reads the port only after seeing its flag (polling the port
     * raced with the server thread writing it, and could connect between bind and listen). */
    __atomic_store_n(&g_ready, 0, __ATOMIC_SEQ_CST);
    if (kl_timer_add(kl_http_server_event_ctx(&g_srv), 0, mark_ready, NULL) < 0) return -1;
    if (pthread_create(&g_tid, NULL, server_thread, &g_srv) != 0) return -1;
    for (int i = 0; i < 500 && !__atomic_load_n(&g_ready, __ATOMIC_SEQ_CST); i++) usleep(10000);
    return __atomic_load_n(&g_ready, __ATOMIC_SEQ_CST) && kl_http_server_bound_port(&g_srv) > 0 ? 0 : -1;
}
static void server_down(void) {
    kl_http_server_stop(&g_srv);
    pthread_join(g_tid, NULL);
    kl_http_server_free(&g_srv);
    kl_compress_miniz_ctx_destroy(g_cctx);
}
static int connect_small(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int rb = 4096;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rb, sizeof rb);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) { close(fd); return -1; }
    const char *req = "GET /gz HTTP/1.1\r\nHost: t\r\nAccept-Encoding: gzip\r\n\r\n";
    if (write(fd, req, strlen(req)) < 0) { close(fd); return -1; }
    return fd;
}

/* ── The reader: de-chunk, then inflate the gzip member as it arrives ─────────────────────────── */
typedef struct {
    int st;                    /* 0 head, 1 size line, 2 data, 3 CRLF after data, 4 done */
    char line[64];
    size_t ll, left, hdr_seen, gz_skip;
    mz_stream z;
    size_t decoded, mismatch;
    char exp[16384];           /* the producer's block that `decoded` falls in */
    size_t exp_at, exp_len;
} Reader;
static int rd_inflate(Reader *r, const unsigned char *p, size_t n) {
    size_t s = n < r->gz_skip ? n : r->gz_skip;
    r->gz_skip -= s; p += s; n -= s;
    r->z.next_in = p;
    r->z.avail_in = (unsigned)n;
    for (;;) {
        unsigned char out[32768];
        r->z.next_out = out;
        r->z.avail_out = sizeof out;
        int rc = mz_inflate(&r->z, MZ_SYNC_FLUSH);
        size_t got = sizeof out - r->z.avail_out;
        /* Compare against the producer's body: blocks laid end to end from offset 0. */
        for (size_t i = 0; i < got; ) {
            while (r->decoded >= r->exp_at + r->exp_len) {
                r->exp_at += r->exp_len;
                r->exp_len = next_block(r->exp_at, r->exp);
            }
            size_t off = r->decoded - r->exp_at, len = r->exp_len - off;
            if (len > got - i) len = got - i;
            if (memcmp(out + i, r->exp + off, len) != 0) r->mismatch++;
            r->decoded += len;
            i += len;
        }
        if (rc == MZ_STREAM_END) return 0;
        if (rc != MZ_OK && rc != MZ_BUF_ERROR) return -1;
        if (r->z.avail_in == 0 && got < sizeof out) return 0;
    }
}
/* Feed raw response bytes; -1 on a malformed response or a decode error. */
static int rd_feed(Reader *r, const unsigned char *p, size_t n) {
    for (size_t i = 0; i < n; ) {
        if (r->st == 0) {   /* skip the head: up to CRLFCRLF */
            r->hdr_seen = (r->hdr_seen << 8 | p[i++]) & 0xffffffffu;
            if (r->hdr_seen == 0x0d0a0d0au) r->st = 1;
        } else if (r->st == 1) {
            char c = (char)p[i++];
            if (c == '\n') { r->line[r->ll] = 0; r->left = strtoul(r->line, NULL, 16); r->ll = 0;
                             r->st = r->left ? 2 : 4; }
            else if (c != '\r' && r->ll < sizeof r->line - 1) r->line[r->ll++] = c;
        } else if (r->st == 2) {
            size_t k = n - i < r->left ? n - i : r->left;
            if (rd_inflate(r, p + i, k) < 0) return -1;
            i += k; r->left -= k;
            if (!r->left) r->st = 3;
        } else if (r->st == 3) {
            if (p[i++] == '\n') r->st = 1;
        } else {
            i = n;
        }
    }
    return 0;
}

/* Wait (up to 10 s) for the producer's first refusal: the stalled reader's buffers have filled. */
static int wait_for_backpressure(void) {
    for (int i = 0; i < 1000; i++) {
        if (__atomic_load_n(&g_p.would_blocks, __ATOMIC_ACQUIRE) > 0) return 1;
        usleep(10000);
    }
    return 0;
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

UTEST(gzip_backpressure, a_stalled_reader_bounds_the_producer_and_the_body_arrives_whole) {
    size_t live0 = g_ca.live;
    ASSERT_EQ(server_up(), 0);
    int fd = connect_small(g_srv.bound_port);
    ASSERT_GE(fd, 0);
    EXPECT_TRUE(wait_for_backpressure());                   /* stall: every buffer fills */
    usleep(100000);
    EXPECT_LT(__atomic_load_n(&g_p.produced, __ATOMIC_ACQUIRE), (size_t)GB_TOTAL);   /* stopped */
    Reader r;
    memset(&r, 0, sizeof r);
    r.gz_skip = 10;
    ASSERT_EQ(mz_inflateInit2(&r.z, -MZ_DEFAULT_WINDOW_BITS), MZ_OK);
    double t0 = now_s();
    static unsigned char buf[65536];
    while (r.st != 4 && now_s() - t0 < 60) {
        struct pollfd pf = { fd, POLLIN, 0 };
        if (poll(&pf, 1, 1000) <= 0) continue;
        ssize_t n = read(fd, buf, sizeof buf);
        if (n <= 0) break;
        ASSERT_EQ(rd_feed(&r, buf, (size_t)n), 0);
    }
    EXPECT_EQ(r.st, 4);                                     /* the terminal chunk arrived */
    /* The terminal chunk leaves inside try_end, before the producer records the end. */
    for (int i = 0; i < 200 && !__atomic_load_n(&g_p.ended, __ATOMIC_ACQUIRE); i++) usleep(10000);
    EXPECT_EQ(r.decoded, __atomic_load_n(&g_p.produced, __ATOMIC_ACQUIRE));
    EXPECT_GE(r.decoded, (size_t)GB_TOTAL);
    EXPECT_EQ(r.mismatch, (size_t)0);                       /* byte-exact */
    EXPECT_TRUE(g_p.ended);
    EXPECT_EQ(g_p.errors, 0);
    EXPECT_LE(g_p.max_pending, (size_t)GB_CAP);             /* the bound held throughout */
    mz_inflateEnd(&r.z);
    close(fd);
    server_down();
    EXPECT_EQ(g_ca.live, live0);                            /* everything returned */
    /* Codec state (~300 KB for miniz) + staging + the response's bounded buffers, not the body. */
    EXPECT_LT(g_ca.peak, (size_t)(1024u * 1024u));
}

UTEST(gzip_backpressure, a_client_that_leaves_mid_stall_cancels_and_releases_the_stream) {
    size_t live0 = g_ca.live;
    ASSERT_EQ(server_up(), 0);
    int fd = connect_small(g_srv.bound_port);
    ASSERT_GE(fd, 0);
    EXPECT_TRUE(wait_for_backpressure());
    close(fd);                                              /* gone without reading */
    for (int i = 0; i < 300 && !__atomic_load_n(&g_p.cancelled, __ATOMIC_ACQUIRE); i++) usleep(10000);
    EXPECT_EQ(g_p.cancelled, 1);
    EXPECT_TRUE(__atomic_load_n(&g_p.cs.comp, __ATOMIC_ACQUIRE) == NULL);   /* released in on_cancel */
    server_down();
    EXPECT_EQ(g_ca.live, live0);
}

UTEST_MAIN()
