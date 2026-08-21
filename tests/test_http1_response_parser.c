#include "utest.h"
#include <keel/http_client.h>
#include <keel/http1_parser.h>
#include <keel/allocator.h>
#include <string.h>

#define free_client_response kl_http_client_response_free

UTEST(response_parser, create_and_destroy) {
    KlAllocator a = kl_allocator_default();
    KlHttp1ResponseParser *p = kl_http1_response_parser_llhttp(0, &a);
    ASSERT_TRUE(p != NULL);
    p->destroy(p);
}

UTEST(response_parser, simple_200) {
    KlAllocator a = kl_allocator_default();
    KlHttp1ResponseParser *p = kl_http1_response_parser_llhttp(0, &a);

    const char *raw = "HTTP/1.1 200 OK\r\n"
                      "Content-Length: 5\r\n"
                      "\r\n"
                      "hello";
    size_t len = strlen(raw);

    KlHttpClientResponse resp;
    memset(&resp, 0, sizeof(resp));
    size_t consumed = 0;

    KlHttp1ParseResult result = p->parse(p, &resp, raw, len, &consumed);
    ASSERT_EQ(result, KL_HTTP1_PARSE_OK);
    ASSERT_EQ(resp.status, 200);
    ASSERT_EQ(resp.body_len, (size_t)5);
    ASSERT_EQ(memcmp(resp.body, "hello", 5), 0);
    ASSERT_EQ(resp.num_headers, 1);
    ASSERT_STREQ(resp.headers[0].name, "Content-Length");
    ASSERT_STREQ(resp.headers[0].value, "5");

    free_client_response(&resp);
    p->destroy(p);
}

UTEST(response_parser, chunked_response) {
    KlAllocator a = kl_allocator_default();
    KlHttp1ResponseParser *p = kl_http1_response_parser_llhttp(0, &a);

    const char *raw = "HTTP/1.1 200 OK\r\n"
                      "Transfer-Encoding: chunked\r\n"
                      "\r\n"
                      "5\r\nhello\r\n"
                      "6\r\n world\r\n"
                      "0\r\n\r\n";
    size_t len = strlen(raw);

    KlHttpClientResponse resp;
    memset(&resp, 0, sizeof(resp));
    size_t consumed = 0;

    KlHttp1ParseResult result = p->parse(p, &resp, raw, len, &consumed);
    ASSERT_EQ(result, KL_HTTP1_PARSE_OK);
    ASSERT_EQ(resp.status, 200);
    ASSERT_EQ(resp.body_len, (size_t)11);
    ASSERT_EQ(memcmp(resp.body, "hello world", 11), 0);

    free_client_response(&resp);
    p->destroy(p);
}

UTEST(response_parser, multiple_headers) {
    KlAllocator a = kl_allocator_default();
    KlHttp1ResponseParser *p = kl_http1_response_parser_llhttp(0, &a);

    const char *raw = "HTTP/1.1 200 OK\r\n"
                      "Content-Type: text/plain\r\n"
                      "Content-Length: 3\r\n"
                      "X-Custom: test\r\n"
                      "\r\n"
                      "abc";
    size_t len = strlen(raw);

    KlHttpClientResponse resp;
    memset(&resp, 0, sizeof(resp));
    size_t consumed = 0;

    KlHttp1ParseResult result = p->parse(p, &resp, raw, len, &consumed);
    ASSERT_EQ(result, KL_HTTP1_PARSE_OK);
    ASSERT_EQ(resp.num_headers, 3);
    ASSERT_STREQ(resp.headers[0].name, "Content-Type");
    ASSERT_STREQ(resp.headers[0].value, "text/plain");
    ASSERT_STREQ(resp.headers[1].name, "Content-Length");
    ASSERT_STREQ(resp.headers[1].value, "3");
    ASSERT_STREQ(resp.headers[2].name, "X-Custom");
    ASSERT_STREQ(resp.headers[2].value, "test");

    free_client_response(&resp);
    p->destroy(p);
}

UTEST(response_parser, body_size_limit) {
    KlAllocator a = kl_allocator_default();
    KlHttp1ResponseParser *p = kl_http1_response_parser_llhttp(3, &a);  /* max 3 bytes */

    const char *raw = "HTTP/1.1 200 OK\r\n"
                      "Content-Length: 5\r\n"
                      "\r\n"
                      "hello";
    size_t len = strlen(raw);

    KlHttpClientResponse resp;
    memset(&resp, 0, sizeof(resp));
    size_t consumed = 0;

    KlHttp1ParseResult result = p->parse(p, &resp, raw, len, &consumed);
    ASSERT_EQ(result, KL_HTTP1_PARSE_ERROR);

    free_client_response(&resp);
    p->destroy(p);
}

UTEST(response_parser, incomplete) {
    KlAllocator a = kl_allocator_default();
    KlHttp1ResponseParser *p = kl_http1_response_parser_llhttp(0, &a);

    const char *raw = "HTTP/1.1 200 OK\r\n"
                      "Content-Length: 100\r\n"
                      "\r\n"
                      "partial";
    size_t len = strlen(raw);

    KlHttpClientResponse resp;
    memset(&resp, 0, sizeof(resp));
    size_t consumed = 0;

    KlHttp1ParseResult result = p->parse(p, &resp, raw, len, &consumed);
    ASSERT_EQ(result, KL_HTTP1_PARSE_INCOMPLETE);

    free_client_response(&resp);
    p->destroy(p);
}

