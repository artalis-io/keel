/*
 * Bounded compressed streaming with the real miniz codec: KlCompress.flush_bound and
 * KL_COMPRESS_FLUSH_SYNC in compress_miniz.c, and kl_http_compress_stream_try_write/_try_end over a
 * socket. Each flushed write must decode, on arrival and on its own, to exactly the input so far
 * (prompt delivery, byte-exact); no feed may emit more than the bound asked before it; the bound
 * must cover output an earlier unflushed feed still owes; the finished stream must be a valid gzip
 * member (CRC32 + ISIZE checked by decompress_miniz). Opt-in (bring-your-own miniz): built via
 * `make decompress-miniz-test MINIZ_DIR=...`.
 */
#include "utest.h"
#include <keel/allocator.h>
#include <keel/compress.h>
#include <keel/decompress.h>
#include <keel/http_compress.h>
#include <keel/http_response.h>
#include "compress_miniz.h"
#include "decompress_miniz.h"
#include <miniz.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static unsigned g_rng = 2463534242u;
static unsigned rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }

/* Growable byte buffer. */
typedef struct { char *p; size_t len, cap; } Buf;
static int buf_put(Buf *b, const char *d, size_t n) {
    if (b->len + n > b->cap) {
        size_t nc = b->cap ? b->cap : 4096;
        while (nc < b->len + n) nc *= 2;
        char *np = realloc(b->p, nc);
        if (!np) return -1;
        b->p = np;
        b->cap = nc;
    }
    if (n) memcpy(b->p + b->len, d, n);
    b->len += n;
    return 0;
}
static int buf_emit(void *ctx, const char *d, size_t n) { return buf_put(ctx, d, n); }

/* A raw-deflate decoder fed the gzip member as it arrives (the 10-byte header skipped). */
typedef struct {
    mz_stream z;
    size_t skip;          /* header bytes still to skip */
    Buf out;
} Inflater;
static int infl_init(Inflater *f) {
    memset(f, 0, sizeof *f);
    f->skip = 10;
    return mz_inflateInit2(&f->z, -MZ_DEFAULT_WINDOW_BITS) == MZ_OK ? 0 : -1;
}
/* Decode everything in `d`; returns 0, or -1 on a decode error. */
static int infl_feed(Inflater *f, const char *d, size_t n) {
    size_t s = n < f->skip ? n : f->skip;
    f->skip -= s;
    d += s;
    n -= s;
    f->z.next_in = (const unsigned char *)d;
    f->z.avail_in = (unsigned)n;
    for (;;) {
        unsigned char tmp[8192];
        f->z.next_out = tmp;
        f->z.avail_out = sizeof tmp;
        int rc = mz_inflate(&f->z, MZ_SYNC_FLUSH);
        size_t got = sizeof tmp - f->z.avail_out;
        if (buf_put(&f->out, (const char *)tmp, got) < 0) return -1;
        if (rc == MZ_STREAM_END) return 0;
        if (rc != MZ_OK && rc != MZ_BUF_ERROR) return -1;
        if (f->z.avail_in == 0 && got < sizeof tmp) return 0;
    }
}
static void infl_free(Inflater *f) { mz_inflateEnd(&f->z); free(f->out.p); }

static KlCompress *new_codec(KlCompressCtx **ctx, KlAllocator *al, int level) {
    *ctx = kl_compress_miniz_ctx_create(level, al);
    return *ctx ? kl_compress_miniz_create(*ctx, al) : NULL;
}

/* Input shapes: SSE events, 1-4 KiB text, 16 KiB compressible blocks, random bytes. */
static size_t make_input(int shape, int i, char *b, size_t cap) {
    static const char *w[] = { "token", "stream", "delta", "content", "the", "model", "gateway" };
    switch (shape) {
    case 0:
        return (size_t)snprintf(b, cap, "data: {\"choices\":[{\"delta\":{\"content\":\" %s%d\"}}]}\n\n",
                                w[rnd() % 7], i);
    case 1: case 2: {
        size_t n = shape == 1 ? 1024 + rnd() % 3073 : 16384, l = 0;
        while (l < n) {
            const char *x = w[rnd() % 7];
            size_t k = strlen(x);
            if (l + k + 1 > n) break;
            memcpy(b + l, x, k);
            l += k;
            b[l++] = ' ';
        }
        while (l < n) b[l++] = '.';
        return n;
    }
    default: {
        size_t n = 1 + rnd() % 2048;
        for (size_t j = 0; j < n; j++) b[j] = (char)(rnd() & 0xff);
        return n;
    }
    }
}

