/*
 * smoke_pollcomp.c — end-to-end HTTP-over-completion roundtrip on POSIX (PAL 8d-0.5).
 *
 * The first runtime validation of the platform-independent completion connection
 * driver (completion_driver.c) OFF Windows: a KlHttpServer pinned to the portable poll()
 * completion backend (BACKEND=pollcomp, the overlapped provider), served by the same
 * accept/recv/send/sendfile completion mechanics the IOCP backend implements — hit by
 * the sync KlClient over loopback. It proves the completion axis is genuinely
 * platform-independent (the driver is reused verbatim) and makes it CI-testable.
 *
 * POSIX sibling of smoke_iocp.c; identical coverage: GET + POST body + file (sendfile)
 * + chunked stream + UDP echo, all over the completion loop.
 */
#include <keel/keel.h>
#include <keel/datagram.h>
#include <keel/datagram_detail.h>
#include "datagram_test_util.h"   /* kl_dg_close_free — public lifecycle */
#include "../src/socket.h"   /* internal kl_socket_provider_pollcomp() */

#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>            /* malloc / free / strtol — bigstream dechunk */
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/time.h>          /* struct timeval (SO_RCVTIMEO) — not guaranteed via socket.h */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>              /* EAGAIN/EWOULDBLOCK — backlog-queued read distinguishes not-reset */

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

static void handle_ok(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_http_response_json(res, 200, SMOKE_BODY, sizeof(SMOKE_BODY) - 1);
}

/* GET /big — a large body (> socket send buffer) so the pollcomp WRITE op must loop
 * over several send() calls (partial-send path). */
#define SMOKE_BIG_LEN (256 * 1024)
static char g_big[SMOKE_BIG_LEN];
static void handle_big(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_http_response_status(res, 200);
    kl_http_response_body_borrow(res, g_big, SMOKE_BIG_LEN);
}

static void handle_echo(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)ctx;
    KlHttpBufReader *br = (KlHttpBufReader *)req->body_reader;
    if (!br || br->len == 0) { kl_http_response_error(res, 400, "body required"); return; }
    kl_http_response_status(res, 200);
    kl_http_response_body_borrow(res, br->data, br->len);
}

static void handle_file(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)ctx;
    int fd = open(SMOKE_FILE_PATH, O_RDONLY);
    if (fd < 0) { kl_http_response_error(res, 500, "open failed"); return; }
    off_t size = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    kl_http_response_status(res, 200);
    kl_http_response_file(res, (KlSocketHandle)fd, size);
}

static void handle_stream(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)ctx;
    KlHttpResponseWriteFn write_fn = NULL;
    void *wctx = NULL;
    if (kl_http_response_begin_stream(res, 200, &write_fn, &wctx) < 0) return;
    write_fn(wctx, "chunk-one;", 10);
    write_fn(wctx, "chunk-two", 9);
    kl_http_response_end_stream(res);
}

/* GET /bigstream — a chunked stream far larger than a slow client's receive window, so the
 * outbound buffer fills and the completion loop must flush it as overlapped sends (8g-1),
 * NOT busy-spin a blocking send (the head-of-line defect). Each chunk is a run of 'S'. */
#define SMOKE_BS_CHUNK   1024
#define SMOKE_BS_CHUNKS  256
#define SMOKE_BS_LEN     (SMOKE_BS_CHUNK * SMOKE_BS_CHUNKS)   /* 256 KiB payload */
static void handle_bigstream(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)ctx;
    KlHttpResponseWriteFn write_fn = NULL;
    void *wctx = NULL;
    if (kl_http_response_begin_stream(res, 200, &write_fn, &wctx) < 0) return;
    static char chunk[SMOKE_BS_CHUNK];
    memset(chunk, 'S', sizeof(chunk));
    for (int i = 0; i < SMOKE_BS_CHUNKS; i++)
        write_fn(wctx, chunk, sizeof(chunk));
    kl_http_response_end_stream(res);
}

static KlDatagram g_udp;
static void udp_echo(void *ud, const void *data, size_t len,
                     const KlSockAddr *peer, const KlSockAddr *local, unsigned flags) {
    (void)local; (void)flags;
    KlDatagramMessage m = { .data = data, .len = len, .peer = peer, .tos = -1 };
    (void)kl_datagram_send((KlDatagram *)ud, &m);
}

