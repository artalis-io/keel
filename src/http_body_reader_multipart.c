#include <keel/http_body_reader_multipart.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <limits.h>

/*
 * Streaming multipart parser.
 *
 * Push (on_data) → internal input buffer → pull (kl_http_multipart_next) →
 * event stream {PART_BEGIN, PART_DATA, PART_END, DONE, NEED_DATA, ERROR}.
 *
 * The state machine never blocks. on_data appends; kl_http_multipart_next
 * consumes as far as it can and returns one event per call (or
 * NEED_DATA if more bytes are required).
 *
 * Body data is never copied: PART_DATA returns a borrowed slice of the
 * internal input buffer. The slice stays valid until the next
 * kl_http_multipart_next() call, at which point the buffer is shifted to
 * reclaim consumed bytes.
 */

/* ── Parser states ───────────────────────────────────────────────────── */

typedef enum {
    KL_HTTP_MP_S_PREAMBLE,        /* before first boundary */
    KL_HTTP_MP_S_AFTER_BOUNDARY,  /* boundary seen, awaiting "\r\n" or "--" */
    KL_HTTP_MP_S_HEADERS,         /* accumulating part headers until \r\n\r\n */
    KL_HTTP_MP_S_BODY,            /* streaming part body until delimiter */
    KL_HTTP_MP_S_DONE,            /* terminator boundary seen */
    KL_HTTP_MP_S_ERROR            /* parser cannot continue */
} KlHttpMultipartParserState;

/* ── Reader struct ───────────────────────────────────────────────────── */

/*
 * KL_HTTP_MP_MAX_BOUNDARY (70) comes from RFC 2046 §5.1.1 — the multipart
 * spec's hard limit on boundary length, NOT a Keel-imposed magic cap.
 * No other fixed-size buffer state exists; both the input buffer and
 * the per-part header accumulator grow on demand, capped by config.
 */

struct KlHttpMultipartReader {
    KlHttpBodyReader base;
    KlAllocator *alloc;

    /* Delimiter: "\r\n--<boundary>" — what marks the END of a part
     * body. For the initial preamble we search for just "--<boundary>".
     * Bounded by the RFC 2046 cap on boundary length, so a static
     * inline buffer is the right shape here. */
    char   delimiter[KL_HTTP_MP_MAX_BOUNDARY + 6];
    size_t delimiter_len;

    KlHttpMultipartConfig      config;
    KlHttpMultipartParserState state;
    KlHttpMultipartErrorCode   last_error;

    /* Input buffer: bytes received via on_data, not yet consumed. */
    char  *input;
    size_t input_len;
    size_t input_cap;
    /* Bytes the most recent kl_http_multipart_next() call earmarked for
     * consumption. The shift happens at the START of the next call so
     * PART_DATA slices stay valid between calls. */
    size_t input_consumed;
    int    stream_ended;  /* on_complete fired */

    /* Counters */
    size_t total_received;     /* bytes ever appended to input */
    size_t current_part_size;  /* body bytes emitted for the current part */
    int    parts_begun;        /* parts for which PART_BEGIN has fired */

    /* Per-part header accumulator. Growable; capped by
     * config.max_headers_size (0 = unlimited). Reset on each part. */
    char  *hdr;
    size_t hdr_len;
    size_t hdr_cap;

    /* Current-part metadata strings, allocated. Replaced each PART_BEGIN. */
    char  *cur_name;       size_t cur_name_len;
    char  *cur_filename;   size_t cur_filename_len;
    char  *cur_ctype;      size_t cur_ctype_len;
};

/* ── Helpers ─────────────────────────────────────────────────────────── */

/* O(n*m) naive search — boundary is at most KL_HTTP_MP_MAX_BOUNDARY + 4 bytes. */
static const void *mp_memmem(const void *hay, size_t hlen,
                              const void *needle, size_t nlen) {
    if (nlen == 0) return hay;
    if (nlen > hlen) return NULL;
    const char *h = hay;
    const char *n = needle;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        if (memcmp(h + i, n, nlen) == 0) return h + i;
    }
    return NULL;
}

/*
 * Find a MIME parameter "<name>=" inside a header value, returning a
 * pointer to the byte AFTER the '='. The match must be at the start
 * of the value or be preceded by ';', SP, or HTAB — otherwise a
 * parameter with a colliding prefix (e.g. "somename=" vs "name=", or
 * "X-boundary=" vs "boundary=") could front-run the real one.
 *
 * `needle` MUST include the trailing '=' (so "name=", "filename=",
 * "boundary="). nlen is its length. Case-insensitive match per
 * RFC 2046 / 7578.
 */