/* Codec level: every sync feed stays within its bound and decodes at once to all input so far. */
UTEST(miniz_bounded, each_sync_flush_is_bounded_and_decodes_on_arrival) {
    KlAllocator al = kl_allocator_default();
    static char in[1 << 15];
    for (int level = 1; level <= 9; level += 4) {
        for (int shape = 0; shape < 4; shape++) {
            KlCompressCtx *ctx;
            KlCompress *c = new_codec(&ctx, &al, level);
            ASSERT_TRUE(c && c->flush_bound);
            Inflater f;
            ASSERT_EQ(infl_init(&f), 0);
            Buf all = { 0 };
            for (int i = 0; i < 120; i++) {
                size_t n = make_input(shape, i, in, sizeof in);
                size_t bound = c->flush_bound(c, n, 0);
                ASSERT_EQ(c->flush_bound(c, n, 0), bound);         /* non-mutating */
                Buf out = { 0 };
                ASSERT_EQ(c->feed(c, in, n, KL_COMPRESS_FLUSH_SYNC, buf_emit, &out), 0);
                ASSERT_LE(out.len, bound);
                ASSERT_EQ(buf_put(&all, in, n), 0);
                ASSERT_EQ(infl_feed(&f, out.p, out.len), 0);
                ASSERT_EQ(f.out.len, all.len);                      /* nothing held back */
                ASSERT_EQ(memcmp(f.out.p, all.p, all.len), 0);
                free(out.p);
            }
            Buf tail = { 0 };
            size_t fb = c->flush_bound(c, 0, 1);
            ASSERT_EQ(c->feed(c, NULL, 0, 1, buf_emit, &tail), 0);
            ASSERT_LE(tail.len, fb);
            free(tail.p);
            free(all.p);
            infl_free(&f);
            c->destroy(c);
            kl_compress_miniz_ctx_destroy(ctx);
        }
    }
}

/* The bound covers output an unflushed feed retained, and the header before the first feed. */
UTEST(miniz_bounded, the_bound_covers_retained_input_and_the_header) {
    KlAllocator al = kl_allocator_default();
    KlCompressCtx *ctx;
    KlCompress *c = new_codec(&ctx, &al, 6);
    ASSERT_TRUE(c != NULL);
    size_t first = c->flush_bound(c, 100, 0);
    static char r[200000];
    for (size_t j = 0; j < sizeof r; j++) r[j] = (char)(rnd() & 0xff);
    /* Unflushed random input: miniz retains up to a block of it, owed at the next flush. */
    Buf out = { 0 };
    ASSERT_EQ(c->feed(c, r, 30000, 0, buf_emit, &out), 0);
    size_t owed_bound = c->flush_bound(c, 1, 0);
    ASSERT_GT(owed_bound, (size_t)30000 - out.len);              /* it counts the retained input */
    out.len = 0;
    ASSERT_EQ(c->feed(c, r + 30000, 1, KL_COMPRESS_FLUSH_SYNC, buf_emit, &out), 0);
    ASSERT_LE(out.len, owed_bound);
    ASSERT_GT(out.len, (size_t)1000);                            /* a real backlog was flushed */
    /* Flushed: the bound for the same write is back to the small one (no header any more). */
    ASSERT_EQ(c->flush_bound(c, 100, 0), first - 10);
    ASSERT_EQ(c->flush_bound(c, SIZE_MAX, 0), SIZE_MAX);         /* overflow */
    ASSERT_EQ(c->flush_bound(c, SIZE_MAX / 4 + 1, 1), SIZE_MAX);
    ASSERT_LT(c->flush_bound(c, SIZE_MAX / 8, 1), SIZE_MAX);   /* large but representable */
    free(out.p);
    c->destroy(c);
    kl_compress_miniz_ctx_destroy(ctx);
}

/* Large incompressible writes: deflate's stored-block worst case stays within the bound. */
UTEST(miniz_bounded, incompressible_writes_stay_within_the_bound) {
    KlAllocator al = kl_allocator_default();
    KlCompressCtx *ctx;
    KlCompress *c = new_codec(&ctx, &al, 9);
    ASSERT_TRUE(c != NULL);
    static char r[300000];
    for (size_t j = 0; j < sizeof r; j++) r[j] = (char)(rnd() & 0xff);
    size_t sizes[] = { 1, 31744, 65535, 65536, 131072, 300000 };
    for (size_t k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
        size_t bound = c->flush_bound(c, sizes[k], 0);
        Buf out = { 0 };
        ASSERT_EQ(c->feed(c, r, sizes[k], KL_COMPRESS_FLUSH_SYNC, buf_emit, &out), 0);
        ASSERT_LE(out.len, bound);
        free(out.p);
    }
    c->destroy(c);
    kl_compress_miniz_ctx_destroy(ctx);
}

