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

/* An SSE parser ends a line at CR, LF or CRLF. Returns 1 if s[0..len) holds any of them. */
static int has_line_break(const char *s, size_t len) {
    return memchr(s, '\n', len) != NULL || memchr(s, '\r', len) != NULL;
}

/* Write `text` one line per field, each with `prefix`, splitting on CRLF, CR and LF alike, so no
 * line break in the caller's text can start a field it did not write. Empty text writes one
 * empty field. */
static int write_lines(KlHttpSse *sse, const char *prefix, size_t plen,
                       const char *text, size_t len) {
    const char *p = text;
    const char *end = text + len;
    if (len == 0) return write_field(sse, prefix, plen, "", 0);
    while (p < end) {
        const char *q = p;
        while (q < end && *q != '\n' && *q != '\r') q++;
        if (write_field(sse, prefix, plen, p, (size_t)(q - p)) < 0) return -1;
        if (q == end) break;
        if (*q == '\r' && q + 1 < end && q[1] == '\n') q++;   /* CRLF is one break */
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
    if (event) {
        if (write_field(sse, "event: ", 7, event, strlen(event)) < 0)
            return -1;
    }
    if (id) {
        if (write_field(sse, "id: ", 4, id, strlen(id)) < 0)
            return -1;
    }

    /* Each line of data gets its own "data: " field ("data: \n" for empty data). */
    if (write_lines(sse, "data: ", 6, data, data_len) < 0)
        return -1;

    /* Blank line terminates the event */
    if (sse->write_fn(sse->write_ctx, "\n", 1) < 0) return -1;
    return 0;
}

int kl_http_sse_comment(KlHttpSse *sse, const char *text, size_t len) {
    if (!sse) return -1;
    if (len > 0 && !text) return -1;
    return write_lines(sse, ": ", 2, text, len);   /* every line stays a comment */
}

int kl_http_sse_end(KlHttpSse *sse) {
    if (!sse) return -1;
    return kl_http_response_end_stream(sse->res);
}
