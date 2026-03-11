#ifndef KEEL_PARSER_H
#define KEEL_PARSER_H

#include <keel/allocator.h>
#include <keel/request.h>

typedef enum {
    KL_PARSE_OK,
    KL_PARSE_INCOMPLETE,
    KL_PARSE_HEADERS_OK,    /* headers complete, body pending */
    KL_PARSE_ERROR
} KlParseResult;

/* ── Request parser (server-side) ─────────────────────────────────── */

typedef struct KlRequestParser KlRequestParser;

struct KlRequestParser {
    KlParseResult (*parse)(KlRequestParser *self, KlRequest *req,
                           const char *buf, size_t len, size_t *consumed);
    void (*reset)(KlRequestParser *self);
    void (*destroy)(KlRequestParser *self);
};

/**
 * @brief Create an llhttp-based HTTP/1.1 request parser.
 * @param alloc Allocator for parser state.
 * @return Parser instance, or NULL on allocation failure.
 */
KlRequestParser *kl_request_parser_llhttp(KlAllocator *alloc);

/* Backward compatibility — existing code can use the old names */
typedef KlRequestParser KlParser;
#define kl_parser_llhttp kl_request_parser_llhttp

/* ── Response parser (client-side) ────────────────────────────────── */

typedef struct KlClientResponse KlClientResponse;
typedef struct KlResponseParser KlResponseParser;

struct KlResponseParser {
    KlParseResult (*parse)(KlResponseParser *self, KlClientResponse *resp,
                           const char *buf, size_t len, size_t *consumed);
    void (*reset)(KlResponseParser *self);
    void (*destroy)(KlResponseParser *self);
};

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