/* Minimal echo HTTP/2 server session (no nghttp2/HPACK): it echoes whatever bytes the
 * peer sends back through the send callback. Enough to exercise the h2 completion driver
 * (comp_h2_drive) end-to-end over pollcomp — feed received bytes → session → drain output
 * → send — without a real h2 stack. Entered via an h2c Upgrade (8d-1). */
typedef struct {
    KlH2ServerSession   base;
    KlH2ServerCallbacks cb;
    void               *cb_ud;
    KlAllocator        *alloc;
    char                pending[256];
    size_t              pending_len;
} EchoH2;

static kl_ssize_t echo_recv(KlH2ServerSession *self, const void *data, size_t len) {
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

/* Idle-timeout roundtrip (hardening): open a connection, send nothing, and confirm the
 * server times it out and closes it (read_timeout_ms=400) — the completion-path slowloris
 * defense. A slow/idle client must not hold a slot + overlapped read forever. */
static int idle_timeout_closes(void) {
    int cs = socket(AF_INET, SOCK_STREAM, 0);
    if (cs < 0) return 0;
    struct timeval tmo = { 3, 0 };   /* wait up to 3s for the server to close */
    setsockopt(cs, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof(tmo));
    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(SMOKE_PORT);
    inet_pton(AF_INET, "127.0.0.1", &to.sin_addr);
    if (connect(cs, (struct sockaddr *)&to, sizeof(to)) < 0) { close(cs); return 0; }

    /* Send nothing. The server should time out the idle read and close (best-effort 408
     * then EOF). read() returns >0 (the 408) or 0 (EOF); a timeout (-1) means it did not
     * close — the bug this test guards. */
    char b[128];
    ssize_t n = read(cs, b, sizeof(b));
    if (n > 0) n = read(cs, b, sizeof(b));   /* drain the 408, then expect EOF */
    close(cs);
    return n == 0;
}

/* Connect a blocking TCP client to the server; -1 on failure. */
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

/* Keep-alive churn: N sequential HTTP/1.1 requests on ONE connection, each fully read
 * before the next. Exercises the completion keep-alive reset (send_complete → post_recv)
 * repeatedly on a single conn. */
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
            if (strstr(buf, SMOKE_BODY)) { found = 1; break; }  /* full body = complete */
        }
        if (!found) { close(cs); return 0; }
    }
    close(cs);
    return 1;
}

/* Resilience: an abrupt mid-request drop and a malformed request must be cleaned up
 * without crashing/leaking; then a normal request must still succeed (server survived).
 * The leak/UAF signal comes from the ASan CI run over these cleanup paths. */
static int resilience_ok(KlAllocator *alloc, KlClientConfig *ccfg) {
    int cs = connect_client();       /* 1. partial request, then abrupt close */
    if (cs >= 0) { (void)!write(cs, "GET / HTT", 9); close(cs); }
    cs = connect_client();           /* 2. malformed request */
    if (cs >= 0) {
        (void)!write(cs, "@@@ not-http\r\n\r\n", 16);
        char b[128];
        (void)!read(cs, b, sizeof(b));   /* 400 or EOF — either is fine */
        close(cs);
    }
    nap_ms(50);
    KlClientResponse resp;            /* 3. server survived */
    memset(&resp, 0, sizeof(resp));
    int rc = kl_client_request(alloc, ccfg, "GET", "http://127.0.0.1:18092/",
                               NULL, 0, NULL, 0, &resp);
    int ok = (rc == 0 && resp.status == 200);
    if (rc == 0) kl_client_response_free(&resp);
    return ok;
}

/* 8g-1 head-of-line: a slow client requests the big stream and then stalls (tiny receive
 * window, no reads). The server's outbound buffer fills and it must post overlapped sends
 * and move on — NOT busy-spin a blocking flush. We prove the loop stayed free by driving a
 * *second*, normal request to completion while the first is stalled; then we drain the
 * first fully and verify every byte of the 256 KiB stream arrived (dechunked). Before
 * 8g-1, comp_send_stream spin-flushed the blocked socket and starved the second client. */
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
        while (stop < end && *stop != '\n') stop++;  /* skip to end of size line */
        if (stop >= end) break;
        p = stop + 1;                                /* chunk data starts here */
        if (sz == 0) break;                          /* terminating chunk */
        if (p + sz > end) break;
        payload += (size_t)sz;
        p += sz + 2;                                 /* skip data + trailing CRLF */
    }
    return payload;
}