/* A block closes part way through a large write (about 31 KiB of literals, or a full code buffer)
 * with lookahead left: the flush must still complete. Every size across that window, decoded. */
UTEST(miniz_bounded, a_sync_flush_across_a_block_boundary_delivers_every_byte) {
    KlAllocator al = kl_allocator_default();
    static char r[34000];
    for (size_t j = 0; j < sizeof r; j++) r[j] = (char)(rnd() & 0xff);
    int levels[] = { 1, 6, 9 };
    for (int li = 0; li < 3; li++) {
        for (size_t n = 31000; n <= 33000; n += 7) {
            KlCompressCtx *ctx;
            KlCompress *c = new_codec(&ctx, &al, levels[li]);
            ASSERT_TRUE(c != NULL);
            size_t bound = c->flush_bound(c, n, 0);
            Buf out = { 0 };
            ASSERT_EQ(c->feed(c, r, n, KL_COMPRESS_FLUSH_SYNC, buf_emit, &out), 0);
            ASSERT_LE(out.len, bound);
            Inflater f;
            ASSERT_EQ(infl_init(&f), 0);
            ASSERT_EQ(infl_feed(&f, out.p, out.len), 0);
            ASSERT_EQ(f.out.len, n);                               /* nothing held back */
            ASSERT_EQ(memcmp(f.out.p, r, n), 0);
            Buf tail = { 0 };
            size_t fb = c->flush_bound(c, 0, 1);
            ASSERT_EQ(c->feed(c, NULL, 0, 1, buf_emit, &tail), 0);
            ASSERT_LE(tail.len, fb);
            free(tail.p);
            free(out.p);
            infl_free(&f);
            c->destroy(c);
            kl_compress_miniz_ctx_destroy(ctx);
        }
    }
}

/* Random sizes (to 140 KB), random content mixes and levels, with unflushed (legacy) feeds mixed
 * in: every sync feed and finish stays within the bound asked just before it, and the stream
 * decodes to exactly the input after every sync feed. */
UTEST(miniz_bounded, randomized_feeds_stay_within_the_bound_and_decode_exactly) {
    KlAllocator al = kl_allocator_default();
    static char in[140000];
    for (int round = 0; round < 60; round++) {
        KlCompressCtx *ctx;
        KlCompress *c = new_codec(&ctx, &al, 1 + (int)(rnd() % 9));
        ASSERT_TRUE(c != NULL);
        Inflater f;
        ASSERT_EQ(infl_init(&f), 0);
        Buf all = { 0 };
        for (int w = 0; w < 12; w++) {
            size_t n = (rnd() % 4 == 0) ? rnd() % sizeof in : rnd() % 4096;
            int kind = (int)(rnd() % 4);
            for (size_t j = 0; j < n; j++) {
                unsigned x = rnd();
                in[j] = kind == 0 ? (char)(x & 0xff)                      /* random */
                      : kind == 1 ? (char)('a' + x % 4)                    /* low entropy */
                      : kind == 2 ? (char)(j % 251)                        /* long matches */
                      : (char)((x & 7) ? 'q' : (x >> 8));                  /* mostly runs */
            }
            int legacy = rnd() % 5 == 0;
            size_t bound = c->flush_bound(c, n, 0);
            Buf out = { 0 };
            ASSERT_EQ(c->feed(c, in, n, legacy ? 0 : KL_COMPRESS_FLUSH_SYNC, buf_emit, &out), 0);
            if (!legacy) ASSERT_LE(out.len, bound);
            ASSERT_EQ(buf_put(&all, in, n), 0);
            ASSERT_EQ(infl_feed(&f, out.p, out.len), 0);
            if (!legacy) {
                ASSERT_EQ(f.out.len, all.len);
                ASSERT_EQ(memcmp(f.out.p, all.p, all.len), 0);
            }
            free(out.p);
        }
        Buf tail = { 0 };
        size_t fb = c->flush_bound(c, 0, 1);
        ASSERT_EQ(c->feed(c, NULL, 0, 1, buf_emit, &tail), 0);
        ASSERT_LE(tail.len, fb);
        ASSERT_EQ(infl_feed(&f, tail.p, tail.len > 8 ? tail.len - 8 : 0), 0);
        ASSERT_EQ(f.out.len, all.len);
        ASSERT_EQ(memcmp(f.out.p, all.p, all.len), 0);
        free(tail.p);
        free(all.p);
        infl_free(&f);
        c->destroy(c);
        kl_compress_miniz_ctx_destroy(ctx);
    }
}

/* ── Over HTTP: bounded writes over a socket, read back, de-chunked, decoded ─────────────────── */

