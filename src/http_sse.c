#include <keel/http_sse.h>
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

/* Helper: write a field prefix + value + newline.  Returns -1 on error. */
static int write_field(KlHttpSse *sse, const char *prefix, size_t plen,
                       const char *value, size_t vlen) {
    if (sse->write_fn(sse->write_ctx, prefix, plen) < 0) return -1;
    if (sse->write_fn(sse->write_ctx, value, vlen) < 0) return -1;
    if (sse->write_fn(sse->write_ctx, "\n", 1) < 0) return -1;
    return 0;
}

int kl_http_sse_event(KlHttpSse *sse, const char *event,
                 const char *data, size_t data_len, const char *id) {
    if (!sse) return -1;
    if (data_len > 0 && !data) return -1;
    if (event) {
        if (write_field(sse, "event: ", 7, event, strlen(event)) < 0)
            return -1;
    }
    if (id) {
        if (write_field(sse, "id: ", 4, id, strlen(id)) < 0)
            return -1;
    }

    /* Split data on '\n' — each line gets "data: " prefix */
    const char *p = data;
    const char *end = data + data_len;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t line_len = nl ? (size_t)(nl - p) : (size_t)(end - p);
        if (write_field(sse, "data: ", 6, p, line_len) < 0)
            return -1;
        p += line_len + (nl ? 1 : line_len);
    }

    /* Empty data → single "data: \n" */
    if (data_len == 0) {
        if (write_field(sse, "data: ", 6, "", 0) < 0)
            return -1;
    }

    /* Blank line terminates the event */
    if (sse->write_fn(sse->write_ctx, "\n", 1) < 0) return -1;
    return 0;
}

int kl_http_sse_comment(KlHttpSse *sse, const char *text, size_t len) {
    if (!sse) return -1;
    if (len > 0 && !text) return -1;
    return write_field(sse, ": ", 2, text, len);
}

int kl_http_sse_end(KlHttpSse *sse) {
    if (!sse) return -1;
    return kl_http_response_end_stream(sse->res);
}