static int bigstream_no_hol_ok(void) {
    /* Conn A — slow reader: tiny receive window, request the big stream, then DON'T read. */
    int a = socket(AF_INET, SOCK_STREAM, 0);
    if (a < 0) return 0;
    int rcv = 2048;
    setsockopt(a, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));
    struct timeval tmo = { 3, 0 };
    setsockopt(a, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof(tmo));
    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(SMOKE_PORT);
    inet_pton(AF_INET, "127.0.0.1", &to.sin_addr);
    if (connect(a, (struct sockaddr *)&to, sizeof(to)) < 0) { close(a); return 0; }
    const char *reqa = "GET /bigstream HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    if (write(a, reqa, strlen(reqa)) < 0) { close(a); return 0; }

    /* Let the server dispatch, fill A's window, and post the overlapped stream send. */
    nap_ms(150);

    /* Conn B — a normal request must complete promptly while A is stalled (the HOL check). */
    int b = connect_client();
    if (b < 0) { close(a); return 0; }
    const char *reqb = "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    int b_ok = 0;
    if (write(b, reqb, strlen(reqb)) >= 0) {
        char bb[1024]; size_t got = 0;
        while (got < sizeof(bb) - 1) {
            ssize_t r = read(b, bb + got, sizeof(bb) - 1 - got);
            if (r <= 0) break;
            got += (size_t)r; bb[got] = 0;
            if (strstr(bb, SMOKE_BODY)) { b_ok = 1; break; }
        }
    }
    close(b);

    /* Now drain A fully and confirm the whole stream arrived intact. */
    char *acc = malloc(SMOKE_BS_LEN * 2);
    size_t alen = 0;
    int a_ok = 0;
    if (acc) {
        for (;;) {
            ssize_t r = read(a, acc + alen, (SMOKE_BS_LEN * 2) - alen);
            if (r <= 0) break;
            alen += (size_t)r;
            if (alen >= SMOKE_BS_LEN * 2) break;
        }
        a_ok = (dechunk_body_len(acc, alen) == SMOKE_BS_LEN);
        free(acc);
    }
    close(a);
    return b_ok && a_ok;
}

static KlHttpServer g_srv;

static void *server_thread(void *arg) {
    (void)arg;
    kl_http_server_run(&g_srv);
    return NULL;
}

#define SMOKE_H2 "PING-OVER-H2C-COMPLETION"

/* Raw h2c roundtrip: HTTP/1.1 request with Upgrade: h2c → server 101 → then the echo
 * h2 session round-trips a frame. Exercises comp_h2_drive over the completion loop
 * (8d-1), plaintext, no nghttp2. */
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

    /* Read the 101 Switching Protocols (headers only, ends at CRLFCRLF). */
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

    /* Send an h2 "frame" and expect the echo session to bounce it back. */
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

#define SMOKE_H2PK "FRAME-VIA-PRIOR-KNOWLEDGE"

/* h2 prior-knowledge roundtrip (8d-2): the client opens straight into h2 by sending the
 * 24-byte connection preface (no HTTP/1.1 Upgrade), followed by a frame. Exercises the
 * completion path's preface detection + comp_h2_drive. */
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

/* PROXY-over-completion: a trusted-source connection sends a plaintext PROXY v1 header before
 * its HTTP request; the completion driver's PROXY-header phase (comp_drive_proxy) must parse it
 * and the handler must see the real client address (1.2.3.4), not the socket's 127.0.0.1 —
 * exercising accept → KL_HTTP_CONN_PROXY_HEADER → parse → READING → handler over the completion loop.
 * Returns 1 on success. */
