/*
 * response_parser_llhttp.c — llhttp backend for KlHttp1ResponseParser vtable
 *
 * Uses llhttp in HTTP_RESPONSE mode to parse response status, headers,
 * and body. Accumulates headers and body into the KlClientResponse struct.
 *
 * All dynamic memory goes through the KlAllocator vtable.
 */

#include <keel/client.h>
#include <keel/http1_parser.h>
#include <keel/allocator.h>
#include "llhttp.h"

#include <stdint.h>
#include <string.h>

#define KL_MAX_RESPONSE_HEADERS 64
#define KL_MAX_HEADER_SIZE      8192

/* ── Parser state ────────────────────────────────────────────────── */

typedef struct {
    KlHttp1ResponseParser base;         /* vtable — must be first */
    llhttp_t         parser;
    llhttp_settings_t settings;
    KlAllocator     *alloc;

    /* Accumulation state */
    KlClientResponse *resp;
    size_t           max_body;
    int              complete;
    int              error;

    /* Header accumulation */
    char            *hdr_name;
    size_t           hdr_name_len;
    size_t           hdr_name_cap;
    char            *hdr_value;
    size_t           hdr_value_len;
    size_t           hdr_value_cap;

    /* Body accumulation */
    char            *body;
    size_t           body_len;
    size_t           body_cap;

    /* Header array */
    KlClientHeader  *headers;
    int              num_headers;
    int              headers_cap;

    /* Streaming callbacks (NULL = accumulate as before) */
    KlClientBodyFn     on_body_cb;
    KlClientHeadersFn  on_headers_cb;
    void             (*on_complete_cb)(void *user_data);
    void              *stream_data;
    size_t             body_streamed;   /**< total bytes forwarded (for size limit) */
} RespLlhttpParser;

/* ── String accumulation helpers ─────────────────────────────────── */

static int accum_append(KlAllocator *alloc,
                         char **buf, size_t *len, size_t *cap,
                         const char *data, size_t data_len)
{
    if (data_len == 0)
        return 0;   /* no-op; also avoids NULL+0 when *buf is unallocated */
    if (data_len > SIZE_MAX - *len)
        return -1;
    size_t needed = *len + data_len;
    if (needed > *cap) {
        size_t new_cap = *cap ? *cap * 2 : 256;
        while (new_cap < needed) {
            if (new_cap > SIZE_MAX / 2)
                return -1;
            new_cap *= 2;
        }
        char *new_buf = kl_realloc(alloc, *buf, *cap, new_cap);
        if (!new_buf)
            return -1;
        *buf = new_buf;
        *cap = new_cap;
    }
    memcpy(*buf + *len, data, data_len);
    *len += data_len;
    return 0;
}

/* ── Flush accumulated header ────────────────────────────────────── */

