#include "utest.h"
#include <keel/keel.h>
#include <keel/body_reader_multipart.h>
#include <stdio.h>   /* fprintf */
#include <stdlib.h>  /* abort */
#include <string.h>

/*
 * Streaming multipart parser tests.
 *
 * The parser is pull-shaped:
 *   on_data appends bytes to an internal buffer;
 *   kl_multipart_next returns one event per call.
 *
 * Most tests feed the body in a single on_data call and drive
 * kl_multipart_next to completion, asserting on the event sequence
 * and per-part metadata/bytes. A few tests slice the body across
 * multiple on_data calls to exercise cross-chunk delimiter handling
 * and NEED_DATA back-pressure.
 *
 * All test-side allocations go through the keel allocator API so that
 * sanitizer accounting matches what the production code uses. Naked
 * malloc/realloc/free are intentionally absent from this file.
 */

/* ── Fixtures ──────────────────────────────────────────────────────── */

static KlHttpRequest make_mp_request(const char *content_type) {
    KlHttpRequest req = {0};
    req.method = "POST";
    req.method_len = 4;
    req.path = "/upload";
    req.path_len = 7;
    req.headers[0].name = "Content-Type";
    req.headers[0].name_len = 12;
    req.headers[0].value = content_type;
    req.headers[0].value_len = strlen(content_type);
    req.num_headers = 1;
    return req;
}

/* In-memory collector for one part. body_cap is tracked because kl_free
 * needs the original allocation size; the NUL byte is part of the cap. */
typedef struct {
    char  *name;       size_t name_len;
    char  *filename;   size_t filename_len;
    char  *ctype;      size_t ctype_len;
    char  *body;       size_t body_len;
    size_t body_cap;
} CollectedPart;

typedef struct {
    KlAllocator   *alloc;
    CollectedPart *parts;
    int            count;
    int            cap;
    KlMultipartEvent last_event;
    KlMultipartErrorCode last_error;
} Collector;

/* Abort on alloc failure — test code can't recover meaningfully and
 * silent NULL returns hide bugs. */
static void mp_test_oom(void) {
    fprintf(stderr, "test fixture allocation failure\n");
    abort();
}

static void collector_init(Collector *c, KlAllocator *a) {
    memset(c, 0, sizeof(*c));
    c->alloc = a;
}

