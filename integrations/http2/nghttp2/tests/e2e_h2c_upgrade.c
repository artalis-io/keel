/*
 * e2e_h2c_upgrade.c: an h2c Upgrade (RFC 7540 3.2), then a second request on the same connection.
 *
 * Stream 1 of an h2c upgrade is answered outside the session's receive, so the nghttp2 adapter's
 * flush really sends: the response's END_STREAM closes stream 1 (already half-closed by the
 * client) inside that send, and the close reaches KEEL from inside the flush. KEEL then destroyed
 * the stream a second time, leaving its stream count at -1, so the next stream was written before
 * the stream table and never found again: the second request was not answered. A raw nghttp2
 * client sends the HTTP/1.1 Upgrade request, takes the 101, answers on stream 1, and then a GET on
 * stream 3 must be answered too. Exits non-zero otherwise.
 */
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <keel/keel.h>
#include "keel_http2_nghttp2.h"
#include <nghttp2/nghttp2.h>

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static void nap(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}
static int fail(const char *m) { fprintf(stderr, "FAIL: %s\n", m); return 1; }

static void handle_hello(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)req; (void)ud;
    kl_http_response_json(res, 200, "{\"hello\":1}", 11);
}

static KlHttpServer g_srv;
static void *srv_thread(void *a) { (void)a; kl_http_server_run(&g_srv); return NULL; }

#if defined(_WIN32)
typedef SOCKET RawSock;
#define RAW_BAD INVALID_SOCKET
static void raw_close(RawSock fd) { closesocket(fd); }
static int raw_poll(RawSock fd, int ms) {
    WSAPOLLFD p = { fd, POLLRDNORM, 0 };
    return WSAPoll(&p, 1, ms);
}
#else
typedef int RawSock;
#define RAW_BAD (-1)
static void raw_close(RawSock fd) { close(fd); }
static int raw_poll(RawSock fd, int ms) {
    struct pollfd p = { fd, POLLIN, 0 };
    return poll(&p, 1, ms);
}
#endif

typedef struct {
    RawSock fd;
    nghttp2_session *ng;
    int answered[8];            /* END_STREAM seen, by stream id (odd ids < 8) */
    int status[8];
    int eof;
} RawClient;

static ssize_t raw_send_cb(nghttp2_session *ng, const uint8_t *d, size_t n, int fl, void *ud) {
    (void)ng; (void)fl;
    RawClient *c = ud;
    int w = (int)send(c->fd, (const char *)d, (int)n, 0);
    return w > 0 ? (ssize_t)w : NGHTTP2_ERR_CALLBACK_FAILURE;
}
static int raw_frame_recv_cb(nghttp2_session *ng, const nghttp2_frame *f, void *ud) {
    (void)ng;
    RawClient *c = ud;
    int32_t sid = f->hd.stream_id;
    if (sid > 0 && sid < 8 && (f->hd.type == NGHTTP2_HEADERS || f->hd.type == NGHTTP2_DATA) &&
        (f->hd.flags & NGHTTP2_FLAG_END_STREAM))
        c->answered[sid] = 1;
    return 0;
}
static int raw_header_cb(nghttp2_session *ng, const nghttp2_frame *f, const uint8_t *name,
                         size_t namelen, const uint8_t *value, size_t valuelen, uint8_t flags,
                         void *ud) {
    (void)ng; (void)flags;
    RawClient *c = ud;
    int32_t sid = f->hd.stream_id;
    if (sid > 0 && sid < 8 && namelen == 7 && memcmp(name, ":status", 7) == 0) {
        int v = 0;
        for (size_t i = 0; i < valuelen; i++) v = v * 10 + (value[i] - '0');
        c->status[sid] = v;
    }
    return 0;
}

static void raw_pump(RawClient *c, int ms) {
    if (c->eof) return;
    (void)nghttp2_session_send(c->ng);
    if (raw_poll(c->fd, ms) <= 0) return;
    char buf[16384];
    int n = (int)recv(c->fd, buf, (int)sizeof buf, 0);
    if (n <= 0) { c->eof = 1; return; }
    (void)nghttp2_session_mem_recv(c->ng, (const uint8_t *)buf, (size_t)n);
}

/* SETTINGS_MAX_CONCURRENT_STREAMS = 100, as HTTP2-Settings carries it (base64url, unpadded). */
static const uint8_t k_settings[6] = { 0x00, 0x03, 0x00, 0x00, 0x00, 0x64 };
static const char k_settings_b64[] = "AAMAAABk";

/* Send the Upgrade request and read up to the end of the 101's head. Any bytes after it are the
 * server's first HTTP/2 frames: they go to the session. 0 on a 101, -1 otherwise. */
