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
#include <keel/http_client.h>
#include <keel/http1_parser.h>
#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    KlAllocator alloc = kl_allocator_default();
    KlHttp1ResponseParser *parser = kl_http1_response_parser_llhttp(4 * 1024 * 1024, &alloc);
    if (!parser) return 0;

    KlHttpClientResponse resp;
    memset(&resp, 0, sizeof(resp));

    size_t consumed = 0;
    KlHttp1ParseResult pr = parser->parse(parser, &resp,
                                      (const char *)data, size, &consumed);

    if (pr == KL_HTTP1_PARSE_OK)
        kl_http_client_response_free(&resp);

    parser->reset(parser);

    /* Second parse to exercise reset path */
    memset(&resp, 0, sizeof(resp));
    consumed = 0;
    pr = parser->parse(parser, &resp,
                        (const char *)data, size, &consumed);

    if (pr == KL_HTTP1_PARSE_OK)
        kl_http_client_response_free(&resp);

    parser->destroy(parser);
    return 0;
}
