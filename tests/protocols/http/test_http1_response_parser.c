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

/* ── finish: end of stream ───────────────────────────────────────── */

/* Feed raw (expected INCOMPLETE), then signal EOF; return finish's result. */
static KlHttp1ParseResult feed_then_finish(KlHttp1ResponseParser *p, KlHttpClientResponse *resp,
                                           const char *raw) {
    size_t consumed = 0;
    KlHttp1ParseResult r = p->parse(p, resp, raw, strlen(raw), &consumed);
    if (r != KL_HTTP1_PARSE_INCOMPLETE) return r;
    return p->finish(p, resp);
}

UTEST(response_parser, finish_completes_close_delimited_body) {
    KlAllocator a = kl_allocator_default();
    KlHttp1ResponseParser *p = kl_http1_response_parser_llhttp(0, &a);
    ASSERT_TRUE(p->finish != NULL);
    KlHttpClientResponse resp;
    memset(&resp, 0, sizeof(resp));
    ASSERT_EQ(feed_then_finish(p, &resp, "HTTP/1.1 200 OK\r\nX-A: 1\r\n\r\nhello"),
              KL_HTTP1_PARSE_OK);
    ASSERT_EQ(resp.status, 200);
    ASSERT_EQ(resp.body_len, (size_t)5);
    ASSERT_EQ(memcmp(resp.body, "hello", 5), 0);
    ASSERT_EQ(resp.num_headers, 1);
    ASSERT_STREQ(resp.headers[0].name, "X-A");
    free_client_response(&resp);
    p->destroy(p);
}

UTEST(response_parser, finish_rejects_truncated_content_length_body) {
    KlAllocator a = kl_allocator_default();
    KlHttp1ResponseParser *p = kl_http1_response_parser_llhttp(0, &a);
    KlHttpClientResponse resp;
    memset(&resp, 0, sizeof(resp));
    ASSERT_EQ(feed_then_finish(p, &resp, "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc"),
              KL_HTTP1_PARSE_ERROR);
    free_client_response(&resp);
    p->destroy(p);
}

UTEST(response_parser, finish_rejects_truncated_chunked_body) {
    KlAllocator a = kl_allocator_default();
    KlHttp1ResponseParser *p = kl_http1_response_parser_llhttp(0, &a);
    KlHttpClientResponse resp;
    memset(&resp, 0, sizeof(resp));
    ASSERT_EQ(feed_then_finish(p, &resp,
                               "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nab"),
              KL_HTTP1_PARSE_ERROR);
    free_client_response(&resp);
    p->destroy(p);
}

UTEST(response_parser, finish_rejects_truncated_headers) {
    KlAllocator a = kl_allocator_default();
    KlHttp1ResponseParser *p = kl_http1_response_parser_llhttp(0, &a);
    KlHttpClientResponse resp;
    memset(&resp, 0, sizeof(resp));
    ASSERT_EQ(feed_then_finish(p, &resp, "HTTP/1.1 200 OK\r\nContent-Le"), KL_HTTP1_PARSE_ERROR);
    free_client_response(&resp);
    p->destroy(p);
}

UTEST(response_parser, finish_with_nothing_received_is_an_error) {
    KlAllocator a = kl_allocator_default();
    KlHttp1ResponseParser *p = kl_http1_response_parser_llhttp(0, &a);
    KlHttpClientResponse resp;
    memset(&resp, 0, sizeof(resp));
    ASSERT_EQ(p->finish(p, &resp), KL_HTTP1_PARSE_ERROR);
    p->destroy(p);
}

/* ── expect_no_body: the response to a HEAD request ─────────────── */

UTEST(response_parser, expect_no_body_completes_at_end_of_headers) {
    KlAllocator a = kl_allocator_default();
    KlHttp1ResponseParser *p = kl_http1_response_parser_llhttp(0, &a);
    ASSERT_TRUE(p->expect_no_body != NULL);
    p->expect_no_body(p);
    const char *raw = "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n";
    KlHttpClientResponse resp;
    memset(&resp, 0, sizeof(resp));
    size_t consumed = 0;
    ASSERT_EQ(p->parse(p, &resp, raw, strlen(raw), &consumed), KL_HTTP1_PARSE_OK);
    ASSERT_EQ(resp.status, 200);
    ASSERT_EQ(resp.body_len, (size_t)0);
    ASSERT_EQ(resp.num_headers, 1);
    free_client_response(&resp);
    p->destroy(p);
}

UTEST(response_parser, expect_no_body_is_cleared_by_reset) {
    KlAllocator a = kl_allocator_default();
    KlHttp1ResponseParser *p = kl_http1_response_parser_llhttp(0, &a);
    p->expect_no_body(p);
    p->reset(p);
    const char *raw = "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n";
    KlHttpClientResponse resp;
    memset(&resp, 0, sizeof(resp));
    size_t consumed = 0;
    ASSERT_EQ(p->parse(p, &resp, raw, strlen(raw), &consumed), KL_HTTP1_PARSE_INCOMPLETE);
    ASSERT_EQ(p->finish(p, &resp), KL_HTTP1_PARSE_ERROR);   /* a GET's body really is missing */
    p->destroy(p);
}

/* ── Interim (1xx) responses ─────────────────────────────────────────
 * 100 Continue / 102 Processing / 103 Early Hints are complete messages but not the response: the
 * final one follows (RFC 9110 15.2). Before the fix the parser stopped at the interim message and
 * reported it, so the client returned "100" or "103" as the response and never read the real one. */

