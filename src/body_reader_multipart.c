#include <keel/body_reader_multipart.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <limits.h>

/* ── Helpers ────────────────────────────────────────────────────────── */

/* O(n*m) naive search — acceptable since needle (boundary) is ≤76 bytes */
static void *mp_memmem(const void *hay, size_t hlen,
                       const void *needle, size_t nlen) {
    if (nlen == 0) return (void *)hay;
    if (nlen > hlen) return NULL;
    const char *h = hay;
    const char *n = needle;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        if (memcmp(h + i, n, nlen) == 0) return (void *)(h + i);
    }
    return NULL;
}

static char *mp_strdup(KlAllocator *alloc, const char *s, size_t len) {
    char *d = kl_malloc(alloc, len + 1);
    if (!d) return NULL;
    memcpy(d, s, len);
    d[len] = '\0';
    return d;
}

/* ── Part management ────────────────────────────────────────────────── */

static int mp_add_part(KlMultipartReader *mr) {
    if (mr->config.max_parts > 0 && mr->num_parts >= mr->config.max_parts)
        return -1;

    if (mr->num_parts >= mr->parts_cap) {
        if (mr->parts_cap > INT_MAX / 2) return -1;
        int new_cap = mr->parts_cap ? mr->parts_cap * 2 : 4;
        size_t old_sz = sizeof(KlMultipartPart) * (size_t)mr->parts_cap;
        size_t new_sz = sizeof(KlMultipartPart) * (size_t)new_cap;
        KlMultipartPart *p = kl_realloc(mr->alloc, mr->parts, old_sz, new_sz);
        if (!p) return -1;
        mr->parts = p;
        mr->parts_cap = new_cap;
    }

    KlMultipartPart *part = &mr->parts[mr->num_parts];
    memset(part, 0, sizeof(*part));
    mr->num_parts++;
    return 0;
}

static int mp_append_data(KlMultipartReader *mr, const char *data, size_t len) {
    if (len == 0) return 0;
    KlMultipartPart *part = &mr->parts[mr->num_parts - 1];

    /* C-2: Guard against size_t wrap on additions */
    if (len > SIZE_MAX - part->data_len) return -1;
    if (len > SIZE_MAX - mr->total_received) return -1;

    if (mr->config.max_part_size > 0 &&
        part->data_len + len > mr->config.max_part_size)
        return -1;
    if (mr->config.max_total_size > 0 &&
        mr->total_received + len > mr->config.max_total_size)
        return -1;

    size_t need = part->data_len + len;
    if (need > part->data_cap) {
        /* C-3: Guard against size_t wrap on capacity doubling */
        size_t new_cap;
        if (part->data_cap == 0) {
            new_cap = 1024;
        } else if (part->data_cap > SIZE_MAX / 2) {
            new_cap = need;
        } else {
            new_cap = part->data_cap * 2;
        }
        if (new_cap < need) new_cap = need;
        char *nd = kl_realloc(mr->alloc, part->data, part->data_cap, new_cap);
        if (!nd) return -1;
        part->data = nd;
        part->data_cap = new_cap;
    }

    memcpy(part->data + part->data_len, data, len);
    part->data_len += len;
    mr->total_received += len;
    return 0;
}

/* ── Header parsing ─────────────────────────────────────────────────── */

static int mp_parse_disposition(KlMultipartReader *mr, const char *val,
                                size_t vlen) {
    KlMultipartPart *part = &mr->parts[mr->num_parts - 1];

    const char *np = mp_memmem(val, vlen, "name=", 5);
    if (!np) return -1;
    np += 5;
    size_t remain = vlen - (size_t)(np - val);

    if (remain > 0 && *np == '"') {
        np++;
        remain--;
        const char *end = memchr(np, '"', remain);
        if (!end) return -1;
        part->name = mp_strdup(mr->alloc, np, (size_t)(end - np));
    } else {
        const char *end = memchr(np, ';', remain);
        size_t nlen = end ? (size_t)(end - np) : remain;
        part->name = mp_strdup(mr->alloc, np, nlen);
    }
    if (!part->name) return -1;

    const char *fp = mp_memmem(val, vlen, "filename=", 9);
    if (fp) {
        fp += 9;
        size_t fremain = vlen - (size_t)(fp - val);
        if (fremain > 0 && *fp == '"') {
            fp++;
            fremain--;
            const char *end = memchr(fp, '"', fremain);
            if (end)
                part->filename = mp_strdup(mr->alloc, fp, (size_t)(end - fp));
        } else {
            const char *end = memchr(fp, ';', fremain);
            size_t flen = end ? (size_t)(end - fp) : fremain;
            part->filename = mp_strdup(mr->alloc, fp, flen);
        }
    }

    return 0;
}

