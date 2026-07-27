/*
 * smoke_iocp.c — end-to-end HTTP-over-IOCP roundtrip (PAL Phase 8a, 4b).
 *
 * The first real runtime validation of the IOCP completion connection driver:
 * a KlServer running on the IOCP completion loop (BACKEND=iocp, the overlapped
 * provider) served by AcceptEx/WSARecv/WSASend, hit by the sync KlClient over
 * loopback. Windows-only (references the internal IOCP provider); the Windows-IOCP
 * CI job is its oracle. Mirrors smoke_tcp.c, with the server pinned to the IOCP
 * completion axis. GET only — request bodies over IOCP are a later increment.
 */
#include <keel/keel.h>
#include "../src/socket.h"   /* internal kl_socket_provider_iocp() */

#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <windows.h>
#include <io.h>       /* _open / _lseek / _write */
#include <fcntl.h>    /* _O_* */

static void nap_ms(int ms) { Sleep(ms); }

#define SMOKE_PORT 18082
#define SMOKE_BODY "{\"iocp\":true}"
#define SMOKE_POST "hello-over-iocp-body"
#define SMOKE_FILE "file-body-served-over-iocp-via-transmitfile"
#define SMOKE_FILE_PATH "smoke_iocp_file.tmp"
#define SMOKE_STREAM "chunk-one;chunk-two"   /* two chunks, dechunked by the client */

static void handle_ok(KlRequest *req, KlResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_response_json(res, 200, SMOKE_BODY, sizeof(SMOKE_BODY) - 1);
}

/* POST /echo — reads the request body via the buffer reader (READING_BODY over
 * IOCP) and echoes it back, exercising the completion body path (8b-1). */
static void handle_echo(KlRequest *req, KlResponse *res, void *ctx) {
    (void)ctx;
    KlBufReader *br = (KlBufReader *)req->body_reader;
    if (!br || br->len == 0) { kl_response_error(res, 400, "body required"); return; }
    kl_response_status(res, 200);
    kl_response_body_borrow(res, br->data, br->len);
}

/* GET /file — serve a file body via kl_response_file (TransmitFile over IOCP, 8b-2).
 * Opens the pre-written temp file per request; the response owns and closes the fd. */
static void handle_file(KlRequest *req, KlResponse *res, void *ctx) {
    (void)req; (void)ctx;
    int fd = _open(SMOKE_FILE_PATH, _O_RDONLY | _O_BINARY);
    if (fd < 0) { kl_response_error(res, 500, "open failed"); return; }
    long size = _lseek(fd, 0, SEEK_END);
    _lseek(fd, 0, SEEK_SET);
    kl_response_status(res, 200);
    kl_response_file(res, (KlSocketHandle)fd, (off_t)size);
}

/* GET /stream — a synchronous chunked stream produced during the handler
 * (KL_BODY_STREAM over IOCP, 8b-3). */
static void handle_stream(KlRequest *req, KlResponse *res, void *ctx) {
    (void)req; (void)ctx;
    KlWriteFn write = NULL;
    void *wctx = NULL;
    if (kl_response_begin_stream(res, 200, &write, &wctx) < 0) return;
    write(wctx, "chunk-one;", 10);
    write(wctx, "chunk-two", 9);
    kl_response_end_stream(res);
}

static KlServer g_srv;

static void *server_thread(void *arg) {
    (void)arg;
    kl_server_run(&g_srv);
    return NULL;
}

