/*
 * test_http_gzip_pause.c: response-body pause/resume through the real miniz gzip decoder.
 *
 * The miniz decoder supplies KlDecompress.dfeed_stoppable, so the async HTTP client stops it at the
 * fragment the user paused on and holds no decoded output itself. These cases drive the public client
 * API against a loopback peer serving real gzip bodies: a text body paused on every fragment, a body
 * at deflate's maximum ratio (all zeros, about 1000:1) with the client's peak allocation checked, and
 * a corrupt CRC32 that must still fail the request after every decoded byte was delivered across the
 * pauses. The maximum-ratio body is also run with dfeed_stoppable hidden, through the client's
 * fallback for decoders without it (bounded input slices, held-back output).
 *
 * POSIX-only harness (built by `make decompress-miniz-test`).
 */
#if defined(__linux__)
#define _DEFAULT_SOURCE   /* sockets, poll, pthread under -std=c11 (glibc) */
#endif
#include "utest.h"
#include <keel/keel.h>
#include <keel/http_client.h>
#include <keel/compress.h>
#include <keel/decompress.h>
#include "compress_miniz.h"
#include "decompress_miniz.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* ── Peer: serve one canned response, then hold the connection ───── */

typedef struct {
    int         lfd, port;
    const char *resp;
    size_t      resp_len;
    pthread_t   tid;
} Peer;

static void *peer_main(void *arg) {
    Peer *p = arg;
    struct pollfd pf = { p->lfd, POLLIN, 0 };
    if (poll(&pf, 1, 5000) <= 0) return NULL;
    int c = accept(p->lfd, NULL, NULL);
    if (c < 0) return NULL;
    char buf[2048];
    (void)recv(c, buf, sizeof buf, 0);   /* the request (small; one read is enough on loopback) */
    size_t off = 0;
    while (off < p->resp_len) {
        ssize_t w = send(c, p->resp + off, p->resp_len - off, 0);
        if (w <= 0) break;
        off += (size_t)w;
    }
    pf.fd = c;
    while (poll(&pf, 1, 5000) > 0 && recv(c, buf, sizeof buf, 0) > 0) {}   /* until the client closes */
    close(c);
    return NULL;
}

static int peer_start(Peer *p, const char *resp, size_t len) {
    memset(p, 0, sizeof *p);
    p->resp = resp;
    p->resp_len = len;
    p->lfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t al = sizeof a;
    if (p->lfd < 0 || bind(p->lfd, (struct sockaddr *)&a, sizeof a) != 0 || listen(p->lfd, 2) != 0 ||
        getsockname(p->lfd, (struct sockaddr *)&a, &al) != 0)
        return -1;
    p->port = ntohs(a.sin_port);
    return pthread_create(&p->tid, NULL, peer_main, p);
}

static void peer_join(Peer *p) {
    pthread_join(p->tid, NULL);
    close(p->lfd);
}

/* ── Counting allocator for the client ───────────────────────────── */

typedef struct { size_t cur, peak; } Count;
static void *c_malloc(void *ctx, size_t n) {
    Count *c = ctx; void *p = malloc(n);
    if (p) { c->cur += n; if (c->cur > c->peak) c->peak = c->cur; }
    return p;
}
static void *c_realloc(void *ctx, void *p, size_t o, size_t n) {
    Count *c = ctx; void *q = realloc(p, n);
    if (q) { c->cur = c->cur - o + n; if (c->cur > c->peak) c->peak = c->cur; }
    return q;
}
static void c_free(void *ctx, void *p, size_t n) { Count *c = ctx; if (p) c->cur -= n; free(p); }

/* ── Consumer ────────────────────────────────────────────────────── */

typedef struct {
    KlHttpClient *client;
    char   *got;
    size_t  len, cap;
    int     paused, done, completes, bad;
    KlError err;
} Sink;

static int on_body(const char *d, size_t n, void *ud) {
    Sink *s = ud;
    if (s->paused || n == 0) s->bad = 1;
    if (s->len + n > s->cap) {
        size_t nc = s->cap ? s->cap : 4096;
        while (nc < s->len + n) nc *= 2;
        s->got = realloc(s->got, nc);
        s->cap = nc;
    }
    memcpy(s->got + s->len, d, n);
    s->len += n;
    s->paused = 1;
    return KL_HTTP_CLIENT_BODY_PAUSE;   /* every fragment */
}
static void on_complete(void *ud) { Sink *s = ud; if (s->paused) s->bad = 1; s->completes++; }
static void on_done(KlHttpClient *c, void *ud) {
    Sink *s = ud;
    s->done = 1;
    s->err = kl_http_client_last_error(c);
}