static int mp_parse_headers(KlMultipartReader *mr) {
    const char *buf = mr->hdr_buf;
    size_t len = mr->hdr_len;
    int got_disposition = 0;
    KlMultipartPart *part = &mr->parts[mr->num_parts - 1];

    while (len > 0) {
        const char *eol = mp_memmem(buf, len, "\r\n", 2);
        size_t line_len = eol ? (size_t)(eol - buf) : len;

        if (line_len == 0) {
            break;
        }

        const char *colon = memchr(buf, ':', line_len);
        if (colon) {
            size_t name_len = (size_t)(colon - buf);
            const char *val = colon + 1;
            size_t val_len = line_len - name_len - 1;
            while (val_len > 0 && *val == ' ') { val++; val_len--; }

            if (name_len == 19 &&
                strncasecmp(buf, "Content-Disposition", 19) == 0) {
                if (mp_parse_disposition(mr, val, val_len) < 0)
                    return -1;
                got_disposition = 1;
            } else if (name_len == 12 &&
                       strncasecmp(buf, "Content-Type", 12) == 0) {
                part->content_type = mp_strdup(mr->alloc, val, val_len);
                if (!part->content_type) return -1;
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

/* ── State machine ──────────────────────────────────────────────────── */

/*
 * Process a contiguous buffer through the state machine.
 * Returns 0 on success, -1 on error.
 *
 * Recursion depth is bounded to 2: PREAMBLE and HEADERS each recurse
 * at most once into mp_process with leftover data, transitioning to a
 * different state (AFTER_BOUNDARY or BODY) that does not recurse.
 */
static int mp_process(KlMultipartReader *mr, const char *data, size_t len) {
    while (len > 0 && mr->state != KL_MP_DONE && mr->state != KL_MP_ERROR) {

        if (mr->state == KL_MP_PREAMBLE) {
            /*
             * Accumulate into hdr_buf to handle initial boundary
             * split across multiple on_data calls.
             */
            size_t space = sizeof(mr->hdr_buf) - mr->hdr_len;
            size_t take = len < space ? len : space;
            if (space == 0) { mr->state = KL_MP_ERROR; return -1; }
            memcpy(mr->hdr_buf + mr->hdr_len, data, take);
            mr->hdr_len += take;
            data += take;
            len -= take;

            /* Search accumulated data for "--boundary" */
            const char *needle = mr->delimiter + 2; /* "--boundary" */
            size_t nlen = mr->delimiter_len - 2;
            const char *found = mp_memmem(mr->hdr_buf, mr->hdr_len,
                                          needle, nlen);
            if (!found) {
                /* Compact: keep only trailing bytes that could be partial */
                if (mr->hdr_len >= nlen) {
                    size_t keep = nlen - 1;
                    memmove(mr->hdr_buf,
                            mr->hdr_buf + mr->hdr_len - keep, keep);
                    mr->hdr_len = keep;
                }
                /* If more input data remains, loop to accumulate more */
                continue;
            }

            /* Boundary found. Copy remaining bytes to stack buffer,
             * reset hdr_buf, and process remainder recursively. */
            size_t after = (size_t)(found - mr->hdr_buf) + nlen;
            size_t buf_remain = mr->hdr_len - after;
            /* buf_remain <= hdr_len <= sizeof(hdr_buf) == 2048, so
             * leftover is always large enough. Clamp defensively. */
            char leftover[2048];
            if (buf_remain > sizeof(leftover))
                buf_remain = sizeof(leftover);
            if (buf_remain > 0)
                memcpy(leftover, mr->hdr_buf + after, buf_remain);
            mr->hdr_len = 0;

            mr->state = KL_MP_AFTER_BOUNDARY;
            mr->overlap_len = 0;

            /* Process leftover from hdr_buf */
            if (buf_remain > 0) {
                int rc = mp_process(mr, leftover, buf_remain);
                if (rc < 0) return rc;
            }
            /* Continue main loop with remaining input data */
            continue;

        } else if (mr->state == KL_MP_AFTER_BOUNDARY) {
            /*
             * Accumulate 2 bytes in overlap to determine:
             *   \r\n → next part (HEADERS)
             *   --   → end (DONE)
             */
            while (mr->overlap_len < 2 && len > 0) {
                mr->overlap[mr->overlap_len++] = *data;
                data++;
                len--;
            }
            if (mr->overlap_len < 2) return 0; /* need more data */

            if (mr->overlap[0] == '-' && mr->overlap[1] == '-') {
                mr->state = KL_MP_DONE;
                mr->overlap_len = 0;
                return 0;
            }
            if (mr->overlap[0] == '\r' && mr->overlap[1] == '\n') {
                mr->overlap_len = 0;
                mr->hdr_len = 0;
                if (mp_add_part(mr) < 0) {
                    mr->state = KL_MP_ERROR;
                    return -1;
                }
                mr->state = KL_MP_HEADERS;
                continue;
            }
            mr->state = KL_MP_ERROR;
            mr->overlap_len = 0;
            return -1;

        } else if (mr->state == KL_MP_HEADERS) {
            /* Accumulate into hdr_buf, then search for \r\n\r\n.
             * This handles \r\n\r\n spanning multiple on_data calls. */
            size_t space = sizeof(mr->hdr_buf) - mr->hdr_len;
            size_t take = len < space ? len : space;
            if (space == 0) { mr->state = KL_MP_ERROR; return -1; }
            memcpy(mr->hdr_buf + mr->hdr_len, data, take);
            mr->hdr_len += take;
            data += take;
            len -= take;

            const char *end = mp_memmem(mr->hdr_buf, mr->hdr_len,
                                        "\r\n\r\n", 4);
            if (!end) continue; /* accumulate more */

            /* Found. Everything after \r\n\r\n is body data. */
            size_t hdr_end = (size_t)(end - mr->hdr_buf) + 4;
            size_t body_leftover = mr->hdr_len - hdr_end;

            /* Parse headers (only the header portion) */
            mr->hdr_len = hdr_end;
            if (mp_parse_headers(mr) < 0) {
                mr->state = KL_MP_ERROR;
                return -1;
            }

            mr->state = KL_MP_BODY;
            mr->hdr_len = 0;

            /* Process body data that was in hdr_buf after the headers.
             * body_leftover <= hdr_len <= sizeof(hdr_buf) == 2048. */
            if (body_leftover > 0) {
                char leftover[2048];
                memcpy(leftover, mr->hdr_buf + hdr_end, body_leftover);
                /* Restore hdr_len info was already consumed; leftover
                 * has been copied out so hdr_buf is free for reuse */
                int rc = mp_process(mr, leftover, body_leftover);
                if (rc < 0) return rc;
            }
            continue;

        } else if (mr->state == KL_MP_BODY) {
            const char *found = mp_memmem(data, len, mr->delimiter,
                                          mr->delimiter_len);
            if (found) {
                size_t body_len = (size_t)(found - data);
                if (body_len > 0) {
                    if (mp_append_data(mr, data, body_len) < 0) {
                        mr->state = KL_MP_ERROR;
                        return -1;
                    }
                }
                data += body_len + mr->delimiter_len;
                len -= body_len + mr->delimiter_len;
                mr->state = KL_MP_AFTER_BOUNDARY;
                mr->overlap_len = 0;
                continue;
            } else {
                /* No boundary — emit safe prefix, save trailing overlap */
                size_t safe = 0;
                if (len > mr->delimiter_len - 1)
                    safe = len - (mr->delimiter_len - 1);

                if (safe > 0) {
                    if (mp_append_data(mr, data, safe) < 0) {
                        mr->state = KL_MP_ERROR;
                        return -1;
                    }
                    data += safe;
                    len -= safe;
                }

                /* len <= delimiter_len - 1 <= KL_MP_MAX_BOUNDARY + 5,
                 * which fits in overlap[KL_MP_MAX_BOUNDARY + 6]. Clamp defensively. */
                if (len > sizeof(mr->overlap))
                    len = sizeof(mr->overlap);
                memcpy(mr->overlap, data, len);
                mr->overlap_len = len;
                return 0;
            }
        }
    }

    return 0;
}

/* ── vtable callbacks ───────────────────────────────────────────────── */

static int mp_on_data(KlBodyReader *self, const char *data, size_t len) {
    KlMultipartReader *mr = (KlMultipartReader *)self;

    if (mr->state == KL_MP_DONE || mr->state == KL_MP_ERROR)
        return mr->state == KL_MP_ERROR ? -1 : 0;

    if (mr->overlap_len > 0 && mr->state == KL_MP_BODY) {
        /*
         * Overlap bytes were held back from the previous on_data because
         * they could be the start of a boundary.  Concatenate with new
         * data and process as one contiguous buffer so mp_process can
         * detect boundaries that span chunks.
         */
        size_t combined_len = mr->overlap_len + len;
        if (combined_len < mr->overlap_len) {
            mr->state = KL_MP_ERROR;
            return -1;
        }
        char stack_buf[KL_MP_MAX_BOUNDARY + 6 + 8192];
        char *combined = stack_buf;
        int heap = 0;

        if (combined_len > sizeof(stack_buf)) {
            combined = kl_malloc(mr->alloc, combined_len);
            if (!combined) { mr->state = KL_MP_ERROR; return -1; }
            heap = 1;
        }

        memcpy(combined, mr->overlap, mr->overlap_len);
        memcpy(combined + mr->overlap_len, data, len);
        mr->overlap_len = 0;

        int rc = mp_process(mr, combined, combined_len);
        if (heap) kl_free(mr->alloc, combined, combined_len);
        if (rc < 0) return -1;
    } else if (len > 0) {
        if (mp_process(mr, data, len) < 0) return -1;
    }

    return mr->state == KL_MP_ERROR ? -1 : 0;
}

static void mp_on_complete(KlBodyReader *self) {
    KlMultipartReader *mr = (KlMultipartReader *)self;

    if (mr->overlap_len > 0 && mr->state == KL_MP_BODY && mr->num_parts > 0) {
        if (mp_append_data(mr, mr->overlap, mr->overlap_len) < 0) {
            mr->state = KL_MP_ERROR;
            mr->overlap_len = 0;
            return;
        }
        mr->overlap_len = 0;
    }

    mr->state = KL_MP_DONE;
}

static void mp_on_error(KlBodyReader *self) {
    KlMultipartReader *mr = (KlMultipartReader *)self;
    mr->state = KL_MP_ERROR;
}

static void mp_destroy(KlBodyReader *self) {
    KlMultipartReader *mr = (KlMultipartReader *)self;

    for (int i = 0; i < mr->num_parts; i++) {
        KlMultipartPart *p = &mr->parts[i];
        if (p->name)
            kl_free(mr->alloc, (void *)p->name, strlen(p->name) + 1);
        if (p->filename)
            kl_free(mr->alloc, (void *)p->filename, strlen(p->filename) + 1);
        if (p->content_type)
            kl_free(mr->alloc, (void *)p->content_type,
                    strlen(p->content_type) + 1);
        if (p->data)
            kl_free(mr->alloc, p->data, p->data_cap);
    }
    if (mr->parts)
        kl_free(mr->alloc, mr->parts,
                sizeof(KlMultipartPart) * (size_t)mr->parts_cap);
    kl_free(mr->alloc, mr, sizeof(KlMultipartReader));
}

/* ── Factory ────────────────────────────────────────────────────────── */

KlBodyReader *kl_body_reader_multipart(KlAllocator *alloc, const KlRequest *req,
                                        void *user_data) {
    size_t ct_len = 0;
    const char *ct = kl_request_header_len(req, "Content-Type", &ct_len);
    if (!ct) return NULL;

    const char *prefix = "multipart/form-data";
    size_t prefix_len = 19;
    if (ct_len < prefix_len) return NULL;
    if (strncasecmp(ct, prefix, prefix_len) != 0) return NULL;

    const char *bp = mp_memmem(ct, ct_len, "boundary=", 9);
    if (!bp) return NULL;
    bp += 9;
    size_t bp_remain = ct_len - (size_t)(bp - ct);

    const char *bnd_start;
    size_t bnd_len;

    if (bp_remain > 0 && *bp == '"') {
        bp++;
        bp_remain--;
        const char *end = memchr(bp, '"', bp_remain);
        if (!end) return NULL;
        bnd_start = bp;
        bnd_len = (size_t)(end - bp);
    } else {
        bnd_start = bp;
        bnd_len = 0;
        while (bnd_len < bp_remain && bnd_start[bnd_len] != ';' &&
               bnd_start[bnd_len] != ' ' && bnd_start[bnd_len] != '\r' &&
               bnd_start[bnd_len] != '\n')
            bnd_len++;
    }

    if (bnd_len == 0 || bnd_len > KL_MP_MAX_BOUNDARY) return NULL;

    KlMultipartReader *mr = kl_malloc(alloc, sizeof(KlMultipartReader));
    if (!mr) return NULL;
    memset(mr, 0, sizeof(*mr));

    mr->base.on_data = mp_on_data;
    mr->base.on_complete = mp_on_complete;
    mr->base.on_error = mp_on_error;
    mr->base.destroy = mp_destroy;
    mr->alloc = alloc;

    mr->delimiter[0] = '\r';
    mr->delimiter[1] = '\n';
    mr->delimiter[2] = '-';
    mr->delimiter[3] = '-';
    memcpy(mr->delimiter + 4, bnd_start, bnd_len);
    mr->delimiter_len = 4 + bnd_len;

    mr->state = KL_MP_PREAMBLE;

    if (user_data)
        mr->config = *(KlMultipartConfig *)user_data;

    return &mr->base;
}
