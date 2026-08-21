#include <keel/http1_parser.h>
#include <keel/http_body_reader.h>
#include <string.h>
#include "llhttp.h"

/*
 * llhttp parser backend for KEEL.
 *
 * Two-phase parsing:
 *   1. Parse headers → return KL_HTTP1_PARSE_HEADERS_OK (paused after headers)
 *   2. Resume parsing body → forward chunks to body_reader → KL_HTTP1_PARSE_OK
 */

typedef struct {
    KlHttp1RequestParser base;          /* vtable — must be first */
    KlAllocator *alloc;
    llhttp_t llhttp;
    llhttp_settings_t settings;
    KlHttpRequest *current_req; /* set during parse() */
    int headers_done;       /* on_headers_complete fired */
    int complete;           /* message_complete fired */

    /* Accumulator for current header field/value */
    const char *hdr_field;
    size_t hdr_field_len;
} LlhttpParser;

/* --- llhttp callbacks --- */

static int on_url(llhttp_t *p, const char *at, size_t len) {
    LlhttpParser *lp = p->data;
    KlHttpRequest *req = lp->current_req;

    if (!req->path) {
        req->path = at;
        req->path_len = len;
    } else {
        /* Continuation — extend */
        req->path_len = (size_t)(at + len - req->path);
    }
    return 0;
}

static int on_url_complete(llhttp_t *p) {
    LlhttpParser *lp = p->data;
    KlHttpRequest *req = lp->current_req;

    /* Split path and query at '?' */
    for (size_t i = 0; i < req->path_len; i++) {
        if (req->path[i] == '?') {
            req->query = req->path + i + 1;
            req->query_len = req->path_len - i - 1;
            req->path_len = i;
            break;
        }
    }
    return 0;
}

static int on_method(llhttp_t *p, const char *at, size_t len) {
    LlhttpParser *lp = p->data;
    KlHttpRequest *req = lp->current_req;

    if (!req->method) {
        req->method = at;
        req->method_len = len;
    } else {
        req->method_len = (size_t)(at + len - req->method);
    }
    return 0;
}

static int on_header_field(llhttp_t *p, const char *at, size_t len) {
    LlhttpParser *lp = p->data;

    if (!lp->hdr_field) {
        lp->hdr_field = at;
        lp->hdr_field_len = len;
    } else {
        lp->hdr_field_len = (size_t)(at + len - lp->hdr_field);
    }
    return 0;
}

static int on_header_value(llhttp_t *p, const char *at, size_t len) {
    LlhttpParser *lp = p->data;
    KlHttpRequest *req = lp->current_req;

    if (req->num_headers >= KL_MAX_HEADERS) return -1;  /* reject: too many headers */

    int idx = req->num_headers;
    if (req->headers[idx].value) {
        /* Continuation of existing value */
        req->headers[idx].value_len = (size_t)(at + len - req->headers[idx].value);
    } else {
        req->headers[idx].name = lp->hdr_field;
        req->headers[idx].name_len = lp->hdr_field_len;
        req->headers[idx].value = at;
        req->headers[idx].value_len = len;
    }
    return 0;
}

static int on_header_value_complete(llhttp_t *p) {
    LlhttpParser *lp = p->data;
    KlHttpRequest *req = lp->current_req;

    req->num_headers++;
    lp->hdr_field = NULL;
    lp->hdr_field_len = 0;
    return 0;
}

static int on_headers_complete(llhttp_t *p) {
    LlhttpParser *lp = p->data;
    KlHttpRequest *req = lp->current_req;

    req->version_major = (int)p->http_major;
    req->version_minor = (int)p->http_minor;
    req->content_length = p->content_length;
    req->chunked = (p->flags & F_CHUNKED) ? 1 : 0;
    req->keep_alive = llhttp_should_keep_alive(p);

    /* RFC 7230 §3.3.3: when Transfer-Encoding is present, ignore
     * Content-Length to prevent CL/TE request smuggling. */
    if (req->chunked)
        req->content_length = 0;

    lp->headers_done = 1;
    return HPE_PAUSED;  /* pause to let connection layer route + create body reader */
}

