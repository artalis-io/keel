#ifndef KEEL_HTTP1_PARSER_H
#define KEEL_HTTP1_PARSER_H

#include <keel/allocator.h>
#include <keel/http_request.h>

typedef enum {
    KL_HTTP1_PARSE_OK,            /**< Full request/response parsed */
    KL_HTTP1_PARSE_INCOMPLETE,    /**< Need more data */
    KL_HTTP1_PARSE_HEADERS_OK,    /**< headers complete, body pending */
    KL_HTTP1_PARSE_ERROR          /**< Parse error */
} KlHttp1ParseResult;

/* ── Request parser (server-side) ─────────────────────────────────── */

typedef struct KlHttp1RequestParser KlHttp1RequestParser;

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

struct KlHttp1ResponseParser {
    KlHttp1ParseResult (*parse)(KlHttp1ResponseParser *self, KlHttpClientResponse *resp,
                           const char *buf, size_t len, size_t *consumed); /**< Parse response bytes */
    void (*reset)(KlHttp1ResponseParser *self);   /**< Reset for next response */
    void (*destroy)(KlHttp1ResponseParser *self); /**< Free parser resources */
};

/** @brief Factory function for creating response parsers. */
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

#endif
