/*
 * compress_server.c: Response compression with miniz gzip backend
 *
 * Concepts: KlCompress vtable, KlCompressConfig, kl_http_response_body_compress,
 * KlHttpCompressStream, kl_http_compress_stream_begin/write/end.
 *
 * Demonstrates:
 *   /json    : buffer body compression (gzip if Accept-Encoding: gzip)
 *   /stream  : streaming compression over chunked transfer
 *
 * Build:  make examples KEEL_COMPRESS=miniz MINIZ_DIR=/path/to/miniz
 * Run:    ./examples/compress_server
 * Test:   curl -H "Accept-Encoding: gzip" localhost:8080/json | gunzip
 *         curl -H "Accept-Encoding: gzip" localhost:8080/stream | gunzip
 */

#include <keel/http_compress.h>
#include <keel/keel.h>
#include "compress_miniz.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    KlHttpServer *server;
    KlCompressConfig *compress;
} AppCtx;

/* Buffer body compression; compresses the entire response at once */
static void handle_json(KlHttpRequest *req, KlHttpResponse *res, void *user_data) {
    AppCtx *app = user_data;
    const char *json =
        "{\"message\":\"Hello from Keel!\","
        "\"description\":\"This response is gzip-compressed when the client "
        "sends Accept-Encoding: gzip. The compression uses a pluggable vtable "
        "with a miniz gzip backend. If the compressed output is larger than "
        "the original, Keel falls back to sending uncompressed data.\","
        "\"items\":[1,2,3,4,5,6,7,8,9,10]}";

    kl_http_response_status(res, 200);
    kl_http_response_header(res, "Content-Type", "application/json");

    /* Only compress if client accepts gzip */
    const char *ae = kl_http_request_header(req, "Accept-Encoding");
    if (ae && strstr(ae, "gzip") && app->compress) {
        kl_http_response_body_compress(res, app->compress, json, strlen(json));
    } else {
        kl_http_response_body_borrow(res, json, strlen(json));
    }
}

/* Streaming compression; compresses chunks as they are written */
static void handle_stream(KlHttpRequest *req, KlHttpResponse *res, void *user_data) {
    AppCtx *app = user_data;

    const char *ae = kl_http_request_header(req, "Accept-Encoding");
    if (ae && strstr(ae, "gzip") && app->compress) {
        /* Compressed chunked stream */
        kl_http_response_header(res, "Content-Type", "text/plain");

        KlHttpCompressStream cs;
        if (kl_http_compress_stream_begin(res, app->compress, 200, &cs) < 0)
            return;

        for (int i = 0; i < 10; i++) {
            char line[64];
            int n = snprintf(line, sizeof(line), "chunk %d: hello world\n", i);
            if (n < 0) break;
            if (kl_http_compress_stream_write(&cs, line, (size_t)n) < 0) break;
        }

        kl_http_compress_stream_end(&cs);
    } else {
        /* Uncompressed chunked stream */
        KlHttpResponseWriteFn write_fn;
        void *write_ctx;
        kl_http_response_header(res, "Content-Type", "text/plain");
        if (kl_http_response_begin_stream(res, 200, &write_fn, &write_ctx) < 0)
            return;

        for (int i = 0; i < 10; i++) {
            char line[64];
            int n = snprintf(line, sizeof(line), "chunk %d: hello world\n", i);
            if (n < 0) break;
            if (write_fn(write_ctx, line, (size_t)n) < 0) break;
        }

        kl_http_response_end_stream(res);
    }
}

int main(void) {
    /* Create gzip compression context (level 6 = default) */
    KlAllocator alloc = kl_allocator_default();
    KlCompressCtx *cctx = kl_compress_miniz_ctx_create(6, &alloc);
    if (!cctx) { fprintf(stderr, "compress ctx failed\n"); return 1; }

    KlCompressConfig comp = {
        .ctx = cctx,
        .factory = kl_compress_miniz_create,
        .ctx_destroy = kl_compress_miniz_ctx_destroy,
    };

    KlHttpServer s;
    KlHttpServerConfig cfg = {
        .port = 8080,
        .compress = &comp,
        .install_signal_handlers = 1,
    };
    if (kl_http_server_init(&s, &cfg) < 0) return 1;

    AppCtx app = { .server = &s, .compress = &comp };
    kl_http_server_route(&s, "GET", "/json", handle_json, &app, NULL);
    kl_http_server_route(&s, "GET", "/stream", handle_stream, &app, NULL);

    printf("Compression example listening on :8080\n");
    printf("  curl -H 'Accept-Encoding: gzip' localhost:8080/json | gunzip\n");
    printf("  curl -H 'Accept-Encoding: gzip' localhost:8080/stream | gunzip\n");
    printf("  curl localhost:8080/json   (no compression)\n");
    kl_http_server_run(&s);
    kl_http_server_free(&s);
    return 0;
}