/* gzip `body` with the miniz compressor. Caller frees *out with kl_free(&al, *out, *out_len). */
static int gzip_body(const char *body, size_t n, char **out, size_t *out_len, KlAllocator *al) {
    KlCompressCtx *ctx = kl_compress_miniz_ctx_create(6, al);
    KlCompress *cz = ctx ? kl_compress_miniz_create(ctx, al) : NULL;
    int rc = cz ? cz->compress(cz, body, n, out, out_len, al) : -1;
    if (cz) cz->destroy(cz);
    if (ctx) kl_compress_miniz_ctx_destroy(ctx);
    return rc;
}

/* The miniz decoder with dfeed_stoppable hidden: a decoder that only has dfeed. */
static KlDecompress *legacy_miniz_create(KlCompressCtx *ctx, KlAllocator *alloc) {
    KlDecompress *d = kl_decompress_miniz_create(ctx, alloc);
    if (d) d->dfeed_stoppable = NULL;
    return d;
}

/* Serve `gz` as a gzip Content-Length response and stream it through the client, pausing on every
 * fragment and resuming on a later tick. */
static void fetch_with(const char *gz, size_t gz_len, Sink *s, Count *cnt, KlDecompressFactory fac) {
    static char resp[8 * 1024 * 1024];
    int hl = snprintf(resp, sizeof resp,
                      "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: %zu\r\n\r\n", gz_len);
    memcpy(resp + hl, gz, gz_len);
    Peer p;
    if (peer_start(&p, resp, (size_t)hl + gz_len) != 0) return;
    static KlAllocator ca;
    ca = (KlAllocator){ .malloc = c_malloc, .realloc = c_realloc, .free = c_free, .ctx = cnt };
    KlAllocator da = kl_allocator_default();
    KlCompressCtx *zctx = kl_compress_miniz_ctx_create(6, &da);
    KlDecompressConfig dc = { .ctx = zctx, .factory = fac, .ctx_destroy = NULL };
    KlHttpClientConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.timeout_ms = 60000;
    cfg.decompress = &dc;
    cfg.max_response_size = 64u * 1024 * 1024;
    KlEventCtx ev;
    kl_event_ctx_init(&ev, &da);
    KlHttpClientStreamCfg sc;
    memset(&sc, 0, sizeof sc);
    sc.on_body = on_body;
    sc.on_complete = on_complete;
    sc.user_data = s;
    char url[64];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/", p.port);
    s->client = kl_http_client_start_s(&ev, &ca, &cfg, "GET", url, NULL, 0, NULL, 0, &sc, on_done, s);
    for (int i = 0; s->client && i < 2000000 && !s->done; i++) {
        (void)kl_event_ctx_run(&ev, 16, 2);
        if (s->paused && !s->done) {
            s->paused = 0;
            if (kl_http_client_resume(s->client) != 0) { s->bad = 1; break; }
        }
    }
    kl_http_client_free(s->client);
    kl_event_ctx_free(&ev);
    kl_compress_miniz_ctx_destroy(zctx);
    peer_join(&p);
}

static void fetch(const char *gz, size_t gz_len, Sink *s, Count *cnt) {
    fetch_with(gz, gz_len, s, cnt, kl_decompress_miniz_create);
}

UTEST(gzip_pause, text_body_every_fragment) {
    KlAllocator al = kl_allocator_default();
    size_t n = 300000;
    char *body = malloc(n);
    for (size_t i = 0; i < n; i++) body[i] = (char)("keel streams bytes "[i % 19] ^ (char)((i / 977) & 7));
    char *gz; size_t gzl;
    ASSERT_EQ(gzip_body(body, n, &gz, &gzl, &al), 0);
    Sink s; memset(&s, 0, sizeof s);
    Count cnt; memset(&cnt, 0, sizeof cnt);
    fetch(gz, gzl, &s, &cnt);
    ASSERT_EQ(s.done, 1);
    ASSERT_EQ((int)s.err, (int)KL_ERR_NONE);
    ASSERT_EQ(s.len, n);
    ASSERT_EQ(memcmp(s.got, body, n), 0);
    ASSERT_EQ(s.completes, 1);
    ASSERT_EQ(s.bad, 0);
    ASSERT_EQ(cnt.cur, (size_t)0);
    kl_free(&al, gz, gzl);
    free(body); free(s.got);
}

