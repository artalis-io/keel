/*
 * libFuzzer harness for the HTTP parser.
 *
 * Build:
 *   make fuzz  (requires clang with -fsanitize=fuzzer support)
 *
 * Run:
 *   ./fuzz/fuzz_parser fuzz/corpus_parser/ -max_total_time=60
 */
#include <keel/allocator.h>
#include <keel/parser.h>
#include <keel/http_request.h>
#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    KlAllocator alloc = kl_allocator_default();
    KlHttp1Parser *parser = kl_http1_parser_llhttp(&alloc);
    if (!parser) return 0;

    KlHttpRequest req;
    memset(&req, 0, sizeof(req));

    size_t consumed = 0;
    KlHttp1ParseResult pr = parser->parse(parser, &req,
                                      (const char *)data, size, &consumed);

    /* If headers parsed, try feeding remaining bytes as body */
    if (pr == KL_HTTP1_PARSE_HEADERS_OK && consumed < size) {
        size_t body_consumed = 0;
        parser->parse(parser, &req,
                      (const char *)data + consumed, size - consumed,
                      &body_consumed);
    }

    parser->reset(parser);

    /* Second parse to exercise reset path */
    memset(&req, 0, sizeof(req));
    consumed = 0;
    parser->parse(parser, &req, (const char *)data, size, &consumed);

    parser->destroy(parser);
    return 0;
}
