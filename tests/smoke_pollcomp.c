/*
 * smoke_pollcomp.c — end-to-end HTTP-over-completion roundtrip on POSIX (PAL 8d-0.5).
 *
 * The first runtime validation of the platform-independent completion connection
 * driver (completion_driver.c) OFF Windows: a KlServer pinned to the portable poll()
 * completion backend (BACKEND=pollcomp, the overlapped provider), served by the same
 * accept/recv/send/sendfile completion mechanics the IOCP backend implements — hit by
 * the sync KlClient over loopback. It proves the completion axis is genuinely
 * platform-independent (the driver is reused verbatim) and makes it CI-testable.
 *
 * POSIX sibling of smoke_iocp.c; identical coverage: GET + POST body + file (sendfile)
 * + chunked stream + UDP echo, all over the completion loop.
 */
#include <keel/keel.h>
#include "../src/socket.h"   /* internal kl_socket_provider_pollcomp() */

#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define SMOKE_PORT 18092
#define SMOKE_UDP_PORT 18093
#define SMOKE_BODY "{\"pollcomp\":true}"
#define SMOKE_POST "hello-over-pollcomp-body"
#define SMOKE_FILE "file-body-served-over-completion-via-sendfile"
#define SMOKE_FILE_PATH "smoke_pollcomp_file.tmp"
#define SMOKE_STREAM "chunk-one;chunk-two"
#define SMOKE_UDP "udp-echo-over-pollcomp"