UTEST(gzip_pause, max_ratio_body_bounded_hold_back) {
    KlAllocator al = kl_allocator_default();
    size_t n = (size_t)24 * 1024 * 1024;   /* zeros: ~1000:1, so ~24 KiB on the wire */
    char *body = calloc(1, n);
    char *gz; size_t gzl;
    ASSERT_EQ(gzip_body(body, n, &gz, &gzl, &al), 0);
    Sink s; memset(&s, 0, sizeof s);
    Count cnt; memset(&cnt, 0, sizeof cnt);
    fetch(gz, gzl, &s, &cnt);
    ASSERT_EQ(s.done, 1);
    ASSERT_EQ((int)s.err, (int)KL_ERR_NONE);
    ASSERT_EQ(s.len, n);
    ASSERT_EQ(memcmp(s.got, body, n), 0);
    ASSERT_EQ(s.bad, 0);
    /* The decoder session (dictionary 32 KiB + state ~11 KiB), the 8 KiB receive buffer and the
     * client itself: nothing scales with the ratio, since the stopped decoder keeps its output. */
    ASSERT_LT(cnt.peak, (size_t)96 * 1024);
    ASSERT_EQ(cnt.cur, (size_t)0);
    kl_free(&al, gz, gzl);
    free(body); free(s.got);
}

UTEST(gzip_pause, max_ratio_body_legacy_fallback_bounded) {
    KlAllocator al = kl_allocator_default();
    size_t n = (size_t)24 * 1024 * 1024;
    char *body = calloc(1, n);
    char *gz; size_t gzl;
    ASSERT_EQ(gzip_body(body, n, &gz, &gzl, &al), 0);
    Sink s; memset(&s, 0, sizeof s);
    Count cnt; memset(&cnt, 0, sizeof cnt);
    fetch_with(gz, gzl, &s, &cnt, legacy_miniz_create);
    ASSERT_EQ(s.done, 1);
    ASSERT_EQ((int)s.err, (int)KL_ERR_NONE);
    ASSERT_EQ(s.len, n);
    ASSERT_EQ(memcmp(s.got, body, n), 0);
    ASSERT_EQ(s.bad, 0);
    /* The session, the receive buffer and at most one 512-byte slice's output held back (deflate
     * tops out near 1032:1, so about 516 KiB): under the fallback's cap, well under 1 MiB. */
    ASSERT_LT(cnt.peak, (size_t)1024 * 1024);
    ASSERT_GT(cnt.peak, (size_t)256 * 1024);   /* it did hold output back: the path under test */
    ASSERT_EQ(cnt.cur, (size_t)0);
    kl_free(&al, gz, gzl);
    free(body); free(s.got);
}

UTEST(gzip_pause, corrupt_crc_fails_after_paused_delivery) {
    KlAllocator al = kl_allocator_default();
    const char *text = "hello paused world, hello paused world, hello paused world";
    char *gz; size_t gzl;
    ASSERT_EQ(gzip_body(text, strlen(text), &gz, &gzl, &al), 0);
    gz[gzl - 8] ^= 0x5a;   /* CRC32 in the trailer */
    Sink s; memset(&s, 0, sizeof s);
    Count cnt; memset(&cnt, 0, sizeof cnt);
    fetch(gz, gzl, &s, &cnt);
    ASSERT_EQ(s.done, 1);
    /* miniz checks the trailer as soon as its 8 bytes arrive, inside the body phase, and the client
     * has always reported a decoder failure there as a parse failure (a failure only at the final
     * flush is KL_ERR_COMPRESS). Pausing changes neither. */
    ASSERT_EQ((int)s.err, (int)KL_ERR_PARSE);
    ASSERT_EQ(s.len, strlen(text));   /* everything decoded was delivered before the check failed */
    ASSERT_EQ(memcmp(s.got, text, s.len), 0);
    ASSERT_EQ(s.bad, 0);
    kl_free(&al, gz, gzl);
    free(s.got);
}

UTEST_MAIN()
