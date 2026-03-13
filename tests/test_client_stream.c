#include "utest.h"
#include <keel/keel.h>
#include <keel/client.h>
#include <keel/parser.h>
#include <keel/allocator.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>

/* ── Test constants ────────────────────────────────────────────────── */

#define TEST_BUF_SIZE       8192
#define TEST_TIMEOUT_MS     5000
#define TEST_LONG_TIMEOUT   10000
#define TEST_POLL_MS        50
#define TEST_MAX_EVENTS     16
#define TEST_MAX_BODY       65536
#define TEST_LARGE_BODY     32768
#define TEST_CHUNK_1K       1024

/* ── Streaming test helpers ──────────────────────────────────────── */

typedef struct {
    char    buf[TEST_BUF_SIZE];
    size_t  len;
    int     complete;
    int     headers_called;
    int     headers_status;
    int     headers_count;
    int     body_calls;
} StreamCtx;

static int test_on_body(const char *data, size_t len, void *user_data)
{
    StreamCtx *ctx = user_data;
    if (ctx->len + len > sizeof(ctx->buf))
        return -1;
    memcpy(ctx->buf + ctx->len, data, len);
    ctx->len += len;
    ctx->body_calls++;
    return 0;
}

static int test_on_headers(int status, const KlClientHeader *headers,
                            int num_headers, void *user_data)
{
    StreamCtx *ctx = user_data;
    ctx->headers_called = 1;
    ctx->headers_status = status;
    ctx->headers_count = num_headers;
    (void)headers;
    return 0;
}

static void test_on_complete(void *user_data)
{
    StreamCtx *ctx = user_data;
    ctx->complete = 1;
}

/* on_body that ignores user_data (for null-context tests) */
static int test_on_body_noop(const char *data, size_t len, void *user_data)
{
    (void)data; (void)len; (void)user_data;
    return 0;
}

/* on_body that returns -1 (abort) */
static int test_on_body_abort(const char *data, size_t len, void *user_data)
{
    (void)data; (void)len; (void)user_data;
    return -1;
}

/* on_headers that returns -1 (abort) */
static int test_on_headers_abort(int status, const KlClientHeader *headers,
                                  int num_headers, void *user_data)
{
    (void)status; (void)headers; (void)num_headers; (void)user_data;
    return -1;
}

/* ══════════════════════════════════════════════════════════════════
 * Response streaming tests (parser-level, no network)
 * ══════════════════════════════════════════════════════════════════ */

UTEST(client_stream, stream_body_simple) {
    KlAllocator a = kl_allocator_default();
    StreamCtx ctx;
    memset(&ctx, 0, sizeof(ctx));

    KlResponseParser *p = kl_response_parser_llhttp_s(0, &a,
        test_on_body, test_on_headers, test_on_complete, &ctx);
    ASSERT_TRUE(p != NULL);

    const char *raw = "HTTP/1.1 200 OK\r\n"
                      "Content-Length: 11\r\n"
                      "\r\n"
                      "hello world";

    KlClientResponse resp;
    memset(&resp, 0, sizeof(resp));
    size_t consumed = 0;

    KlParseResult result = p->parse(p, &resp, raw, strlen(raw), &consumed);
    ASSERT_EQ(result, KL_PARSE_OK);
    ASSERT_EQ(resp.status, 200);

    /* Body was streamed, not buffered */
    ASSERT_TRUE(resp.body == NULL);
    ASSERT_EQ(resp.body_len, (size_t)0);

    /* on_body received the data */
    ASSERT_EQ(ctx.len, (size_t)11);
    ASSERT_EQ(memcmp(ctx.buf, "hello world", 11), 0);

    /* on_headers was called */
    ASSERT_TRUE(ctx.headers_called);
    ASSERT_EQ(ctx.headers_status, 200);
    ASSERT_EQ(ctx.headers_count, 1);

    /* on_complete was called */
    ASSERT_TRUE(ctx.complete);

    /* Headers still transferred to response */
    ASSERT_EQ(resp.num_headers, 1);
    ASSERT_STREQ(resp.headers[0].name, "Content-Length");
    ASSERT_STREQ(resp.headers[0].value, "11");

    kl_client_response_free(&resp);
    p->destroy(p);
}

UTEST(client_stream, stream_body_chunked) {
    KlAllocator a = kl_allocator_default();
    StreamCtx ctx;
    memset(&ctx, 0, sizeof(ctx));

    KlResponseParser *p = kl_response_parser_llhttp_s(0, &a,
        test_on_body, NULL, test_on_complete, &ctx);
    ASSERT_TRUE(p != NULL);

    const char *raw = "HTTP/1.1 200 OK\r\n"
                      "Transfer-Encoding: chunked\r\n"
                      "\r\n"
                      "5\r\nhello\r\n"
                      "6\r\n world\r\n"
                      "0\r\n\r\n";

    KlClientResponse resp;
    memset(&resp, 0, sizeof(resp));
    size_t consumed = 0;

    KlParseResult result = p->parse(p, &resp, raw, strlen(raw), &consumed);
    ASSERT_EQ(result, KL_PARSE_OK);
    ASSERT_TRUE(resp.body == NULL);
    ASSERT_EQ(ctx.len, (size_t)11);
    ASSERT_EQ(memcmp(ctx.buf, "hello world", 11), 0);
    ASSERT_TRUE(ctx.complete);
    ASSERT_GE(ctx.body_calls, 2);  /* at least 2 chunks */

    kl_client_response_free(&resp);
    p->destroy(p);
}

