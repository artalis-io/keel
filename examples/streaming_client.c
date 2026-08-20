/*
 * streaming_client.c — HTTP/1.1 client with response and request streaming
 *
 * Concepts: kl_http_client_request_s (sync streaming), KlHttpClientStreamCfg,
 * KlHttpClientBodyFn (response push), KlHttpClientReadFn (request pull).
 *
 * Build:  make examples
 * Run:    ./examples/streaming_client
 *
 * Requires a running HTTP server on localhost:8080 (e.g. ./examples/hello_server).
 */

#include <keel/keel.h>
#include <stdio.h>
#include <string.h>

/* ── Response streaming demo ─────────────────────────────────────── */

static int on_body(const char *data, size_t len, void *user_data)
{
    (void)user_data;
    printf("  [chunk %zu bytes] %.*s\n", len, (int)len, data);
    return 0;
}

static int on_headers(int status, const KlHttpClientHeader *headers,
                       int num_headers, void *user_data)
{
    (void)user_data;
    printf("  status: %d (%d headers)\n", status, num_headers);
    for (int i = 0; i < num_headers; i++)
        printf("    %s: %s\n", headers[i].name, headers[i].value);
    return 0;
}

static void on_complete(void *user_data)
{
    (void)user_data;
    printf("  [stream complete]\n");
}

static void response_streaming_demo(void)
{
    printf("--- Response Streaming GET ---\n");

    KlAllocator alloc = kl_allocator_default();
    KlHttpClientConfig cfg = {.timeout_ms = 5000};
    KlHttpClientResponse resp;

    KlHttpClientStreamCfg stream = {
        .on_body     = on_body,
        .on_headers  = on_headers,
        .on_complete = on_complete,
        .user_data   = NULL,
    };

    int rc = kl_http_client_request_s(&alloc, &cfg, "GET",
                                   "http://localhost:8080/hello",
                                   NULL, 0, NULL, 0, &stream, &resp);
    if (rc < 0) {
        fprintf(stderr, "streaming request failed\n");
        return;
    }

    printf("  resp.body is %s (body_len=%zu)\n",
           resp.body ? "set" : "NULL", resp.body_len);
    kl_http_client_response_free(&resp);
}

/* ── Request streaming demo ──────────────────────────────────────── */

typedef struct {
    const char *data;
    size_t      len;
    size_t      pos;
} UploadCtx;

static kl_ssize_t upload_read(char *buf, size_t buf_len, void *user_data)
{
    UploadCtx *ctx = user_data;
    if (ctx->pos >= ctx->len)
        return 0;  /* EOF */
    size_t avail = ctx->len - ctx->pos;
    size_t to_copy = avail < buf_len ? avail : buf_len;
    memcpy(buf, ctx->data + ctx->pos, to_copy);
    ctx->pos += to_copy;
    return (ssize_t)to_copy;
}

static void request_streaming_demo(void)
{
    printf("\n--- Request Streaming POST ---\n");

    KlAllocator alloc = kl_allocator_default();
    KlHttpClientConfig cfg = {.timeout_ms = 5000};
    KlHttpClientResponse resp;

    const char *payload = "{\"message\":\"streamed upload\"}";
    UploadCtx upload = {
        .data = payload,
        .len  = strlen(payload),
        .pos  = 0,
    };

    KlHttpClientHeader headers[] = {
        {"Content-Type", "application/json"},
    };

    KlHttpClientStreamCfg stream = {
        .body_read = upload_read,
        .user_data = &upload,
    };

    int rc = kl_http_client_request_s(&alloc, &cfg, "POST",
                                   "http://localhost:8080/hello",
                                   headers, 1, NULL, 0, &stream, &resp);
    if (rc < 0) {
        fprintf(stderr, "streaming POST failed (server may not accept POST)\n");
        return;
    }

    printf("  status: %d\n", resp.status);
    if (resp.body)
        printf("  body:   %.*s\n", (int)resp.body_len, resp.body);
    kl_http_client_response_free(&resp);
}

/* ── Main ────────────────────────────────────────────────────────── */

int main(void)
{
    response_streaming_demo();
    request_streaming_demo();
    return 0;
}