#define SMOKE_PROXY_PORT 18097
static char g_proxy_ip[64];
static void handle_proxy_probe(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)ctx;
    uint16_t port = 0;
    g_proxy_ip[0] = '\0';
    kl_http_request_peer_addr(req, g_proxy_ip, sizeof(g_proxy_ip), &port);
    kl_http_response_json(res, 200, SMOKE_BODY, sizeof(SMOKE_BODY) - 1);
}
static void *run_server_arg(void *arg) { kl_http_server_run((KlHttpServer *)arg); return NULL; }
static int proxy_over_completion_ok(void) {
    KlHttpServer srv;
    KlHttpServerConfig cfg = { .port = SMOKE_PROXY_PORT, .bind_addr = "127.0.0.1",
                     .sockets = kl_socket_provider_pollcomp(),
                     .proxy_trusted_cidrs = "127.0.0.1/32" };
    if (kl_http_server_init(&srv, &cfg) != 0) return 0;   /* must NOT be rejected — now supported */
    kl_http_server_route(&srv, "GET", "/p", handle_proxy_probe, NULL, NULL);
    pthread_t th;
    if (pthread_create(&th, NULL, run_server_arg, &srv) != 0) { kl_http_server_free(&srv); return 0; }
    for (int i = 0; i < 200 && srv.bound_port == 0; i++) nap_ms(5);

    int ok = 0;
    int cs = socket(AF_INET, SOCK_STREAM, 0);
    if (cs >= 0) {
        struct timeval tmo = { 2, 0 };
        setsockopt(cs, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof(tmo));
        struct sockaddr_in to;
        memset(&to, 0, sizeof(to));
        to.sin_family = AF_INET;
        to.sin_port = htons(SMOKE_PROXY_PORT);
        inet_pton(AF_INET, "127.0.0.1", &to.sin_addr);
        if (connect(cs, (struct sockaddr *)&to, sizeof(to)) == 0) {
            /* PROXY v1 header (real client 1.2.3.4) + the HTTP request, one write. */
            const char *msg = "PROXY TCP4 1.2.3.4 5.6.7.8 1111 2222\r\n"
                              "GET /p HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
            if (write(cs, msg, strlen(msg)) == (ssize_t)strlen(msg)) {
                char buf[1024]; size_t got = 0;
                while (got < sizeof(buf) - 1) {
                    ssize_t n = read(cs, buf + got, sizeof(buf) - 1 - got);
                    if (n <= 0) break;
                    got += (size_t)n; buf[got] = 0;
                    if (strstr(buf, SMOKE_BODY)) break;
                }
                ok = (strstr(buf, "200 OK") != NULL && strcmp(g_proxy_ip, "1.2.3.4") == 0);
            }
        }
        close(cs);
    }
    kl_http_server_stop(&srv);
    pthread_join(th, NULL);
    kl_http_server_free(&srv);
    return ok;
}

/* Backlog exhaustion (6B-3 2b-ii): with ONE connection slot, a post-driven completion server must
 * QUEUE a second connection in the kernel backlog — not accept-and-drop/reset it — and serve it
 * once the first slot frees. Proves the reserve-before-post PAUSE end to end over real sockets:
 *   1. client A occupies the one slot (keep-alive: its conn stays READING after the response);
 *   2. client B connects + sends, but is NOT served while the pool is exhausted, and NOT reset;
 *   3. client A closes → the slot frees → the listener resumes and accepts B;
 *   4. B's request now completes over the same previously-queued connection.
 * Returns 1 on success. */
