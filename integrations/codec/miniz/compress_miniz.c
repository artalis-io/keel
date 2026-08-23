#include "compress_miniz.h"
#include <string.h>

/* miniz public API — included via -I$(MINIZ_DIR) */
#include <miniz.h>

/* miniz defines 'compress' as a macro → conflicts with vtable field name */
#undef compress

/* ── Internal context struct ─────────────────────────────────────── */

typedef struct {
    int          level;  /* compression level 1-9 */
    KlAllocator *alloc;  /* allocator used to create this context */
} KlMinizCtx;

/* ── Internal session struct ─────────────────────────────────────── */

typedef struct {
    KlCompress       base;      /* vtable — must be first */
    int              level;
    KlAllocator     *alloc;
    tdefl_compressor comp;      /* ~300KB, embedded in this allocation */
    uint32_t         crc;       /* running CRC32 for gzip trailer */
    uint32_t         total_in;  /* total input bytes for ISIZE trailer */
    int              started;   /* gzip header emitted? */
} KlMinizSession;

/* ── Gzip constants ──────────────────────────────────────────────── */

/* 10-byte gzip header: ID1=0x1f, ID2=0x8b, CM=8 (deflate),
 * FLG=0, MTIME=0, XFL=0, OS=0xff (unknown) */
static const unsigned char gzip_header[10] = {
    0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff
};

/* Map compression level to tdefl flags */
static int level_to_flags(int level) {
    /* tdefl_create_comp_flags_from_zip_params maps level to internal flags.
     * We pass window_bits=-1 (raw deflate) and strategy=0 (default). */
    int flags = tdefl_create_comp_flags_from_zip_params(level, -15, 0);
    return flags;
}

/* Write a 32-bit value in little-endian */
static void write_le32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
    p[2] = (unsigned char)((v >> 16) & 0xff);
    p[3] = (unsigned char)((v >> 24) & 0xff);
}

/* ── Vtable: compress (one-shot) ─────────────────────────────────── */

static int miniz_compress(KlCompress *self, const char *in, size_t in_len,
                           char **out, size_t *out_len, KlAllocator *alloc) {
    KlMinizSession *s = (KlMinizSession *)self;

    /* Output: 10 (header) + compress_bound + 8 (trailer) */
    size_t bound = (size_t)mz_compressBound((mz_ulong)in_len);
    if (bound > SIZE_MAX / 2) return -1;
    size_t buf_size = 10 + bound + 8;
    char *buf = kl_malloc(alloc, buf_size);
    if (!buf) return -1;

    /* Write gzip header */
    memcpy(buf, gzip_header, 10);

    /* Init compressor and compress in one shot */
    int flags = level_to_flags(s->level);
    if (tdefl_init(&s->comp, NULL, NULL, flags) != TDEFL_STATUS_OKAY) {
        kl_free(alloc, buf, buf_size);
        return -1;
    }

    size_t in_bytes = in_len;
    size_t out_bytes = bound;
    tdefl_status status = tdefl_compress(&s->comp,
                                          in, &in_bytes,
                                          buf + 10, &out_bytes,
                                          TDEFL_FINISH);
    if (status != TDEFL_STATUS_DONE) {
        kl_free(alloc, buf, buf_size);
        return -1;
    }

    /* CRC32 and ISIZE trailer */
    uint32_t crc = (uint32_t)mz_crc32(MZ_CRC32_INIT,
                                        (const unsigned char *)in, in_len);
    size_t total = 10 + out_bytes + 8;
    write_le32((unsigned char *)buf + 10 + out_bytes, crc);
    write_le32((unsigned char *)buf + 10 + out_bytes + 4, (uint32_t)in_len);

    *out = buf;
    *out_len = total;
    return 0;
}

/* ── Vtable: feed (streaming) ────────────────────────────────────── */