static const char *mp_find_param(const char *val, size_t vlen,
                                  const char *needle, size_t nlen) {
    if (nlen == 0 || nlen > vlen) return NULL;
    /* nlen <= vlen guaranteed above, so vlen - nlen does not underflow.
     * Use the subtractive form rather than `i + nlen <= vlen` so the
     * loop guard is overflow-safe even on adversarial vlen. */
    for (size_t i = 0; i <= vlen - nlen; i++) {
        if (i > 0) {
            char prev = val[i - 1];
            if (prev != ';' && prev != ' ' && prev != '\t')
                continue;
        }
        if (strncasecmp(val + i, needle, nlen) == 0)
            return val + i + nlen;
    }
    return NULL;
}

static char *mp_strdup(KlAllocator *alloc, const char *s, size_t len) {
    if (len > SIZE_MAX - 1) return NULL;
    char *d = kl_malloc(alloc, len + 1);
    if (!d) return NULL;
    if (len > 0) memcpy(d, s, len);
    d[len] = '\0';
    return d;
}

static void mp_free_str(KlAllocator *alloc, char **p, size_t *len) {
    if (*p) {
        kl_free(alloc, *p, *len + 1);
        *p = NULL;
    }
    *len = 0;
}

/*
 * L6: reject control bytes (< 0x20, or DEL 0x7F) in a disposition
 * name/filename. A full CRLF cannot appear (the value comes from a line
 * already split on "\r\n"), but a lone CR/LF/TAB/NUL can survive inside a
 * quoted string and would otherwise reach application form/file handlers as
 * a log-injection or NUL-truncation vector. Returns 1 if any is present.
 */
static int mp_has_ctl(const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7F) return 1;
    }
    return 0;
}

/*
 * Generic growable-buffer reserve. cap_limit == 0 means unbounded
 * (constrained only by the allocator). Growth is exponential from
 * whatever the current cap is; if the buffer is empty, it grows to
 * exactly the requested size on the first call (avoiding any magic
 * initial-size constant).
 */
static int mp_grow_buf(KlAllocator *alloc, char **buf, size_t *cap,
                       size_t cur_len, size_t additional, size_t cap_limit) {
    if (additional > SIZE_MAX - cur_len) return -1;
    size_t need = cur_len + additional;
    if (cap_limit > 0 && need > cap_limit) return -1;

    if (need <= *cap) return 0;

    size_t new_cap = *cap;
    if (new_cap == 0) {
        new_cap = need;
    } else {
        while (new_cap < need) {
            if (new_cap > SIZE_MAX / 2) { new_cap = need; break; }
            new_cap *= 2;
        }
    }
    if (cap_limit > 0 && new_cap > cap_limit) new_cap = cap_limit;
    if (new_cap < need) return -1;

    char *nb = kl_realloc(alloc, *buf, *cap, new_cap);
    if (!nb) return -1;
    *buf = nb;
    *cap = new_cap;
    return 0;
}

static int mp_input_reserve(KlHttpMultipartReader *mr, size_t additional) {
    return mp_grow_buf(mr->alloc, &mr->input, &mr->input_cap,
                       mr->input_len, additional,
                       mr->config.max_input_buffer);
}

static int mp_hdr_reserve(KlHttpMultipartReader *mr, size_t additional) {
    return mp_grow_buf(mr->alloc, &mr->hdr, &mr->hdr_cap,
                       mr->hdr_len, additional,
                       mr->config.max_headers_size);
}

/* Apply the deferred consumption from the previous kl_http_multipart_next call. */
static void mp_apply_consume(KlHttpMultipartReader *mr) {
    if (mr->input_consumed == 0) return;
    if (mr->input_consumed >= mr->input_len) {
        mr->input_len = 0;
    } else {
        memmove(mr->input, mr->input + mr->input_consumed,
                mr->input_len - mr->input_consumed);
        mr->input_len -= mr->input_consumed;
    }
    mr->input_consumed = 0;
}

/* ── Header parsing ──────────────────────────────────────────────────── */