UTEST(client_stream, stream_body_multi_feed) {
    KlAllocator a = kl_allocator_default();
    StreamCtx ctx;
    memset(&ctx, 0, sizeof(ctx));

    KlResponseParser *p = kl_response_parser_llhttp_s(0, &a,
        test_on_body, NULL, test_on_complete, &ctx);
    ASSERT_TRUE(p != NULL);

    const char *raw = "HTTP/1.1 200 OK\r\n"
                      "Content-Length: 10\r\n"
                      "\r\n"
                      "0123456789";
    size_t total = strlen(raw);

    KlClientResponse resp;
    memset(&resp, 0, sizeof(resp));

    /* Feed 5 bytes at a time */
    size_t pos = 0;
    KlParseResult result = KL_PARSE_INCOMPLETE;
    while (pos < total && result == KL_PARSE_INCOMPLETE) {
        size_t chunk = 5;
        if (pos + chunk > total)
            chunk = total - pos;
        size_t consumed = 0;
        result = p->parse(p, &resp, raw + pos, chunk, &consumed);
        pos += consumed;
    }

    ASSERT_EQ(result, KL_PARSE_OK);
    ASSERT_EQ(ctx.len, (size_t)10);
    ASSERT_EQ(memcmp(ctx.buf, "0123456789", 10), 0);
    ASSERT_TRUE(ctx.complete);

    kl_client_response_free(&resp);
    p->destroy(p);
}

UTEST(client_stream, stream_body_abort) {
    KlAllocator a = kl_allocator_default();

    KlResponseParser *p = kl_response_parser_llhttp_s(0, &a,
        test_on_body_abort, NULL, NULL, NULL);
    ASSERT_TRUE(p != NULL);

    const char *raw = "HTTP/1.1 200 OK\r\n"
                      "Content-Length: 5\r\n"
                      "\r\n"
                      "hello";

    KlClientResponse resp;
    memset(&resp, 0, sizeof(resp));
    size_t consumed = 0;

    KlParseResult result = p->parse(p, &resp, raw, strlen(raw), &consumed);
    ASSERT_EQ(result, KL_PARSE_ERROR);

    kl_client_response_free(&resp);
    p->destroy(p);
}

UTEST(client_stream, stream_headers_callback) {
    KlAllocator a = kl_allocator_default();
    StreamCtx ctx;
    memset(&ctx, 0, sizeof(ctx));

    KlResponseParser *p = kl_response_parser_llhttp_s(0, &a,
        test_on_body, test_on_headers, NULL, &ctx);
    ASSERT_TRUE(p != NULL);

    const char *raw = "HTTP/1.1 201 Created\r\n"
                      "Content-Type: application/json\r\n"
                      "Content-Length: 2\r\n"
                      "\r\n"
                      "{}";

    KlClientResponse resp;
    memset(&resp, 0, sizeof(resp));
    size_t consumed = 0;

    KlParseResult result = p->parse(p, &resp, raw, strlen(raw), &consumed);
    ASSERT_EQ(result, KL_PARSE_OK);
    ASSERT_TRUE(ctx.headers_called);
    ASSERT_EQ(ctx.headers_status, 201);
    ASSERT_EQ(ctx.headers_count, 2);

    kl_client_response_free(&resp);
    p->destroy(p);
}

UTEST(client_stream, stream_headers_abort) {
    KlAllocator a = kl_allocator_default();

    KlResponseParser *p = kl_response_parser_llhttp_s(0, &a,
        test_on_body, test_on_headers_abort, NULL, NULL);
    ASSERT_TRUE(p != NULL);

    const char *raw = "HTTP/1.1 200 OK\r\n"
                      "Content-Length: 5\r\n"
                      "\r\n"
                      "hello";

    KlClientResponse resp;
    memset(&resp, 0, sizeof(resp));
    size_t consumed = 0;

    KlParseResult result = p->parse(p, &resp, raw, strlen(raw), &consumed);
    ASSERT_EQ(result, KL_PARSE_ERROR);

    kl_client_response_free(&resp);
    p->destroy(p);
}

