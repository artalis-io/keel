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
#include <winsock2.h>   /* raw UDP client (socket/sendto/recvfrom) — before windows.h */
#include <ws2tcpip.h>
#include <keel/keel.h>
#include "../src/socket.h"   /* internal kl_socket_provider_iocp() */

#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <windows.h>
#include <io.h>       /* _open / _lseek / _write */
#include <fcntl.h>    /* _O_* */

#define SMOKE_UDP_PORT 18083
#define SMOKE_UDP "udp-echo-over-iocp"

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

/* UDP echo over the IOCP completion loop (8b-4c): a KlUdp on the server's ctx
 * receives via WSARecvFrom completions and echoes each datagram back to its source
 * (synchronous sendto — overlapped UDP send is 8b-4d). */
static KlUdp g_udp;
/* Set when a received datagram carried its local (destination) address — proves the IOCP
 * WSARecvMsg + IP_PKTINFO path captures it (parity with io_uring/pollcomp). */
static int g_udp_local_ok = 0;
static void udp_echo(KlUdp *udp, const void *data, size_t len,
                     const struct sockaddr *src, socklen_t src_len,
                     const struct sockaddr *local, socklen_t local_len, void *ud) {
    (void)ud;
    if (local && local_len > 0) g_udp_local_ok = 1;
    kl_udp_send_to(udp, data, len, src, src_len);
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

    /* UDP echo on the same IOCP loop (recv via overlapped WSARecvMsg). recv_pktinfo asks
     * for the datagram's local (dest) address so the WSARecvMsg + IP_PKTINFO capture is
     * exercised (udp_echo asserts local was delivered). */
    KlUdpConfig ucfg = { .ctx = &g_srv.ev, .bind_addr = "127.0.0.1",
                         .bind_port = SMOKE_UDP_PORT, .recv_pktinfo = 1 };
    int udp_ready = (kl_udp_init(&g_udp, &ucfg) == 0 &&
                     kl_udp_recv_start(&g_udp, udp_echo, NULL) == 0);
    if (!udp_ready) fprintf(stderr, "smoke-iocp: udp init/recv_start failed\n");

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

    /* UDP echo roundtrip — a raw datagram client hits the KlUdp on the IOCP loop. */
    int udp_ok = 0;
    if (ok && post_ok && file_ok && stream_ok && udp_ready) {
        SOCKET cs = socket(AF_INET, SOCK_DGRAM, 0);
        if (cs != INVALID_SOCKET) {
            DWORD tmo = 500;
            setsockopt(cs, SOL_SOCKET, SO_RCVTIMEO, (char *)&tmo, sizeof(tmo));
            struct sockaddr_in to;
            memset(&to, 0, sizeof(to));
            to.sin_family = AF_INET;
            to.sin_port = htons(SMOKE_UDP_PORT);
            inet_pton(AF_INET, "127.0.0.1", &to.sin_addr);
            for (int i = 0; i < 20 && !udp_ok; i++) {
                sendto(cs, SMOKE_UDP, sizeof(SMOKE_UDP) - 1, 0,
                       (struct sockaddr *)&to, sizeof(to));
                char rb[64];
                int n = recvfrom(cs, rb, sizeof(rb), 0, NULL, NULL);
                if (n == (int)(sizeof(SMOKE_UDP) - 1) &&
                    memcmp(rb, SMOKE_UDP, (size_t)n) == 0)
                    udp_ok = 1;
                else
                    nap_ms(50);
            }
            closesocket(cs);
        }
    }

    kl_udp_recv_stop(&g_udp);
    kl_udp_free(&g_udp);
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
    if (!udp_ok) {
        fprintf(stderr, "smoke-iocp: UDP echo (WSARecvMsg) roundtrip FAILED\n");
        return 1;
    }
    if (!g_udp_local_ok) {
        fprintf(stderr, "smoke-iocp: UDP local (dest) address not captured over IOCP "
                        "(WSARecvMsg/IP_PKTINFO) FAILED\n");
        return 1;
    }
    printf("smoke-iocp: over-IOCP roundtrip OK (GET + POST body + file + stream + UDP + udp-local)\n");
    return 0;
}
