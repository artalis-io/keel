#include "decompress_miniz.h"
#include <stdlib.h>
#include <string.h>

/* miniz public API — included via -I$(MINIZ_DIR) */
#include <miniz.h>

/* Hard ceiling on decompressed output — anti-decompression-bomb bound.
 * The gzip trailer ISIZE is untrusted (and only 32 bits, so it wraps), so
 * it cannot be relied on to bound output.  Both the one-shot growth loop
 * and the streaming path enforce this cap independently.  Override at build
 * time with -DKL_DECOMP_MAX_OUTPUT=<bytes>. */
#ifndef KL_DECOMP_MAX_OUTPUT
#define KL_DECOMP_MAX_OUTPUT ((size_t)256 * 1024 * 1024)   /* 256 MB */
#endif

/* ── Internal session struct ─────────────────────────────────────── */

typedef struct {
    KlDecompress        base;       /* vtable — must be first */
    KlAllocator        *alloc;
    tinfl_decompressor   decomp;    /* ~11KB */
    uint32_t            crc;
    uint32_t            total_out;
    uint64_t            out_full;   /* non-wrapping cumulative output (bomb cap) */
    int                 started;    /* gzip header parsed? */
    int                 done;       /* decompression finished? */
    unsigned char       hdr_buf[10];
    size_t              hdr_len;
    /* Trailer accumulation for streaming */
    unsigned char       trail_buf[8];
    size_t              trail_len;
} KlMinizDecompSession;

/* ── Gzip header helpers ─────────────────────────────────────────── */