UTEST(client_stream, stream_no_body) {
    KlAllocator a = kl_allocator_default();
    StreamCtx ctx;
    memset(&ctx, 0, sizeof(ctx));

    KlResponseParser *p = kl_response_parser_llhttp_s(0, &a,
        test_on_body, test_on_headers, test_on_complete, &ctx);
    ASSERT_TRUE(p != NULL);

    const char *raw = "HTTP/1.1 204 No Content\r\n"
                      "Content-Length: 0\r\n"
                      "\r\n";

    KlClientResponse resp;
    memset(&resp, 0, sizeof(resp));
    size_t consumed = 0;

    KlParseResult result = p->parse(p, &resp, raw, strlen(raw), &consumed);
    ASSERT_EQ(result, KL_PARSE_OK);
    ASSERT_EQ(resp.status, 204);
    ASSERT_EQ(ctx.body_calls, 0);  /* on_body never called */
    ASSERT_TRUE(ctx.complete);

    kl_client_response_free(&resp);
    p->destroy(p);
}

UTEST(client_stream, stream_null_optional_callbacks) {
    KlAllocator a = kl_allocator_default();

    /* on_body set, but on_headers=NULL and on_complete=NULL — should not crash */
    KlResponseParser *p = kl_response_parser_llhttp_s(0, &a,
        test_on_body_noop, NULL, NULL, NULL);
    ASSERT_TRUE(p != NULL);

    const char *raw = "HTTP/1.1 200 OK\r\n"
                      "Content-Length: 3\r\n"
                      "\r\n"
                      "abc";

    KlClientResponse resp;
    memset(&resp, 0, sizeof(resp));
    size_t consumed = 0;

    KlParseResult result = p->parse(p, &resp, raw, strlen(raw), &consumed);
    ASSERT_EQ(result, KL_PARSE_OK);
    ASSERT_TRUE(resp.body == NULL);

    kl_client_response_free(&resp);
    p->destroy(p);
}

UTEST(client_stream, stream_body_size_limit) {
    KlAllocator a = kl_allocator_default();
    StreamCtx ctx;
    memset(&ctx, 0, sizeof(ctx));

    /* max 3 bytes in streaming mode */
    KlResponseParser *p = kl_response_parser_llhttp_s(3, &a,
        test_on_body, NULL, NULL, &ctx);
    ASSERT_TRUE(p != NULL);

    const char *raw = "HTTP/1.1 200 OK\r\n"
                      "Content-Length: 10\r\n"
                      "\r\n"
                      "0123456789";

    KlClientResponse resp;
    memset(&resp, 0, sizeof(resp));
    size_t consumed = 0;

    KlParseResult result = p->parse(p, &resp, raw, strlen(raw), &consumed);
    ASSERT_EQ(result, KL_PARSE_ERROR);

    kl_client_response_free(&resp);
    p->destroy(p);
}

/* ══════════════════════════════════════════════════════════════════
 * Request streaming tests (chunked format verification)
 * ══════════════════════════════════════════════════════════════════ */

/* Mock body_read that produces known data */
typedef struct {
    const char *data;
    size_t      len;
    size_t      pos;
    size_t      chunk_size;  /* max bytes per read */
} MockReader;

static ssize_t mock_body_read(char *buf, size_t buf_len, void *user_data)
{
    MockReader *r = user_data;
    if (r->pos >= r->len)
        return 0;  /* EOF */
    size_t avail = r->len - r->pos;
    size_t to_copy = avail < buf_len ? avail : buf_len;
    if (r->chunk_size > 0 && to_copy > r->chunk_size)
        to_copy = r->chunk_size;
    memcpy(buf, r->data + r->pos, to_copy);
    r->pos += to_copy;
    return (ssize_t)to_copy;
}

static ssize_t mock_body_read_eof(char *buf, size_t buf_len, void *user_data)
{
    (void)buf; (void)buf_len; (void)user_data;
    return 0;  /* immediate EOF */
}

static ssize_t mock_body_read_error(char *buf, size_t buf_len, void *user_data)
{
    (void)buf; (void)buf_len; (void)user_data;
    return -1;  /* error */
}

/* ══════════════════════════════════════════════════════════════════
 * Integration test infrastructure — real server + client
 * ══════════════════════════════════════════════════════════════════ */

/* Wait for server to bind (max 2s) */
static void wait_for_bind(KlServer *s) {
    for (int i = 0; i < 200 && s->bound_port == 0; i++) usleep(10000);
}

/* Server handler: echo body back */
static void srv_echo(KlRequest *req, KlResponse *res, void *ctx)
{
    (void)ctx;
    KlBufReader *br = (KlBufReader *)req->body_reader;
    kl_response_status(res, 200);
    kl_response_header(res, "Content-Type", "text/plain");
    if (br && br->len > 0)
        kl_response_body_borrow(res, br->data, br->len);
    else
        kl_response_body_borrow(res, "no body", 7);
}

/* Server handler: fixed JSON response */
static void srv_hello(KlRequest *req, KlResponse *res, void *ctx)
{
    (void)req; (void)ctx;
    kl_response_json(res, 200, "{\"ok\":true}", 11);
}

