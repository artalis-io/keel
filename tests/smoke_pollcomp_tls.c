/*
 * smoke_pollcomp_tls.c: TLS-over-completion roundtrip on POSIX (mock-TLS backend).
 *
 * The first RUNTIME validation of the TLS-over-completion driver off Windows/mbedTLS:
 * a KlHttpServer on the pollcomp completion loop with an identity mock TLS (mock_tls.h),
 * hit by the sync KlHttpClient using the same mock TLS over loopback. Because the mock is a
 * passthrough, no crypto/mbedTLS is needed; yet the driver's TLS paths run for real:
 * comp_tls_drive (handshake → feed → decrypt), comp_tls_send_response (buffered),
 * comp_tls_send_file_chunk (file), comp_tls_send_stream (chunked). Retires the
 * "compile-gated only" caveat on TLS-over-completion, and runs under the ASan CI.
 */
#include <keel/keel.h>
#include "../src/socket.h"     /* internal kl_socket_provider_pollcomp() */
#include "mock_tls.h"

#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>

#define PORT 18095
#define BODY   "{\"tls-completion\":true}"
#define POSTB  "hello-tls-over-completion-body"
#define FDATA  "file-body-over-tls-completion"
#define FPATH  "smoke_pollcomp_tls_file.tmp"
#define STREAMB "tls-chunk-one;tls-chunk-two"

static void nap_ms(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static void h_ok(KlHttpRequest *q, KlHttpResponse *r, void *c) {
    (void)q; (void)c;
    kl_http_response_json(r, 200, BODY, sizeof(BODY) - 1);
}
static void h_echo(KlHttpRequest *q, KlHttpResponse *r, void *c) {
    (void)c;
    KlHttpBufReader *br = (KlHttpBufReader *)q->body_reader;
    if (!br || br->len == 0) { kl_http_response_error(r, 400, "body required"); return; }
    kl_http_response_status(r, 200);
    kl_http_response_body_borrow(r, br->data, br->len);
}
static void h_file(KlHttpRequest *q, KlHttpResponse *r, void *c) {
    (void)q; (void)c;
    int fd = open(FPATH, O_RDONLY);
    if (fd < 0) { kl_http_response_error(r, 500, "open"); return; }
    off_t sz = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    kl_http_response_status(r, 200);
    kl_http_response_file(r, (KlSocketHandle)fd, sz);
}
static void h_stream(KlHttpRequest *q, KlHttpResponse *r, void *c) {
    (void)q; (void)c;
    KlHttpResponseWriteFn w = NULL;
    void *wc = NULL;
    if (kl_http_response_begin_stream(r, 200, &w, &wc) < 0) return;
    w(wc, "tls-chunk-one;", 14);
    w(wc, "tls-chunk-two", 13);
    kl_http_response_end_stream(r);
}

static KlHttpServer g_srv;
static void *server_thread(void *arg) { (void)arg; kl_http_server_run(&g_srv); return NULL; }

/* One https request via the mock-TLS sync client; returns 1 if status/body match. */
static int req(KlAllocator *alloc, KlHttpClientConfig *ccfg, const char *method,
               const char *path, const char *reqbody, size_t reqlen,
               const char *want, size_t wantlen) {
    char url[128];
    snprintf(url, sizeof(url), "https://127.0.0.1:%d%s", PORT, path);
    KlHttpClientResponse resp;
    memset(&resp, 0, sizeof(resp));
    int rc = kl_http_client_request(alloc, ccfg, method, url, NULL, 0, reqbody, reqlen, &resp);
    if (rc != 0) return 0;
    int ok = (resp.status == 200 && resp.body_len == wantlen &&
              resp.body && memcmp(resp.body, want, wantlen) == 0);
    kl_http_client_response_free(&resp);
    return ok;
}

int main(void) {
    KlTlsConfig srv_tls = { .ctx = NULL, .factory = mock_tls_create, .ctx_destroy = NULL };
    KlHttpServerConfig cfg = { .port = PORT, .bind_addr = "127.0.0.1",
                     .sockets = kl_socket_provider_pollcomp(), .tls = &srv_tls };
    if (kl_http_server_init(&g_srv, &cfg) < 0) {
        fprintf(stderr, "smoke-pollcomp-tls: server init failed (err=%d)\n", g_srv.last_error);
        return 1;
    }
    kl_http_server_route(&g_srv, "GET", "/", h_ok, NULL, NULL);
    kl_http_server_route(&g_srv, "POST", "/echo", h_echo, NULL, kl_http_body_reader_buffer);
    kl_http_server_route(&g_srv, "GET", "/file", h_file, NULL, NULL);
    kl_http_server_route(&g_srv, "GET", "/stream", h_stream, NULL, NULL);

    int wfd = open(FPATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (wfd < 0 || write(wfd, FDATA, sizeof(FDATA) - 1) != (ssize_t)(sizeof(FDATA) - 1)) {
        fprintf(stderr, "smoke-pollcomp-tls: temp file write failed\n");
        if (wfd >= 0) close(wfd);
        kl_http_server_free(&g_srv);
        return 1;
    }
    close(wfd);

    pthread_t th;
    if (pthread_create(&th, NULL, server_thread, NULL) != 0) {
        kl_http_server_free(&g_srv);
        return 1;
    }

    KlAllocator alloc = kl_allocator_default();
    KlTlsConfig cli_tls = { .ctx = NULL, .factory = mock_tls_create, .ctx_destroy = NULL };
    KlHttpClientConfig ccfg = { .timeout_ms = 1000, .max_response_size = 2 * 1024 * 1024,
                            .tls = &cli_tls };

    int get_ok = 0;
    for (int i = 0; i < 50 && !get_ok; i++) {
        nap_ms(50);
        get_ok = req(&alloc, &ccfg, "GET", "/", NULL, 0, BODY, sizeof(BODY) - 1);
    }
    int post_ok = get_ok && req(&alloc, &ccfg, "POST", "/echo", POSTB, sizeof(POSTB) - 1,
                                POSTB, sizeof(POSTB) - 1);
    int file_ok = post_ok && req(&alloc, &ccfg, "GET", "/file", NULL, 0,
                                 FDATA, sizeof(FDATA) - 1);
    int stream_ok = file_ok && req(&alloc, &ccfg, "GET", "/stream", NULL, 0,
                                   STREAMB, sizeof(STREAMB) - 1);

    kl_http_server_stop(&g_srv);
    pthread_join(th, NULL);
    kl_http_server_free(&g_srv);
    unlink(FPATH);

    if (!get_ok)    { fprintf(stderr, "smoke-pollcomp-tls: GET (buffered) FAILED\n");   return 1; }
    if (!post_ok)   { fprintf(stderr, "smoke-pollcomp-tls: POST/echo (body) FAILED\n"); return 1; }
    if (!file_ok)   { fprintf(stderr, "smoke-pollcomp-tls: GET/file FAILED\n");         return 1; }
    if (!stream_ok) { fprintf(stderr, "smoke-pollcomp-tls: GET/stream FAILED\n");       return 1; }

    printf("smoke-pollcomp-tls: TLS-over-completion roundtrip OK (GET + POST body + file + stream)\n");
    return 0;
}