#define SMOKE_BACKLOG_PORT 18096
static int read_until_body(int fd, char *buf, size_t cap) {   /* 1 if SMOKE_BODY seen, else 0 */
    size_t got = 0;
    while (got < cap - 1) {
        ssize_t n = read(fd, buf + got, cap - 1 - got);
        if (n <= 0) break;
        got += (size_t)n; buf[got] = 0;
        if (strstr(buf, SMOKE_BODY)) return 1;
    }
    return 0;
}
static int backlog_exhaustion_queues_not_drops(void) {
    KlHttpServer srv;
    KlHttpServerConfig cfg = { .port = SMOKE_BACKLOG_PORT, .bind_addr = "127.0.0.1",
                     .sockets = kl_socket_provider_pollcomp(),
                     .max_connections = 1,          /* exactly one connection slot */
                     .read_timeout_ms = 10000 };    /* generous, so A is not idle-swept mid-test */
    if (kl_http_server_init(&srv, &cfg) != 0) return 0;
    kl_http_server_route(&srv, "GET", "/", handle_ok, NULL, NULL);
    pthread_t th;
    if (pthread_create(&th, NULL, run_server_arg, &srv) != 0) { kl_http_server_free(&srv); return 0; }
    for (int i = 0; i < 200 && srv.bound_port == 0; i++) nap_ms(5);

    int ok = 0, a = -1, b = -1;
    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(SMOKE_BACKLOG_PORT);
    inet_pton(AF_INET, "127.0.0.1", &to.sin_addr);
    char buf[512];

    /* 1. Client A: keep-alive request; read its 200 so we KNOW the one slot is now occupied. */
    a = socket(AF_INET, SOCK_STREAM, 0);
    if (a < 0) goto done;
    { struct timeval t = { 2, 0 }; setsockopt(a, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof(t)); }
    if (connect(a, (struct sockaddr *)&to, sizeof(to)) != 0) goto done;
    { const char *r = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";      /* keep-alive (no Connection: close) */
      if (write(a, r, strlen(r)) < 0) goto done; }
    if (!read_until_body(a, buf, sizeof(buf))) goto done;      /* A served → holds the only slot */

    /* 2. Client B: connect (into the kernel backlog) + send. While the pool is exhausted the server
     *    posts no accept, so B is neither served nor reset — a short read blocks out (EAGAIN). */
    b = socket(AF_INET, SOCK_STREAM, 0);
    if (b < 0) goto done;
    { struct timeval t = { 0, 400000 }; setsockopt(b, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof(t)); }
    if (connect(b, (struct sockaddr *)&to, sizeof(to)) != 0) goto done;
    { const char *r = "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
      if (write(b, r, strlen(r)) < 0) goto done; }
    { ssize_t n = read(b, buf, sizeof(buf) - 1);
      if (n > 0) goto done;    /* served while exhausted → accept-and-drop regression */
      if (n == 0) goto done;   /* EOF/reset while exhausted → dropped */
      if (errno != EAGAIN && errno != EWOULDBLOCK) goto done;   /* reset (ECONNRESET) etc. → dropped */
    }

    /* 3. Client A closes → its slot frees → the listener resumes and accepts B. */
    close(a); a = -1;

    /* 4. B's request now completes over the previously-queued connection. */
    { struct timeval t = { 3, 0 }; setsockopt(b, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof(t)); }
    ok = read_until_body(b, buf, sizeof(buf));

done:
    if (a >= 0) close(a);
    if (b >= 0) close(b);
    kl_http_server_stop(&srv);
    pthread_join(th, NULL);
    kl_http_server_free(&srv);
    return ok;
}