/* Server handler: streaming chunked response */
static void srv_chunked(KlRequest *req, KlResponse *res, void *ctx)
{
    (void)req; (void)ctx;
    kl_response_header(res, "Content-Type", "text/plain");
    KlWriteFn write_fn;
    void *write_ctx;
    kl_response_begin_stream(res, 200, &write_fn, &write_ctx);
    write_fn(write_ctx, "chunk1", 6);
    write_fn(write_ctx, "chunk2", 6);
    write_fn(write_ctx, "chunk3", 6);
    kl_response_end_stream(res);
}

/* Server handler: 204 No Content */
static void srv_no_content(KlRequest *req, KlResponse *res, void *ctx)
{
    (void)req; (void)ctx;
    kl_response_status(res, 204);
}

static KlServer stream_test_server;
static pthread_t stream_test_tid;

static void *stream_server_thread(void *arg)
{
    (void)arg;
    kl_server_run(&stream_test_server);
    return NULL;
}

static int stream_test_port;

static void start_stream_server(void)
{
    KlConfig cfg = {.port = 0, .max_body_size = TEST_MAX_BODY};
    kl_server_init(&stream_test_server, &cfg);
    kl_server_route(&stream_test_server, "GET", "/hello",
                    srv_hello, NULL, NULL);
    kl_server_route(&stream_test_server, "GET", "/chunked",
                    srv_chunked, NULL, NULL);
    kl_server_route(&stream_test_server, "GET", "/nocontent",
                    srv_no_content, NULL, NULL);
    kl_server_route(&stream_test_server, "POST", "/echo",
                    srv_echo, (void *)(size_t)TEST_MAX_BODY,
                    kl_body_reader_buffer);
    pthread_create(&stream_test_tid, NULL, stream_server_thread, NULL);
    wait_for_bind(&stream_test_server);
    stream_test_port = stream_test_server.bound_port;
}

static void stop_stream_server(void)
{
    kl_server_stop(&stream_test_server);
    pthread_join(stream_test_tid, NULL);
    kl_server_free(&stream_test_server);
}

/* ── Async completion context ──────────────────────────────────────── */

typedef struct {
    StreamCtx  stream;     /* reuse for on_body/on_headers/on_complete */
    int        done;       /* set by on_done */
    int        error;      /* kl_client_error result */
    int        status;     /* response status */
    char       body[TEST_BUF_SIZE]; /* buffered body (non-streaming path) */
    size_t     body_len;
} AsyncCtx;

static void async_on_done(KlClient *client, void *user_data)
{
    AsyncCtx *ctx = user_data;
    ctx->error = kl_client_error(client);
    if (ctx->error == 0) {
        const KlClientResponse *resp = kl_client_response(client);
        if (resp) {
            ctx->status = resp->status;
            if (resp->body && resp->body_len > 0) {
                size_t n = resp->body_len < sizeof(ctx->body) - 1
                         ? resp->body_len : sizeof(ctx->body) - 1;
                memcpy(ctx->body, resp->body, n);
                ctx->body_len = n;
            }
        }
    }
    ctx->done = 1;
}

/* Run event loop until ctx->done is set or timeout */
static int run_until_done(KlEventCtx *ev, AsyncCtx *ctx, int timeout_ms)
{
    int elapsed = 0;
    while (!ctx->done && elapsed < timeout_ms) {
        int n = kl_event_ctx_run(ev, TEST_MAX_EVENTS, TEST_POLL_MS);
        if (n < 0) return -1;
        elapsed += TEST_POLL_MS;
    }
    return ctx->done ? 0 : -1;
}

/* URL helper for stream test port */
static char url_buf[128];
static const char *test_url(const char *path)
{
    snprintf(url_buf, sizeof(url_buf),
             "http://127.0.0.1:%d%s", stream_test_port, path);
    return url_buf;
}

UTEST(client_stream, stream_req_api_validation) {
    KlAllocator a = kl_allocator_default();
    KlClientResponse resp;

    /* NULL alloc */
    ASSERT_EQ(kl_client_request_s(NULL, NULL, "GET", "http://x",
                                    NULL, 0, NULL, 0, NULL, &resp), -1);

    /* NULL method */
    ASSERT_EQ(kl_client_request_s(&a, NULL, NULL, "http://x",
                                    NULL, 0, NULL, 0, NULL, &resp), -1);

    /* NULL url */
    ASSERT_EQ(kl_client_request_s(&a, NULL, "GET", NULL,
                                    NULL, 0, NULL, 0, NULL, &resp), -1);

    /* NULL resp */
    ASSERT_EQ(kl_client_request_s(&a, NULL, "GET", "http://x",
                                    NULL, 0, NULL, 0, NULL, NULL), -1);

    /* Bad URL */
    ASSERT_EQ(kl_client_request_s(&a, NULL, "GET", "ftp://x",
                                    NULL, 0, NULL, 0, NULL, &resp), -1);
}

UTEST(client_stream, stream_req_null_stream) {
    KlAllocator a = kl_allocator_default();
    KlClientResponse resp;

    /* NULL stream behaves like kl_client_request (fails to connect = -1) */
    int rc = kl_client_request_s(&a, NULL, "GET", "http://127.0.0.1:1",
                                   NULL, 0, NULL, 0, NULL, &resp);
    ASSERT_EQ(rc, -1);
}