/*
 * Parse a Content-Disposition value for `name=` and `filename=` attrs.
 * Returns 0 on success, -1 on failure. On NOMEM, sets mr->last_error to
 * KL_HTTP_MP_ERR_NOMEM so the caller can preserve the cause via mp_fail_or_keep.
 * On any other failure (missing name=, empty name, unterminated quoted
 * attribute), leaves last_error alone — caller maps to MALFORMED.
 *
 * Asymmetry: `name=` MUST be present AND non-empty (it's the form-field
 * key — an empty key is semantically meaningless and would confuse
 * downstream form handlers). `filename=""` is accepted and produces a
 * zero-length string; clients legitimately send empty filename to mark
 * a part as binary-data-without-filename.
 */
static int mp_parse_disposition(KlHttpMultipartReader *mr, const char *val,
                                size_t vlen) {
    mp_free_str(mr->alloc, &mr->cur_name, &mr->cur_name_len);
    mp_free_str(mr->alloc, &mr->cur_filename, &mr->cur_filename_len);

    /* name= is required. */
    const char *np = mp_find_param(val, vlen, "name=", 5);
    if (!np) return -1;
    size_t nremain = vlen - (size_t)(np - val);

    const char *name_src;
    size_t      name_len;
    if (nremain > 0 && *np == '"') {
        np++; nremain--;
        const char *end = memchr(np, '"', nremain);
        if (!end) return -1;  /* unterminated quoted name → MALFORMED */
        name_src = np;
        name_len = (size_t)(end - np);
    } else {
        const char *end = memchr(np, ';', nremain);
        name_src = np;
        name_len = end ? (size_t)(end - np) : nremain;
        /* L9: strip trailing LWS from unquoted token. */
        while (name_len > 0 &&
               (name_src[name_len - 1] == ' ' ||
                name_src[name_len - 1] == '\t'))
            name_len--;
    }
    if (name_len == 0) return -1;  /* empty form-field name → MALFORMED */
    if (mp_has_ctl(name_src, name_len)) return -1;  /* L6 → MALFORMED */
    mr->cur_name = mp_strdup(mr->alloc, name_src, name_len);
    if (!mr->cur_name) { mr->last_error = KL_HTTP_MP_ERR_NOMEM; return -1; }
    mr->cur_name_len = name_len;

    /* filename= is optional. */
    const char *fp = mp_find_param(val, vlen, "filename=", 9);
    if (!fp) return 0;
    size_t fremain = vlen - (size_t)(fp - val);

    const char *fn_src;
    size_t      fn_len;
    if (fremain > 0 && *fp == '"') {
        fp++; fremain--;
        const char *end = memchr(fp, '"', fremain);
        if (!end) return -1;  /* unterminated quoted filename → MALFORMED */
        fn_src = fp;
        fn_len = (size_t)(end - fp);
    } else {
        const char *end = memchr(fp, ';', fremain);
        fn_src = fp;
        fn_len = end ? (size_t)(end - fp) : fremain;
        /* L9: strip trailing LWS from unquoted token. */
        while (fn_len > 0 &&
               (fn_src[fn_len - 1] == ' ' ||
                fn_src[fn_len - 1] == '\t'))
            fn_len--;
    }
    if (mp_has_ctl(fn_src, fn_len)) return -1;  /* L6 → MALFORMED */
    mr->cur_filename = mp_strdup(mr->alloc, fn_src, fn_len);
    if (!mr->cur_filename) { mr->last_error = KL_HTTP_MP_ERR_NOMEM; return -1; }
    mr->cur_filename_len = fn_len;
    return 0;
}

/*
 * Parse the accumulated headers in mr->hdr_buf[0 .. mr->hdr_len].
 * Returns 0 if a Content-Disposition was found (name extracted) and -1
 * otherwise. Content-Type is optional.
 */
