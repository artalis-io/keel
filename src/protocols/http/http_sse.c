#include <keel/http_sse.h>
#include "http_response_internal.h"
#include <string.h>

int kl_http_sse_begin(KlHttpResponse *res, KlHttpSse *sse) {
    if (!res || !sse) return -1;
    if (kl_http_response_header(res, "Content-Type", "text/event-stream") < 0)
        return -1;
    if (kl_http_response_header(res, "Cache-Control", "no-cache") < 0)
        return -1;
    if (kl_http_response_begin_stream(res, 200, &sse->write_fn, &sse->write_ctx) < 0)
        return -1;
    sse->res = res;
    return 0;
}

/* An SSE parser ends a line at CR, LF or CRLF. Returns 1 if s[0..len) holds any of them. */
static int has_line_break(const char *s, size_t len) {
    return memchr(s, '\n', len) != NULL || memchr(s, '\r', len) != NULL;
}

typedef struct {
    KlHttpSse *sse;
    KlIoVec iov[KL_HTTP_STREAM_IOV_MAX];
    int count;
} KlSseBatch;

static int batch_flush(KlSseBatch *batch) {
    if (batch->count == 0) return 0;
    if (batch->sse->write_fn == kl_http_response_stream_write) {
        if (kl_http_response_stream_writev(batch->sse->write_ctx,
                                           batch->iov, batch->count) < 0)
            return -1;
    } else {
        for (int i = 0; i < batch->count; i++) {
            if (batch->sse->write_fn(batch->sse->write_ctx, batch->iov[i].base,
                                     batch->iov[i].len) < 0)
                return -1;
        }
    }
    batch->count = 0;
    return 0;
}

static int batch_append(KlSseBatch *batch, const char *data, size_t len) {
    if (batch->count == KL_HTTP_STREAM_IOV_MAX && batch_flush(batch) < 0)
        return -1;
    batch->iov[batch->count].base = (void *)data;
    batch->iov[batch->count].len = len;
    batch->count++;
    return 0;
}

static int batch_field(KlSseBatch *batch, const char *prefix, size_t plen,
                       const char *value, size_t vlen) {
    if (batch_append(batch, prefix, plen) < 0 ||
        batch_append(batch, value, vlen) < 0 ||
        batch_append(batch, "\n", 1) < 0)
        return -1;
    return 0;
}

static int batch_lines(KlSseBatch *batch, const char *prefix, size_t plen,
                       const char *text, size_t len) {
    if (len == 0) return batch_field(batch, prefix, plen, "", 0);
    const char *p = text;
    const char *end = text + len;
    while (p < end) {
        const char *q = p;
        while (q < end && *q != '\n' && *q != '\r') q++;
        if (batch_field(batch, prefix, plen, p, (size_t)(q - p)) < 0) return -1;
        if (q == end) break;
        if (*q == '\r' && q + 1 < end && q[1] == '\n') q++;
        p = q + 1;
    }
    return 0;
}

int kl_http_sse_event(KlHttpSse *sse, const char *event,
                 const char *data, size_t data_len, const char *id) {
    if (!sse) return -1;
    if (data_len > 0 && !data) return -1;
    /* An event name or id is a single line: one with a line break would inject fields. */
    if (event && has_line_break(event, strlen(event))) return -1;
    if (id && has_line_break(id, strlen(id))) return -1;
    KlSseBatch batch = { .sse = sse, .count = 0 };
    if (event && batch_field(&batch, "event: ", 7, event, strlen(event)) < 0)
        return -1;
    if (id && batch_field(&batch, "id: ", 4, id, strlen(id)) < 0)
        return -1;
    if (batch_lines(&batch, "data: ", 6, data, data_len) < 0 ||
        batch_append(&batch, "\n", 1) < 0)
        return -1;
    return batch_flush(&batch);
}

int kl_http_sse_comment(KlHttpSse *sse, const char *text, size_t len) {
    if (!sse) return -1;
    if (len > 0 && !text) return -1;
    KlSseBatch batch = { .sse = sse, .count = 0 };
    if (batch_lines(&batch, ": ", 2, text, len) < 0)
        return -1;
    return batch_flush(&batch);
}

int kl_http_sse_end(KlHttpSse *sse) {
    if (!sse) return -1;
    return kl_http_response_end_stream(sse->res);
}