UTEST(client_stream, async_stream_api_validation) {
    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);

    /* NULL args */
    ASSERT_TRUE(kl_client_start_s(NULL, NULL, NULL, "GET", "http://x",
                                    NULL, 0, NULL, 0, NULL, NULL, NULL) == NULL);

    /* Bad URL */
    ASSERT_TRUE(kl_client_start_s(&ev, &a, NULL, "GET", "ftp://x",
                                    NULL, 0, NULL, 0, NULL, NULL, NULL) == NULL);

    /* HTTPS without TLS */
    ASSERT_TRUE(kl_client_start_s(&ev, &a, NULL, "GET", "https://x",
                                    NULL, 0, NULL, 0, NULL, NULL, NULL) == NULL);

    kl_event_ctx_free(&ev);
}

/* ══════════════════════════════════════════════════════════════════
 * Sync integration tests (real server + kl_client_request_s)
 * ══════════════════════════════════════════════════════════════════ */

UTEST(sync_stream, response_stream_fixed_body) {
    start_stream_server();

    KlAllocator a = kl_allocator_default();
    KlClientConfig cfg = {.timeout_ms = TEST_TIMEOUT_MS};
    KlClientResponse resp;
    StreamCtx ctx;
    memset(&ctx, 0, sizeof(ctx));

    KlClientStreamCfg stream = {
        .on_body     = test_on_body,
        .on_headers  = test_on_headers,
        .on_complete = test_on_complete,
        .user_data   = &ctx,
    };

    int rc = kl_client_request_s(&a, &cfg, "GET", test_url("/hello"),
                                   NULL, 0, NULL, 0, &stream, &resp);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(resp.status, 200);

    /* Body was streamed, not buffered */
    ASSERT_TRUE(resp.body == NULL);
    ASSERT_EQ(resp.body_len, (size_t)0);

    /* on_body received the JSON */
    ASSERT_EQ(ctx.len, (size_t)11);
    ASSERT_EQ(memcmp(ctx.buf, "{\"ok\":true}", 11), 0);

    /* Headers callback fired */
    ASSERT_TRUE(ctx.headers_called);
    ASSERT_EQ(ctx.headers_status, 200);

    /* Completion callback fired */
    ASSERT_TRUE(ctx.complete);

    kl_client_response_free(&resp);
    stop_stream_server();
}

UTEST(sync_stream, response_stream_chunked) {
    start_stream_server();

    KlAllocator a = kl_allocator_default();
    KlClientConfig cfg = {.timeout_ms = TEST_TIMEOUT_MS};
    KlClientResponse resp;
    StreamCtx ctx;
    memset(&ctx, 0, sizeof(ctx));

    KlClientStreamCfg stream = {
        .on_body     = test_on_body,
        .on_complete = test_on_complete,
        .user_data   = &ctx,
    };

    int rc = kl_client_request_s(&a, &cfg, "GET", test_url("/chunked"),
                                   NULL, 0, NULL, 0, &stream, &resp);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(resp.status, 200);
    ASSERT_TRUE(resp.body == NULL);

    /* Received all 3 chunks */
    ASSERT_EQ(ctx.len, (size_t)18);
    ASSERT_EQ(memcmp(ctx.buf, "chunk1chunk2chunk3", 18), 0);
    ASSERT_GE(ctx.body_calls, 1);
    ASSERT_TRUE(ctx.complete);

    kl_client_response_free(&resp);
    stop_stream_server();
}

UTEST(sync_stream, response_stream_no_body) {
    start_stream_server();

    KlAllocator a = kl_allocator_default();
    KlClientConfig cfg = {.timeout_ms = TEST_TIMEOUT_MS};
    KlClientResponse resp;
    StreamCtx ctx;
    memset(&ctx, 0, sizeof(ctx));

    KlClientStreamCfg stream = {
        .on_body     = test_on_body,
        .on_complete = test_on_complete,
        .user_data   = &ctx,
    };

    int rc = kl_client_request_s(&a, &cfg, "GET", test_url("/nocontent"),
                                   NULL, 0, NULL, 0, &stream, &resp);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(resp.status, 204);
    ASSERT_EQ(ctx.body_calls, 0);
    ASSERT_TRUE(ctx.complete);

    kl_client_response_free(&resp);
    stop_stream_server();
}

UTEST(sync_stream, request_stream_chunked_upload) {
    start_stream_server();

    KlAllocator a = kl_allocator_default();
    KlClientConfig cfg = {.timeout_ms = TEST_TIMEOUT_MS};
    KlClientResponse resp;

    const char *payload = "streamed upload data";
    MockReader mr = {
        .data = payload,
        .len  = strlen(payload),
        .pos  = 0,
        .chunk_size = 8,  /* send in 8-byte chunks */
    };

    KlClientHeader headers[] = {
        {"Content-Type", "text/plain"},
    };

    KlClientStreamCfg stream = {
        .body_read = mock_body_read,
        .user_data = &mr,
    };

    int rc = kl_client_request_s(&a, &cfg, "POST", test_url("/echo"),
                                   headers, 1, NULL, 0, &stream, &resp);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(resp.status, 200);

    /* Server echoed back our streamed body */
    ASSERT_EQ(resp.body_len, strlen(payload));
    ASSERT_EQ(memcmp(resp.body, payload, resp.body_len), 0);

    kl_client_response_free(&resp);
    stop_stream_server();
}

