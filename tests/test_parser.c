#include "utest.h"
#include <keel/parser.h>
#include <keel/body_reader.h>
#include <string.h>

UTEST(parser, create_and_destroy) {
    KlAllocator a = kl_allocator_default();
    KlParser *p = kl_parser_llhttp(&a);
    ASSERT_TRUE(p != NULL);
    p->destroy(p);
}

UTEST(parser, parse_simple_get) {
    KlAllocator a = kl_allocator_default();
    KlParser *p = kl_parser_llhttp(&a);

    const char *raw = "GET /hello HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "\r\n";
    size_t len = strlen(raw);

    KlRequest req;
    memset(&req, 0, sizeof(req));
    size_t consumed = 0;

    /* First parse returns HEADERS_OK */
    KlParseResult result = p->parse(p, &req, raw, len, &consumed);
    ASSERT_EQ(result, KL_PARSE_HEADERS_OK);
    ASSERT_EQ(req.method_len, (size_t)3);
    ASSERT_EQ(memcmp(req.method, "GET", 3), 0);
    ASSERT_EQ(req.path_len, (size_t)6);
    ASSERT_EQ(memcmp(req.path, "/hello", 6), 0);
    ASSERT_EQ(req.version_major, 1);
    ASSERT_EQ(req.version_minor, 1);
    ASSERT_EQ(req.num_headers, 1);

    /* Resume with remaining data to complete */
    size_t leftover = len - consumed;
    if (leftover > 0) {
        size_t consumed2 = 0;
        result = p->parse(p, &req, raw + consumed, leftover, &consumed2);
        ASSERT_EQ(result, KL_PARSE_OK);
    }

    p->destroy(p);
}

UTEST(parser, parse_with_query) {
    KlAllocator a = kl_allocator_default();
    KlParser *p = kl_parser_llhttp(&a);

    const char *raw = "GET /search?q=test&page=2 HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "\r\n";
    size_t len = strlen(raw);

    KlRequest req;
    memset(&req, 0, sizeof(req));
    size_t consumed = 0;

    KlParseResult result = p->parse(p, &req, raw, len, &consumed);
    ASSERT_EQ(result, KL_PARSE_HEADERS_OK);
    ASSERT_EQ(req.path_len, (size_t)7);
    ASSERT_EQ(memcmp(req.path, "/search", 7), 0);
    ASSERT_TRUE(req.query != NULL);
    ASSERT_EQ(req.query_len, (size_t)13);
    ASSERT_EQ(memcmp(req.query, "q=test&page=2", 13), 0);

    p->destroy(p);
}

UTEST(parser, parse_post_with_body) {
    KlAllocator a = kl_allocator_default();
    KlParser *p = kl_parser_llhttp(&a);

    const char *raw = "POST /data HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Content-Length: 13\r\n"
                      "\r\n"
                      "{\"key\":\"val\"}";
    size_t len = strlen(raw);

    KlRequest req;
    memset(&req, 0, sizeof(req));
    size_t consumed = 0;

    /* Headers first */
    KlParseResult result = p->parse(p, &req, raw, len, &consumed);
    ASSERT_EQ(result, KL_PARSE_HEADERS_OK);
    ASSERT_EQ(req.method_len, (size_t)4);
    ASSERT_EQ(memcmp(req.method, "POST", 4), 0);
    ASSERT_EQ(req.content_length, (size_t)13);

    /* Set up buffer reader to receive body */
    KlBodyReader *br = kl_body_reader_buffer(&a, &req, NULL);
    ASSERT_TRUE(br != NULL);
    req.body_reader = br;

    /* Resume parse with remaining data — body forwarded to reader */
    size_t leftover = len - consumed;
    ASSERT_TRUE(leftover > 0);
    size_t consumed2 = 0;
    result = p->parse(p, &req, raw + consumed, leftover, &consumed2);
    ASSERT_EQ(result, KL_PARSE_OK);

    KlBufReader *buf = (KlBufReader *)br;
    ASSERT_EQ(buf->len, (size_t)13);
    ASSERT_EQ(memcmp(buf->data, "{\"key\":\"val\"}", 13), 0);

    br->destroy(br);
    p->destroy(p);
}

UTEST(parser, parse_multiple_headers) {
    KlAllocator a = kl_allocator_default();
    KlParser *p = kl_parser_llhttp(&a);

    const char *raw = "GET / HTTP/1.1\r\n"
                      "Host: example.com\r\n"
                      "Accept: text/html\r\n"
                      "Connection: keep-alive\r\n"
                      "\r\n";
    size_t len = strlen(raw);

    KlRequest req;
    memset(&req, 0, sizeof(req));
    size_t consumed = 0;

    KlParseResult result = p->parse(p, &req, raw, len, &consumed);
    ASSERT_EQ(result, KL_PARSE_HEADERS_OK);
    ASSERT_EQ(req.num_headers, 3);

    p->destroy(p);
}

UTEST(parser, incomplete_request) {
    KlAllocator a = kl_allocator_default();
    KlParser *p = kl_parser_llhttp(&a);

    const char *raw = "GET /hello HTTP/1.1\r\n";
    size_t len = strlen(raw);

    KlRequest req;
    memset(&req, 0, sizeof(req));
    size_t consumed = 0;

    KlParseResult result = p->parse(p, &req, raw, len, &consumed);
    ASSERT_EQ(result, KL_PARSE_INCOMPLETE);

    p->destroy(p);
}

UTEST(parser, reset_between_requests) {
    KlAllocator a = kl_allocator_default();
    KlParser *p = kl_parser_llhttp(&a);

    const char *raw1 = "GET /first HTTP/1.1\r\nHost: localhost\r\n\r\n";
    KlRequest req1;
    memset(&req1, 0, sizeof(req1));
    size_t consumed1 = 0;
    KlParseResult r1 = p->parse(p, &req1, raw1, strlen(raw1), &consumed1);
    /* May get HEADERS_OK or OK depending on message completeness */
    ASSERT_TRUE(r1 == KL_PARSE_HEADERS_OK || r1 == KL_PARSE_OK);

    p->reset(p);

    const char *raw2 = "POST /second HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n";
    KlRequest req2;
    memset(&req2, 0, sizeof(req2));
    size_t consumed2 = 0;
    KlParseResult r2 = p->parse(p, &req2, raw2, strlen(raw2), &consumed2);
    ASSERT_TRUE(r2 == KL_PARSE_HEADERS_OK || r2 == KL_PARSE_OK);
    ASSERT_EQ(memcmp(req2.method, "POST", 4), 0);

    p->destroy(p);
}

UTEST_MAIN();