static int flush_header(RespLlhttpParser *p)
{
    if (p->hdr_name_len == 0)
        return 0;

    if (p->num_headers >= p->headers_cap) {
        if (p->num_headers >= KL_MAX_RESPONSE_HEADERS)
            return -1;
        int new_cap = p->headers_cap ? p->headers_cap * 2 : 16;
        if (new_cap > KL_MAX_RESPONSE_HEADERS)
            new_cap = KL_MAX_RESPONSE_HEADERS;
        size_t old_size = (size_t)p->headers_cap * sizeof(KlClientHeader);
        size_t new_size = (size_t)new_cap * sizeof(KlClientHeader);
        KlClientHeader *new_hdrs = kl_realloc(p->alloc, p->headers,
                                                old_size, new_size);
        if (!new_hdrs)
            return -1;
        p->headers = new_hdrs;
        p->headers_cap = new_cap;
    }

    /* NUL-terminate name and value */
    if (accum_append(p->alloc, &p->hdr_name, &p->hdr_name_len,
                      &p->hdr_name_cap, "\0", 1) != 0)
        return -1;
    if (accum_append(p->alloc, &p->hdr_value, &p->hdr_value_len,
                      &p->hdr_value_cap, "\0", 1) != 0)
        return -1;

    /* Make exact-sized copies */
    char *name_copy = kl_malloc(p->alloc, p->hdr_name_len);
    if (!name_copy)
        return -1;
    memcpy(name_copy, p->hdr_name, p->hdr_name_len);

    char *value_copy = kl_malloc(p->alloc, p->hdr_value_len);
    if (!value_copy) {
        kl_free(p->alloc, name_copy, p->hdr_name_len);
        return -1;
    }
    memcpy(value_copy, p->hdr_value, p->hdr_value_len);

    /* Free accumulation buffers */
    kl_free(p->alloc, p->hdr_name, p->hdr_name_cap);
    kl_free(p->alloc, p->hdr_value, p->hdr_value_cap);

    /* Store in headers array */
    p->headers[p->num_headers].name = name_copy;
    p->headers[p->num_headers].value = value_copy;
    p->num_headers++;

    /* Reset accumulators */
    p->hdr_name = NULL;
    p->hdr_name_len = 0;
    p->hdr_name_cap = 0;
    p->hdr_value = NULL;
    p->hdr_value_len = 0;
    p->hdr_value_cap = 0;

    return 0;
}

/* ── llhttp callbacks ────────────────────────────────────────────── */

static int resp_on_status(llhttp_t *parser, const char *at, size_t len)
{
    (void)at; (void)len;
    RespLlhttpParser *p = (RespLlhttpParser *)parser->data;
    p->resp->status = (int)parser->status_code;
    return 0;
}

static int resp_on_header_field(llhttp_t *parser, const char *at, size_t len)
{
    RespLlhttpParser *p = (RespLlhttpParser *)parser->data;

    /* A completed header (name+value) is flushed at on_header_value_complete,
     * so the accumulators are empty here even after an empty-valued header —
     * no heuristic flush needed (which would merge empty-valued headers). */
    if (p->hdr_name_len + len > KL_MAX_HEADER_SIZE)
        return -1;

    return accum_append(p->alloc, &p->hdr_name, &p->hdr_name_len,
                         &p->hdr_name_cap, at, len);
}

static int resp_on_header_value(llhttp_t *parser, const char *at, size_t len)
{
    RespLlhttpParser *p = (RespLlhttpParser *)parser->data;

    if (p->hdr_value_len + len > KL_MAX_HEADER_SIZE)
        return -1;

    return accum_append(p->alloc, &p->hdr_value, &p->hdr_value_len,
                         &p->hdr_value_cap, at, len);
}

static int resp_on_header_value_complete(llhttp_t *parser)
{
    RespLlhttpParser *p = (RespLlhttpParser *)parser->data;
    /* llhttp signals the end of each header's value here — including empty
     * values — so this is the correct boundary to commit name+value. */
    return flush_header(p);
}

static int resp_on_headers_complete(llhttp_t *parser)
{
    RespLlhttpParser *p = (RespLlhttpParser *)parser->data;

    if (p->hdr_name_len > 0) {
        if (flush_header(p) != 0)
            return -1;
    }

    p->resp->status = (int)parser->status_code;

    /* Streaming: invoke on_headers callback */
    if (p->on_headers_cb) {
        if (p->on_headers_cb(p->resp->status, p->headers, p->num_headers,
                              p->stream_data) != 0) {
            p->error = 1;
            return -1;
        }
    }

    return 0;
}

static int resp_on_body(llhttp_t *parser, const char *at, size_t len)
{
    RespLlhttpParser *p = (RespLlhttpParser *)parser->data;

    /* Streaming mode: forward chunks to callback */
    if (p->on_body_cb) {
        if (p->max_body > 0 && p->body_streamed + len > p->max_body) {
            p->error = 1;
            return -1;
        }
        p->body_streamed += len;
        return p->on_body_cb(at, len, p->stream_data);
    }

    /* Buffer mode: accumulate */
    if (p->max_body > 0 && p->body_len + len > p->max_body) {
        p->error = 1;
        return -1;
    }

    return accum_append(p->alloc, &p->body, &p->body_len,
                         &p->body_cap, at, len);
}