static int mp_parse_headers(KlHttpMultipartReader *mr) {
    const char *buf = mr->hdr;
    size_t len = mr->hdr_len;
    int got_disposition = 0;

    mp_free_str(mr->alloc, &mr->cur_ctype, &mr->cur_ctype_len);

    while (len > 0) {
        const char *eol = mp_memmem(buf, len, "\r\n", 2);
        size_t line_len = eol ? (size_t)(eol - buf) : len;
        if (line_len == 0) break;

        const char *colon = memchr(buf, ':', line_len);
        if (colon) {
            size_t name_len = (size_t)(colon - buf);
            const char *val = colon + 1;
            size_t val_len = line_len - name_len - 1;
            while (val_len > 0 && *val == ' ') { val++; val_len--; }

            if (name_len == 19 &&
                strncasecmp(buf, "Content-Disposition", 19) == 0) {
                if (mp_parse_disposition(mr, val, val_len) < 0) return -1;
                got_disposition = 1;
            } else if (name_len == 12 &&
                       strncasecmp(buf, "Content-Type", 12) == 0) {
                /* Free any prior Content-Type allocation in this same
                 * part — duplicate header lines would otherwise leak
                 * the first allocation. Last header wins. */
                mp_free_str(mr->alloc, &mr->cur_ctype, &mr->cur_ctype_len);
                mr->cur_ctype = mp_strdup(mr->alloc, val, val_len);
                if (!mr->cur_ctype) {
                    mr->last_error = KL_HTTP_MP_ERR_NOMEM;
                    return -1;
                }
                mr->cur_ctype_len = val_len;
            }
        }

        if (eol) {
            buf = eol + 2;
            len -= line_len + 2;
        } else {
            break;
        }
    }
    return got_disposition ? 0 : -1;
}

/* ── on_data: push bytes into the input buffer ───────────────────────── */

static int mp_on_data(KlHttpBodyReader *self, const char *data, size_t len) {
    KlHttpMultipartReader *mr = (KlHttpMultipartReader *)self;
    if (mr->state == KL_HTTP_MP_S_ERROR) return -1;
    if (mr->state == KL_HTTP_MP_S_DONE)  return 0;

    if (len == 0) return 0;

    if (len > SIZE_MAX - mr->total_received) {
        mr->state = KL_HTTP_MP_S_ERROR;
        mr->last_error = KL_HTTP_MP_ERR_TOTAL_TOO_LARGE;
        return -1;
    }
    if (mr->config.max_total_size > 0 &&
        mr->total_received + len > mr->config.max_total_size) {
        mr->state = KL_HTTP_MP_S_ERROR;
        mr->last_error = KL_HTTP_MP_ERR_TOTAL_TOO_LARGE;
        return -1;
    }

    if (mp_input_reserve(mr, len) < 0) {
        mr->state = KL_HTTP_MP_S_ERROR;
        mr->last_error = KL_HTTP_MP_ERR_INPUT_OVERFLOW;
        return -1;
    }
    memcpy(mr->input + mr->input_len, data, len);
    mr->input_len += len;
    mr->total_received += len;
    return 0;
}

static void mp_on_complete(KlHttpBodyReader *self) {
    KlHttpMultipartReader *mr = (KlHttpMultipartReader *)self;
    mr->stream_ended = 1;
}

static void mp_on_error(KlHttpBodyReader *self) {
    KlHttpMultipartReader *mr = (KlHttpMultipartReader *)self;
    if (mr->state != KL_HTTP_MP_S_DONE) {
        mr->state = KL_HTTP_MP_S_ERROR;
        if (mr->last_error == KL_HTTP_MP_ERR_NONE)
            mr->last_error = KL_HTTP_MP_ERR_PREMATURE_EOF;
    }
}

static void mp_destroy(KlHttpBodyReader *self) {
    KlHttpMultipartReader *mr = (KlHttpMultipartReader *)self;
    mp_free_str(mr->alloc, &mr->cur_name, &mr->cur_name_len);
    mp_free_str(mr->alloc, &mr->cur_filename, &mr->cur_filename_len);
    mp_free_str(mr->alloc, &mr->cur_ctype, &mr->cur_ctype_len);
    if (mr->input) kl_free(mr->alloc, mr->input, mr->input_cap);
    if (mr->hdr)   kl_free(mr->alloc, mr->hdr,   mr->hdr_cap);
    kl_free(mr->alloc, mr, sizeof(*mr));
}

/* ── kl_http_multipart_next: pull events ──────────────────────────────────── */

static KlHttpMultipartEvent mp_fail(KlHttpMultipartReader *mr,
                                KlHttpMultipartErrorCode code) {
    mr->state = KL_HTTP_MP_S_ERROR;
    mr->last_error = code;
    return KL_HTTP_MP_EVT_ERROR;
}

/* Like mp_fail, but if an inner parser already pinned a more specific
 * cause (e.g. KL_HTTP_MP_ERR_NOMEM from a failed allocation), keep it. */