/* Read whatever the socket holds (non-blocking) and de-chunk it into `body`. */
typedef struct { Buf raw; size_t pos; int done; } Dechunk;
static void pull(int fd, Dechunk *d) {
    char tmp[65536];
    for (;;) {
        ssize_t n = read(fd, tmp, sizeof tmp);
        if (n <= 0) break;
        (void)buf_put(&d->raw, tmp, (size_t)n);
    }
}
/* Next complete chunk's payload [*p, *p + *n); 0 when none is complete yet. */
static int next_chunk(Dechunk *d, const char **p, size_t *n) {
    const char *s = d->raw.p + d->pos, *e = d->raw.p + d->raw.len;
    const char *crlf = NULL;
    for (const char *q = s; q + 1 < e; q++) if (q[0] == '\r' && q[1] == '\n') { crlf = q; break; }
    if (!crlf) return 0;
    size_t len = (size_t)strtoul(s, NULL, 16);
    if ((size_t)(e - (crlf + 2)) < len + 2) return 0;
    *p = crlf + 2;
    *n = len;
    d->pos = (size_t)(crlf + 2 + len + 2 - d->raw.p);
    if (len == 0) d->done = 1;
    return 1;
}

UTEST(miniz_bounded, http_writes_arrive_decodable_and_the_stream_is_a_valid_gzip_member) {
    KlAllocator al = kl_allocator_default();
    int sv[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    ASSERT_EQ(fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL) | O_NONBLOCK), 0);
    ASSERT_EQ(fcntl(sv[1], F_SETFL, fcntl(sv[1], F_GETFL) | O_NONBLOCK), 0);
    KlHttpResponse res;
    ASSERT_EQ(kl_http_response_init(&res, &al), 0);
    res.conn_fd = sv[1];
    KlCompressCtx *ctx = kl_compress_miniz_ctx_create(6, &al);
    KlCompressConfig cfg = { .ctx = ctx, .factory = kl_compress_miniz_create };
    KlHttpCompressStream cs;
    ASSERT_EQ(kl_http_compress_stream_begin(&res, &cfg, 200, &cs), 0);
    ASSERT_TRUE(kl_http_compress_stream_bounded(&cs));
    Dechunk d = { 0 };
    pull(sv[0], &d);
    char *hend = strstr(d.raw.p, "\r\n\r\n");
    ASSERT_TRUE(hend != NULL);
    d.pos = (size_t)(hend + 4 - d.raw.p);
    Inflater f;
    ASSERT_EQ(infl_init(&f), 0);
    Buf all = { 0 }, gz = { 0 };
    static char in[1 << 15];
    for (int i = 0; i < 400; i++) {
        size_t n = make_input(i % 4, i, in, sizeof in);
        KlHttpStreamStatus st = kl_http_compress_stream_try_write(&cs, in, n);
        ASSERT_EQ((int)st, (int)KL_HTTP_STREAM_ACCEPTED);
        ASSERT_EQ(buf_put(&all, in, n), 0);
        pull(sv[0], &d);
        const char *p;
        size_t cn;
        int chunks = 0;
        while (next_chunk(&d, &p, &cn)) {
            ASSERT_EQ(infl_feed(&f, p, cn), 0);
            ASSERT_EQ(buf_put(&gz, p, cn), 0);
            chunks++;
        }
        ASSERT_EQ(chunks, 1);                                 /* one chunk per accepted write */
        ASSERT_EQ(f.out.len, all.len);                       /* decodable the moment it arrives */
        ASSERT_EQ(memcmp(f.out.p, all.p, all.len), 0);
    }
    ASSERT_EQ((int)kl_http_compress_stream_try_end(&cs), (int)KL_HTTP_STREAM_ACCEPTED);
    pull(sv[0], &d);
    const char *p;
    size_t cn;
    while (next_chunk(&d, &p, &cn)) ASSERT_EQ(buf_put(&gz, p, cn), 0);
    ASSERT_TRUE(d.done);                                     /* the terminal chunk arrived */
    /* The whole member through the stock decompressor: CRC32 and ISIZE verified. */
    KlDecompress *dz = kl_decompress_miniz_create(ctx, &al);
    ASSERT_TRUE(dz != NULL);
    char *plain = NULL;
    size_t plain_len = 0;
    ASSERT_EQ(dz->decompress(dz, gz.p, gz.len, &plain, &plain_len, &al), 0);
    ASSERT_EQ(plain_len, all.len);
    ASSERT_EQ(memcmp(plain, all.p, all.len), 0);
    kl_free(&al, plain, plain_len);
    dz->destroy(dz);
    kl_compress_miniz_ctx_destroy(ctx);
    free(all.p);
    free(gz.p);
    free(d.raw.p);
    infl_free(&f);
    kl_http_response_free(&res);
    close(sv[0]);
    close(sv[1]);
}

UTEST_MAIN()
