#ifndef KEEL_PARSER_H
#define KEEL_PARSER_H

#include <keel/allocator.h>
#include <keel/http_request.h>

typedef enum {
    KL_PARSE_OK,            /**< Full request/response parsed */
    KL_PARSE_INCOMPLETE,    /**< Need more data */
    KL_PARSE_HEADERS_OK,    /**< headers complete, body pending */
    KL_PARSE_ERROR          /**< Parse error */
} KlParseResult;

/* ── Request parser (server-side) ─────────────────────────────────── */

typedef struct KlRequestParser KlRequestParser;

struct KlRequestParser {
    KlParseResult (*parse)(KlRequestParser *self, KlHttpRequest *req,
                           const char *buf, size_t len, size_t *consumed); /**< Parse request bytes */
    void (*reset)(KlRequestParser *self);   /**< Reset for next request */
    void (*destroy)(KlRequestParser *self); /**< Free parser resources */
};

/**
 * @brief Create an llhttp-based HTTP/1.1 request parser.
 * @param alloc Allocator for parser state.
 * @return Parser instance, or NULL on allocation failure.
 */
KlRequestParser *kl_request_parser_llhttp(KlAllocator *alloc);

/** @brief Backward compatibility — existing code can use the old name. */
typedef KlRequestParser KlParser;
/** @brief Backward compatibility alias for kl_request_parser_llhttp. */
#define kl_parser_llhttp kl_request_parser_llhttp

/* ── Response parser (client-side) ────────────────────────────────── */

typedef struct KlClientResponse KlClientResponse;
typedef struct KlResponseParser KlResponseParser;

struct KlResponseParser {
    KlParseResult (*parse)(KlResponseParser *self, KlClientResponse *resp,
                           const char *buf, size_t len, size_t *consumed); /**< Parse response bytes */
    void (*reset)(KlResponseParser *self);   /**< Reset for next response */
    void (*destroy)(KlResponseParser *self); /**< Free parser resources */
};

/** @brief Factory function for creating response parsers. */
typedef KlResponseParser *(*KlResponseParserFactory)(size_t max_response_size,
                                                       KlAllocator *alloc);

/**
 * @brief Create an llhttp-based HTTP/1.1 response parser.
 * @param max_response_size Maximum response body size (0 = no limit).
 * @param alloc Allocator for parser state.
 * @return Parser instance, or NULL on allocation failure.
 */
KlResponseParser *kl_response_parser_llhttp(size_t max_response_size,
                                             KlAllocator *alloc);

#endif