static KlHttpMultipartEvent mp_fail_or_keep(KlHttpMultipartReader *mr,
                                        KlHttpMultipartErrorCode fallback) {
    mr->state = KL_HTTP_MP_S_ERROR;
    if (mr->last_error == KL_HTTP_MP_ERR_NONE) mr->last_error = fallback;
    return KL_HTTP_MP_EVT_ERROR;
}

static KlHttpMultipartEvent mp_need_or_eof(KlHttpMultipartReader *mr) {
    if (mr->stream_ended) return mp_fail(mr, KL_HTTP_MP_ERR_PREMATURE_EOF);
    return KL_HTTP_MP_EVT_NEED_DATA;
}

KlHttpMultipartEvent kl_http_multipart_next(KlHttpBodyReader *br,
                                    KlHttpMultipartPartMeta *meta,
                                    const char **data,
                                    size_t *data_len) {
    /* Identity check via vtable: only a multipart reader's on_data is
     * mp_on_data. Cheap and safe. */
    if (!br || br->on_data != mp_on_data) {
        if (data) *data = NULL;
        if (data_len) *data_len = 0;
        return KL_HTTP_MP_EVT_ERROR;
    }
    KlHttpMultipartReader *mr = (KlHttpMultipartReader *)br;

    /* Apply the deferred consumption from the previous call. */
    mp_apply_consume(mr);

    if (data) *data = NULL;
    if (data_len) *data_len = 0;

    for (;;) {
        if (mr->state == KL_HTTP_MP_S_ERROR) return KL_HTTP_MP_EVT_ERROR;
        if (mr->state == KL_HTTP_MP_S_DONE)  return KL_HTTP_MP_EVT_DONE;

        switch (mr->state) {

        case KL_HTTP_MP_S_PREAMBLE: {
            /*
             * Search for "--<boundary>" (delimiter without the leading
             * \r\n). The preamble can contain arbitrary garbage; we
             * tolerate it. We DO need to keep at least nlen-1 trailing
             * bytes in input if not found, to detect a delimiter that
             * splits across on_data boundaries.
             */
            const char *needle = mr->delimiter + 2;     /* skip "\r\n" */
            size_t      nlen   = mr->delimiter_len - 2;

            if (mr->input_len < nlen) return mp_need_or_eof(mr);

            const char *found = mp_memmem(mr->input, mr->input_len, needle, nlen);
            if (found) {
                size_t skip = (size_t)(found - mr->input) + nlen;
                mr->input_consumed = skip;
                mp_apply_consume(mr);  /* immediate — no slice handed out */
                mr->state = KL_HTTP_MP_S_AFTER_BOUNDARY;
                continue;
            }
            /* Not found. Keep nlen-1 trailing bytes; shift the rest. */
            size_t keep = nlen - 1;
            if (mr->input_len > keep) {
                mr->input_consumed = mr->input_len - keep;
                mp_apply_consume(mr);
            }
            return mp_need_or_eof(mr);
        }

        case KL_HTTP_MP_S_AFTER_BOUNDARY: {
            if (mr->input_len < 2) return mp_need_or_eof(mr);
            char a = mr->input[0], b = mr->input[1];
            if (a == '-' && b == '-') {
                mr->input_consumed = 2;
                /* Optional trailing CRLF + epilogue is discarded. */
                mp_apply_consume(mr);
                mr->state = KL_HTTP_MP_S_DONE;
                return KL_HTTP_MP_EVT_DONE;
            }
            if (a == '\r' && b == '\n') {
                /* New part beginning. */
                if (mr->config.max_parts > 0 &&
                    mr->parts_begun >= mr->config.max_parts)
                    return mp_fail(mr, KL_HTTP_MP_ERR_TOO_MANY_PARTS);

                mr->input_consumed = 2;
                mp_apply_consume(mr);
                mr->state = KL_HTTP_MP_S_HEADERS;
                mr->hdr_len = 0;
                mr->current_part_size = 0;
                continue;
            }
            return mp_fail(mr, KL_HTTP_MP_ERR_MALFORMED);
        }

        case KL_HTTP_MP_S_HEADERS: {
            /*
             * Pull bytes from input into the growable header buffer
             * until "\r\n\r\n". We must NOT copy past max_headers_size,
             * otherwise a chunk that carries [headers + lots of body]
             * would trip HEADERS_TOO_LARGE even when the headers fit.
             * Copy only as much as the cap allows, then search; if the
             * cap is exhausted without finding the terminator, error.
             */
            if (mr->input_len == 0) return mp_need_or_eof(mr);

            size_t cap_limit = mr->config.max_headers_size;
            size_t available = cap_limit > 0
                ? (cap_limit > mr->hdr_len ? cap_limit - mr->hdr_len : 0)
                : SIZE_MAX;
            if (cap_limit > 0 && available == 0)
                return mp_fail(mr, KL_HTTP_MP_ERR_HEADERS_TOO_LARGE);

            size_t take = mr->input_len < available ? mr->input_len : available;

            if (mp_hdr_reserve(mr, take) < 0)
                return mp_fail(mr, KL_HTTP_MP_ERR_HEADERS_TOO_LARGE);

            memcpy(mr->hdr + mr->hdr_len, mr->input, take);
            mr->hdr_len += take;
            mr->input_consumed = take;
            mp_apply_consume(mr);

            const char *end = mp_memmem(mr->hdr, mr->hdr_len, "\r\n\r\n", 4);
            if (!end) {
                /* No terminator yet. If we just filled hdr to the cap
                 * but more input is still pending, the headers exceed
                 * what the caller permitted. */
                if (cap_limit > 0 && mr->hdr_len >= cap_limit &&
                    mr->input_len > 0)
                    return mp_fail(mr, KL_HTTP_MP_ERR_HEADERS_TOO_LARGE);
                return mp_need_or_eof(mr);
            }

            /* Found the terminator. We may have copied bytes BEYOND it
             * (the take semantics drained at most `available` bytes;
             * the overshoot is whatever sits past hdr_end). Push that
             * tail back to the front of input so BODY processes it. */
            size_t hdr_end   = (size_t)(end - mr->hdr) + 4;
            size_t overshoot = mr->hdr_len - hdr_end;
            if (overshoot > 0) {
                if (mp_input_reserve(mr, overshoot) < 0)
                    return mp_fail(mr, KL_HTTP_MP_ERR_INPUT_OVERFLOW);
                if (mr->input_len > 0) {
                    memmove(mr->input + overshoot, mr->input, mr->input_len);
                }
                memcpy(mr->input, mr->hdr + hdr_end, overshoot);
                mr->input_len += overshoot;
            }

            mr->hdr_len = hdr_end;
            if (mp_parse_headers(mr) < 0)
                return mp_fail_or_keep(mr, KL_HTTP_MP_ERR_MALFORMED);

            mr->parts_begun++;
            mr->state = KL_HTTP_MP_S_BODY;
            mr->hdr_len = 0;  /* reuse hdr buffer for the next part */

            if (meta) {
                meta->name             = mr->cur_name;
                meta->name_len         = mr->cur_name_len;
                meta->filename         = mr->cur_filename;
                meta->filename_len     = mr->cur_filename_len;
                meta->content_type     = mr->cur_ctype;
                meta->content_type_len = mr->cur_ctype_len;
            }
            return KL_HTTP_MP_EVT_PART_BEGIN;
        }

        case KL_HTTP_MP_S_BODY: {
            if (mr->input_len == 0) return mp_need_or_eof(mr);

            const char *found = mp_memmem(mr->input, mr->input_len,
                                          mr->delimiter, mr->delimiter_len);
            if (found) {
                size_t body_len = (size_t)(found - mr->input);
                if (body_len > 0) {
                    /* Emit the body bytes up to the delimiter as PART_DATA.
                     * The delimiter itself remains in input; the next call
                     * will see it at offset 0 and emit PART_END. */
                    if (mr->config.max_part_size > 0 &&
                        (body_len > mr->config.max_part_size ||
                         mr->current_part_size >
                             mr->config.max_part_size - body_len))
                        return mp_fail(mr, KL_HTTP_MP_ERR_PART_TOO_LARGE);

                    mr->current_part_size += body_len;
                    if (data)     *data     = mr->input;
                    if (data_len) *data_len = body_len;
                    mr->input_consumed = body_len;
                    return KL_HTTP_MP_EVT_PART_DATA;
                }
                /* body_len == 0: delimiter is at the very front. Consume
                 * it and emit PART_END. */
                mr->input_consumed = mr->delimiter_len;
                mp_apply_consume(mr);
                mr->state = KL_HTTP_MP_S_AFTER_BOUNDARY;
                return KL_HTTP_MP_EVT_PART_END;
            }

            /* Delimiter not in input. Emit a safe prefix that cannot be
             * the start of a delimiter. */
            if (mr->input_len > mr->delimiter_len - 1) {
                size_t safe = mr->input_len - (mr->delimiter_len - 1);
                if (mr->config.max_part_size > 0 &&
                    (safe > mr->config.max_part_size ||
                     mr->current_part_size >
                         mr->config.max_part_size - safe))
                    return mp_fail(mr, KL_HTTP_MP_ERR_PART_TOO_LARGE);

                mr->current_part_size += safe;
                if (data)     *data     = mr->input;
                if (data_len) *data_len = safe;
                mr->input_consumed = safe;
                return KL_HTTP_MP_EVT_PART_DATA;
            }

            /* input could be a delimiter prefix — wait for more. */
            return mp_need_or_eof(mr);
        }

        default:
            return mp_fail(mr, KL_HTTP_MP_ERR_MALFORMED);
        }
    }
}