static int resp_on_message_complete(llhttp_t *parser)
{
    RespLlhttpParser *p = (RespLlhttpParser *)parser->data;
    p->complete = 1;

    /* Streaming: invoke on_complete callback */
    if (p->on_complete_cb)
        p->on_complete_cb(p->stream_data);

    return HPE_PAUSED;
}

/* ── Transfer parser data to response ─────────────────────────────── */

static KlHttp1ParseResult transfer_to_response(RespLlhttpParser *p,
                                            KlClientResponse *resp)
{
    /* Streaming mode: body was forwarded via callback, skip body transfer */
    if (p->on_body_cb) {
        resp->body = NULL;
        resp->body_len = 0;
    } else {
        /* NUL-terminate body and realloc to exact size */
        if (p->body && p->body_len > 0) {
            char *exact = kl_realloc(p->alloc, p->body, p->body_cap,
                                      p->body_len + 1);
            if (!exact)
                return KL_HTTP1_PARSE_ERROR;
            exact[p->body_len] = '\0';
            p->body = exact;
        }

        resp->body = p->body;
        resp->body_len = p->body_len;

        p->body = NULL;
        p->body_len = 0;
        p->body_cap = 0;
    }

    /* Make exact-sized copy of headers array */
    if (p->headers && p->num_headers > 0) {
        size_t exact_size = (size_t)p->num_headers * sizeof(KlClientHeader);
        size_t old_size = (size_t)p->headers_cap * sizeof(KlClientHeader);
        KlClientHeader *exact_hdrs = kl_malloc(p->alloc, exact_size);
        if (!exact_hdrs)
            return KL_HTTP1_PARSE_ERROR;
        memcpy(exact_hdrs, p->headers, exact_size);
        kl_free(p->alloc, p->headers, old_size);
        p->headers = exact_hdrs;
    }

    /* Transfer ownership to response */
    resp->headers = p->headers;
    resp->num_headers = p->num_headers;
    resp->alloc = *p->alloc;  /* copy by value — safe after caller returns */

    /* Clear parser pointers */
    p->headers = NULL;
    p->num_headers = 0;
    p->headers_cap = 0;

    return KL_HTTP1_PARSE_OK;
}

/* ── KlHttp1ResponseParser vtable ────────────────────────────────────── */

static KlHttp1ParseResult resp_parser_parse(KlHttp1ResponseParser *self,
                                         KlClientResponse *resp,
                                         const char *buf, size_t len,
                                         size_t *consumed)
{
    RespLlhttpParser *p = (RespLlhttpParser *)self;
    p->resp = resp;

    enum llhttp_errno err = llhttp_execute(&p->parser, buf, len);

    /* HPE_OK: all data consumed. Pause/error: error_pos marks stop point */
    if (err == HPE_OK)
        *consumed = len;
    else
        *consumed = (size_t)(llhttp_get_error_pos(&p->parser) - buf);

    if (p->error)
        return KL_HTTP1_PARSE_ERROR;

    if (p->complete)
        return transfer_to_response(p, resp);

    if (err == HPE_PAUSED) {
        llhttp_resume(&p->parser);
        return transfer_to_response(p, resp);
    }

    if (err != HPE_OK)
        return KL_HTTP1_PARSE_ERROR;

    return KL_HTTP1_PARSE_INCOMPLETE;
}