int main(void) {
    /* PROXY-over-completion is rejected at init (no completion PROXY-header driver). */
    int proxy_ok = proxy_over_completion_ok();
    int backlog_ok = backlog_exhaustion_queues_not_drops();

    static KlH2ServerConfig h2cfg = { .factory = echo_factory };
    KlHttpServerConfig cfg = { .port = SMOKE_PORT, .bind_addr = "127.0.0.1",
                     .sockets = kl_socket_provider_pollcomp(), .h2 = &h2cfg,
                     .read_timeout_ms = 400 };   /* short, to exercise the idle sweep */
    if (kl_http_server_init(&g_srv, &cfg) < 0) {
        fprintf(stderr, "smoke-pollcomp: server init failed (err=%d)\n", g_srv.last_error);
        return 1;
    }
    kl_http_server_route(&g_srv, "GET", "/", handle_ok, NULL, NULL);
    kl_http_server_route(&g_srv, "POST", "/echo", handle_echo, NULL, kl_http_body_reader_buffer);
    kl_http_server_route(&g_srv, "GET", "/file", handle_file, NULL, NULL);
    kl_http_server_route(&g_srv, "GET", "/stream", handle_stream, NULL, NULL);
    kl_http_server_route(&g_srv, "GET", "/big", handle_big, NULL, NULL);
    kl_http_server_route(&g_srv, "GET", "/bigstream", handle_bigstream, NULL, NULL);
    memset(g_big, 'A', sizeof(g_big));

    int wfd = open(SMOKE_FILE_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (wfd < 0 || write(wfd, SMOKE_FILE, sizeof(SMOKE_FILE) - 1) != (ssize_t)(sizeof(SMOKE_FILE) - 1)) {
        fprintf(stderr, "smoke-pollcomp: temp file write failed\n");
        if (wfd >= 0) close(wfd);
        kl_http_server_free(&g_srv);
        return 1;
    }
    close(wfd);

    KlDatagramSocketConfig ucfg = { .ctx = &g_srv.ev, .bind_addr = "127.0.0.1",
                         .bind_port = SMOKE_UDP_PORT };
    int udp_ready = (kl_datagram_socket_init(&g_udp, &ucfg) == 0 &&
                     kl_datagram_recv_start(&g_udp, udp_echo, &g_udp) == 0);
    if (!udp_ready) fprintf(stderr, "smoke-pollcomp: udp init/recv_start failed\n");

    pthread_t th;
    if (pthread_create(&th, NULL, server_thread, NULL) != 0) {
        fprintf(stderr, "smoke-pollcomp: pthread_create failed\n");
        kl_http_server_free(&g_srv);
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
                sendto(cs, SMOKE_UDP, sizeof(SMOKE_UDP) - 1, 0, (struct sockaddr *)&to, sizeof(to));
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

    /* h2c Upgrade → h2-over-completion echo roundtrip (comp_h2_drive, 8d-1). */
    int h2_ok = 0;
    if (ok && post_ok && file_ok && stream_ok) {
        for (int i = 0; i < 20 && !h2_ok; i++) {
            h2_ok = h2c_roundtrip();
            if (!h2_ok) nap_ms(50);
        }
    }

    /* h2 prior-knowledge (preface) → echo roundtrip (8d-2). */
    int h2pk_ok = 0;
    if (h2_ok) {
        for (int i = 0; i < 20 && !h2pk_ok; i++) {
            h2pk_ok = h2_prior_knowledge_roundtrip();
            if (!h2pk_ok) nap_ms(50);
        }
    }

    /* Idle-timeout / slowloris defense on the completion loop (hardening). */
    int idle_ok = h2pk_ok ? idle_timeout_closes() : 0;

    /* Adversarial edge-case batch (hardening) — all under the ASan CI run. */
    int churn_ok = idle_ok ? keepalive_churn(50) : 0;       /* keep-alive reset churn */
    int resil_ok = churn_ok ? resilience_ok(&alloc, &ccfg) : 0;  /* drop + malformed cleanup */
    int big_ok = 0;                                          /* partial-send WRITE loop */
    if (resil_ok) {
        KlClientResponse resp;
        memset(&resp, 0, sizeof(resp));
        int rc = kl_client_request(&alloc, &ccfg, "GET", "http://127.0.0.1:18092/big",
                                   NULL, 0, NULL, 0, &resp);
        big_ok = (rc == 0 && resp.status == 200 && resp.body_len == SMOKE_BIG_LEN);
        if (rc == 0) kl_client_response_free(&resp);
    }
    /* 8g-1: overlapped streaming flush — a stalled slow reader must not block the loop, and
     * the full stream must still arrive (comp_stream_pump + comp_on_write re-pump). */
    int bigstream_ok = big_ok ? bigstream_no_hol_ok() : 0;

    kl_http_server_stop(&g_srv);
    pthread_join(th, NULL);
    kl_dg_close_free(&g_srv.ev, &g_udp);   /* loop idle now — safe to pump the public close */
    kl_http_server_free(&g_srv);
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
    if (!h2_ok) { fprintf(stderr, "smoke-pollcomp: h2c/h2-over-completion echo FAILED\n"); return 1; }
    if (!h2pk_ok) { fprintf(stderr, "smoke-pollcomp: h2 prior-knowledge echo FAILED\n"); return 1; }
    if (!idle_ok) { fprintf(stderr, "smoke-pollcomp: idle-timeout (slowloris) close FAILED\n"); return 1; }
    if (!churn_ok) { fprintf(stderr, "smoke-pollcomp: keep-alive churn FAILED\n"); return 1; }
    if (!resil_ok) { fprintf(stderr, "smoke-pollcomp: resilience (drop/malformed) FAILED\n"); return 1; }
    if (!big_ok) { fprintf(stderr, "smoke-pollcomp: large-response partial-send FAILED\n"); return 1; }
    if (!bigstream_ok) { fprintf(stderr, "smoke-pollcomp: bigstream overlapped flush / HOL FAILED\n"); return 1; }
    if (!proxy_ok) { fprintf(stderr, "smoke-pollcomp: PROXY-over-completion roundtrip FAILED\n"); return 1; }
    if (!backlog_ok) { fprintf(stderr, "smoke-pollcomp: backlog exhaustion (queue-not-drop) FAILED\n"); return 1; }

    printf("smoke-pollcomp: over-completion roundtrip OK (GET + POST + file + stream + UDP + h2c + h2-pk + idle-timeout + keepalive + resilience + large + bigstream + proxy + backlog)\n");
    return 0;
}
