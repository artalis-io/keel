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

typedef struct KlParser KlParser;

struct KlParser {
    KlParseResult (*parse)(KlParser *self, KlRequest *req,
                           const char *buf, size_t len, size_t *consumed);
    void (*reset)(KlParser *self);
    void (*destroy)(KlParser *self);
};

/**
 * @brief Create an llhttp-based HTTP/1.1 parser.
 * @param alloc Allocator for parser state.
 * @return Parser instance, or NULL on allocation failure.
 */
KlParser *kl_parser_llhttp(KlAllocator *alloc);

#endif