UTEST(response_parser, malformed) {
    KlAllocator a = kl_allocator_default();
    KlHttp1ResponseParser *p = kl_http1_response_parser_llhttp(0, &a);

    const char *raw = "GARBAGE DATA\r\n\r\n";
    size_t len = strlen(raw);

    KlHttpClientResponse resp;
    memset(&resp, 0, sizeof(resp));
    size_t consumed = 0;

    KlHttp1ParseResult result = p->parse(p, &resp, raw, len, &consumed);
    ASSERT_EQ(result, KL_HTTP1_PARSE_ERROR);

    free_client_response(&resp);
    p->destroy(p);
}

UTEST(response_parser, status_404) {
    KlAllocator a = kl_allocator_default();
    KlHttp1ResponseParser *p = kl_http1_response_parser_llhttp(0, &a);

    const char *raw = "HTTP/1.1 404 Not Found\r\n"
                      "Content-Length: 9\r\n"
                      "\r\n"
                      "not found";
    size_t len = strlen(raw);

    KlHttpClientResponse resp;
    memset(&resp, 0, sizeof(resp));
    size_t consumed = 0;

    KlHttp1ParseResult result = p->parse(p, &resp, raw, len, &consumed);
    ASSERT_EQ(result, KL_HTTP1_PARSE_OK);
    ASSERT_EQ(resp.status, 404);
    ASSERT_EQ(resp.body_len, (size_t)9);

    free_client_response(&resp);
    p->destroy(p);
}

UTEST(response_parser, empty_body) {
    KlAllocator a = kl_allocator_default();
    KlHttp1ResponseParser *p = kl_http1_response_parser_llhttp(0, &a);

    const char *raw = "HTTP/1.1 204 No Content\r\n"
                      "Content-Length: 0\r\n"
                      "\r\n";
    size_t len = strlen(raw);

    KlHttpClientResponse resp;
    memset(&resp, 0, sizeof(resp));
    size_t consumed = 0;

    KlHttp1ParseResult result = p->parse(p, &resp, raw, len, &consumed);
    ASSERT_EQ(result, KL_HTTP1_PARSE_OK);
    ASSERT_EQ(resp.status, 204);
    ASSERT_EQ(resp.body_len, (size_t)0);

    free_client_response(&resp);
    p->destroy(p);
}

UTEST(response_parser, reset_and_reparse) {
    KlAllocator a = kl_allocator_default();
    KlHttp1ResponseParser *p = kl_http1_response_parser_llhttp(0, &a);

    const char *raw1 = "HTTP/1.1 200 OK\r\n"
                       "Content-Length: 2\r\n"
                       "\r\n"
                       "ok";

    KlHttpClientResponse resp1;
    memset(&resp1, 0, sizeof(resp1));
    size_t consumed1 = 0;

    KlHttp1ParseResult r1 = p->parse(p, &resp1, raw1, strlen(raw1), &consumed1);
    ASSERT_EQ(r1, KL_HTTP1_PARSE_OK);
    ASSERT_EQ(resp1.status, 200);
    free_client_response(&resp1);

    p->reset(p);

    const char *raw2 = "HTTP/1.1 301 Moved\r\n"
                       "Content-Length: 0\r\n"
                       "\r\n";

    KlHttpClientResponse resp2;
    memset(&resp2, 0, sizeof(resp2));
    size_t consumed2 = 0;

    KlHttp1ParseResult r2 = p->parse(p, &resp2, raw2, strlen(raw2), &consumed2);
    ASSERT_EQ(r2, KL_HTTP1_PARSE_OK);
    ASSERT_EQ(resp2.status, 301);
    free_client_response(&resp2);

    p->destroy(p);
}

/* Regression (audit L2): an empty-valued header must be preserved as its own
 * header, not merged into the following header's name. */
UTEST(response_parser, empty_valued_header) {
    KlAllocator a = kl_allocator_default();
    KlHttp1ResponseParser *p = kl_http1_response_parser_llhttp(0, &a);

    const char *raw = "HTTP/1.1 200 OK\r\n"
                      "X-Empty:\r\n"
                      "X-Next: v\r\n"
                      "Content-Length: 0\r\n"
                      "\r\n";
    size_t len = strlen(raw);

    KlHttpClientResponse resp;
    memset(&resp, 0, sizeof(resp));
    size_t consumed = 0;

    KlHttp1ParseResult result = p->parse(p, &resp, raw, len, &consumed);
    ASSERT_EQ(result, KL_HTTP1_PARSE_OK);
    ASSERT_EQ(resp.num_headers, 3);
    ASSERT_STREQ(resp.headers[0].name, "X-Empty");
    ASSERT_STREQ(resp.headers[0].value, "");      /* preserved, not merged */
    ASSERT_STREQ(resp.headers[1].name, "X-Next");
    ASSERT_STREQ(resp.headers[1].value, "v");
    ASSERT_STREQ(resp.headers[2].name, "Content-Length");
    ASSERT_STREQ(resp.headers[2].value, "0");

    free_client_response(&resp);
    p->destroy(p);
}

UTEST_MAIN();
