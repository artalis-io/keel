/*
 * smoke_iouring.c — end-to-end HTTP-over-completion roundtrip on the completion-native
 * io_uring backend (PAL Phase 8f).
 *
 * The runtime validation of the THIRD completion backend (event_iouring.c): a
 * KlServer pinned to the io_uring completion loop (BACKEND=iouring, the overlapped
 * provider), served by the SAME completion driver (completion_driver.c) the IOCP and
 * pollcomp backends drive — reused verbatim — hit by the sync KlClient over loopback. It
 * proves the completion axis is genuinely platform-independent on a real, completion-native
 * kernel engine, and is the first production Linux completion backend (pollcomp is a test
 * facade). Linux-only (io_uring); the Completion CI job runs it.
 *
 * Sibling of smoke_pollcomp.c / smoke_iocp.c; identical coverage: GET + POST body + file
 * (sendfile) + chunked stream + large partial-send + UDP echo + h2c + h2 prior-knowledge +
 * idle-timeout + keep-alive churn + resilience, all over the io_uring completion loop.
 */
#include <keel/keel.h>
#include "../src/socket.h"   /* internal kl_socket_provider_iouring() */

#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/time.h>          /* struct timeval (SO_RCVTIMEO) */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define SMOKE_PORT 18094
#define SMOKE_UDP_PORT 18095
#define SMOKE_BODY "{\"iouring\":true}"
#define SMOKE_POST "hello-over-iouring-completion-body"
#define SMOKE_FILE "file-body-served-over-io_uring-completion-via-sendfile"
#define SMOKE_FILE_PATH "smoke_iouring_file.tmp"
#define SMOKE_BIGFILE_PATH "smoke_iouring_bigfile.tmp"
#define SMOKE_BIGFILE_LEN (200 * 1024)   /* > pipe capacity → multi-chunk splice + short-out */
#define SMOKE_STREAM "chunk-one;chunk-two"
#define SMOKE_UDP "udp-echo-over-iouring"

