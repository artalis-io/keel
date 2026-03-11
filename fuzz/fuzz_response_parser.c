/*
 * libFuzzer harness for the HTTP response parser.
 *
 * Build:
 *   make fuzz  (requires clang with -fsanitize=fuzzer support)
 *
 * Run:
 *   ./fuzz/fuzz_response_parser fuzz/corpus_response_parser/ -max_total_time=60
 */
#include <keel/allocator.h>
#include <keel/client.h>
#include <keel/parser.h>
#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    KlAllocator alloc = kl_allocator_default();
    KlResponseParser *parser = kl_response_parser_llhttp(4 * 1024 * 1024, &alloc);
    if (!parser) return 0;

    KlClientResponse resp;
    memset(&resp, 0, sizeof(resp));

    size_t consumed = 0;
    KlParseResult pr = parser->parse(parser, &resp,
                                      (const char *)data, size, &consumed);

    if (pr == KL_PARSE_OK)
        kl_client_response_free(&resp);

    parser->reset(parser);

    /* Second parse to exercise reset path */
    memset(&resp, 0, sizeof(resp));
    consumed = 0;
    pr = parser->parse(parser, &resp,
                        (const char *)data, size, &consumed);

    if (pr == KL_PARSE_OK)
        kl_client_response_free(&resp);

    parser->destroy(parser);
    return 0;
}
