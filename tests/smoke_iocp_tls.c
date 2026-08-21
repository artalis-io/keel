/*
 * smoke_iocp_tls.c — TLS-over-IOCP roundtrip on Windows (mock-TLS backend).
 *
 * Runtime-tests the IOCP-SPECIFIC TLS mechanics in event_iocp.c that pollcomp cannot
 * cover: the KL_IOCP_TLS_RECV op (WSARecv ciphertext into the op buffer), kl_comp_drain
 * feeding it to the engine (tls->feed_input) before surfacing a READ, and the overlapped
 * WSASend of the drained response ciphertext. It uses the portable identity mock TLS
 * (mock_tls.h) so no mbedTLS is needed — a KlHttpServer pinned to the IOCP completion loop
 * with the mock TLS, hit by the sync KlHttpClient using the same mock over loopback.
 *
 * Windows-only (references the internal IOCP provider); the Windows-IOCP CI job runs it.
 * Sibling of smoke_iocp.c (plaintext) and smoke_pollcomp_tls.c (POSIX/pollcomp).
 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <keel/keel.h>
#include "../src/socket.h"     /* internal kl_socket_provider_iocp() */
#include "mock_tls.h"

#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <windows.h>
#include <io.h>
#include <fcntl.h>

#define PORT 18097
#define BODY   "{\"tls-iocp\":true}"
#define POSTB  "hello-tls-over-iocp-body"
#define FDATA  "file-body-over-tls-iocp-via-mock"
#define FPATH  "smoke_iocp_tls_file.tmp"
#define STREAMB "iocp-tls-one;iocp-tls-two"

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
    int fd = _open(FPATH, _O_RDONLY | _O_BINARY);
    if (fd < 0) { kl_http_response_error(r, 500, "open"); return; }
    long sz = _lseek(fd, 0, SEEK_END);
    _lseek(fd, 0, SEEK_SET);
    kl_http_response_status(r, 200);
    kl_http_response_file(r, (KlSocketHandle)fd, (off_t)sz);
}
static void h_stream(KlHttpRequest *q, KlHttpResponse *r, void *c) {
    (void)q; (void)c;
    KlHttpResponseWriteFn w = NULL;
    void *wc = NULL;
    if (kl_http_response_begin_stream(r, 200, &w, &wc) < 0) return;
    w(wc, "iocp-tls-one;", 13);
    w(wc, "iocp-tls-two", 12);
    kl_http_response_end_stream(r);
}

static KlHttpServer g_srv;
static void *server_thread(void *arg) { (void)arg; kl_http_server_run(&g_srv); return NULL; }

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
                     .sockets = kl_socket_provider_iocp(), .tls = &srv_tls };
    if (kl_http_server_init(&g_srv, &cfg) < 0) {
        fprintf(stderr, "smoke-iocp-tls: server init failed (err=%d)\n", g_srv.last_error);
        return 1;
    }
    kl_http_server_route(&g_srv, "GET", "/", h_ok, NULL, NULL);
    kl_http_server_route(&g_srv, "POST", "/echo", h_echo, NULL, kl_http_body_reader_buffer);
    kl_http_server_route(&g_srv, "GET", "/file", h_file, NULL, NULL);
    kl_http_server_route(&g_srv, "GET", "/stream", h_stream, NULL, NULL);

    int wfd = _open(FPATH, _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, 0644);
    if (wfd < 0 || _write(wfd, FDATA, sizeof(FDATA) - 1) != (int)(sizeof(FDATA) - 1)) {
        fprintf(stderr, "smoke-iocp-tls: temp file write failed\n");
        if (wfd >= 0) _close(wfd);
        kl_http_server_free(&g_srv);
        return 1;
    }
    _close(wfd);

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
        Sleep(50);
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
    _unlink(FPATH);

    if (!get_ok)    { fprintf(stderr, "smoke-iocp-tls: GET (buffered) FAILED\n");   return 1; }
    if (!post_ok)   { fprintf(stderr, "smoke-iocp-tls: POST/echo (body) FAILED\n"); return 1; }
    if (!file_ok)   { fprintf(stderr, "smoke-iocp-tls: GET/file FAILED\n");         return 1; }
    if (!stream_ok) { fprintf(stderr, "smoke-iocp-tls: GET/stream FAILED\n");       return 1; }

    printf("smoke-iocp-tls: TLS-over-IOCP roundtrip OK (GET + POST body + file + stream)\n");
    return 0;
}