static int on_body(llhttp_t *p, const char *at, size_t len) {
    LlhttpParser *lp = p->data;
    KlHttpRequest *req = lp->current_req;

    /* Forward to body reader if present */
    if (req->body_reader) {
        if (req->body_reader->on_data(req->body_reader, at, len) < 0)
            return -1;  /* reader rejected — abort parse */
    }
    /* No body reader → discard */
    return 0;
}

static int on_message_complete(llhttp_t *p) {
    LlhttpParser *lp = p->data;
    KlHttpRequest *req = lp->current_req;

    lp->complete = 1;

    if (req->body_reader)
        req->body_reader->on_complete(req->body_reader);

    return HPE_PAUSED;  /* pause to return control */
}

/* --- KlHttp1Parser vtable implementation --- */

static KlHttp1ParseResult llhttp_parse(KlHttp1RequestParser *self, KlHttpRequest *req,
                                   const char *buf, size_t len,
                                   size_t *consumed) {
    LlhttpParser *lp = (LlhttpParser *)self;
    lp->current_req = req;

    llhttp_errno_t err = llhttp_execute(&lp->llhttp, buf, len);

    if (lp->complete) {
        const char *error_pos = llhttp_get_error_pos(&lp->llhttp);
        *consumed = (size_t)(error_pos - buf);
        llhttp_resume(&lp->llhttp);
        lp->complete = 0;
        return KL_HTTP1_PARSE_OK;
    }

    if (lp->headers_done && err == HPE_PAUSED) {
        /* Headers just completed — pause for routing */
        const char *error_pos = llhttp_get_error_pos(&lp->llhttp);
        *consumed = (size_t)(error_pos - buf);
        llhttp_resume(&lp->llhttp);
        lp->headers_done = 0;  /* consume flag so body resume won't re-trigger */
        return KL_HTTP1_PARSE_HEADERS_OK;
    }

    if (err == HPE_OK) {
        /* All data consumed but message not complete */
        *consumed = len;
        return KL_HTTP1_PARSE_INCOMPLETE;
    }

    if (err == HPE_PAUSED) {
        const char *error_pos = llhttp_get_error_pos(&lp->llhttp);
        *consumed = (size_t)(error_pos - buf);
        llhttp_resume(&lp->llhttp);
        return KL_HTTP1_PARSE_OK;
    }

    /* Parse error */
    *consumed = 0;
    return KL_HTTP1_PARSE_ERROR;
}

static void llhttp_reset_parser(KlHttp1RequestParser *self) {
    LlhttpParser *lp = (LlhttpParser *)self;
    llhttp_init(&lp->llhttp, HTTP_REQUEST, &lp->settings);
    lp->llhttp.data = lp;
    lp->current_req = NULL;
    lp->headers_done = 0;
    lp->complete = 0;
    lp->hdr_field = NULL;
    lp->hdr_field_len = 0;
}

static void llhttp_destroy_parser(KlHttp1RequestParser *self) {
    LlhttpParser *lp = (LlhttpParser *)self;
    kl_free(lp->alloc, lp, sizeof(LlhttpParser));
}

KlHttp1RequestParser *kl_http1_request_parser_llhttp(KlAllocator *alloc) {
    LlhttpParser *lp = kl_malloc(alloc, sizeof(LlhttpParser));
    if (!lp) return NULL;

    memset(lp, 0, sizeof(LlhttpParser));
    lp->alloc = alloc;

    /* Set up vtable */
    lp->base.parse = llhttp_parse;
    lp->base.reset = llhttp_reset_parser;
    lp->base.destroy = llhttp_destroy_parser;

    /* Set up llhttp settings */
    llhttp_settings_init(&lp->settings);
    lp->settings.on_url = on_url;
    lp->settings.on_url_complete = on_url_complete;
    lp->settings.on_method = on_method;
    lp->settings.on_header_field = on_header_field;
    lp->settings.on_header_value = on_header_value;
    lp->settings.on_header_value_complete = on_header_value_complete;
    lp->settings.on_headers_complete = on_headers_complete;
    lp->settings.on_body = on_body;
    lp->settings.on_message_complete = on_message_complete;

    /* Init llhttp */
    llhttp_init(&lp->llhttp, HTTP_REQUEST, &lp->settings);
    lp->llhttp.data = lp;

    return &lp->base;
}