UTEST(sync_stream, request_stream_eof_immediate) {
    start_stream_server();

    KlAllocator a = kl_allocator_default();
    KlClientConfig cfg = {.timeout_ms = TEST_TIMEOUT_MS};
    KlClientResponse resp;

    KlClientStreamCfg stream = {
        .body_read = mock_body_read_eof,
        .user_data = NULL,
    };

    int rc = kl_client_request_s(&a, &cfg, "POST", test_url("/echo"),
                                   NULL, 0, NULL, 0, &stream, &resp);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(resp.status, 200);

    /* Empty chunked body → server sees no body */
    ASSERT_TRUE(resp.body != NULL);
    ASSERT_EQ(memcmp(resp.body, "no body", 7), 0);

    kl_client_response_free(&resp);
    stop_stream_server();
}

UTEST(sync_stream, request_stream_error_aborts) {
    start_stream_server();

    KlAllocator a = kl_allocator_default();
    KlClientConfig cfg = {.timeout_ms = TEST_TIMEOUT_MS};
    KlClientResponse resp;

    KlClientStreamCfg stream = {
        .body_read = mock_body_read_error,
        .user_data = NULL,
    };

    int rc = kl_client_request_s(&a, &cfg, "POST", test_url("/echo"),
                                   NULL, 0, NULL, 0, &stream, &resp);
    ASSERT_EQ(rc, -1);  /* body_read error propagates */

    stop_stream_server();
}

/* Combined context for bidirectional streaming (body_read + on_body share user_data) */
typedef struct {
    MockReader  reader;
    StreamCtx   stream;
} BiDiCtx;

static ssize_t bidi_body_read(char *buf, size_t buf_len, void *user_data)
{
    BiDiCtx *ctx = user_data;
    return mock_body_read(buf, buf_len, &ctx->reader);
}

static int bidi_on_body(const char *data, size_t len, void *user_data)
{
    BiDiCtx *ctx = user_data;
    return test_on_body(data, len, &ctx->stream);
}

static int bidi_on_headers(int status, const KlClientHeader *headers,
                            int num_headers, void *user_data)
{
    BiDiCtx *ctx = user_data;
    return test_on_headers(status, headers, num_headers, &ctx->stream);
}

static void bidi_on_complete(void *user_data)
{
    BiDiCtx *ctx = user_data;
    test_on_complete(&ctx->stream);
}

UTEST(sync_stream, bidirectional_streaming) {
    start_stream_server();

    KlAllocator a = kl_allocator_default();
    KlClientConfig cfg = {.timeout_ms = TEST_TIMEOUT_MS};
    KlClientResponse resp;

    const char *payload = "bidirectional test";
    BiDiCtx bctx;
    memset(&bctx, 0, sizeof(bctx));
    bctx.reader.data = payload;
    bctx.reader.len  = strlen(payload);

    KlClientHeader headers[] = {
        {"Content-Type", "text/plain"},
    };

    KlClientStreamCfg stream = {
        .body_read   = bidi_body_read,
        .on_body     = bidi_on_body,
        .on_headers  = bidi_on_headers,
        .on_complete = bidi_on_complete,
        .user_data   = &bctx,
    };

    int rc = kl_client_request_s(&a, &cfg, "POST", test_url("/echo"),
                                   headers, 1, NULL, 0, &stream, &resp);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(resp.status, 200);

    /* Response body was streamed via on_body, not buffered */
    ASSERT_TRUE(resp.body == NULL);
    ASSERT_EQ(resp.body_len, (size_t)0);

    /* on_body received the echoed payload */
    ASSERT_EQ(bctx.stream.len, strlen(payload));
    ASSERT_EQ(memcmp(bctx.stream.buf, payload, bctx.stream.len), 0);

    /* Headers and completion fired */
    ASSERT_TRUE(bctx.stream.headers_called);
    ASSERT_EQ(bctx.stream.headers_status, 200);
    ASSERT_TRUE(bctx.stream.complete);

    kl_client_response_free(&resp);
    stop_stream_server();
}

/* ══════════════════════════════════════════════════════════════════
 * Async integration tests (real server + kl_client_start_s)
 * ══════════════════════════════════════════════════════════════════ */