static void resp_parser_reset(KlHttp1ResponseParser *self)
{
    RespLlhttpParser *p = (RespLlhttpParser *)self;
    llhttp_reset(&p->parser);
    p->complete = 0;
    p->error = 0;

    kl_free(p->alloc, p->hdr_name, p->hdr_name_cap);
    kl_free(p->alloc, p->hdr_value, p->hdr_value_cap);
    kl_free(p->alloc, p->body, p->body_cap);

    p->hdr_name = NULL;
    p->hdr_name_len = 0;
    p->hdr_name_cap = 0;
    p->hdr_value = NULL;
    p->hdr_value_len = 0;
    p->hdr_value_cap = 0;
    p->body = NULL;
    p->body_len = 0;
    p->body_cap = 0;

    for (int i = 0; i < p->num_headers; i++) {
        kl_free(p->alloc, (char *)p->headers[i].name,
                strlen(p->headers[i].name) + 1);
        kl_free(p->alloc, (char *)p->headers[i].value,
                strlen(p->headers[i].value) + 1);
    }
    kl_free(p->alloc, p->headers,
            (size_t)p->headers_cap * sizeof(KlClientHeader));
    p->headers = NULL;
    p->num_headers = 0;
    p->headers_cap = 0;
}

static void resp_parser_destroy(KlHttp1ResponseParser *self)
{
    if (!self)
        return;

    RespLlhttpParser *p = (RespLlhttpParser *)self;
    KlAllocator *alloc = p->alloc;

    kl_free(alloc, p->hdr_name, p->hdr_name_cap);
    kl_free(alloc, p->hdr_value, p->hdr_value_cap);
    kl_free(alloc, p->body, p->body_cap);

    for (int i = 0; i < p->num_headers; i++) {
        kl_free(alloc, (char *)p->headers[i].name,
                strlen(p->headers[i].name) + 1);
        kl_free(alloc, (char *)p->headers[i].value,
                strlen(p->headers[i].value) + 1);
    }
    kl_free(alloc, p->headers,
            (size_t)p->headers_cap * sizeof(KlClientHeader));

    kl_free(alloc, p, sizeof(RespLlhttpParser));
}

/* ── Factory ─────────────────────────────────────────────────────── */

static KlHttp1ResponseParser *create_parser(size_t max_response_size,
                                         KlAllocator *alloc,
                                         KlClientBodyFn on_body,
                                         KlClientHeadersFn on_headers,
                                         void (*on_complete)(void *),
                                         void *stream_user_data)
{
    RespLlhttpParser *p = kl_malloc(alloc, sizeof(*p));
    if (!p)
        return NULL;
    memset(p, 0, sizeof(*p));

    p->alloc = alloc;

    /* Set up vtable */
    p->base.parse   = resp_parser_parse;
    p->base.reset   = resp_parser_reset;
    p->base.destroy = resp_parser_destroy;

    /* Configure llhttp callbacks */
    llhttp_settings_init(&p->settings);
    p->settings.on_status           = resp_on_status;
    p->settings.on_header_field          = resp_on_header_field;
    p->settings.on_header_value          = resp_on_header_value;
    p->settings.on_header_value_complete = resp_on_header_value_complete;
    p->settings.on_headers_complete      = resp_on_headers_complete;
    p->settings.on_body             = resp_on_body;
    p->settings.on_message_complete = resp_on_message_complete;

    /* Initialize parser in HTTP_RESPONSE mode */
    llhttp_init(&p->parser, HTTP_RESPONSE, &p->settings);
    p->parser.data = p;

    p->max_body = max_response_size;

    /* Streaming callbacks */
    p->on_body_cb     = on_body;
    p->on_headers_cb  = on_headers;
    p->on_complete_cb = on_complete;
    p->stream_data    = stream_user_data;

    return &p->base;
}

KlHttp1ResponseParser *kl_http1_response_parser_llhttp(size_t max_response_size,
                                             KlAllocator *alloc)
{
    return create_parser(max_response_size, alloc, NULL, NULL, NULL, NULL);
}

KlHttp1ResponseParser *kl_http1_response_parser_llhttp_s(size_t max_response_size,
                                                KlAllocator *alloc,
                                                KlClientBodyFn on_body,
                                                KlClientHeadersFn on_headers,
                                                void (*on_complete)(void *),
                                                void *stream_user_data)
{
    return create_parser(max_response_size, alloc,
                          on_body, on_headers, on_complete, stream_user_data);
}
