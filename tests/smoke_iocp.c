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
#include <stdlib.h>   /* malloc / free / strtol — bigstream dechunk */
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

/* GET /bigfile — serve a file larger than the (test-lowered, KEEL_IOCP_TF_CHUNK) TransmitFile
 * per-call cap, so the body is transmitted as several offset-advancing TransmitFile chunks.
 * The payload is a byte pattern (i & 0xFF) so the client can verify every chunk landed at the
 * right file offset (a wrong offset would scramble/repeat bytes). */
#define SMOKE_BIGFILE_PATH "smoke_iocp_bigfile.tmp"
#define SMOKE_BIGFILE_LEN  (256 * 1024)
static void handle_bigfile(KlRequest *req, KlResponse *res, void *ctx) {
    (void)req; (void)ctx;
    int fd = _open(SMOKE_BIGFILE_PATH, _O_RDONLY | _O_BINARY);
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

/* GET /bigstream — a chunked stream far larger than a slow client's receive window, so the
 * outbound buffer fills and the IOCP loop must flush it as overlapped WSASend chunks (8g-1)
 * rather than busy-spin a blocking send (the head-of-line defect). Each chunk is a run of 'S'. */
#define SMOKE_BS_CHUNK   1024
#define SMOKE_BS_CHUNKS  256
#define SMOKE_BS_LEN     (SMOKE_BS_CHUNK * SMOKE_BS_CHUNKS)   /* 256 KiB payload */
static void handle_bigstream(KlRequest *req, KlResponse *res, void *ctx) {
    (void)req; (void)ctx;
    KlWriteFn write = NULL;
    void *wctx = NULL;
    if (kl_response_begin_stream(res, 200, &write, &wctx) < 0) return;
    static char chunk[SMOKE_BS_CHUNK];
    memset(chunk, 'S', sizeof(chunk));
    for (int i = 0; i < SMOKE_BS_CHUNKS; i++)
        write(wctx, chunk, sizeof(chunk));
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
                     const KlSockAddr *src, const KlSockAddr *local, void *ud) {
    (void)ud;
    if (local) g_udp_local_ok = 1;
    kl_udp_send_to(udp, data, len, src);
}

static KlServer g_srv;

static void *server_thread(void *arg) {
    (void)arg;
    kl_server_run(&g_srv);
    return NULL;
}

/* PROXY-over-IOCP: a trusted-source connection sends a plaintext PROXY v1 header before its
 * HTTP request; the completion driver's PROXY-header phase (comp_drive_proxy) must parse it and
 * the handler must see the real client address (1.2.3.4), not the socket's 127.0.0.1. Exercises
 * accept → KL_CONN_PROXY_HEADER → parse → READING → handler over the IOCP loop (the header recv
 * is plaintext even though this is a completion backend). */
#define SMOKE_PROXY_PORT 18084
static char g_proxy_ip[64];
static void handle_proxy_probe(KlRequest *req, KlResponse *res, void *ctx) {
    (void)ctx;
    uint16_t port = 0;
    g_proxy_ip[0] = '\0';
    kl_request_peer_addr(req, g_proxy_ip, sizeof(g_proxy_ip), &port);
    kl_response_json(res, 200, SMOKE_BODY, sizeof(SMOKE_BODY) - 1);
}
static KlServer g_proxy_srv;
static void *proxy_server_thread(void *arg) { (void)arg; kl_server_run(&g_proxy_srv); return NULL; }
static int proxy_over_completion_ok(void) {
    KlConfig cfg = { .port = SMOKE_PROXY_PORT, .bind_addr = "127.0.0.1",
                     .sockets = kl_socket_provider_iocp(),
                     .proxy_trusted_cidrs = "127.0.0.1/32" };
    if (kl_server_init(&g_proxy_srv, &cfg) != 0) return 0;   /* now supported, not rejected */
    kl_server_route(&g_proxy_srv, "GET", "/p", handle_proxy_probe, NULL, NULL);
    pthread_t th;
    if (pthread_create(&th, NULL, proxy_server_thread, NULL) != 0) { kl_server_free(&g_proxy_srv); return 0; }
    for (int i = 0; i < 200 && g_proxy_srv.bound_port == 0; i++) nap_ms(5);

    int ok = 0;
    SOCKET cs = socket(AF_INET, SOCK_STREAM, 0);
    if (cs != INVALID_SOCKET) {
        DWORD tmo = 2000;
        setsockopt(cs, SOL_SOCKET, SO_RCVTIMEO, (char *)&tmo, sizeof(tmo));
        struct sockaddr_in to;
        memset(&to, 0, sizeof(to));
        to.sin_family = AF_INET;
        to.sin_port = htons(SMOKE_PROXY_PORT);
        inet_pton(AF_INET, "127.0.0.1", &to.sin_addr);
        if (connect(cs, (struct sockaddr *)&to, sizeof(to)) == 0) {
            const char *msg = "PROXY TCP4 1.2.3.4 5.6.7.8 1111 2222\r\n"
                              "GET /p HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
            if (send(cs, msg, (int)strlen(msg), 0) == (int)strlen(msg)) {
                char buf[1024]; int got = 0;
                while (got < (int)sizeof(buf) - 1) {
                    int n = recv(cs, buf + got, (int)sizeof(buf) - 1 - got, 0);
                    if (n <= 0) break;
                    got += n; buf[got] = 0;
                    if (strstr(buf, SMOKE_BODY)) break;
                }
                ok = (strstr(buf, "200 OK") != NULL && strcmp(g_proxy_ip, "1.2.3.4") == 0);
            }
        }
        closesocket(cs);
    }
    kl_server_stop(&g_proxy_srv);
    pthread_join(th, NULL);
    kl_server_free(&g_proxy_srv);
    return ok;
}

/* 8g-1 head-of-line: a slow client requests the big stream and stalls (tiny receive window,
 * no reads). The server's outbound buffer fills and it must post overlapped WSASend chunks and
 * move on — NOT busy-spin a blocking flush. We prove the loop stayed free by driving a second,
 * normal request to completion while the first is stalled, then drain the first fully and
 * verify every byte of the 256 KiB stream arrived (dechunked). */
static size_t dechunk_body_len(const char *buf, size_t n) {
    const char *end = buf + n;
    const char *p = NULL;
    for (const char *q = buf; q + 4 <= end; q++) {   /* find end of response headers */
        if (q[0] == '\r' && q[1] == '\n' && q[2] == '\r' && q[3] == '\n') { p = q + 4; break; }
    }
    if (!p) return 0;
    size_t payload = 0;
    while (p < end) {
        char *stop = NULL;
        long sz = strtol(p, &stop, 16);              /* chunk-size line (hex) */
        if (stop == p) break;
        while (stop < end && *stop != '\n') stop++;
        if (stop >= end) break;
        p = stop + 1;
        if (sz == 0) break;                          /* terminating chunk */
        if (p + sz > end) break;
        payload += (size_t)sz;
        p += sz + 2;                                 /* skip data + trailing CRLF */
    }
    return payload;
}

static int bigstream_no_hol_ok(void) {
    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(SMOKE_PORT);
    inet_pton(AF_INET, "127.0.0.1", &to.sin_addr);

    /* Conn A — slow reader: tiny receive window, request the big stream, then DON'T read. */
    SOCKET a = socket(AF_INET, SOCK_STREAM, 0);
    if (a == INVALID_SOCKET) return 0;
    int rcv = 2048;
    DWORD tmo = 3000;
    setsockopt(a, SOL_SOCKET, SO_RCVBUF, (char *)&rcv, sizeof(rcv));
    setsockopt(a, SOL_SOCKET, SO_RCVTIMEO, (char *)&tmo, sizeof(tmo));
    if (connect(a, (struct sockaddr *)&to, sizeof(to)) != 0) { closesocket(a); return 0; }
    const char *reqa = "GET /bigstream HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    if (send(a, reqa, (int)strlen(reqa), 0) < 0) { closesocket(a); return 0; }

    nap_ms(150);   /* let the server dispatch, fill A's window, post the overlapped send */

    /* Conn B — a normal request must complete promptly while A is stalled (the HOL check). */
    int b_ok = 0;
    SOCKET b = socket(AF_INET, SOCK_STREAM, 0);
    if (b != INVALID_SOCKET) {
        DWORD tb = 2000;
        setsockopt(b, SOL_SOCKET, SO_RCVTIMEO, (char *)&tb, sizeof(tb));
        if (connect(b, (struct sockaddr *)&to, sizeof(to)) == 0) {
            const char *reqb = "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
            if (send(b, reqb, (int)strlen(reqb), 0) >= 0) {
                char bb[1024]; int got = 0;
                while (got < (int)sizeof(bb) - 1) {
                    int r = recv(b, bb + got, (int)sizeof(bb) - 1 - got, 0);
                    if (r <= 0) break;
                    got += r; bb[got] = 0;
                    if (strstr(bb, SMOKE_BODY)) { b_ok = 1; break; }
                }
            }
        }
        closesocket(b);
    }

    /* Drain A fully and confirm the whole stream arrived intact. */
    char *acc = malloc(SMOKE_BS_LEN * 2);
    size_t alen = 0;
    int a_ok = 0;
    if (acc) {
        for (;;) {
            int r = recv(a, acc + alen, (int)((SMOKE_BS_LEN * 2) - alen), 0);
            if (r <= 0) break;
            alen += (size_t)r;
            if (alen >= SMOKE_BS_LEN * 2) break;
        }
        a_ok = (dechunk_body_len(acc, alen) == SMOKE_BS_LEN);
        free(acc);
    }
    closesocket(a);
    return b_ok && a_ok;
}

int main(void) {
    /* Lower TransmitFile's per-call cap so a modest /bigfile splits into several chunks —
     * exercises the offset-advancing multi-chunk TransmitFile path without a >2 GiB fixture.
     * Must be set before the first sendfile post (the cap is read + cached once). */
    _putenv_s("KEEL_IOCP_TF_CHUNK", "16384");

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
    kl_server_route(&g_srv, "GET", "/bigstream", handle_bigstream, NULL, NULL);
    kl_server_route(&g_srv, "GET", "/bigfile", handle_bigfile, NULL, NULL);

    /* Write the file the /file route serves. */
    int wfd = _open(SMOKE_FILE_PATH, _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, 0644);
    if (wfd < 0 || _write(wfd, SMOKE_FILE, sizeof(SMOKE_FILE) - 1) != (int)(sizeof(SMOKE_FILE) - 1)) {
        fprintf(stderr, "smoke-iocp: temp file write failed\n");
        if (wfd >= 0) _close(wfd);
        kl_server_free(&g_srv);
        return 1;
    }
    _close(wfd);

    /* Write the /bigfile payload — a byte pattern so the client can verify chunk offsets. */
    int bfd = _open(SMOKE_BIGFILE_PATH, _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, 0644);
    if (bfd < 0) { fprintf(stderr, "smoke-iocp: bigfile create failed\n"); kl_server_free(&g_srv); return 1; }
    {
        static unsigned char bfbuf[SMOKE_BIGFILE_LEN];
        for (int i = 0; i < SMOKE_BIGFILE_LEN; i++) bfbuf[i] = (unsigned char)(i & 0xFF);
        int wr = _write(bfd, bfbuf, SMOKE_BIGFILE_LEN);
        _close(bfd);
        if (wr != SMOKE_BIGFILE_LEN) {
            fprintf(stderr, "smoke-iocp: bigfile write failed\n");
            kl_server_free(&g_srv);
            return 1;
        }
    }

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

    /* GET /bigfile — chunked TransmitFile (>cap file split into offset-advancing chunks).
     * Verify the whole 256 KiB arrived AND matches the i&0xFF pattern (a wrong chunk offset
     * would scramble/repeat bytes). KEEL_IOCP_TF_CHUNK=16384 → 16 chunks. */
    int bigfile_ok = 0;
    if (ok && post_ok && file_ok && stream_ok) {
        KlClientResponse resp;
        memset(&resp, 0, sizeof(resp));
        int rc = kl_client_request(&alloc, &ccfg, "GET",
                                   "http://127.0.0.1:18082/bigfile",
                                   NULL, 0, NULL, 0, &resp);
        if (rc == 0) {
            bigfile_ok = (resp.status == 200 && resp.body_len == SMOKE_BIGFILE_LEN && resp.body);
            if (bigfile_ok) {
                const unsigned char *b = (const unsigned char *)resp.body;
                for (size_t i = 0; i < SMOKE_BIGFILE_LEN; i++)
                    if (b[i] != (unsigned char)(i & 0xFF)) { bigfile_ok = 0; break; }
            }
            last_status = resp.status;
            kl_client_response_free(&resp);
        } else {
            last_rc = rc;
        }
    }

    /* 8g-1: overlapped streaming flush — a stalled slow reader must not block the loop, and
     * the full stream must still arrive (comp_stream_pump + comp_on_write re-pump). */
    int bigstream_ok = (ok && post_ok && file_ok && stream_ok && bigfile_ok) ? bigstream_no_hol_ok() : 0;

    /* PROXY protocol over the IOCP loop — trusted-source header parsed, handler sees the real IP. */
    int proxy_ok = (ok && post_ok && file_ok && stream_ok && bigfile_ok && bigstream_ok)
                       ? proxy_over_completion_ok() : 0;

    /* UDP echo roundtrip — a raw datagram client hits the KlUdp on the IOCP loop. */
    int udp_ok = 0;
    if (ok && post_ok && file_ok && stream_ok && bigfile_ok && bigstream_ok && udp_ready) {
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
                sendto(cs, SMOKE_UDP, sizeof(SMOKE_UDP) - 1, 0, (struct sockaddr *)&to, sizeof(to));
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
    _unlink(SMOKE_BIGFILE_PATH);

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
    if (!bigfile_ok) {
        fprintf(stderr, "smoke-iocp: GET/bigfile (chunked TransmitFile >cap) roundtrip FAILED (rc=%d status=%d)\n",
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
    if (!bigstream_ok) {
        fprintf(stderr, "smoke-iocp: bigstream overlapped flush / HOL FAILED\n");
        return 1;
    }
    if (!proxy_ok) {
        fprintf(stderr, "smoke-iocp: PROXY-over-IOCP roundtrip FAILED\n");
        return 1;
    }
    printf("smoke-iocp: over-IOCP roundtrip OK (GET + POST body + file + bigfile-chunked + stream + bigstream + proxy + UDP + udp-local)\n");
    return 0;
}