UTEST(async_stream, response_stream_fixed_body) {
    start_stream_server();

    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);

    KlClientConfig cfg = {.timeout_ms = TEST_TIMEOUT_MS};
    StreamCtx sctx;
    memset(&sctx, 0, sizeof(sctx));
    AsyncCtx actx;
    memset(&actx, 0, sizeof(actx));
    actx.stream = sctx;

    KlClientStreamCfg stream = {
        .on_body     = test_on_body,
        .on_headers  = test_on_headers,
        .on_complete = test_on_complete,
        .user_data   = &actx.stream,
    };

    KlClient *c = kl_client_start_s(&ev, &a, &cfg, "GET", test_url("/hello"),
                                       NULL, 0, NULL, 0, &stream,
                                       async_on_done, &actx);
    ASSERT_TRUE(c != NULL);

    ASSERT_EQ(run_until_done(&ev, &actx, TEST_TIMEOUT_MS), 0);
    ASSERT_TRUE(actx.done);
    ASSERT_EQ(actx.error, 0);
    ASSERT_EQ(actx.status, 200);

    /* Body was streamed via on_body, not buffered */
    ASSERT_EQ(actx.body_len, (size_t)0);
    ASSERT_EQ(actx.stream.len, (size_t)11);
    ASSERT_EQ(memcmp(actx.stream.buf, "{\"ok\":true}", 11), 0);
    ASSERT_TRUE(actx.stream.headers_called);
    ASSERT_EQ(actx.stream.headers_status, 200);
    ASSERT_TRUE(actx.stream.complete);

    kl_client_free(c);
    kl_event_ctx_free(&ev);
    stop_stream_server();
}

UTEST(async_stream, response_stream_chunked) {
    start_stream_server();

    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);

    KlClientConfig cfg = {.timeout_ms = TEST_TIMEOUT_MS};
    AsyncCtx actx;
    memset(&actx, 0, sizeof(actx));

    KlClientStreamCfg stream = {
        .on_body     = test_on_body,
        .on_complete = test_on_complete,
        .user_data   = &actx.stream,
    };

    KlClient *c = kl_client_start_s(&ev, &a, &cfg, "GET", test_url("/chunked"),
                                       NULL, 0, NULL, 0, &stream,
                                       async_on_done, &actx);
    ASSERT_TRUE(c != NULL);

    ASSERT_EQ(run_until_done(&ev, &actx, TEST_TIMEOUT_MS), 0);
    ASSERT_EQ(actx.error, 0);
    ASSERT_EQ(actx.status, 200);

    /* All 3 chunks received */
    ASSERT_EQ(actx.stream.len, (size_t)18);
    ASSERT_EQ(memcmp(actx.stream.buf, "chunk1chunk2chunk3", 18), 0);
    ASSERT_TRUE(actx.stream.complete);

    kl_client_free(c);
    kl_event_ctx_free(&ev);
    stop_stream_server();
}

UTEST(async_stream, response_stream_no_body) {
    start_stream_server();

    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);

    KlClientConfig cfg = {.timeout_ms = TEST_TIMEOUT_MS};
    AsyncCtx actx;
    memset(&actx, 0, sizeof(actx));

    KlClientStreamCfg stream = {
        .on_body     = test_on_body,
        .on_complete = test_on_complete,
        .user_data   = &actx.stream,
    };

    KlClient *c = kl_client_start_s(&ev, &a, &cfg, "GET", test_url("/nocontent"),
                                       NULL, 0, NULL, 0, &stream,
                                       async_on_done, &actx);
    ASSERT_TRUE(c != NULL);

    ASSERT_EQ(run_until_done(&ev, &actx, TEST_TIMEOUT_MS), 0);
    ASSERT_EQ(actx.error, 0);
    ASSERT_EQ(actx.status, 204);
    ASSERT_EQ(actx.stream.body_calls, 0);
    ASSERT_TRUE(actx.stream.complete);

    kl_client_free(c);
    kl_event_ctx_free(&ev);
    stop_stream_server();
}

UTEST(async_stream, request_stream_chunked_upload) {
    start_stream_server();

    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);

    KlClientConfig cfg = {.timeout_ms = TEST_TIMEOUT_MS};
    AsyncCtx actx;
    memset(&actx, 0, sizeof(actx));

    const char *payload = "async chunked upload";
    MockReader mr = {
        .data = payload,
        .len  = strlen(payload),
        .pos  = 0,
        .chunk_size = 6,  /* small chunks */
    };

    KlClientHeader headers[] = {
        {"Content-Type", "text/plain"},
    };

    KlClientStreamCfg stream = {
        .body_read = mock_body_read,
        .user_data = &mr,
    };

    KlClient *c = kl_client_start_s(&ev, &a, &cfg, "POST", test_url("/echo"),
                                       headers, 1, NULL, 0, &stream,
                                       async_on_done, &actx);
    ASSERT_TRUE(c != NULL);

    ASSERT_EQ(run_until_done(&ev, &actx, TEST_TIMEOUT_MS), 0);
    ASSERT_EQ(actx.error, 0);
    ASSERT_EQ(actx.status, 200);

    /* Server echoed back our streamed body */
    ASSERT_EQ(actx.body_len, strlen(payload));
    ASSERT_EQ(memcmp(actx.body, payload, actx.body_len), 0);

    kl_client_free(c);
    kl_event_ctx_free(&ev);
    stop_stream_server();
}

