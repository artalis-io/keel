/*
 * libFuzzer harness for the multipart body reader.
 *
 * Drives the whole reader: on_data in variable fragments, kl_http_multipart_next pulled 0..N times
 * between fragments (so the parser runs on partial input, consumes it and compacts its buffer), then
 * on_complete (or on_error) and the final pulls, then destroy. Body slices and part metadata are read
 * in full, so ASan sees any slice that outlives its bytes.
 *
 * Input layout:
 *   - starting with '-' (the seed corpus: plain bodies): the whole input is the body, with fixed caps
 *     and boundary FUZZBOUNDARY, fragment sizes taken from the body, pulls until NEED_DATA after
 *     every fragment;
 *   - otherwise: 6 control bytes, then the body. The control bytes pick the boundary, the fragment
 *     pattern, the pull counts (0..4 per fragment), small caps (max_headers_size, max_input_buffer,
 *     max_part_size, max_total_size, max_parts), on_complete or on_error at the end, and an
 *     allocation failure to inject.
 *
 * Build:
 *   make fuzz  (requires clang with -fsanitize=fuzzer support)
 *
 * Run:
 *   ./fuzz/fuzz_multipart fuzz/corpus_multipart/ -max_total_time=60
 */
#include <keel/allocator.h>
#include <keel/http_body_reader.h>
#include <keel/http_body_reader_multipart.h>
#include <keel/http_request.h>
#include <stdint.h>
#include <string.h>

/* The default allocator, failing the fail_at'th allocation (or reallocation) when fail_at >= 0. */
typedef struct {
    KlAllocator inner;
    long        fail_at;
    long        count;
} FailAlloc;

static void *fa_malloc(void *ctx, size_t n) {
    FailAlloc *f = ctx;
    if (f->fail_at >= 0 && f->count++ == f->fail_at) return NULL;
    return f->inner.malloc(f->inner.ctx, n);
}
static void *fa_realloc(void *ctx, void *p, size_t o, size_t n) {
    FailAlloc *f = ctx;
    if (f->fail_at >= 0 && f->count++ == f->fail_at) return NULL;
    return f->inner.realloc(f->inner.ctx, p, o, n);
}
static void fa_free(void *ctx, void *p, size_t n) {
    FailAlloc *f = ctx;
    f->inner.free(f->inner.ctx, p, n);
}

/* Pull up to `pulls` events; returns 1 once the reader is DONE or in ERROR. */
static int pull(KlHttpBodyReader *br, long pulls, volatile unsigned *sink) {
    for (long i = 0; i < pulls; i++) {
        KlHttpMultipartPartMeta m;
        const char *d = NULL;
        size_t dl = 0;
        KlHttpMultipartEvent e = kl_http_multipart_next(br, &m, &d, &dl);
        switch (e) {
        case KL_HTTP_MP_EVT_PART_BEGIN:
            for (size_t j = 0; j < m.name_len; j++) *sink += (unsigned char)m.name[j];
            for (size_t j = 0; m.filename && j < m.filename_len; j++) *sink += (unsigned char)m.filename[j];
            for (size_t j = 0; m.content_type && j < m.content_type_len; j++)
                *sink += (unsigned char)m.content_type[j];
            break;
        case KL_HTTP_MP_EVT_PART_DATA:
            for (size_t j = 0; j < dl; j++) *sink += (unsigned char)d[j];
            break;
        case KL_HTTP_MP_EVT_PART_END:
            break;
        case KL_HTTP_MP_EVT_NEED_DATA:
            return 0;
        case KL_HTTP_MP_EVT_DONE:
        case KL_HTTP_MP_EVT_ERROR:
            (void)kl_http_multipart_last_error(br);
            return 1;
        }
    }
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    volatile unsigned sink = 0;
    FailAlloc fa = { kl_allocator_default(), -1, 0 };
    KlAllocator alloc = { fa_malloc, fa_realloc, fa_free, &fa };

    KlHttpMultipartConfig cfg = {
        .max_part_size = 64 * 1024,
        .max_total_size = 256 * 1024,
        .max_parts = 16,
    };
    const char *ct = "multipart/form-data; boundary=FUZZBOUNDARY";
    unsigned frag_mode = 2;     /* 0: 1 byte, 1: 1..4, 2: 1..64 */
    unsigned pull_seed = 0;
    unsigned pull_mod = 0;      /* 0: pull until NEED_DATA; else (round * 7 + seed) % mod pulls */
    int      on_error_end = 0;

    if (size > 0 && data[0] != '-') {
        if (size < 6) return 0;
        uint8_t c0 = data[0], c1 = data[1], c2 = data[2], c3 = data[3], c4 = data[4], c5 = data[5];
        data += 6;
        size -= 6;
        if (c0 & 1) ct = "multipart/form-data; boundary=B";
        frag_mode = (c0 >> 1) & 3;
        if (frag_mode > 2) frag_mode = 2;
        pull_seed = c0 >> 3;
        pull_mod = 5;           /* 0..4 pulls per fragment: some fragments are not pulled at all */
        cfg.max_headers_size = (c1 & 1) ? (size_t)1 + (c1 >> 1) : 0;
        cfg.max_input_buffer = (c2 & 1) ? (size_t)1 + (size_t)(c2 >> 1) * 4 : 0;
        cfg.max_part_size    = (c3 & 1) ? (size_t)(c3 >> 1) * 8 : 0;
        cfg.max_total_size   = (c3 & 2) ? (size_t)(c2 >> 1) * 64 + 1 : 0;
        cfg.max_parts        = (c4 & 1) ? (c4 >> 5) : 0;
        on_error_end         = (c4 & 2) != 0;
        fa.fail_at           = (c5 & 0x80) ? (long)(c5 & 0x1f) : -1;
    }

    KlHttpRequest req;
    memset(&req, 0, sizeof(req));
    req.method = "POST";
    req.method_len = 4;
    req.path = "/upload";
    req.path_len = 7;
    req.content_length = size;
    req.headers[0].name = "Content-Type";
    req.headers[0].name_len = 12;
    req.headers[0].value = ct;
    req.headers[0].value_len = strlen(ct);
    req.num_headers = 1;

    KlHttpBodyReader *br = kl_http_body_reader_multipart(&alloc, &req, &cfg);
    if (!br) return 0;

    size_t off = 0;
    unsigned round = 0;
    int ended = 0;
    while (off < size && !ended) {
        size_t chunk = frag_mode == 0 ? 1
                     : (size_t)((data[off] ^ (uint8_t)round) % (frag_mode == 1 ? 4 : 64)) + 1;
        if (chunk > size - off) chunk = size - off;
        if (br->on_data(br, (const char *)data + off, chunk) < 0) break;
        off += chunk;
        long pulls = pull_mod == 0 ? 1L << 30 : (long)((round * 7u + pull_seed) % pull_mod);
        round++;
        ended = pull(br, pulls, &sink);
    }
    if (off >= size && !on_error_end) {
        br->on_complete(br);
        if (!ended) (void)pull(br, 1L << 30, &sink);
    } else {
        br->on_error(br);
        (void)pull(br, 4, &sink);
    }
    br->destroy(br);
    (void)sink;
    return 0;
}