static char *xdup(KlAllocator *a, const char *s, size_t n) {
    if (!s) return NULL;
    char *d = kl_malloc(a, n + 1);
    if (!d) mp_test_oom();
    if (n > 0) memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

static void xfree(KlAllocator *a, char *p, size_t n) {
    if (p) kl_free(a, p, n + 1);  /* +1 for the NUL */
}

static void collector_add_part(Collector *c, const KlMultipartPartMeta *m) {
    if (c->count >= c->cap) {
        int    nc       = c->cap ? c->cap * 2 : 4;
        size_t old_sz   = (size_t)c->cap * sizeof(*c->parts);
        size_t new_sz   = (size_t)nc     * sizeof(*c->parts);
        CollectedPart *np = kl_realloc(c->alloc, c->parts, old_sz, new_sz);
        if (!np) mp_test_oom();
        c->parts = np;
        c->cap   = nc;
    }
    CollectedPart *p = &c->parts[c->count++];
    memset(p, 0, sizeof(*p));
    if (m) {
        p->name         = xdup(c->alloc, m->name, m->name_len);
        p->name_len     = m->name_len;
        p->filename     = xdup(c->alloc, m->filename, m->filename_len);
        p->filename_len = m->filename_len;
        p->ctype        = xdup(c->alloc, m->content_type, m->content_type_len);
        p->ctype_len    = m->content_type_len;
    }
}

static void collector_append_body(Collector *c, const char *data, size_t n) {
    if (c->count == 0) return;
    CollectedPart *p = &c->parts[c->count - 1];
    size_t need = p->body_len + n + 1;
    if (need > p->body_cap) {
        size_t nc = p->body_cap ? p->body_cap : need;
        while (nc < need) nc *= 2;
        char *nb = kl_realloc(c->alloc, p->body, p->body_cap, nc);
        if (!nb) mp_test_oom();
        p->body     = nb;
        p->body_cap = nc;
    }
    memcpy(p->body + p->body_len, data, n);
    p->body_len += n;
    p->body[p->body_len] = '\0';
}

static void collector_free(Collector *c) {
    for (int i = 0; i < c->count; i++) {
        CollectedPart *p = &c->parts[i];
        xfree(c->alloc, p->name,     p->name_len);
        xfree(c->alloc, p->filename, p->filename_len);
        xfree(c->alloc, p->ctype,    p->ctype_len);
        if (p->body)
            kl_free(c->alloc, p->body, p->body_cap);
    }
    if (c->parts)
        kl_free(c->alloc, c->parts, (size_t)c->cap * sizeof(*c->parts));
    memset(c, 0, sizeof(*c));
}

/*
 * Drive the iterator until DONE or ERROR. Assumes the entire body has
 * been fed via on_data before calling, so NEED_DATA implies a malformed
 * truncation (treated as error).
 */
static void collect_eager(KlBodyReader *br, Collector *c) {
    for (;;) {
        KlMultipartPartMeta meta;
        const char *data = NULL;
        size_t      data_len = 0;
        KlMultipartEvent e = kl_multipart_next(br, &meta, &data, &data_len);
        c->last_event = e;
        switch (e) {
        case KL_MP_EVT_PART_BEGIN:
            collector_add_part(c, &meta);
            break;
        case KL_MP_EVT_PART_DATA:
            collector_append_body(c, data, data_len);
            break;
        case KL_MP_EVT_PART_END:
            break;
        case KL_MP_EVT_DONE:
            return;
        case KL_MP_EVT_NEED_DATA:
            /* Treat unexpected NEED_DATA after a full feed as error. */
            c->last_error = kl_multipart_last_error(br);
            return;
        case KL_MP_EVT_ERROR:
            c->last_error = kl_multipart_last_error(br);
            return;
        }
    }
}

/* ── Factory-level rejection ──────────────────────────────────────── */

UTEST(mp, reject_non_multipart_content_type) {
    KlAllocator a = kl_allocator_default();
    KlHttpRequest req = make_mp_request("application/json");
    KlBodyReader *br = kl_body_reader_multipart(&a, &req, NULL);
    ASSERT_TRUE(br == NULL);
}

UTEST(mp, reject_missing_boundary) {
    KlAllocator a = kl_allocator_default();
    KlHttpRequest req = make_mp_request("multipart/form-data");
    KlBodyReader *br = kl_body_reader_multipart(&a, &req, NULL);
    ASSERT_TRUE(br == NULL);
}

UTEST(mp, reject_oversized_boundary) {
    KlAllocator a = kl_allocator_default();
    /* 71-byte boundary → over RFC 2046's 70 limit */
    KlHttpRequest req = make_mp_request(
        "multipart/form-data; boundary="
        "01234567890123456789012345678901234567890123456789"
        "012345678901234567890");
    KlBodyReader *br = kl_body_reader_multipart(&a, &req, NULL);
    ASSERT_TRUE(br == NULL);
}

UTEST(mp, accept_quoted_boundary) {
    KlAllocator a = kl_allocator_default();
    KlHttpRequest req = make_mp_request(
        "multipart/form-data; boundary=\"qb123\"");
    KlBodyReader *br = kl_body_reader_multipart(&a, &req, NULL);
    ASSERT_TRUE(br != NULL);

    const char *body =
        "--qb123\r\n"
        "Content-Disposition: form-data; name=\"x\"\r\n\r\n"
        "value\r\n"
        "--qb123--\r\n";
    ASSERT_EQ(br->on_data(br, body, strlen(body)), 0);
    br->on_complete(br);

    Collector c; collector_init(&c, &a);
    collect_eager(br, &c);
    ASSERT_EQ(c.last_event, KL_MP_EVT_DONE);
    ASSERT_EQ(c.count, 1);
    ASSERT_STREQ(c.parts[0].name, "x");
    ASSERT_STREQ(c.parts[0].body, "value");

    collector_free(&c);
    br->destroy(br);
}

/* ── Happy paths ──────────────────────────────────────────────────── */

UTEST(mp, single_field) {
    KlAllocator a = kl_allocator_default();
    KlHttpRequest req = make_mp_request(
        "multipart/form-data; boundary=abc123");

    KlBodyReader *br = kl_body_reader_multipart(&a, &req, NULL);
    ASSERT_TRUE(br != NULL);

    const char *body =
        "--abc123\r\n"
        "Content-Disposition: form-data; name=\"field1\"\r\n"
        "\r\n"
        "value1"
        "\r\n--abc123--\r\n";
    ASSERT_EQ(br->on_data(br, body, strlen(body)), 0);
    br->on_complete(br);

    Collector c; collector_init(&c, &a);
    collect_eager(br, &c);
    ASSERT_EQ(c.last_event, KL_MP_EVT_DONE);
    ASSERT_EQ(c.count, 1);
    ASSERT_STREQ(c.parts[0].name, "field1");
    ASSERT_EQ(c.parts[0].body_len, (size_t)6);
    ASSERT_STREQ(c.parts[0].body, "value1");
    ASSERT_TRUE(c.parts[0].filename == NULL);
    ASSERT_TRUE(c.parts[0].ctype == NULL);

    collector_free(&c);
    br->destroy(br);
}

UTEST(mp, two_fields) {
    KlAllocator a = kl_allocator_default();
    KlHttpRequest req = make_mp_request(
        "multipart/form-data; boundary=sep");
    KlBodyReader *br = kl_body_reader_multipart(&a, &req, NULL);

    const char *body =
        "--sep\r\n"
        "Content-Disposition: form-data; name=\"a\"\r\n\r\n"
        "alpha\r\n"
        "--sep\r\n"
        "Content-Disposition: form-data; name=\"b\"\r\n\r\n"
        "beta\r\n"
        "--sep--\r\n";
    ASSERT_EQ(br->on_data(br, body, strlen(body)), 0);
    br->on_complete(br);

    Collector c; collector_init(&c, &a);
    collect_eager(br, &c);
    ASSERT_EQ(c.last_event, KL_MP_EVT_DONE);
    ASSERT_EQ(c.count, 2);
    ASSERT_STREQ(c.parts[0].name, "a");
    ASSERT_STREQ(c.parts[0].body, "alpha");
    ASSERT_STREQ(c.parts[1].name, "b");
    ASSERT_STREQ(c.parts[1].body, "beta");

    collector_free(&c);
    br->destroy(br);
}

UTEST(mp, file_upload) {
    KlAllocator a = kl_allocator_default();
    KlHttpRequest req = make_mp_request(
        "multipart/form-data; boundary=fileBnd");
    KlBodyReader *br = kl_body_reader_multipart(&a, &req, NULL);

    const char *body =
        "--fileBnd\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"test.txt\"\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "file contents here"
        "\r\n--fileBnd--\r\n";
    ASSERT_EQ(br->on_data(br, body, strlen(body)), 0);
    br->on_complete(br);

    Collector c; collector_init(&c, &a);
    collect_eager(br, &c);
    ASSERT_EQ(c.last_event, KL_MP_EVT_DONE);
    ASSERT_EQ(c.count, 1);
    ASSERT_STREQ(c.parts[0].name, "file");
    ASSERT_STREQ(c.parts[0].filename, "test.txt");
    ASSERT_STREQ(c.parts[0].ctype, "text/plain");
    ASSERT_STREQ(c.parts[0].body, "file contents here");

    collector_free(&c);
    br->destroy(br);
}

UTEST(mp, binary_body) {
    KlAllocator a = kl_allocator_default();
    KlHttpRequest req = make_mp_request(
        "multipart/form-data; boundary=BIN");
    KlBodyReader *br = kl_body_reader_multipart(&a, &req, NULL);

    /* Build body manually so we can include NUL bytes. */
    const char *prefix =
        "--BIN\r\n"
        "Content-Disposition: form-data; name=\"blob\"; filename=\"f.bin\"\r\n"
        "Content-Type: application/octet-stream\r\n\r\n";
    const char binary[8] = { 0x00, 0xff, 0x10, 0x20, 0x00, 0x7f, 0x80, 0x01 };
    const char *suffix = "\r\n--BIN--\r\n";

    char buf[256];
    size_t off = 0;
    memcpy(buf + off, prefix, strlen(prefix)); off += strlen(prefix);
    memcpy(buf + off, binary, sizeof(binary));  off += sizeof(binary);
    memcpy(buf + off, suffix, strlen(suffix));  off += strlen(suffix);

    ASSERT_EQ(br->on_data(br, buf, off), 0);
    br->on_complete(br);

    Collector c; collector_init(&c, &a);
    collect_eager(br, &c);
    ASSERT_EQ(c.last_event, KL_MP_EVT_DONE);
    ASSERT_EQ(c.count, 1);
    ASSERT_EQ(c.parts[0].body_len, (size_t)8);
    ASSERT_EQ(memcmp(c.parts[0].body, binary, 8), 0);

    collector_free(&c);
    br->destroy(br);
}

UTEST(mp, three_fields_with_preamble) {
    /* Some clients prepend an explanatory preamble before the first
     * boundary. The parser must skip it. */
    KlAllocator a = kl_allocator_default();
    KlHttpRequest req = make_mp_request(
        "multipart/form-data; boundary=BND");
    KlBodyReader *br = kl_body_reader_multipart(&a, &req, NULL);

    const char *body =
        "This is the preamble, ignore me.\r\n"
        "--BND\r\n"
        "Content-Disposition: form-data; name=\"one\"\r\n\r\n1\r\n"
        "--BND\r\n"
        "Content-Disposition: form-data; name=\"two\"\r\n\r\n2\r\n"
        "--BND\r\n"
        "Content-Disposition: form-data; name=\"three\"\r\n\r\n3\r\n"
        "--BND--\r\n";
    ASSERT_EQ(br->on_data(br, body, strlen(body)), 0);
    br->on_complete(br);

    Collector c; collector_init(&c, &a);
    collect_eager(br, &c);
    ASSERT_EQ(c.last_event, KL_MP_EVT_DONE);
    ASSERT_EQ(c.count, 3);
    ASSERT_STREQ(c.parts[0].body, "1");
    ASSERT_STREQ(c.parts[1].body, "2");
    ASSERT_STREQ(c.parts[2].body, "3");

    collector_free(&c);
    br->destroy(br);
}

/* ── Cross-chunk delivery ─────────────────────────────────────────── */

UTEST(mp, body_byte_by_byte) {
    /* Feed every byte through a separate on_data call. */
    KlAllocator a = kl_allocator_default();
    KlHttpRequest req = make_mp_request(
        "multipart/form-data; boundary=BB");
    KlBodyReader *br = kl_body_reader_multipart(&a, &req, NULL);

    const char *body =
        "--BB\r\n"
        "Content-Disposition: form-data; name=\"x\"\r\n\r\n"
        "hello world"
        "\r\n--BB--\r\n";
    for (size_t i = 0; i < strlen(body); i++)
        ASSERT_EQ(br->on_data(br, body + i, 1), 0);
    br->on_complete(br);

    Collector c; collector_init(&c, &a);
    collect_eager(br, &c);
    ASSERT_EQ(c.last_event, KL_MP_EVT_DONE);
    ASSERT_EQ(c.count, 1);
    ASSERT_STREQ(c.parts[0].body, "hello world");

    collector_free(&c);
    br->destroy(br);
}

UTEST(mp, delimiter_split_across_chunks) {
    /* Carefully fragment so the boundary sequence is split between two
     * on_data calls — the parser must keep the trailing potential-
     * boundary prefix and not flush it as PART_DATA. */
    KlAllocator a = kl_allocator_default();
    KlHttpRequest req = make_mp_request(
        "multipart/form-data; boundary=XYZ");
    KlBodyReader *br = kl_body_reader_multipart(&a, &req, NULL);

    const char *part_a =
        "--XYZ\r\n"
        "Content-Disposition: form-data; name=\"q\"\r\n\r\n"
        "answer\r\n--XY";       /* first chunk ends mid-boundary */
    const char *part_b =
        "Z--\r\n";              /* terminator continues here */

    ASSERT_EQ(br->on_data(br, part_a, strlen(part_a)), 0);
    ASSERT_EQ(br->on_data(br, part_b, strlen(part_b)), 0);
    br->on_complete(br);

    Collector c; collector_init(&c, &a);
    collect_eager(br, &c);
    ASSERT_EQ(c.last_event, KL_MP_EVT_DONE);
    ASSERT_EQ(c.count, 1);
    ASSERT_STREQ(c.parts[0].body, "answer");

    collector_free(&c);
    br->destroy(br);
}

UTEST(mp, drain_then_need_data_then_resume) {
    /* Pull events incrementally. Get PART_BEGIN, some DATA, then
     * NEED_DATA when input drains. Feed more, resume. */
    KlAllocator a = kl_allocator_default();
    KlHttpRequest req = make_mp_request(
        "multipart/form-data; boundary=R");
    KlBodyReader *br = kl_body_reader_multipart(&a, &req, NULL);

    const char *chunk1 =
        "--R\r\n"
        "Content-Disposition: form-data; name=\"f\"\r\n\r\n"
        "abc";  /* part body, no terminator yet */
    ASSERT_EQ(br->on_data(br, chunk1, strlen(chunk1)), 0);

    KlMultipartPartMeta m;
    const char *d = NULL;
    size_t dn = 0;

    KlMultipartEvent e = kl_multipart_next(br, &m, &d, &dn);
    ASSERT_EQ(e, KL_MP_EVT_PART_BEGIN);
    ASSERT_TRUE(memcmp(m.name, "f", m.name_len) == 0);

    /* Body is "abc" but we don't know if a delimiter is starting next.
     * Parser must hold back delimiter_len - 1 trailing bytes. So we
     * should NOT get all 3 bytes back yet. */
    e = kl_multipart_next(br, &m, &d, &dn);
    ASSERT_EQ(e, KL_MP_EVT_NEED_DATA);

    /* Feed more body. */
    const char *chunk2 = "def";
    ASSERT_EQ(br->on_data(br, chunk2, strlen(chunk2)), 0);

    /* Should now get some bytes (head of "abcdef"); exact length
     * depends on the delimiter length, but at least 1. */
    e = kl_multipart_next(br, &m, &d, &dn);
    ASSERT_EQ(e, KL_MP_EVT_PART_DATA);
    ASSERT_TRUE(dn > 0);

    /* Feed the terminator. */
    const char *chunk3 = "\r\n--R--\r\n";
    ASSERT_EQ(br->on_data(br, chunk3, strlen(chunk3)), 0);
    br->on_complete(br);

    /* Drain remaining DATA + PART_END + DONE. */
    size_t collected = dn;  /* already returned some */
    for (;;) {
        e = kl_multipart_next(br, &m, &d, &dn);
        if (e == KL_MP_EVT_PART_DATA) { collected += dn; continue; }
        if (e == KL_MP_EVT_PART_END)  continue;
        if (e == KL_MP_EVT_DONE)      break;
        ASSERT_TRUE(0);  /* unexpected */
    }
    ASSERT_EQ(collected, (size_t)6);  /* abc + def */

    br->destroy(br);
}

/* ── Caps ─────────────────────────────────────────────────────────── */

UTEST(mp, max_part_size_exceeded_yields_part_too_large) {
    KlAllocator a = kl_allocator_default();
    KlMultipartConfig cfg = {0};
    cfg.max_part_size = 4;
    KlHttpRequest req = make_mp_request(
        "multipart/form-data; boundary=LIM");
    KlBodyReader *br = kl_body_reader_multipart(&a, &req, &cfg);

    const char *body =
        "--LIM\r\n"
        "Content-Disposition: form-data; name=\"x\"\r\n\r\n"
        "12345"   /* 5 bytes; cap is 4 */
        "\r\n--LIM--\r\n";
    ASSERT_EQ(br->on_data(br, body, strlen(body)), 0);
    br->on_complete(br);

    Collector c; collector_init(&c, &a);
    collect_eager(br, &c);
    ASSERT_EQ(c.last_event, KL_MP_EVT_ERROR);
    ASSERT_EQ(c.last_error, KL_MP_ERR_PART_TOO_LARGE);

    collector_free(&c);
    br->destroy(br);
}

UTEST(mp, max_total_size_exceeded_yields_total_too_large) {
    KlAllocator a = kl_allocator_default();
    KlMultipartConfig cfg = {0};
    cfg.max_total_size = 30;
    KlHttpRequest req = make_mp_request(
        "multipart/form-data; boundary=TOT");
    KlBodyReader *br = kl_body_reader_multipart(&a, &req, &cfg);

    const char *body =
        "--TOT\r\n"
        "Content-Disposition: form-data; name=\"x\"\r\n\r\n"
        "............"   /* push total over 30 */
        "\r\n--TOT--\r\n";
    /* on_data itself signals failure. */
    int rc = br->on_data(br, body, strlen(body));
    ASSERT_EQ(rc, -1);
    ASSERT_EQ(kl_multipart_last_error(br), KL_MP_ERR_TOTAL_TOO_LARGE);

    br->destroy(br);
}

UTEST(mp, max_parts_exceeded_yields_too_many_parts) {
    KlAllocator a = kl_allocator_default();
    KlMultipartConfig cfg = {0};
    cfg.max_parts = 2;
    KlHttpRequest req = make_mp_request(
        "multipart/form-data; boundary=MP");
    KlBodyReader *br = kl_body_reader_multipart(&a, &req, &cfg);

    const char *body =
        "--MP\r\n"
        "Content-Disposition: form-data; name=\"a\"\r\n\r\n1\r\n"
        "--MP\r\n"
        "Content-Disposition: form-data; name=\"b\"\r\n\r\n2\r\n"
        "--MP\r\n"
        "Content-Disposition: form-data; name=\"c\"\r\n\r\n3\r\n"
        "--MP--\r\n";
    ASSERT_EQ(br->on_data(br, body, strlen(body)), 0);
    br->on_complete(br);

    Collector c; collector_init(&c, &a);
    collect_eager(br, &c);
    ASSERT_EQ(c.last_event, KL_MP_EVT_ERROR);
    ASSERT_EQ(c.last_error, KL_MP_ERR_TOO_MANY_PARTS);
    /* First two parts were accepted before the cap tripped. */
    ASSERT_EQ(c.count, 2);

    collector_free(&c);
    br->destroy(br);
}

UTEST(mp, max_input_buffer_exceeded) {
    /* Set a tiny input buffer (1 KiB). A small body that totals more
     * than 1 KiB before the consumer drains via kl_multipart_next will
     * make on_data return -1 with KL_MP_ERR_INPUT_OVERFLOW. */
    KlAllocator a = kl_allocator_default();
    KlMultipartConfig cfg = {0};
    cfg.max_input_buffer = 1024;
    KlHttpRequest req = make_mp_request(
        "multipart/form-data; boundary=IO");
    KlBodyReader *br = kl_body_reader_multipart(&a, &req, &cfg);

    /* Build a > 1024 KiB body without draining. */
    size_t big_n = 2048;
    char *big = kl_malloc(&a, big_n);
    ASSERT_TRUE(big != NULL);
    memset(big, 'A', big_n);
    int rc = br->on_data(br, big, big_n);
    ASSERT_EQ(rc, -1);
    ASSERT_EQ(kl_multipart_last_error(br), KL_MP_ERR_INPUT_OVERFLOW);

    kl_free(&a, big, big_n);
    br->destroy(br);
}

UTEST(mp, headers_oversize) {
    /* Set an explicit per-part headers cap; the streaming parser has
     * no built-in default limit. */
    KlAllocator a = kl_allocator_default();
    KlMultipartConfig cfg = {0};
    cfg.max_headers_size = 512;
    KlHttpRequest req = make_mp_request(
        "multipart/form-data; boundary=HO");
    KlBodyReader *br = kl_body_reader_multipart(&a, &req, &cfg);

    /* Build > 512 bytes of header lines for the first part. */
    char header_line[80];
    memset(header_line, 'X', sizeof(header_line) - 2);
    header_line[sizeof(header_line) - 2] = '\r';
    header_line[sizeof(header_line) - 1] = '\n';

    const char *opener = "--HO\r\n";
    ASSERT_EQ(br->on_data(br, opener, strlen(opener)), 0);
    for (int i = 0; i < 20; i++) {
        ASSERT_EQ(br->on_data(br, header_line, sizeof(header_line)), 0);
    }
    br->on_complete(br);

    Collector c; collector_init(&c, &a);
    collect_eager(br, &c);
    ASSERT_EQ(c.last_event, KL_MP_EVT_ERROR);
    ASSERT_EQ(c.last_error, KL_MP_ERR_HEADERS_TOO_LARGE);

    collector_free(&c);
    br->destroy(br);
}

UTEST(mp, malformed_disposition_yields_error) {
    KlAllocator a = kl_allocator_default();
    KlHttpRequest req = make_mp_request(
        "multipart/form-data; boundary=E");
    KlBodyReader *br = kl_body_reader_multipart(&a, &req, NULL);

    /* Content-Disposition without name= attribute. */
    const char *body =
        "--E\r\n"
        "Content-Disposition: form-data\r\n\r\n"
        "value\r\n"
        "--E--\r\n";
    ASSERT_EQ(br->on_data(br, body, strlen(body)), 0);
    br->on_complete(br);

    Collector c; collector_init(&c, &a);
    collect_eager(br, &c);
    ASSERT_EQ(c.last_event, KL_MP_EVT_ERROR);
    ASSERT_EQ(c.last_error, KL_MP_ERR_MALFORMED);

    collector_free(&c);
    br->destroy(br);
}

UTEST(mp, premature_eof_mid_part_yields_error) {
    KlAllocator a = kl_allocator_default();
    KlHttpRequest req = make_mp_request(
        "multipart/form-data; boundary=PE");
    KlBodyReader *br = kl_body_reader_multipart(&a, &req, NULL);

    /* No closing boundary — connection truncated. */
    const char *body =
        "--PE\r\n"
        "Content-Disposition: form-data; name=\"x\"\r\n\r\n"
        "partial";
    ASSERT_EQ(br->on_data(br, body, strlen(body)), 0);
    br->on_complete(br);

    Collector c; collector_init(&c, &a);
    collect_eager(br, &c);
    ASSERT_EQ(c.last_event, KL_MP_EVT_ERROR);
    ASSERT_EQ(c.last_error, KL_MP_ERR_PREMATURE_EOF);

    collector_free(&c);
    br->destroy(br);
}

UTEST(mp, on_error_marks_reader) {
    KlAllocator a = kl_allocator_default();
    KlHttpRequest req = make_mp_request(
        "multipart/form-data; boundary=OE");
    KlBodyReader *br = kl_body_reader_multipart(&a, &req, NULL);

    br->on_error(br);
    ASSERT_EQ(kl_multipart_last_error(br), KL_MP_ERR_PREMATURE_EOF);

    KlMultipartEvent e = kl_multipart_next(br, NULL, NULL, NULL);
    ASSERT_EQ(e, KL_MP_EVT_ERROR);

    br->destroy(br);
}

/* ── Coverage extensions from c-audit recommendations ──────────────── */

UTEST(mp, empty_name_attribute_rejected_as_malformed) {
    /* L11 fix: name="" or name= with no value is malformed because the
     * form-field key cannot meaningfully be empty. Distinct from the
     * filename case which DOES accept empty. */
    KlAllocator a = kl_allocator_default();
    KlHttpRequest req = make_mp_request(
        "multipart/form-data; boundary=EN");
    KlBodyReader *br = kl_body_reader_multipart(&a, &req, NULL);

    const char *body =
        "--EN\r\n"
        "Content-Disposition: form-data; name=\"\"\r\n"
        "\r\n"
        "v\r\n"
        "--EN--\r\n";
    ASSERT_EQ(br->on_data(br, body, strlen(body)), 0);
    br->on_complete(br);

    Collector c; collector_init(&c, &a);
    collect_eager(br, &c);
    ASSERT_EQ(c.last_event, KL_MP_EVT_ERROR);
    ASSERT_EQ(c.last_error, KL_MP_ERR_MALFORMED);

    collector_free(&c);
    br->destroy(br);
}

UTEST(mp, empty_filename_attribute) {
    /* filename="" was supplied. Should produce a zero-length but
     * non-NULL filename string (a positive signal that the client
     * sent the attribute, vs. omitting it). */
    KlAllocator a = kl_allocator_default();
    KlHttpRequest req = make_mp_request(
        "multipart/form-data; boundary=EF");
    KlBodyReader *br = kl_body_reader_multipart(&a, &req, NULL);

    const char *body =
        "--EF\r\n"
        "Content-Disposition: form-data; name=\"f\"; filename=\"\"\r\n"
        "Content-Type: application/octet-stream\r\n\r\n"
        "x"
        "\r\n--EF--\r\n";
    ASSERT_EQ(br->on_data(br, body, strlen(body)), 0);
    br->on_complete(br);

    Collector c; collector_init(&c, &a);
    collect_eager(br, &c);
    ASSERT_EQ(c.last_event, KL_MP_EVT_DONE);
    ASSERT_EQ(c.count, 1);
    ASSERT_TRUE(c.parts[0].filename != NULL);
    ASSERT_EQ(c.parts[0].filename_len, (size_t)0);
    ASSERT_STREQ(c.parts[0].filename, "");

    collector_free(&c);
    br->destroy(br);
}

UTEST(mp, empty_content_type_header) {
    /* Content-Type: <blank> is unusual but well-formed. The parser
     * should accept it and report ctype_len == 0. */
    KlAllocator a = kl_allocator_default();
    KlHttpRequest req = make_mp_request(
        "multipart/form-data; boundary=EC");
    KlBodyReader *br = kl_body_reader_multipart(&a, &req, NULL);

    const char *body =
        "--EC\r\n"
        "Content-Disposition: form-data; name=\"x\"\r\n"
        "Content-Type: \r\n"
        "\r\n"
        "v"
        "\r\n--EC--\r\n";
    ASSERT_EQ(br->on_data(br, body, strlen(body)), 0);
    br->on_complete(br);

    Collector c; collector_init(&c, &a);
    collect_eager(br, &c);
    ASSERT_EQ(c.last_event, KL_MP_EVT_DONE);
    ASSERT_EQ(c.count, 1);
    ASSERT_TRUE(c.parts[0].ctype != NULL);
    ASSERT_EQ(c.parts[0].ctype_len, (size_t)0);

    collector_free(&c);
    br->destroy(br);
}

UTEST(mp, zero_body_part_immediate_boundary) {
    /* A part with no body bytes between headers and the next boundary.
     * Must emit PART_BEGIN then PART_END with no intervening PART_DATA. */
    KlAllocator a = kl_allocator_default();
    KlHttpRequest req = make_mp_request(
        "multipart/form-data; boundary=ZB");
    KlBodyReader *br = kl_body_reader_multipart(&a, &req, NULL);

    const char *body =
        "--ZB\r\n"
        "Content-Disposition: form-data; name=\"empty\"\r\n\r\n"
        "\r\n--ZB\r\n"
        "Content-Disposition: form-data; name=\"after\"\r\n\r\n"
        "value"
        "\r\n--ZB--\r\n";
    ASSERT_EQ(br->on_data(br, body, strlen(body)), 0);
    br->on_complete(br);

    /* Walk the events explicitly to verify the sequence. */
    KlMultipartPartMeta m;
    const char *d = NULL;
    size_t dn = 0;

    ASSERT_EQ(kl_multipart_next(br, &m, &d, &dn), KL_MP_EVT_PART_BEGIN);
    ASSERT_TRUE(memcmp(m.name, "empty", m.name_len) == 0);
    /* No PART_DATA — the next event is PART_END. */
    ASSERT_EQ(kl_multipart_next(br, &m, &d, &dn), KL_MP_EVT_PART_END);

    ASSERT_EQ(kl_multipart_next(br, &m, &d, &dn), KL_MP_EVT_PART_BEGIN);
    ASSERT_TRUE(memcmp(m.name, "after", m.name_len) == 0);
    ASSERT_EQ(kl_multipart_next(br, &m, &d, &dn), KL_MP_EVT_PART_DATA);
    ASSERT_EQ(dn, (size_t)5);
    ASSERT_TRUE(memcmp(d, "value", 5) == 0);
    ASSERT_EQ(kl_multipart_next(br, &m, &d, &dn), KL_MP_EVT_PART_END);
    ASSERT_EQ(kl_multipart_next(br, &m, &d, &dn), KL_MP_EVT_DONE);

    br->destroy(br);
}

UTEST(mp, rejects_subtype_prefix_collision) {
    /* "multipart/form-data-extra" must NOT be accepted as form-data
     * even though it shares the prefix. */
    KlAllocator a = kl_allocator_default();
    KlHttpRequest req = make_mp_request(
        "multipart/form-data-extra; boundary=PC");
    KlBodyReader *br = kl_body_reader_multipart(&a, &req, NULL);
    ASSERT_TRUE(br == NULL);
}

UTEST(mp, headers_cap_not_tripped_by_co_resident_body) {
    /* Regression for the case where a single chunk carries
     * [small headers + large body]. The HEADERS state must NOT count
     * the body bytes toward max_headers_size — only the bytes up to
     * \r\n\r\n. */
    KlAllocator a = kl_allocator_default();
    KlMultipartConfig cfg = {0};
    cfg.max_headers_size = 256;  /* tighter than total body */
    KlHttpRequest req = make_mp_request(
        "multipart/form-data; boundary=CR");
    KlBodyReader *br = kl_body_reader_multipart(&a, &req, &cfg);
    ASSERT_TRUE(br != NULL);

    /* Body has ~80 bytes of headers and ~2 KiB of body bytes — all in
     * a single on_data call. */
    const char *prefix =
        "--CR\r\n"
        "Content-Disposition: form-data; name=\"f\"\r\n"
        "Content-Type: application/octet-stream\r\n\r\n";
    size_t prefix_len = strlen(prefix);

    size_t body_n = 2048;
    char  *buf    = kl_malloc(&a, prefix_len + body_n + 16);
    ASSERT_TRUE(buf != NULL);
    memcpy(buf, prefix, prefix_len);
    memset(buf + prefix_len, 'X', body_n);
    const char *suffix = "\r\n--CR--\r\n";
    memcpy(buf + prefix_len + body_n, suffix, strlen(suffix));
    size_t total = prefix_len + body_n + strlen(suffix);

    ASSERT_EQ(br->on_data(br, buf, total), 0);
    br->on_complete(br);

    Collector c; collector_init(&c, &a);
    collect_eager(br, &c);
    ASSERT_EQ(c.last_event, KL_MP_EVT_DONE);
    ASSERT_EQ(c.count, 1);
    ASSERT_EQ(c.parts[0].body_len, body_n);

    collector_free(&c);
    kl_free(&a, buf, prefix_len + body_n + 16);
    br->destroy(br);
}

UTEST(mp, case_insensitive_boundary_param) {
    KlAllocator a = kl_allocator_default();
    KlHttpRequest req = make_mp_request(
        "multipart/form-data; BOUNDARY=CI");
    KlBodyReader *br = kl_body_reader_multipart(&a, &req, NULL);
    ASSERT_TRUE(br != NULL);

    const char *body =
        "--CI\r\n"
        "Content-Disposition: form-data; name=\"x\"\r\n\r\n"
        "v\r\n"
        "--CI--\r\n";
    ASSERT_EQ(br->on_data(br, body, strlen(body)), 0);
    br->on_complete(br);

    Collector c; collector_init(&c, &a);
    collect_eager(br, &c);
    ASSERT_EQ(c.last_event, KL_MP_EVT_DONE);
    ASSERT_EQ(c.count, 1);
    ASSERT_STREQ(c.parts[0].name, "x");
    ASSERT_STREQ(c.parts[0].body, "v");

    collector_free(&c);
    br->destroy(br);
}

UTEST(mp, case_insensitive_name_and_filename) {
    KlAllocator a = kl_allocator_default();
    KlHttpRequest req = make_mp_request(
        "multipart/form-data; boundary=NI");
    KlBodyReader *br = kl_body_reader_multipart(&a, &req, NULL);

    const char *body =
        "--NI\r\n"
        "Content-Disposition: form-data; NAME=\"upload\"; FileName=\"r.txt\"\r\n"
        "\r\n"
        "data\r\n"
        "--NI--\r\n";
    ASSERT_EQ(br->on_data(br, body, strlen(body)), 0);
    br->on_complete(br);

    Collector c; collector_init(&c, &a);
    collect_eager(br, &c);
    ASSERT_EQ(c.last_event, KL_MP_EVT_DONE);
    ASSERT_EQ(c.count, 1);
    ASSERT_STREQ(c.parts[0].name, "upload");
    ASSERT_STREQ(c.parts[0].filename, "r.txt");
    ASSERT_STREQ(c.parts[0].body, "data");

    collector_free(&c);
    br->destroy(br);
}

UTEST(mp, tab_terminates_unquoted_boundary_param) {
    /* RFC 2046 allows linear whitespace including TAB between MIME
     * parameters. An unquoted boundary followed by a tab must not
     * fold the tab into the boundary string. */
    KlAllocator a = kl_allocator_default();
    KlHttpRequest req = make_mp_request(
        "multipart/form-data; boundary=TB\t");
    KlBodyReader *br = kl_body_reader_multipart(&a, &req, NULL);
    ASSERT_TRUE(br != NULL);

    const char *body =
        "--TB\r\n"
        "Content-Disposition: form-data; name=\"x\"\r\n\r\n"
        "ok\r\n"
        "--TB--\r\n";
    ASSERT_EQ(br->on_data(br, body, strlen(body)), 0);
    br->on_complete(br);

    Collector c; collector_init(&c, &a);
    collect_eager(br, &c);
    ASSERT_EQ(c.last_event, KL_MP_EVT_DONE);
    ASSERT_EQ(c.count, 1);
    ASSERT_STREQ(c.parts[0].body, "ok");

    collector_free(&c);
    br->destroy(br);
}

UTEST(mp, duplicate_content_type_header_no_leak) {
    /* Two Content-Type headers in the same part. The second wins;
     * the first allocation must be freed (ASan would catch the leak). */
    KlAllocator a = kl_allocator_default();
    KlHttpRequest req = make_mp_request(
        "multipart/form-data; boundary=DC");
    KlBodyReader *br = kl_body_reader_multipart(&a, &req, NULL);

    const char *body =
        "--DC\r\n"
        "Content-Disposition: form-data; name=\"f\"\r\n"
        "Content-Type: application/json\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "hello\r\n"
        "--DC--\r\n";
    ASSERT_EQ(br->on_data(br, body, strlen(body)), 0);
    br->on_complete(br);

    Collector c; collector_init(&c, &a);
    collect_eager(br, &c);
    ASSERT_EQ(c.last_event, KL_MP_EVT_DONE);
    ASSERT_EQ(c.count, 1);
    /* Second Content-Type wins. */
    ASSERT_STREQ(c.parts[0].ctype, "text/plain");

    collector_free(&c);
    br->destroy(br);
}

UTEST(mp, name_does_not_collide_with_other_param_suffix) {
    /* M5 regression: `somename=` must NOT be matched as `name=`.
     * Real `name=` parameter must win. */
    KlAllocator a = kl_allocator_default();
    KlHttpRequest req = make_mp_request(
        "multipart/form-data; boundary=NC");
    KlBodyReader *br = kl_body_reader_multipart(&a, &req, NULL);

    const char *body =
        "--NC\r\n"
        "Content-Disposition: form-data; somename=ghost; name=real\r\n"
        "\r\n"
        "value\r\n"
        "--NC--\r\n";
    ASSERT_EQ(br->on_data(br, body, strlen(body)), 0);
    br->on_complete(br);

    Collector c; collector_init(&c, &a);
    collect_eager(br, &c);
    ASSERT_EQ(c.last_event, KL_MP_EVT_DONE);
    ASSERT_EQ(c.count, 1);
    ASSERT_STREQ(c.parts[0].name, "real");

    collector_free(&c);
    br->destroy(br);
}

UTEST(mp, filename_does_not_collide_with_other_param_suffix) {
    /* M5 regression for filename=. */
    KlAllocator a = kl_allocator_default();
    KlHttpRequest req = make_mp_request(
        "multipart/form-data; boundary=FC");
    KlBodyReader *br = kl_body_reader_multipart(&a, &req, NULL);

    const char *body =
        "--FC\r\n"
        "Content-Disposition: form-data; name=\"f\"; myfilename=ghost; filename=real.txt\r\n"
        "\r\n"
        "v\r\n"
        "--FC--\r\n";
    ASSERT_EQ(br->on_data(br, body, strlen(body)), 0);
    br->on_complete(br);

    Collector c; collector_init(&c, &a);
    collect_eager(br, &c);
    ASSERT_EQ(c.last_event, KL_MP_EVT_DONE);
    ASSERT_EQ(c.count, 1);
    ASSERT_STREQ(c.parts[0].filename, "real.txt");

    collector_free(&c);
    br->destroy(br);
}

UTEST(mp, boundary_does_not_collide_with_other_param_suffix) {
    /* M5 regression at factory layer. `xboundary=` must NOT match;
     * real `boundary=` must win. */
    KlAllocator a = kl_allocator_default();
    KlHttpRequest req = make_mp_request(
        "multipart/form-data; xboundary=ghost; boundary=REAL");
    KlBodyReader *br = kl_body_reader_multipart(&a, &req, NULL);
    ASSERT_TRUE(br != NULL);

    /* Use the real boundary in the body to prove it parsed correctly. */
    const char *body =
        "--REAL\r\n"
        "Content-Disposition: form-data; name=\"x\"\r\n\r\n"
        "ok\r\n"
        "--REAL--\r\n";
    ASSERT_EQ(br->on_data(br, body, strlen(body)), 0);
    br->on_complete(br);

    Collector c; collector_init(&c, &a);
    collect_eager(br, &c);
    ASSERT_EQ(c.last_event, KL_MP_EVT_DONE);
    ASSERT_EQ(c.count, 1);
    ASSERT_STREQ(c.parts[0].body, "ok");

    collector_free(&c);
    br->destroy(br);
}

UTEST(mp, unquoted_param_trims_trailing_lws) {
    /* L9 regression: trailing space / tab on an unquoted token is
     * stripped from name and filename. */
    KlAllocator a = kl_allocator_default();
    KlHttpRequest req = make_mp_request(
        "multipart/form-data; boundary=TR");
    KlBodyReader *br = kl_body_reader_multipart(&a, &req, NULL);

    const char *body =
        "--TR\r\n"
        "Content-Disposition: form-data; name=trimmed \t; filename=fname \t\r\n"
        "\r\n"
        "v\r\n"
        "--TR--\r\n";
    ASSERT_EQ(br->on_data(br, body, strlen(body)), 0);
    br->on_complete(br);

    Collector c; collector_init(&c, &a);
    collect_eager(br, &c);
    ASSERT_EQ(c.last_event, KL_MP_EVT_DONE);
    ASSERT_EQ(c.count, 1);
    ASSERT_STREQ(c.parts[0].name, "trimmed");
    ASSERT_STREQ(c.parts[0].filename, "fname");

    collector_free(&c);
    br->destroy(br);
}

UTEST(mp, rejects_null_alloc_or_req) {
    KlAllocator a = kl_allocator_default();
    KlHttpRequest req = make_mp_request(
        "multipart/form-data; boundary=NA");
    ASSERT_TRUE(kl_body_reader_multipart(NULL, &req, NULL) == NULL);
    ASSERT_TRUE(kl_body_reader_multipart(&a, NULL, NULL) == NULL);
}

UTEST(mp, wrong_reader_kind_in_next_yields_error) {
    KlAllocator a = kl_allocator_default();
    KlHttpRequest req = {0};
    KlBodyReader *bufr = kl_body_reader_buffer(&a, &req, NULL);
    ASSERT_TRUE(bufr != NULL);
    /* Pass a KlBufReader to kl_multipart_next — must not crash. */
    KlMultipartEvent e = kl_multipart_next(bufr, NULL, NULL, NULL);
    ASSERT_EQ(e, KL_MP_EVT_ERROR);
    ASSERT_EQ(kl_multipart_last_error(bufr), KL_MP_ERR_NONE);
    bufr->destroy(bufr);
}

UTEST_MAIN();