static KlHttp1ParseResult feed_all(KlHttp1ResponseParser *p, KlHttpClientResponse *resp,
                                   const char *raw) {
    size_t consumed = 0;
    return p->parse(p, resp, raw, strlen(raw), &consumed);
}

static const char *hdr(const KlHttpClientResponse *r, const char *name) {
    for (int i = 0; i < r->num_headers; i++)
        if (strcmp(r->headers[i].name, name) == 0) return r->headers[i].value;
    return NULL;
}

UTEST(response_parser, interim_100_is_skipped_for_the_final_response) {
    KlAllocator a = kl_allocator_default();
    KlHttp1ResponseParser *p = kl_http1_response_parser_llhttp(0, &a);
    KlHttpClientResponse resp;
    memset(&resp, 0, sizeof(resp));
    ASSERT_EQ(feed_all(p, &resp, "HTTP/1.1 100 Continue\r\n\r\n"
                                 "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"), KL_HTTP1_PARSE_OK);
    ASSERT_EQ(resp.status, 200);                     /* was: 100 */
    ASSERT_EQ(resp.body_len, (size_t)2);
    ASSERT_EQ(memcmp(resp.body, "ok", 2), 0);
    free_client_response(&resp);
    p->destroy(p);
}

UTEST(response_parser, interim_103_headers_do_not_leak_into_the_final_response) {
    KlAllocator a = kl_allocator_default();
    KlHttp1ResponseParser *p = kl_http1_response_parser_llhttp(0, &a);
    KlHttpClientResponse resp;
    memset(&resp, 0, sizeof(resp));
    ASSERT_EQ(feed_all(p, &resp,
                       "HTTP/1.1 103 Early Hints\r\nLink: </style.css>; rel=preload\r\n\r\n"
                       "HTTP/1.1 102 Processing\r\n\r\n"
                       "HTTP/1.1 200 OK\r\nX-Final: yes\r\nContent-Length: 5\r\n\r\nhello"),
              KL_HTTP1_PARSE_OK);
    ASSERT_EQ(resp.status, 200);
    ASSERT_EQ(resp.num_headers, 2);                  /* X-Final + Content-Length only */
    ASSERT_TRUE(hdr(&resp, "Link") == NULL);
    ASSERT_STREQ(hdr(&resp, "X-Final"), "yes");
    ASSERT_EQ(memcmp(resp.body, "hello", 5), 0);
    free_client_response(&resp);
    p->destroy(p);
}

UTEST(response_parser, interim_then_final_across_reads) {
    KlAllocator a = kl_allocator_default();
    KlHttp1ResponseParser *p = kl_http1_response_parser_llhttp(0, &a);
    KlHttpClientResponse resp;
    memset(&resp, 0, sizeof(resp));
    ASSERT_EQ(feed_all(p, &resp, "HTTP/1.1 100 Continue\r\n\r\n"), KL_HTTP1_PARSE_INCOMPLETE);
    ASSERT_EQ(resp.status, 0);                       /* nothing reported for the interim */
    ASSERT_EQ(feed_all(p, &resp, "HTTP/1.1 204 No Content\r\n\r\n"), KL_HTTP1_PARSE_OK);
    ASSERT_EQ(resp.status, 204);
    free_client_response(&resp);
    p->destroy(p);
}

UTEST(response_parser, interim_then_end_of_stream_is_truncated) {
    KlAllocator a = kl_allocator_default();
    KlHttp1ResponseParser *p = kl_http1_response_parser_llhttp(0, &a);
    KlHttpClientResponse resp;
    memset(&resp, 0, sizeof(resp));
    ASSERT_EQ(feed_all(p, &resp, "HTTP/1.1 103 Early Hints\r\nLink: </a>\r\n\r\n"),
              KL_HTTP1_PARSE_INCOMPLETE);
    ASSERT_EQ(p->finish(p, &resp), KL_HTTP1_PARSE_ERROR);   /* no final response arrived */
    p->destroy(p);
}

UTEST(response_parser, status_101_is_final) {
    KlAllocator a = kl_allocator_default();
    KlHttp1ResponseParser *p = kl_http1_response_parser_llhttp(0, &a);
    KlHttpClientResponse resp;
    memset(&resp, 0, sizeof(resp));
    ASSERT_EQ(feed_all(p, &resp, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: x\r\n"
                                 "Connection: Upgrade\r\n\r\n"), KL_HTTP1_PARSE_OK);
    ASSERT_EQ(resp.status, 101);
    free_client_response(&resp);
    p->destroy(p);
}

static int g_hdr_calls, g_hdr_status;
static int count_headers(int status, const KlHttpClientHeader *h, int n, void *ud) {
    (void)h; (void)n; (void)ud;
    g_hdr_calls++;
    g_hdr_status = status;
    return 0;
}

UTEST(response_parser, streaming_on_headers_fires_once_for_the_final_response) {
    KlAllocator a = kl_allocator_default();
    g_hdr_calls = 0; g_hdr_status = 0;
    KlHttp1ResponseParser *p = kl_http1_response_parser_llhttp_s(0, &a, NULL, count_headers,
                                                                  NULL, NULL);
    KlHttpClientResponse resp;
    memset(&resp, 0, sizeof(resp));
    ASSERT_EQ(feed_all(p, &resp, "HTTP/1.1 103 Early Hints\r\nLink: </a>\r\n\r\n"
                                 "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n"), KL_HTTP1_PARSE_OK);
    ASSERT_EQ(g_hdr_calls, 1);                       /* was: 2, the first with status 103 */
    ASSERT_EQ(g_hdr_status, 200);
    free_client_response(&resp);
    p->destroy(p);
}

UTEST_MAIN();