static int raw_upgrade(RawClient *c, int port) {
    memset(c, 0, sizeof *c);
    c->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (c->fd == RAW_BAD) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    a.sin_addr.s_addr = htonl(0x7f000001u);
    if (connect(c->fd, (struct sockaddr *)&a, sizeof a) != 0) return -1;

    char req[256];
    int rl = snprintf(req, sizeof req,
                      "GET /hello HTTP/1.1\r\nHost: x\r\nConnection: Upgrade, HTTP2-Settings\r\n"
                      "Upgrade: h2c\r\nHTTP2-Settings: %s\r\n\r\n", k_settings_b64);
    if (send(c->fd, req, rl, 0) != rl) return -1;

    char buf[4096];
    size_t got = 0;
    const char *end = NULL;
    for (int i = 0; i < 200 && !end; i++) {
        if (raw_poll(c->fd, 10) <= 0) continue;
        int n = (int)recv(c->fd, buf + got, (int)(sizeof buf - 1 - got), 0);
        if (n <= 0) return -1;
        got += (size_t)n;
        buf[got] = '\0';
        end = strstr(buf, "\r\n\r\n");
        if (got >= sizeof buf - 1) break;
    }
    if (!end || strncmp(buf, "HTTP/1.1 101", 12) != 0) return -1;

    nghttp2_session_callbacks *cbs = NULL;
    nghttp2_session_callbacks_new(&cbs);
    nghttp2_session_callbacks_set_send_callback(cbs, raw_send_cb);
    nghttp2_session_callbacks_set_on_frame_recv_callback(cbs, raw_frame_recv_cb);
    nghttp2_session_callbacks_set_on_header_callback(cbs, raw_header_cb);
    nghttp2_session_client_new(&c->ng, cbs, c);
    nghttp2_session_callbacks_del(cbs);
    /* Stream 1 is the upgrading request, half-closed (local) on the client. The connection
     * preface's SETTINGS must carry the same values as HTTP2-Settings. */
    if (nghttp2_session_upgrade2(c->ng, k_settings, sizeof k_settings, 0, NULL) != 0) return -1;
    nghttp2_settings_entry iv = { NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100 };
    nghttp2_submit_settings(c->ng, NGHTTP2_FLAG_NONE, &iv, 1);
    size_t rest = got - (size_t)(end + 4 - buf);
    if (rest > 0)
        (void)nghttp2_session_mem_recv(c->ng, (const uint8_t *)end + 4, rest);
    return 0;
}

int main(void) {
    KlHttp2ServerConfig h2cfg = { .factory = kl_http2_nghttp2_server_session };
    KlHttpServerConfig cfg = { .port = 0, .bind_addr = "127.0.0.1", .h2 = &h2cfg };
    if (kl_http_server_init(&g_srv, &cfg) < 0) return fail("server init");
    kl_http_server_route(&g_srv, "GET", "/hello", handle_hello, NULL, NULL);
    pthread_t tid;
    if (pthread_create(&tid, NULL, srv_thread, NULL) != 0) return fail("server thread");
    for (int i = 0; i < 200 && g_srv.bound_port == 0; i++) nap(5);
    if (g_srv.bound_port <= 0) return fail("server never bound");

    RawClient c;
    int up = raw_upgrade(&c, g_srv.bound_port);
    int first = 0, second = 0, status1 = 0, status3 = 0;
    if (up == 0) {
        for (int i = 0; i < 200 && !c.answered[1] && !c.eof; i++) raw_pump(&c, 10);
        first = c.answered[1];
        status1 = c.status[1];
        nghttp2_nv nva[] = {
            { (uint8_t *)":method", (uint8_t *)"GET", 7, 3, NGHTTP2_NV_FLAG_NONE },
            { (uint8_t *)":path", (uint8_t *)"/hello", 5, 6, NGHTTP2_NV_FLAG_NONE },
            { (uint8_t *)":scheme", (uint8_t *)"http", 7, 4, NGHTTP2_NV_FLAG_NONE },
            { (uint8_t *)":authority", (uint8_t *)"x", 10, 1, NGHTTP2_NV_FLAG_NONE },
        };
        int32_t sid = nghttp2_submit_request(c.ng, NULL, nva, 4, NULL, NULL);
        for (int i = 0; sid == 3 && i < 200 && !c.answered[3] && !c.eof; i++) raw_pump(&c, 10);
        second = c.answered[3];
        status3 = c.status[3];
    }
    if (c.ng) nghttp2_session_del(c.ng);
    if (c.fd != RAW_BAD) raw_close(c.fd);

    kl_http_server_stop(&g_srv);
    pthread_join(tid, NULL);
    kl_http_server_free(&g_srv);
    printf("  h2c upgrade: 101 %s, stream 1 answered %d (status %d), stream 3 answered %d (status %d)\n",
           up == 0 ? "yes" : "no", first, status1, second, status3);
    if (up != 0) return fail("no 101 Switching Protocols");
    if (!first || status1 != 200) return fail("the upgrading request was not answered on stream 1");
    if (!second || status3 != 200)
        return fail("the request after an h2c upgrade was not answered (stream table corrupt)");
    printf("nghttp2 h2c upgrade then a second request OK\n");
    return 0;
}