/* Read a 32-bit value in little-endian */
static uint32_t read_le32(const unsigned char *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* gzip FLG bits */
#define FHCRC    0x02
#define FEXTRA   0x04
#define FNAME    0x08
#define FCOMMENT 0x10

/**
 * Parse and skip gzip header. Returns offset past header, or 0 on error.
 * Expects at least 10 bytes. May need more if optional fields present.
 */
static size_t skip_gzip_header(const unsigned char *data, size_t len) {
    if (len < 10)
        return 0;

    /* Validate: ID1=0x1f, ID2=0x8b, CM=8 (deflate) */
    if (data[0] != 0x1f || data[1] != 0x8b || data[2] != 0x08)
        return 0;

    unsigned char flg = data[3];
    size_t off = 10;

    /* FEXTRA: 2-byte length + data */
    if (flg & FEXTRA) {
        if (off + 2 > len) return 0;
        size_t xlen = (size_t)data[off] | ((size_t)data[off + 1] << 8);
        off += 2 + xlen;
        if (off > len) return 0;
    }

    /* FNAME: null-terminated string */
    if (flg & FNAME) {
        while (off < len && data[off] != 0) off++;
        if (off >= len) return 0;
        off++;  /* skip null terminator */
    }

    /* FCOMMENT: null-terminated string */
    if (flg & FCOMMENT) {
        while (off < len && data[off] != 0) off++;
        if (off >= len) return 0;
        off++;
    }

    /* FHCRC: 2-byte CRC16 */
    if (flg & FHCRC) {
        off += 2;
        if (off > len) return 0;
    }

    return off;
}

/* ── Vtable: decompress (one-shot) ───────────────────────────────── */

static int miniz_decompress_fn(KlDecompress *self,
                                const char *in, size_t in_len,
                                char **out, size_t *out_len,
                                KlAllocator *alloc) {
    (void)self;

    if (in_len < 18)  /* 10 header + 8 trailer minimum */
        return -1;

    /* Parse gzip header */
    size_t hdr_end = skip_gzip_header((const unsigned char *)in, in_len);
    if (hdr_end == 0)
        return -1;

    /* Extract trailer (last 8 bytes) */
    if (in_len < hdr_end + 8)
        return -1;
    const unsigned char *trailer = (const unsigned char *)in + in_len - 8;
    uint32_t expected_crc = read_le32(trailer);
    uint32_t expected_isize = read_le32(trailer + 4);

    /* Reject up front if the (untrusted) declared output size already
     * exceeds the anti-bomb ceiling.  The growth loop below enforces the
     * same cap against the actual output, so a forged-small ISIZE cannot
     * bypass it. */
    if (expected_isize > KL_DECOMP_MAX_OUTPUT)
        return -1;

    /* Compressed data region */
    const unsigned char *comp_data = (const unsigned char *)in + hdr_end;
    size_t comp_len = in_len - hdr_end - 8;

    /* Allocate output buffer using ISIZE hint (mod 2^32).
     * Cap initial alloc to 16 MB — untrusted ISIZE could be huge.
     * Growth loop below handles larger outputs. */
    #define KL_DECOMP_INIT_MAX (16 * 1024 * 1024)
    size_t buf_size = expected_isize;
    if (buf_size == 0)
        buf_size = comp_len * 4;  /* guess */
    if (buf_size < 64)
        buf_size = 64;
    if (buf_size > KL_DECOMP_INIT_MAX)
        buf_size = KL_DECOMP_INIT_MAX;
    if (buf_size > SIZE_MAX / 2)
        return -1;

    char *buf = kl_malloc(alloc, buf_size);
    if (!buf)
        return -1;

    /* Decompress */
    tinfl_decompressor decomp;
    tinfl_init(&decomp);

    size_t in_pos = 0;
    size_t out_pos = 0;

    for (;;) {
        size_t in_bytes = comp_len - in_pos;
        size_t out_bytes = buf_size - out_pos;

        int flags = TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF;
        if (in_pos + in_bytes < comp_len)
            flags |= TINFL_FLAG_HAS_MORE_INPUT;

        tinfl_status status = tinfl_decompress(&decomp,
                                                comp_data + in_pos, &in_bytes,
                                                (unsigned char *)buf, (unsigned char *)buf + out_pos, &out_bytes,
                                                flags);

        in_pos += in_bytes;
        out_pos += out_bytes;

        if (status == TINFL_STATUS_DONE)
            break;

        if (status == TINFL_STATUS_HAS_MORE_OUTPUT) {
            /* Need bigger buffer */
            if (buf_size >= KL_DECOMP_MAX_OUTPUT) {
                /* Already at the anti-bomb ceiling and still more output. */
                kl_free(alloc, buf, buf_size);
                return -1;
            }
            if (buf_size > SIZE_MAX / 2) {
                kl_free(alloc, buf, buf_size);
                return -1;
            }
            size_t new_size = buf_size * 2;
            if (new_size > KL_DECOMP_MAX_OUTPUT)
                new_size = KL_DECOMP_MAX_OUTPUT;
            char *new_buf = kl_malloc(alloc, new_size);
            if (!new_buf) {
                kl_free(alloc, buf, buf_size);
                return -1;
            }
            memcpy(new_buf, buf, out_pos);
            kl_free(alloc, buf, buf_size);
            buf = new_buf;
            buf_size = new_size;
            continue;
        }

        if (status < TINFL_STATUS_DONE) {
            kl_free(alloc, buf, buf_size);
            return -1;
        }

        /* NEEDS_MORE_INPUT with data remaining — continue */
        if (in_pos >= comp_len && status != TINFL_STATUS_DONE) {
            kl_free(alloc, buf, buf_size);
            return -1;
        }
    }

    /* Verify CRC32 */
    uint32_t actual_crc = (uint32_t)mz_crc32(MZ_CRC32_INIT,
                                               (const unsigned char *)buf,
                                               out_pos);
    if (actual_crc != expected_crc) {
        kl_free(alloc, buf, buf_size);
        return -1;
    }

    /* Verify ISIZE (lower 32 bits of original size) */
    if ((uint32_t)out_pos != expected_isize) {
        kl_free(alloc, buf, buf_size);
        return -1;
    }

    /* Trim buffer if significantly oversized */
    if (out_pos > 0 && out_pos < buf_size) {
        char *trimmed = kl_malloc(alloc, out_pos);
        if (trimmed) {
            memcpy(trimmed, buf, out_pos);
            kl_free(alloc, buf, buf_size);
            buf = trimmed;
        }
        /* else: keep oversized buffer — not an error */
    }

    *out = buf;
    *out_len = out_pos;
    return 0;
}

/* ── Vtable: dfeed (streaming) ───────────────────────────────────── */

static int miniz_dfeed_fn(KlDecompress *self, const char *data, size_t len,
                           int flush,
                           int (*emit)(void *ctx, const char *data, size_t len),
                           void *emit_ctx) {
    KlMinizDecompSession *s = (KlMinizDecompSession *)self;

    if (s->done && !flush)
        return 0;

    const unsigned char *p = (const unsigned char *)data;
    size_t remaining = len;

    /* Parse gzip header on first call */
    if (!s->started) {
        /* Accumulate header bytes */
        while (s->hdr_len < 10 && remaining > 0) {
            s->hdr_buf[s->hdr_len++] = *p++;
            remaining--;
        }
        if (s->hdr_len < 10)
            return 0;  /* need more data */

        /* Validate header */
        if (s->hdr_buf[0] != 0x1f || s->hdr_buf[1] != 0x8b ||
            s->hdr_buf[2] != 0x08)
            return -1;

        unsigned char flg = s->hdr_buf[3];

        /* Skip optional fields from the remaining input */
        if (flg & FEXTRA) {
            if (remaining < 2) return -1;
            size_t xlen = (size_t)p[0] | ((size_t)p[1] << 8);
            p += 2;
            remaining -= 2;
            if (remaining < xlen) return -1;
            p += xlen;
            remaining -= xlen;
        }
        if (flg & FNAME) {
            while (remaining > 0 && *p != 0) { p++; remaining--; }
            if (remaining == 0) return -1;
            p++; remaining--;
        }
        if (flg & FCOMMENT) {
            while (remaining > 0 && *p != 0) { p++; remaining--; }
            if (remaining == 0) return -1;
            p++; remaining--;
        }
        if (flg & FHCRC) {
            if (remaining < 2) return -1;
            p += 2;
            remaining -= 2;
        }

        tinfl_init(&s->decomp);
        s->crc = (uint32_t)MZ_CRC32_INIT;
        s->total_out = 0;
        s->out_full = 0;
        s->started = 1;
    }

    /* Decompress data */
    unsigned char out_buf[4096];

    while (remaining > 0 && !s->done) {
        size_t in_bytes = remaining;
        size_t out_bytes = sizeof(out_buf);

        int flags = TINFL_FLAG_HAS_MORE_INPUT;

        tinfl_status status = tinfl_decompress(&s->decomp,
                                                p, &in_bytes,
                                                out_buf, out_buf, &out_bytes,
                                                flags);

        p += in_bytes;
        remaining -= in_bytes;

        if (out_bytes > 0) {
            /* Enforce the anti-bomb ceiling before emitting.  s->total_out
             * is only 32 bits and wraps at 4 GB, so it cannot bound output;
             * s->out_full is a non-wrapping cumulative counter. */
            s->out_full += out_bytes;
            if (s->out_full > KL_DECOMP_MAX_OUTPUT)
                return -1;
            s->crc = (uint32_t)mz_crc32(s->crc, out_buf, out_bytes);
            s->total_out += (uint32_t)out_bytes;
            if (emit(emit_ctx, (const char *)out_buf, out_bytes) < 0)
                return -1;
        }

        if (status == TINFL_STATUS_DONE) {
            s->done = 1;
            /* Remaining bytes should be the 8-byte trailer */
            if (remaining <= 8) {
                memcpy(s->trail_buf + s->trail_len, p, remaining);
                s->trail_len += remaining;
                remaining = 0;
            }
            break;
        }

        if (status < TINFL_STATUS_DONE)
            return -1;
    }

    /* Accumulate trailer bytes from remaining data */
    if (s->done && remaining > 0 && s->trail_len < 8) {
        size_t need = 8 - s->trail_len;
        size_t take = remaining < need ? remaining : need;
        memcpy(s->trail_buf + s->trail_len, p, take);
        s->trail_len += take;
    }

    /* On flush, verify trailer */
    if (flush && s->trail_len >= 8) {
        uint32_t expected_crc = read_le32(s->trail_buf);
        uint32_t expected_isize = read_le32(s->trail_buf + 4);
        if (s->crc != expected_crc || s->total_out != expected_isize)
            return -1;
    }

    return 0;
}

/* ── Vtable: encoding ────────────────────────────────────────────── */

static const char *miniz_decomp_encoding(KlDecompress *self) {
    (void)self;
    return "gzip";
}

/* ── Vtable: reset ───────────────────────────────────────────────── */

static void miniz_decomp_reset(KlDecompress *self) {
    KlMinizDecompSession *s = (KlMinizDecompSession *)self;
    s->crc = 0;
    s->total_out = 0;
    s->out_full = 0;
    s->started = 0;
    s->done = 0;
    s->hdr_len = 0;
    s->trail_len = 0;
}

/* ── Vtable: destroy ─────────────────────────────────────────────── */

static void miniz_decomp_destroy(KlDecompress *self) {
    KlMinizDecompSession *s = (KlMinizDecompSession *)self;
    KlAllocator *alloc = s->alloc;
    kl_free(alloc, s, sizeof(*s));
}

/* ── Public API ──────────────────────────────────────────────────── */

KlDecompress *kl_decompress_miniz_create(KlCompressCtx *ctx,
                                          KlAllocator *alloc) {
    if (!ctx || !alloc) return NULL;
    (void)ctx;  /* context holds compression level — not used for decompression */

    KlMinizDecompSession *s = kl_malloc(alloc, sizeof(*s));
    if (!s) return NULL;
    memset(s, 0, sizeof(*s));

    s->base.decompress = miniz_decompress_fn;
    s->base.dfeed = miniz_dfeed_fn;
    s->base.encoding = miniz_decomp_encoding;
    s->base.reset = miniz_decomp_reset;
    s->base.destroy = miniz_decomp_destroy;
    s->alloc = alloc;

    return &s->base;
}