static void nap_ms(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static void handle_ok(KlRequest *req, KlResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_response_json(res, 200, SMOKE_BODY, sizeof(SMOKE_BODY) - 1);
}

static void handle_echo(KlRequest *req, KlResponse *res, void *ctx) {
    (void)ctx;
    KlBufReader *br = (KlBufReader *)req->body_reader;
    if (!br || br->len == 0) { kl_response_error(res, 400, "body required"); return; }
    kl_response_status(res, 200);
    kl_response_body_borrow(res, br->data, br->len);
}

static void handle_file(KlRequest *req, KlResponse *res, void *ctx) {
    (void)req; (void)ctx;
    int fd = open(SMOKE_FILE_PATH, O_RDONLY);
    if (fd < 0) { kl_response_error(res, 500, "open failed"); return; }
    off_t size = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    kl_response_status(res, 200);
    kl_response_file(res, (KlSocketHandle)fd, size);
}

static void handle_stream(KlRequest *req, KlResponse *res, void *ctx) {
    (void)req; (void)ctx;
    KlWriteFn write_fn = NULL;
    void *wctx = NULL;
    if (kl_response_begin_stream(res, 200, &write_fn, &wctx) < 0) return;
    write_fn(wctx, "chunk-one;", 10);
    write_fn(wctx, "chunk-two", 9);
    kl_response_end_stream(res);
}

static KlUdp g_udp;
static void udp_echo(KlUdp *udp, const void *data, size_t len,
                     const struct sockaddr *src, socklen_t src_len,
                     const struct sockaddr *local, socklen_t local_len, void *ud) {
    (void)local; (void)local_len; (void)ud;
    kl_udp_send_to(udp, data, len, src, src_len);
}

static KlServer g_srv;

static void *server_thread(void *arg) {
    (void)arg;
    kl_server_run(&g_srv);
    return NULL;
}

int main(void) {
    KlConfig cfg = { .port = SMOKE_PORT, .bind_addr = "127.0.0.1",
                     .sockets = kl_socket_provider_pollcomp() };
    if (kl_server_init(&g_srv, &cfg) < 0) {
        fprintf(stderr, "smoke-pollcomp: server init failed (err=%d)\n", g_srv.last_error);
        return 1;
    }
    kl_server_route(&g_srv, "GET", "/", handle_ok, NULL, NULL);
    kl_server_route(&g_srv, "POST", "/echo", handle_echo, NULL, kl_body_reader_buffer);
    kl_server_route(&g_srv, "GET", "/file", handle_file, NULL, NULL);
    kl_server_route(&g_srv, "GET", "/stream", handle_stream, NULL, NULL);

    int wfd = open(SMOKE_FILE_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (wfd < 0 || write(wfd, SMOKE_FILE, sizeof(SMOKE_FILE) - 1) != (ssize_t)(sizeof(SMOKE_FILE) - 1)) {
        fprintf(stderr, "smoke-pollcomp: temp file write failed\n");
        if (wfd >= 0) close(wfd);
        kl_server_free(&g_srv);
        return 1;
    }
    close(wfd);

    KlUdpConfig ucfg = { .ctx = &g_srv.ev, .bind_addr = "127.0.0.1",
                         .bind_port = SMOKE_UDP_PORT };
    int udp_ready = (kl_udp_init(&g_udp, &ucfg) == 0 &&
                     kl_udp_recv_start(&g_udp, udp_echo, NULL) == 0);
    if (!udp_ready) fprintf(stderr, "smoke-pollcomp: udp init/recv_start failed\n");

    pthread_t th;
    if (pthread_create(&th, NULL, server_thread, NULL) != 0) {
        fprintf(stderr, "smoke-pollcomp: pthread_create failed\n");
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
        last_rc = kl_client_request(&alloc, &ccfg, "GET", "http://127.0.0.1:18092/",
                                    NULL, 0, NULL, 0, &resp);
        if (last_rc == 0) {
            last_status = resp.status;
            last_len = resp.body_len;
            ok = (resp.status == 200 && resp.body_len == sizeof(SMOKE_BODY) - 1 &&
                  resp.body && memcmp(resp.body, SMOKE_BODY, sizeof(SMOKE_BODY) - 1) == 0);
            kl_client_response_free(&resp);
        } else {
            last_err = (int)resp.error;
        }
    }

    int post_ok = 0;
    if (ok) {
        KlClientResponse resp;
        memset(&resp, 0, sizeof(resp));
        int rc = kl_client_request(&alloc, &ccfg, "POST", "http://127.0.0.1:18092/echo",
                                   NULL, 0, SMOKE_POST, sizeof(SMOKE_POST) - 1, &resp);
        if (rc == 0) {
            post_ok = (resp.status == 200 && resp.body_len == sizeof(SMOKE_POST) - 1 &&
                       resp.body && memcmp(resp.body, SMOKE_POST, sizeof(SMOKE_POST) - 1) == 0);
            last_status = resp.status;
            kl_client_response_free(&resp);
        } else { last_rc = rc; }
    }

    int file_ok = 0;
    if (ok && post_ok) {
        KlClientResponse resp;
        memset(&resp, 0, sizeof(resp));
        int rc = kl_client_request(&alloc, &ccfg, "GET", "http://127.0.0.1:18092/file",
                                   NULL, 0, NULL, 0, &resp);
        if (rc == 0) {
            file_ok = (resp.status == 200 && resp.body_len == sizeof(SMOKE_FILE) - 1 &&
                       resp.body && memcmp(resp.body, SMOKE_FILE, sizeof(SMOKE_FILE) - 1) == 0);
            last_status = resp.status;
            kl_client_response_free(&resp);
        } else { last_rc = rc; }
    }

    int stream_ok = 0;
    if (ok && post_ok && file_ok) {
        KlClientResponse resp;
        memset(&resp, 0, sizeof(resp));
        int rc = kl_client_request(&alloc, &ccfg, "GET", "http://127.0.0.1:18092/stream",
                                   NULL, 0, NULL, 0, &resp);
        if (rc == 0) {
            stream_ok = (resp.status == 200 && resp.body_len == sizeof(SMOKE_STREAM) - 1 &&
                         resp.body && memcmp(resp.body, SMOKE_STREAM, sizeof(SMOKE_STREAM) - 1) == 0);
            last_status = resp.status;
            kl_client_response_free(&resp);
        } else { last_rc = rc; }
    }

    int udp_ok = 0;
    if (ok && post_ok && file_ok && stream_ok && udp_ready) {
        int cs = socket(AF_INET, SOCK_DGRAM, 0);
        if (cs >= 0) {
            struct timeval tmo = { 0, 500000 };
            setsockopt(cs, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof(tmo));
            struct sockaddr_in to;
            memset(&to, 0, sizeof(to));
            to.sin_family = AF_INET;
            to.sin_port = htons(SMOKE_UDP_PORT);
            inet_pton(AF_INET, "127.0.0.1", &to.sin_addr);
            for (int i = 0; i < 20 && !udp_ok; i++) {
                sendto(cs, SMOKE_UDP, sizeof(SMOKE_UDP) - 1, 0,
                       (struct sockaddr *)&to, sizeof(to));
                char rb[64];
                ssize_t n = recvfrom(cs, rb, sizeof(rb), 0, NULL, NULL);
                if (n == (ssize_t)(sizeof(SMOKE_UDP) - 1) && memcmp(rb, SMOKE_UDP, (size_t)n) == 0)
                    udp_ok = 1;
                else
                    nap_ms(50);
            }
            close(cs);
        }
    }

    kl_udp_recv_stop(&g_udp);
    kl_udp_free(&g_udp);
    kl_server_stop(&g_srv);
    pthread_join(th, NULL);
    kl_server_free(&g_srv);
    unlink(SMOKE_FILE_PATH);

    if (!ok) {
        fprintf(stderr, "smoke-pollcomp: GET roundtrip FAILED (rc=%d status=%d body_len=%zu err=%d)\n",
                last_rc, last_status, last_len, last_err);
        return 1;
    }
    if (!post_ok) { fprintf(stderr, "smoke-pollcomp: POST/echo FAILED (rc=%d status=%d)\n", last_rc, last_status); return 1; }
    if (!file_ok) { fprintf(stderr, "smoke-pollcomp: GET/file (sendfile) FAILED (rc=%d status=%d)\n", last_rc, last_status); return 1; }
    if (!stream_ok) { fprintf(stderr, "smoke-pollcomp: GET/stream FAILED (rc=%d status=%d)\n", last_rc, last_status); return 1; }
    if (!udp_ok) { fprintf(stderr, "smoke-pollcomp: UDP echo FAILED\n"); return 1; }

    printf("smoke-pollcomp: over-completion roundtrip OK (GET + POST body + file + stream + UDP)\n");
    return 0;
}