UTEST(async_stream, request_stream_eof_immediate) {
    start_stream_server();

    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);

    KlClientConfig cfg = {.timeout_ms = TEST_TIMEOUT_MS};
    AsyncCtx actx;
    memset(&actx, 0, sizeof(actx));

    KlClientStreamCfg stream = {
        .body_read = mock_body_read_eof,
        .user_data = NULL,
    };

    KlClient *c = kl_client_start_s(&ev, &a, &cfg, "POST", test_url("/echo"),
                                       NULL, 0, NULL, 0, &stream,
                                       async_on_done, &actx);
    ASSERT_TRUE(c != NULL);

    ASSERT_EQ(run_until_done(&ev, &actx, TEST_TIMEOUT_MS), 0);
    ASSERT_EQ(actx.error, 0);
    ASSERT_EQ(actx.status, 200);

    /* Empty chunked body → "no body" from echo handler */
    ASSERT_EQ(memcmp(actx.body, "no body", 7), 0);

    kl_client_free(c);
    kl_event_ctx_free(&ev);
    stop_stream_server();
}

UTEST(async_stream, request_stream_error_aborts) {
    start_stream_server();

    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);

    KlClientConfig cfg = {.timeout_ms = TEST_TIMEOUT_MS};
    AsyncCtx actx;
    memset(&actx, 0, sizeof(actx));

    KlClientStreamCfg stream = {
        .body_read = mock_body_read_error,
        .user_data = NULL,
    };

    KlClient *c = kl_client_start_s(&ev, &a, &cfg, "POST", test_url("/echo"),
                                       NULL, 0, NULL, 0, &stream,
                                       async_on_done, &actx);
    ASSERT_TRUE(c != NULL);

    ASSERT_EQ(run_until_done(&ev, &actx, TEST_TIMEOUT_MS), 0);
    ASSERT_TRUE(actx.done);
    ASSERT_EQ(actx.error, -1);  /* body_read error */

    kl_client_free(c);
    kl_event_ctx_free(&ev);
    stop_stream_server();
}

UTEST(async_stream, null_stream_buffered) {
    start_stream_server();

    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);

    KlClientConfig cfg = {.timeout_ms = TEST_TIMEOUT_MS};
    AsyncCtx actx;
    memset(&actx, 0, sizeof(actx));

    /* stream=NULL → behaves like kl_client_start (buffered) */
    KlClient *c = kl_client_start_s(&ev, &a, &cfg, "GET", test_url("/hello"),
                                       NULL, 0, NULL, 0, NULL,
                                       async_on_done, &actx);
    ASSERT_TRUE(c != NULL);

    ASSERT_EQ(run_until_done(&ev, &actx, TEST_TIMEOUT_MS), 0);
    ASSERT_EQ(actx.error, 0);
    ASSERT_EQ(actx.status, 200);

    /* Body was buffered normally */
    ASSERT_EQ(actx.body_len, (size_t)11);
    ASSERT_EQ(memcmp(actx.body, "{\"ok\":true}", 11), 0);

    kl_client_free(c);
    kl_event_ctx_free(&ev);
    stop_stream_server();
}

UTEST(async_stream, request_stream_large_body) {
    start_stream_server();

    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);

    KlClientConfig cfg = {.timeout_ms = TEST_TIMEOUT_MS};
    AsyncCtx actx;
    memset(&actx, 0, sizeof(actx));

    /* 32KB body sent in 1KB chunks */
    size_t payload_len = TEST_LARGE_BODY;
    char *payload = malloc(payload_len);
    ASSERT_TRUE(payload != NULL);
    memset(payload, 'A', payload_len);

    MockReader mr = {
        .data = payload,
        .len  = payload_len,
        .pos  = 0,
        .chunk_size = TEST_CHUNK_1K,
    };

    KlClientHeader headers[] = {
        {"Content-Type", "application/octet-stream"},
    };

    KlClientStreamCfg stream = {
        .body_read = mock_body_read,
        .user_data = &mr,
    };

    KlClient *c = kl_client_start_s(&ev, &a, &cfg, "POST", test_url("/echo"),
                                       headers, 1, NULL, 0, &stream,
                                       async_on_done, &actx);
    ASSERT_TRUE(c != NULL);

    ASSERT_EQ(run_until_done(&ev, &actx, TEST_LONG_TIMEOUT), 0);
    ASSERT_EQ(actx.error, 0);
    ASSERT_EQ(actx.status, 200);

    /* Verify full body echoed back */
    const KlClientResponse *resp = kl_client_response(c);
    ASSERT_TRUE(resp != NULL);
    ASSERT_EQ(resp->body_len, payload_len);
    /* Verify first and last bytes */
    ASSERT_EQ(resp->body[0], 'A');
    ASSERT_EQ(resp->body[payload_len - 1], 'A');

    kl_client_free(c);
    kl_event_ctx_free(&ev);
    free(payload);
    stop_stream_server();
}

UTEST_MAIN();