KlHttpMultipartErrorCode kl_http_multipart_last_error(const KlHttpBodyReader *br) {
    if (!br || br->on_data != mp_on_data) return KL_HTTP_MP_ERR_NONE;
    const KlHttpMultipartReader *mr = (const KlHttpMultipartReader *)br;
    return mr->last_error;
}

/* ── Factory ─────────────────────────────────────────────────────────── */

/* cppcheck-suppress constParameterPointer ; signature must match KlHttpBodyReaderFactory typedef */
KlHttpBodyReader *kl_http_body_reader_multipart(KlAllocator *alloc, const KlHttpRequest *req, void *user_data) {
    if (!alloc || !req) return NULL;

    size_t ct_len = 0;
    const char *ct = kl_http_request_header_len(req, "Content-Type", &ct_len);
    if (!ct) return NULL;

    const char *prefix = "multipart/form-data";
    const size_t prefix_len = 19;
    if (ct_len < prefix_len) return NULL;
    if (strncasecmp(ct, prefix, prefix_len) != 0) return NULL;
    /* Require a separator after the prefix so subtypes like
     * "multipart/form-data-extra" do not falsely match. */
    if (ct_len > prefix_len) {
        char next = ct[prefix_len];
        if (next != ';' && next != ' ' && next != '\t' &&
            next != '\r' && next != '\n')
            return NULL;
    }

    const char *bp = mp_find_param(ct, ct_len, "boundary=", 9);
    if (!bp) return NULL;
    size_t bp_remain = ct_len - (size_t)(bp - ct);

    const char *bnd;
    size_t      bnd_len;
    if (bp_remain > 0 && *bp == '"') {
        bp++;
        bp_remain--;
        const char *end = memchr(bp, '"', bp_remain);
        if (!end) return NULL;
        bnd     = bp;
        bnd_len = (size_t)(end - bp);
    } else {
        bnd = bp;
        bnd_len = 0;
        while (bnd_len < bp_remain && bnd[bnd_len] != ';' &&
               bnd[bnd_len] != ' '  && bnd[bnd_len] != '\t' &&
               bnd[bnd_len] != '\r' && bnd[bnd_len] != '\n')
            bnd_len++;
    }
    if (bnd_len == 0 || bnd_len > KL_HTTP_MP_MAX_BOUNDARY) return NULL;

    KlHttpMultipartReader *mr = kl_malloc(alloc, sizeof(*mr));
    if (!mr) return NULL;
    memset(mr, 0, sizeof(*mr));

    mr->base.on_data    = mp_on_data;
    mr->base.on_complete = mp_on_complete;
    mr->base.on_error   = mp_on_error;
    mr->base.destroy    = mp_destroy;
    mr->alloc           = alloc;

    mr->delimiter[0] = '\r';
    mr->delimiter[1] = '\n';
    mr->delimiter[2] = '-';
    mr->delimiter[3] = '-';
    memcpy(mr->delimiter + 4, bnd, bnd_len);
    mr->delimiter_len = 4 + bnd_len;

    mr->state = KL_HTTP_MP_S_PREAMBLE;
    mr->last_error = KL_HTTP_MP_ERR_NONE;

    if (user_data) {
        mr->config = *(const KlHttpMultipartConfig *)user_data;
    }
    /* All cap fields default to 0 = unlimited. The caller decides. */

    return &mr->base;
}