static void nap_ms(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static void handle_ok(KlRequest *req, KlResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_response_json(res, 200, SMOKE_BODY, sizeof(SMOKE_BODY) - 1);
}

#define SMOKE_BIG_LEN (256 * 1024)
static char g_big[SMOKE_BIG_LEN];
static void handle_big(KlRequest *req, KlResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_response_status(res, 200);
    kl_response_body_borrow(res, g_big, SMOKE_BIG_LEN);
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

/* GET /bigfile — a >pipe-capacity file so the splice sendfile loops over multiple
 * file→pipe→socket chunks and exercises the short-splice-out re-submit path (8f-2). */
static void handle_bigfile(KlRequest *req, KlResponse *res, void *ctx) {
    (void)req; (void)ctx;
    int fd = open(SMOKE_BIGFILE_PATH, O_RDONLY);
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

/* Minimal echo HTTP/2 server session (no nghttp2/HPACK) — echoes bytes through the send
 * callback; enough to exercise comp_h2_drive end to end over io_uring completion. */
typedef struct {
    KlH2ServerSession   base;
    KlH2ServerCallbacks cb;
    void               *cb_ud;
    KlAllocator        *alloc;
    char                pending[256];
    size_t              pending_len;
} EchoH2;

static ssize_t echo_recv(KlH2ServerSession *self, const void *data, size_t len) {
    EchoH2 *e = (EchoH2 *)self;
    if (len > 0) {
        size_t n = len < sizeof(e->pending) ? len : sizeof(e->pending);
        memcpy(e->pending, data, n);
        e->pending_len = n;
    }
    return (ssize_t)len;
}
static int echo_want_write(KlH2ServerSession *self) { return ((EchoH2 *)self)->pending_len > 0; }
static int echo_flush(KlH2ServerSession *self) {
    EchoH2 *e = (EchoH2 *)self;
    if (e->pending_len > 0) { e->cb.send(e->cb_ud, e->pending, e->pending_len); e->pending_len = 0; }
    return 0;
}
static int echo_submit(KlH2ServerSession *self, uint32_t sid, int status,
                       const char **hn, const char **hv, int nh, const void *body, size_t bl) {
    (void)self; (void)sid; (void)status; (void)hn; (void)hv; (void)nh; (void)body; (void)bl;
    return 0;
}
static int echo_shutdown(KlH2ServerSession *self) { (void)self; return 0; }
static void echo_destroy(KlH2ServerSession *self) {
    EchoH2 *e = (EchoH2 *)self;
    kl_free(e->alloc, e, sizeof(*e));
}
static KlH2ServerSession *echo_factory(KlAllocator *alloc, KlH2ServerCallbacks *cb, void *ud) {
    EchoH2 *e = kl_malloc(alloc, sizeof(*e));
    if (!e) return NULL;
    memset(e, 0, sizeof(*e));
    e->base.recv = echo_recv;
    e->base.submit_response = echo_submit;
    e->base.want_write = echo_want_write;
    e->base.flush = echo_flush;
    e->base.shutdown = echo_shutdown;
    e->base.destroy = echo_destroy;
    e->cb = *cb;
    e->cb_ud = ud;
    e->alloc = alloc;
    return &e->base;
}

/* Idle-timeout roundtrip (hardening): open a connection, send nothing, confirm the server
 * times it out and closes it (read_timeout_ms=400) — completion-path slowloris defense
 * via kl_comp_cancel (prep_cancel of the in-flight recv). */
static int idle_timeout_closes(void) {
    int cs = socket(AF_INET, SOCK_STREAM, 0);
    if (cs < 0) return 0;
    struct timeval tmo = { 3, 0 };
    setsockopt(cs, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof(tmo));
    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(SMOKE_PORT);
    inet_pton(AF_INET, "127.0.0.1", &to.sin_addr);
    if (connect(cs, (struct sockaddr *)&to, sizeof(to)) < 0) { close(cs); return 0; }
    char b[128];
    ssize_t n = read(cs, b, sizeof(b));
    if (n > 0) n = read(cs, b, sizeof(b));   /* drain the 408, then expect EOF */
    close(cs);
    return n == 0;
}

static int connect_client(void) {
    int cs = socket(AF_INET, SOCK_STREAM, 0);
    if (cs < 0) return -1;
    struct timeval tmo = { 2, 0 };
    setsockopt(cs, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof(tmo));
    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(SMOKE_PORT);
    inet_pton(AF_INET, "127.0.0.1", &to.sin_addr);
    if (connect(cs, (struct sockaddr *)&to, sizeof(to)) < 0) { close(cs); return -1; }
    return cs;
}

/* Keep-alive churn: N sequential HTTP/1.1 requests on ONE connection. Exercises the
 * completion keep-alive reset (send_complete → post_recv) repeatedly on a single conn. */
static int keepalive_churn(int n) {
    int cs = connect_client();
    if (cs < 0) return 0;
    const char *req = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
    for (int k = 0; k < n; k++) {
        if (write(cs, req, strlen(req)) < 0) { close(cs); return 0; }
        char buf[1024];
        size_t got = 0;
        int found = 0;
        while (got < sizeof(buf) - 1) {
            ssize_t r = read(cs, buf + got, sizeof(buf) - 1 - got);
            if (r <= 0) break;
            got += (size_t)r;
            buf[got] = 0;
            if (strstr(buf, SMOKE_BODY)) { found = 1; break; }
        }
        if (!found) { close(cs); return 0; }
    }
    close(cs);
    return 1;
}

/* Resilience: an abrupt mid-request drop and a malformed request must be cleaned up
 * without crashing/leaking (ASan CI), then a normal request must still succeed. */
static int resilience_ok(KlAllocator *alloc, KlClientConfig *ccfg) {
    int cs = connect_client();
    if (cs >= 0) { (void)!write(cs, "GET / HTT", 9); close(cs); }
    cs = connect_client();
    if (cs >= 0) {
        (void)!write(cs, "@@@ not-http\r\n\r\n", 16);
        char b[128];
        (void)!read(cs, b, sizeof(b));
        close(cs);
    }
    nap_ms(50);
    KlClientResponse resp;
    memset(&resp, 0, sizeof(resp));
    int rc = kl_client_request(alloc, ccfg, "GET", "http://127.0.0.1:18094/",
                               NULL, 0, NULL, 0, &resp);
    int ok = (rc == 0 && resp.status == 200);
    if (rc == 0) kl_client_response_free(&resp);
    return ok;
}

static KlServer g_srv;

static void *server_thread(void *arg) {
    (void)arg;
    kl_server_run(&g_srv);
    return NULL;
}

#define SMOKE_H2 "PING-OVER-H2C-IOURING"

static int h2c_roundtrip(void) {
    int cs = socket(AF_INET, SOCK_STREAM, 0);
    if (cs < 0) return 0;
    struct timeval tmo = { 1, 0 };
    setsockopt(cs, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof(tmo));
    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(SMOKE_PORT);
    inet_pton(AF_INET, "127.0.0.1", &to.sin_addr);
    if (connect(cs, (struct sockaddr *)&to, sizeof(to)) < 0) { close(cs); return 0; }

    const char *req = "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                      "Upgrade: h2c\r\nConnection: Upgrade\r\n\r\n";
    if (write(cs, req, strlen(req)) < 0) { close(cs); return 0; }

    char buf[512];
    size_t got = 0;
    int have_101 = 0;
    while (got < sizeof(buf) - 1) {
        ssize_t n = read(cs, buf + got, sizeof(buf) - 1 - got);
        if (n <= 0) break;
        got += (size_t)n;
        buf[got] = 0;
        if (strstr(buf, "\r\n\r\n")) { have_101 = (strstr(buf, " 101 ") != NULL); break; }
    }
    if (!have_101) { close(cs); return 0; }

    if (write(cs, SMOKE_H2, sizeof(SMOKE_H2) - 1) < 0) { close(cs); return 0; }
    char rb[128];
    size_t rlen = 0;
    while (rlen < sizeof(SMOKE_H2) - 1) {
        ssize_t n = read(cs, rb + rlen, sizeof(rb) - rlen);
        if (n <= 0) break;
        rlen += (size_t)n;
    }
    close(cs);
    return rlen == sizeof(SMOKE_H2) - 1 && memcmp(rb, SMOKE_H2, rlen) == 0;
}

#define SMOKE_H2PK "FRAME-VIA-PRIOR-KNOWLEDGE-IOURING"

static int h2_prior_knowledge_roundtrip(void) {
    int cs = socket(AF_INET, SOCK_STREAM, 0);
    if (cs < 0) return 0;
    struct timeval tmo = { 1, 0 };
    setsockopt(cs, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof(tmo));
    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(SMOKE_PORT);
    inet_pton(AF_INET, "127.0.0.1", &to.sin_addr);
    if (connect(cs, (struct sockaddr *)&to, sizeof(to)) < 0) { close(cs); return 0; }

    char msg[128];
    memcpy(msg, "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n", 24);
    memcpy(msg + 24, SMOKE_H2PK, sizeof(SMOKE_H2PK) - 1);
    if (write(cs, msg, 24 + sizeof(SMOKE_H2PK) - 1) < 0) { close(cs); return 0; }

    char rb[128];
    size_t rlen = 0;
    while (rlen < sizeof(SMOKE_H2PK) - 1) {
        ssize_t n = read(cs, rb + rlen, sizeof(rb) - rlen);
        if (n <= 0) break;
        rlen += (size_t)n;
    }
    close(cs);
    return rlen == sizeof(SMOKE_H2PK) - 1 && memcmp(rb, SMOKE_H2PK, rlen) == 0;
}

int main(void) {
    static KlH2ServerConfig h2cfg = { .factory = echo_factory };
    KlConfig cfg = { .port = SMOKE_PORT, .bind_addr = "127.0.0.1",
                     .sockets = kl_socket_provider_iouring(), .h2 = &h2cfg,
                     .read_timeout_ms = 400 };
    if (kl_server_init(&g_srv, &cfg) < 0) {
        fprintf(stderr, "smoke-iouring: server init failed (err=%d)\n", g_srv.last_error);
        return 1;
    }
    kl_server_route(&g_srv, "GET", "/", handle_ok, NULL, NULL);
    kl_server_route(&g_srv, "POST", "/echo", handle_echo, NULL, kl_body_reader_buffer);
    kl_server_route(&g_srv, "GET", "/file", handle_file, NULL, NULL);
    kl_server_route(&g_srv, "GET", "/stream", handle_stream, NULL, NULL);
    kl_server_route(&g_srv, "GET", "/big", handle_big, NULL, NULL);
    kl_server_route(&g_srv, "GET", "/bigfile", handle_bigfile, NULL, NULL);
    memset(g_big, 'A', sizeof(g_big));

    int wfd = open(SMOKE_FILE_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (wfd < 0 || write(wfd, SMOKE_FILE, sizeof(SMOKE_FILE) - 1) != (ssize_t)(sizeof(SMOKE_FILE) - 1)) {
        fprintf(stderr, "smoke-iouring: temp file write failed\n");
        if (wfd >= 0) close(wfd);
        kl_server_free(&g_srv);
        return 1;
    }
    close(wfd);

    /* A >pipe-capacity file for the multi-chunk splice path (8f-2). */
    int bfd = open(SMOKE_BIGFILE_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int bigfile_ok = (bfd >= 0);
    for (int w = 0; bigfile_ok && w < SMOKE_BIGFILE_LEN; ) {
        char blk[4096];
        memset(blk, 'A', sizeof(blk));
        int chunk = (SMOKE_BIGFILE_LEN - w) < (int)sizeof(blk) ? (SMOKE_BIGFILE_LEN - w) : (int)sizeof(blk);
        if (write(bfd, blk, (size_t)chunk) != chunk) { bigfile_ok = 0; break; }
        w += chunk;
    }
    if (bfd >= 0) close(bfd);
    if (!bigfile_ok) {
        fprintf(stderr, "smoke-iouring: big temp file write failed\n");
        unlink(SMOKE_FILE_PATH);
        kl_server_free(&g_srv);
        return 1;
    }

    KlUdpConfig ucfg = { .ctx = &g_srv.ev, .bind_addr = "127.0.0.1",
                         .bind_port = SMOKE_UDP_PORT };
    int udp_ready = (kl_udp_init(&g_udp, &ucfg) == 0 &&
                     kl_udp_recv_start(&g_udp, udp_echo, NULL) == 0);
    if (!udp_ready) fprintf(stderr, "smoke-iouring: udp init/recv_start failed\n");

    pthread_t th;
    if (pthread_create(&th, NULL, server_thread, NULL) != 0) {
        fprintf(stderr, "smoke-iouring: pthread_create failed\n");
        kl_server_free(&g_srv);
        return 1;
    }

    KlAllocator alloc = kl_allocator_default();
    KlClientConfig ccfg = { .timeout_ms = 1000, .max_response_size = 2 * 1024 * 1024 };
    int ok = 0, last_rc = -1, last_status = -1, last_err = 0;
    size_t last_len = 0;
    for (int i = 0; i < 50 && !ok; i++) {
        nap_ms(50);
        KlClientResponse resp;
        memset(&resp, 0, sizeof(resp));
        last_rc = kl_client_request(&alloc, &ccfg, "GET", "http://127.0.0.1:18094/",
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
        int rc = kl_client_request(&alloc, &ccfg, "POST", "http://127.0.0.1:18094/echo",
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
        int rc = kl_client_request(&alloc, &ccfg, "GET", "http://127.0.0.1:18094/file",
                                   NULL, 0, NULL, 0, &resp);
        if (rc == 0) {
            file_ok = (resp.status == 200 && resp.body_len == sizeof(SMOKE_FILE) - 1 &&
                       resp.body && memcmp(resp.body, SMOKE_FILE, sizeof(SMOKE_FILE) - 1) == 0);
            last_status = resp.status;
            kl_client_response_free(&resp);
        } else { last_rc = rc; }
    }

    /* Large file → multi-chunk splice sendfile + short-splice-out re-submit (8f-2). */
    int bigfile_dl_ok = 0;
    if (ok && post_ok && file_ok) {
        KlClientResponse resp;
        memset(&resp, 0, sizeof(resp));
        int rc = kl_client_request(&alloc, &ccfg, "GET", "http://127.0.0.1:18094/bigfile",
                                   NULL, 0, NULL, 0, &resp);
        if (rc == 0) {
            bigfile_dl_ok = (resp.status == 200 && resp.body_len == SMOKE_BIGFILE_LEN &&
                             resp.body && resp.body[0] == 'A' &&
                             resp.body[SMOKE_BIGFILE_LEN - 1] == 'A');
            last_status = resp.status;
            kl_client_response_free(&resp);
        } else { last_rc = rc; }
    }

    int stream_ok = 0;
    if (ok && post_ok && file_ok && bigfile_dl_ok) {
        KlClientResponse resp;
        memset(&resp, 0, sizeof(resp));
        int rc = kl_client_request(&alloc, &ccfg, "GET", "http://127.0.0.1:18094/stream",
                                   NULL, 0, NULL, 0, &resp);
        if (rc == 0) {
            stream_ok = (resp.status == 200 && resp.body_len == sizeof(SMOKE_STREAM) - 1 &&
                         resp.body && memcmp(resp.body, SMOKE_STREAM, sizeof(SMOKE_STREAM) - 1) == 0);
            last_status = resp.status;
            kl_client_response_free(&resp);
        } else { last_rc = rc; }
    }

    int udp_ok = 0;
    if (ok && post_ok && file_ok && bigfile_dl_ok && stream_ok && udp_ready) {
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

    int h2_ok = 0;
    if (ok && post_ok && file_ok && bigfile_dl_ok && stream_ok) {
        for (int i = 0; i < 20 && !h2_ok; i++) {
            h2_ok = h2c_roundtrip();
            if (!h2_ok) nap_ms(50);
        }
    }

    int h2pk_ok = 0;
    if (h2_ok) {
        for (int i = 0; i < 20 && !h2pk_ok; i++) {
            h2pk_ok = h2_prior_knowledge_roundtrip();
            if (!h2pk_ok) nap_ms(50);
        }
    }

    int idle_ok = h2pk_ok ? idle_timeout_closes() : 0;
    int churn_ok = idle_ok ? keepalive_churn(50) : 0;
    int resil_ok = churn_ok ? resilience_ok(&alloc, &ccfg) : 0;
    int big_ok = 0;
    if (resil_ok) {
        KlClientResponse resp;
        memset(&resp, 0, sizeof(resp));
        int rc = kl_client_request(&alloc, &ccfg, "GET", "http://127.0.0.1:18094/big",
                                   NULL, 0, NULL, 0, &resp);
        big_ok = (rc == 0 && resp.status == 200 && resp.body_len == SMOKE_BIG_LEN);
        if (rc == 0) kl_client_response_free(&resp);
    }

    kl_udp_recv_stop(&g_udp);
    kl_udp_free(&g_udp);
    kl_server_stop(&g_srv);
    pthread_join(th, NULL);
    kl_server_free(&g_srv);
    unlink(SMOKE_FILE_PATH);
    unlink(SMOKE_BIGFILE_PATH);

    if (!ok) {
        fprintf(stderr, "smoke-iouring: GET roundtrip FAILED (rc=%d status=%d body_len=%zu err=%d)\n",
                last_rc, last_status, last_len, last_err);
        return 1;
    }
    if (!post_ok) { fprintf(stderr, "smoke-iouring: POST/echo FAILED (rc=%d status=%d)\n", last_rc, last_status); return 1; }
    if (!file_ok) { fprintf(stderr, "smoke-iouring: GET/file (sendfile) FAILED (rc=%d status=%d)\n", last_rc, last_status); return 1; }
    if (!bigfile_dl_ok) { fprintf(stderr, "smoke-iouring: GET/bigfile (multi-chunk splice) FAILED (rc=%d status=%d)\n", last_rc, last_status); return 1; }
    if (!stream_ok) { fprintf(stderr, "smoke-iouring: GET/stream FAILED (rc=%d status=%d)\n", last_rc, last_status); return 1; }
    if (!udp_ok) { fprintf(stderr, "smoke-iouring: UDP echo FAILED\n"); return 1; }
    if (!h2_ok) { fprintf(stderr, "smoke-iouring: h2c/h2-over-completion echo FAILED\n"); return 1; }
    if (!h2pk_ok) { fprintf(stderr, "smoke-iouring: h2 prior-knowledge echo FAILED\n"); return 1; }
    if (!idle_ok) { fprintf(stderr, "smoke-iouring: idle-timeout (slowloris) close FAILED\n"); return 1; }
    if (!churn_ok) { fprintf(stderr, "smoke-iouring: keep-alive churn FAILED\n"); return 1; }
    if (!resil_ok) { fprintf(stderr, "smoke-iouring: resilience (drop/malformed) FAILED\n"); return 1; }
    if (!big_ok) { fprintf(stderr, "smoke-iouring: large-response partial-send FAILED\n"); return 1; }

    printf("smoke-iouring: over-completion roundtrip OK (GET + POST + file + bigfile-splice + stream + UDP + h2c + h2-pk + idle-timeout + keepalive + resilience + large)\n");
    return 0;
}