int main(void) {
    /* Pin the server to the IOCP completion loop + overlapped provider. */
    KlConfig cfg = { .port = SMOKE_PORT, .bind_addr = "127.0.0.1",
                     .sockets = kl_socket_provider_iocp() };
    if (kl_server_init(&g_srv, &cfg) < 0) {
        fprintf(stderr, "smoke-iocp: server init failed (err=%d)\n", g_srv.last_error);
        return 1;
    }
    kl_server_route(&g_srv, "GET", "/", handle_ok, NULL, NULL);
    kl_server_route(&g_srv, "POST", "/echo", handle_echo, NULL, kl_body_reader_buffer);
    kl_server_route(&g_srv, "GET", "/file", handle_file, NULL, NULL);
    kl_server_route(&g_srv, "GET", "/stream", handle_stream, NULL, NULL);

    /* Write the file the /file route serves. */
    int wfd = _open(SMOKE_FILE_PATH, _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, 0644);
    if (wfd < 0 || _write(wfd, SMOKE_FILE, sizeof(SMOKE_FILE) - 1) != (int)(sizeof(SMOKE_FILE) - 1)) {
        fprintf(stderr, "smoke-iocp: temp file write failed\n");
        if (wfd >= 0) _close(wfd);
        kl_server_free(&g_srv);
        return 1;
    }
    _close(wfd);

    pthread_t th;
    if (pthread_create(&th, NULL, server_thread, NULL) != 0) {
        fprintf(stderr, "smoke-iocp: pthread_create failed\n");
        kl_server_free(&g_srv);
        return 1;
    }

    KlAllocator alloc = kl_allocator_default();
    KlClientConfig ccfg = { .timeout_ms = 1000 };
    int ok = 0, last_rc = -1, last_status = -1, last_err = 0;
    size_t last_len = 0;
    for (int i = 0; i < 50 && !ok; i++) {
        nap_ms(50);
        KlClientResponse resp;
        memset(&resp, 0, sizeof(resp));
        last_rc = kl_client_request(&alloc, &ccfg, "GET",
                                    "http://127.0.0.1:18082/",
                                    NULL, 0, NULL, 0, &resp);
        if (last_rc == 0) {
            last_status = resp.status;
            last_len = resp.body_len;
            ok = (resp.status == 200 &&
                  resp.body_len == sizeof(SMOKE_BODY) - 1 &&
                  resp.body &&
                  memcmp(resp.body, SMOKE_BODY, sizeof(SMOKE_BODY) - 1) == 0);
            kl_client_response_free(&resp);
        } else {
            last_err = (int)resp.error;
        }
    }

    /* POST /echo — exercise the request-body path (READING_BODY over IOCP, 8b-1). */
    int post_ok = 0;
    if (ok) {
        KlClientResponse resp;
        memset(&resp, 0, sizeof(resp));
        int rc = kl_client_request(&alloc, &ccfg, "POST",
                                   "http://127.0.0.1:18082/echo",
                                   NULL, 0, SMOKE_POST, sizeof(SMOKE_POST) - 1, &resp);
        if (rc == 0) {
            post_ok = (resp.status == 200 &&
                       resp.body_len == sizeof(SMOKE_POST) - 1 &&
                       resp.body &&
                       memcmp(resp.body, SMOKE_POST, sizeof(SMOKE_POST) - 1) == 0);
            last_status = resp.status;
            kl_client_response_free(&resp);
        } else {
            last_rc = rc;
        }
    }

    /* GET /file — exercise the file-response path (TransmitFile over IOCP, 8b-2). */
    int file_ok = 0;
    if (ok && post_ok) {
        KlClientResponse resp;
        memset(&resp, 0, sizeof(resp));
        int rc = kl_client_request(&alloc, &ccfg, "GET",
                                   "http://127.0.0.1:18082/file",
                                   NULL, 0, NULL, 0, &resp);
        if (rc == 0) {
            file_ok = (resp.status == 200 &&
                       resp.body_len == sizeof(SMOKE_FILE) - 1 &&
                       resp.body &&
                       memcmp(resp.body, SMOKE_FILE, sizeof(SMOKE_FILE) - 1) == 0);
            last_status = resp.status;
            kl_client_response_free(&resp);
        } else {
            last_rc = rc;
        }
    }

    /* GET /stream — exercise the chunked/streaming path (KL_BODY_STREAM over IOCP,
     * 8b-3). The client dechunks; the body is the concatenated chunks. */
    int stream_ok = 0;
    if (ok && post_ok && file_ok) {
        KlClientResponse resp;
        memset(&resp, 0, sizeof(resp));
        int rc = kl_client_request(&alloc, &ccfg, "GET",
                                   "http://127.0.0.1:18082/stream",
                                   NULL, 0, NULL, 0, &resp);
        if (rc == 0) {
            stream_ok = (resp.status == 200 &&
                         resp.body_len == sizeof(SMOKE_STREAM) - 1 &&
                         resp.body &&
                         memcmp(resp.body, SMOKE_STREAM, sizeof(SMOKE_STREAM) - 1) == 0);
            last_status = resp.status;
            kl_client_response_free(&resp);
        } else {
            last_rc = rc;
        }
    }

    kl_server_stop(&g_srv);
    pthread_join(th, NULL);
    kl_server_free(&g_srv);
    _unlink(SMOKE_FILE_PATH);

    if (!ok) {
        fprintf(stderr, "smoke-iocp: GET roundtrip FAILED (rc=%d status=%d body_len=%zu err=%d)\n",
                last_rc, last_status, last_len, last_err);
        return 1;
    }
    if (!post_ok) {
        fprintf(stderr, "smoke-iocp: POST/echo body roundtrip FAILED (rc=%d status=%d)\n",
                last_rc, last_status);
        return 1;
    }
    if (!file_ok) {
        fprintf(stderr, "smoke-iocp: GET/file (TransmitFile) roundtrip FAILED (rc=%d status=%d)\n",
                last_rc, last_status);
        return 1;
    }
    if (!stream_ok) {
        fprintf(stderr, "smoke-iocp: GET/stream (chunked) roundtrip FAILED (rc=%d status=%d)\n",
                last_rc, last_status);
        return 1;
    }
    printf("smoke-iocp: HTTP-over-IOCP roundtrip OK (GET + POST body + file + stream)\n");
    return 0;
}
