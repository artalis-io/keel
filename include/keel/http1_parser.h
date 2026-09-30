#ifndef KEEL_HTTP1_PARSER_H
#define KEEL_HTTP1_PARSER_H

#include <keel/allocator.h>
#include <keel/http_request.h>
#ifdef __cplusplus
extern "C" {
#endif


typedef enum {
    KL_HTTP1_PARSE_OK,            /**< Full request/response parsed */
    KL_HTTP1_PARSE_INCOMPLETE,    /**< Need more data */
    KL_HTTP1_PARSE_HEADERS_OK,    /**< headers complete, body pending */
    KL_HTTP1_PARSE_ERROR          /**< Parse error */
} KlHttp1ParseResult;

/* ── Request parser (server-side) ─────────────────────────────────── */

typedef struct KlHttp1RequestParser KlHttp1RequestParser;

/*
 * Append-only vtable (see docs/contracts/compatibility.md): implementers
 * zero-initialize and recompile per major version. All three ops (parse, reset,
 * destroy) are required; core calls each. New ops are appended after destroy.
 */
struct KlHttp1RequestParser {
    KlHttp1ParseResult (*parse)(KlHttp1RequestParser *self, KlHttpRequest *req,
                           const char *buf, size_t len, size_t *consumed); /**< Parse request bytes */
    void (*reset)(KlHttp1RequestParser *self);   /**< Reset for next request */
    void (*destroy)(KlHttp1RequestParser *self); /**< Free parser resources */
};

/**
 * @brief Create an llhttp-based HTTP/1.1 request parser.
 * @param alloc Allocator for parser state.
 * @return Parser instance, or NULL on allocation failure.
 */
KlHttp1RequestParser *kl_http1_request_parser_llhttp(KlAllocator *alloc);

/** @brief Backward compatibility: existing code can use the old name. */
typedef KlHttp1RequestParser KlHttp1Parser;
/** @brief Backward compatibility alias for kl_http1_request_parser_llhttp. */
#define kl_http1_parser_llhttp kl_http1_request_parser_llhttp

/* ── Response parser (client-side) ────────────────────────────────── */

typedef struct KlHttpClientResponse KlHttpClientResponse;
typedef struct KlHttp1ResponseParser KlHttp1ResponseParser;

/*
 * Append-only vtable (see docs/contracts/compatibility.md): implementers
 * zero-initialize and recompile per major version. parse, reset and destroy are
 * required; core calls each. New ops are appended after destroy and are optional.
 */
struct KlHttp1ResponseParser {
    KlHttp1ParseResult (*parse)(KlHttp1ResponseParser *self, KlHttpClientResponse *resp,
                           const char *buf, size_t len, size_t *consumed); /**< Parse response bytes */
    void (*reset)(KlHttp1ResponseParser *self);   /**< Reset for next response */
    void (*destroy)(KlHttp1ResponseParser *self); /**< Free parser resources */
    /** Optional: the peer closed the connection (end of stream) before parse returned OK.
     *  Return KL_HTTP1_PARSE_OK, with the response filled in as parse would, if the bytes seen
     *  form a complete message (a close-delimited body ends here); KL_HTTP1_PARSE_ERROR if the
     *  message was truncated (EOF inside the headers, or inside a Content-Length or chunked body).
     *  NULL keeps the older behavior: a response whose status line arrived counts as complete. */
    KlHttp1ParseResult (*finish)(KlHttp1ResponseParser *self, KlHttpClientResponse *resp);
    /** Optional: the request was HEAD, so the next response has no body whatever its
     *  Content-Length or Transfer-Encoding say (RFC 9110 9.3.2): it is complete at the end of its
     *  headers. Lasts until reset. A parser without it cannot tell a HEAD response from a truncated
     *  one, so for a HEAD request the client does not call finish (a response whose status line
     *  arrived counts as complete at end of stream). */
    void (*expect_no_body)(KlHttp1ResponseParser *self);
};

/** @brief Factory function for creating response parsers.
 *  The signature is frozen (see docs/contracts/compatibility.md): pass new
 *  construction inputs through the allocator context, never by changing it. */
typedef KlHttp1ResponseParser *(*KlHttp1ResponseParserFactory)(size_t max_response_size,
                                                       KlAllocator *alloc);

/**
 * @brief Create an llhttp-based HTTP/1.1 response parser.
 * @param max_response_size Maximum response body size (0 = no limit).
 * @param alloc Allocator for parser state.
 * @return Parser instance, or NULL on allocation failure.
 */
KlHttp1ResponseParser *kl_http1_response_parser_llhttp(size_t max_response_size,
                                             KlAllocator *alloc);

#ifdef __cplusplus
}
#endif

#endif