static int miniz_feed(KlCompress *self, const char *data, size_t len,
                       int flush,
                       int (*emit)(void *ctx, const char *data, size_t len),
                       void *emit_ctx) {
    KlMinizSession *s = (KlMinizSession *)self;

    /* First call: emit gzip header and init compressor */
    if (!s->started) {
        if (emit(emit_ctx, (const char *)gzip_header, 10) < 0)
            return -1;
        int flags = level_to_flags(s->level);
        if (tdefl_init(&s->comp, NULL, NULL, flags) != TDEFL_STATUS_OKAY)
            return -1;
        s->crc = (uint32_t)MZ_CRC32_INIT;
        s->total_in = 0;
        s->started = 1;
    }

    /* Update CRC and total count */
    if (data && len > 0) {
        s->crc = (uint32_t)mz_crc32(s->crc,
                                      (const unsigned char *)data, len);
        s->total_in += (uint32_t)len;
    }

    /* Compress data */
    tdefl_flush flush_type = flush ? TDEFL_FINISH : TDEFL_NO_FLUSH;
    const char *p = data;
    size_t remaining = len;

    do {
        unsigned char out_buf[4096];
        size_t in_bytes = remaining;
        size_t out_bytes = sizeof(out_buf);

        tdefl_status status = tdefl_compress(&s->comp,
                                              p, &in_bytes,
                                              out_buf, &out_bytes,
                                              flush_type);
        if (status < TDEFL_STATUS_OKAY) return -1;

        if (out_bytes > 0) {
            if (emit(emit_ctx, (const char *)out_buf, out_bytes) < 0)
                return -1;
        }

        if (p) p += in_bytes;
        remaining -= in_bytes;

        if (status == TDEFL_STATUS_DONE) break;
    } while (remaining > 0);

    /* Emit trailer on final flush */
    if (flush) {
        unsigned char trailer[8];
        write_le32(trailer, s->crc);
        write_le32(trailer + 4, s->total_in);
        if (emit(emit_ctx, (const char *)trailer, 8) < 0)
            return -1;
    }

    return 0;
}

/* ── Vtable: encoding ────────────────────────────────────────────── */

static const char *miniz_encoding(KlCompress *self) {
    (void)self;
    return "gzip";
}

/* ── Vtable: reset ───────────────────────────────────────────────── */

static void miniz_reset(KlCompress *self) {
    KlMinizSession *s = (KlMinizSession *)self;
    s->crc = 0;
    s->total_in = 0;
    s->started = 0;
}

/* ── Vtable: destroy ─────────────────────────────────────────────── */

static void miniz_destroy(KlCompress *self) {
    KlMinizSession *s = (KlMinizSession *)self;
    KlAllocator *alloc = s->alloc;
    kl_free(alloc, s, sizeof(*s));
}

/* ── Public API ──────────────────────────────────────────────────── */

KlCompressCtx *kl_compress_miniz_ctx_create(int level, KlAllocator *alloc) {
    if (!alloc) return NULL;
    if (level < 1) level = 1;
    if (level > 9) level = 9;
    KlMinizCtx *ctx = kl_malloc(alloc, sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->level = level;
    ctx->alloc = alloc;
    return (KlCompressCtx *)ctx;
}

void kl_compress_miniz_ctx_destroy(KlCompressCtx *ctx) {
    if (!ctx) return;
    KlMinizCtx *mctx = (KlMinizCtx *)ctx;
    kl_free(mctx->alloc, mctx, sizeof(*mctx));
}

/* cppcheck-suppress constParameterPointer ; signature must match KlCompressFactory typedef */
KlCompress *kl_compress_miniz_create(KlCompressCtx *ctx, KlAllocator *alloc) {
    if (!ctx || !alloc) return NULL;
    const KlMinizCtx *mctx = (const KlMinizCtx *)ctx;

    KlMinizSession *s = kl_malloc(alloc, sizeof(*s));
    if (!s) return NULL;
    memset(s, 0, sizeof(*s));

    s->base.compress = miniz_compress;
    s->base.feed = miniz_feed;
    s->base.encoding = miniz_encoding;
    s->base.reset = miniz_reset;
    s->base.destroy = miniz_destroy;
    s->level = mctx->level;
    s->alloc = alloc;

    return &s->base;
}
