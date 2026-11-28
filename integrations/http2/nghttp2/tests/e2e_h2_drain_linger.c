/*
 * e2e_h2_drain_linger.c: a graceful drain ends a finished HTTP/2 connection without a reset.
 *
 * A real KEEL server (the nghttp2 session, a real loopback socket) is stopped
 * (kl_http_server_stop) while it sends large responses to a slow client. Once the drain's GOAWAY
 * is out and no stream is left the session is done, and the connection ends. The client keeps
 * talking meanwhile, as HTTP/2 clients do: a PING every 10 ms, a connection WINDOW_UPDATE every
 * 25 ms, and the WINDOW_UPDATEs nghttp2 sends as it consumes DATA. A close over that unread input
 * would be a TCP reset, which drops the tail of the responses still in the server's send queue and
 * fails the client's receive. So the server half-closes once its output is out and lingers,
 * discarding, until the client's FIN.
 *
 * Cases:
 *   chatty: three concurrent streams on one connection (two streamed by timer producers, one
 *     buffered), a slow reader (16 KiB per read, then a pause): every byte of every stream must
 *     arrive (byte-exact), each stream must end (END_STREAM), the GOAWAY must arrive, then a clean
 *     end of stream (a FIN, never a reset); each producer hears completed=1 once; the run returns
 *     soon after the client closes, well before the drain deadline.
 *   abandoned: the client sends its request, then never reads and never closes. The response and
 *     the GOAWAY fit the socket buffers, so the session is done and the connection lingers: it must
 *     still end at the linger deadline (read_timeout_ms), so the run returns then, well before the
 *     drain deadline, with no connection left.
 * With -DE2E_TLS (and the OpenSSL adapter linked) the chatty case also runs over real TLS (ALPN h2,
 * an OpenSSL client): the bytes must arrive whole before the end of stream.
 * Exits non-zero on any failure.
 */
#include <keel/keel.h>
#include "keel_http2_nghttp2.h"
#include <nghttp2/nghttp2.h>
#ifdef E2E_TLS
#include <keel_tls_openssl.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET CliSock;
#define CLI_BAD INVALID_SOCKET
static void cli_close(CliSock fd) { closesocket(fd); }
static int cli_poll(CliSock fd, int ms) { WSAPOLLFD p = { fd, POLLRDNORM, 0 }; return WSAPoll(&p, 1, ms); }
static int sock_err(void) { return WSAGetLastError(); }
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int CliSock;
#define CLI_BAD (-1)
static void cli_close(CliSock fd) { close(fd); }
static int cli_poll(CliSock fd, int ms) { struct pollfd p = { fd, POLLIN, 0 }; return poll(&p, 1, ms); }
static int sock_err(void) { return errno; }
#endif
/* A send to a connection the server reset must fail, not raise SIGPIPE (a test of that reset). */
#if defined(MSG_NOSIGNAL)
#define CLI_SEND_FLAGS MSG_NOSIGNAL
#else
#define CLI_SEND_FLAGS 0
#endif
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define STREAMED_TOTAL (3u * 1024u * 1024u)   /* each streamed response */
#define BUFFERED_TOTAL (3u * 1024u * 1024u)   /* the buffered response */
#define CHUNK          16384
#define STOP_AT        (512u * 1024u)        /* the client asks for the stop once this arrived */
#define NSTREAMS       3

static int fail(const char *m) { fprintf(stderr, "FAIL: %s\n", m); return 1; }
static void nap(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}
static uint64_t now_ms(void) { return kl_monotonic_ms(); }
static unsigned char pattern(int k, size_t i) { return (unsigned char)((i + 7u * (unsigned)k) % 251u); }

/* ── server side ──────────────────────────────────────────────────────────────────────────────── */

static KlHttpServer g_srv;
static atomic_int g_run_returned;
static atomic_uint_fast64_t g_run_return_ms;

typedef struct {
    int k;
    KlHttpResponse *res;
    int64_t timer;
    int timer_armed, ended;
    size_t written;
    atomic_int closed, completed, wrote_after_close;
} Producer;
static Producer g_prod[2];
static unsigned char *g_bigbuf;

static void producer_tick(void *ud);
static void producer_arm(Producer *p) {
    if (p->timer_armed || atomic_load(&p->closed) || p->ended) return;
    p->timer = kl_timer_add(kl_http_server_event_ctx(&g_srv), 0, producer_tick, p);
    p->timer_armed = p->timer >= 0;
}
static void producer_writable(void *ud) { producer_arm(ud); }
static void producer_closed(void *ud, int completed) {
    Producer *p = ud;
    atomic_store(&p->completed, completed);
    atomic_fetch_add(&p->closed, 1);
    if (p->timer_armed) kl_timer_cancel(kl_http_server_event_ctx(&g_srv), p->timer);
    p->timer_armed = 0;
    p->res = NULL;
}
static void producer_tick(void *ud) {
    Producer *p = ud;
    p->timer_armed = 0;
    if (atomic_load(&p->closed)) { atomic_fetch_add(&p->wrote_after_close, 1); return; }
    static unsigned char chunk[CHUNK];
    while (p->written < STREAMED_TOTAL) {
        for (size_t i = 0; i < CHUNK; i++) chunk[i] = pattern(p->k, p->written + i);
        KlHttpStreamStatus st = kl_http_response_stream_try_write(p->res, (const char *)chunk, CHUNK);
        if (st == KL_HTTP_STREAM_ACCEPTED) { p->written += CHUNK; continue; }
        return;                          /* WOULD_BLOCK: the writable edge re-arms; else gone */
    }
    if (!p->ended) { p->ended = 1; kl_http_response_end_stream(p->res); }
}
static void handle_stream(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)req;
    Producer *p = ud;
    KlHttpResponseWriteFn w; void *wc;
    if (kl_http_response_begin_stream(res, 200, &w, &wc) < 0) return;
    p->res = res;
    kl_http_response_stream_on_close(res, producer_closed, p);
    kl_http_response_stream_on_writable(res, producer_writable, p);
    producer_arm(p);
}
static void handle_buf(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)req;
    size_t n = (size_t)(uintptr_t)ud;
    kl_http_response_status(res, 200);
    kl_http_response_body_borrow(res, (const char *)g_bigbuf, n);
}
static void *srv_thread(void *a) {
    (void)a;
    kl_http_server_run(&g_srv);
    atomic_store(&g_run_return_ms, now_ms());
    atomic_store(&g_run_returned, 1);
    return NULL;
}
static int srv_start(KlHttpServerConfig *cfg) {
    atomic_store(&g_run_returned, 0);
    atomic_store(&g_run_return_ms, 0);
    memset(g_prod, 0, sizeof g_prod);
    for (int k = 0; k < 2; k++) g_prod[k].k = k;
    if (kl_http_server_init(&g_srv, cfg) < 0) return -1;
    kl_http_server_route(&g_srv, "GET", "/s0", handle_stream, &g_prod[0], NULL);
    kl_http_server_route(&g_srv, "GET", "/s1", handle_stream, &g_prod[1], NULL);
    kl_http_server_route(&g_srv, "GET", "/buf", handle_buf, (void *)(uintptr_t)BUFFERED_TOTAL, NULL);
    kl_http_server_route(&g_srv, "GET", "/small", handle_buf, (void *)(uintptr_t)16384, NULL);
    pthread_t tid;
    if (pthread_create(&tid, NULL, srv_thread, NULL) != 0) return -1;
    pthread_detach(tid);
    for (int i = 0; i < 400 && kl_http_server_bound_port(&g_srv) <= 0; i++) nap(5);
    return kl_http_server_bound_port(&g_srv) > 0 ? 0 : -1;
}
static int srv_wait_return(int ms) {
    for (int i = 0; i < ms / 10 && !atomic_load(&g_run_returned); i++) nap(10);
    return atomic_load(&g_run_returned) ? 0 : -1;
}

/* ── client side: a raw nghttp2 session over a socket, or OpenSSL over it ────────────────────── */

typedef struct {
    int32_t id;
    int k;               /* 0/1 streamed, 2 buffered */
    size_t bytes, want;
    int bad, ended, status;
} CliStream;

typedef struct {
    CliSock fd;
#ifdef E2E_TLS
    SSL *ssl;
#endif
    nghttp2_session *ng;
    CliStream st[NSTREAMS];
    int nst;
    size_t total;
    int goaway, eof, reset, tls_clean;
} Cli;

static CliStream *cli_stream(Cli *c, int32_t id) {
    for (int i = 0; i < c->nst; i++) if (c->st[i].id == id) return &c->st[i];
    return NULL;
}
static ssize_t cli_send_cb(nghttp2_session *s, const uint8_t *d, size_t n, int fl, void *ud) {
    (void)s; (void)fl;
    Cli *c = ud;
#ifdef E2E_TLS
    if (c->ssl) {
        int w = SSL_write(c->ssl, d, (int)n);
        return w > 0 ? (ssize_t)w : NGHTTP2_ERR_CALLBACK_FAILURE;
    }
#endif
    ssize_t w = (ssize_t)send(c->fd, (const char *)d, (int)n, CLI_SEND_FLAGS);
    return w > 0 ? w : NGHTTP2_ERR_CALLBACK_FAILURE;
}
static int cli_header_cb(nghttp2_session *s, const nghttp2_frame *f, const uint8_t *name, size_t nl,
                         const uint8_t *value, size_t vl, uint8_t fl, void *ud) {
    (void)s; (void)fl;
    CliStream *st = cli_stream(ud, f->hd.stream_id);
    if (st && nl == 7 && memcmp(name, ":status", 7) == 0 && vl == 3)
        st->status = (value[0] - '0') * 100 + (value[1] - '0') * 10 + (value[2] - '0');
    return 0;
}
static int cli_data_cb(nghttp2_session *s, uint8_t fl, int32_t sid, const uint8_t *d, size_t n, void *ud) {
    (void)s; (void)fl;
    Cli *c = ud;
    CliStream *st = cli_stream(c, sid);
    if (!st) return 0;
    for (size_t i = 0; i < n; i++)
        if (d[i] != pattern(st->k, st->bytes + i)) st->bad = 1;
    st->bytes += n;
    c->total += n;
    return 0;
}
static int cli_frame_cb(nghttp2_session *s, const nghttp2_frame *f, void *ud) {
    (void)s;
    Cli *c = ud;
    if (f->hd.type == NGHTTP2_GOAWAY) c->goaway = 1;
    CliStream *st = cli_stream(c, f->hd.stream_id);
    if (st && (f->hd.type == NGHTTP2_DATA || f->hd.type == NGHTTP2_HEADERS) &&
        (f->hd.flags & NGHTTP2_FLAG_END_STREAM))
        st->ended = 1;
    return 0;
}

static int cli_connect(Cli *c, int port, int tls) {
    memset(c, 0, sizeof *c);
    c->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (c->fd == CLI_BAD) return -1;
    int rcv = 32 * 1024;                 /* a small receive buffer: the server's send queue fills */
    setsockopt(c->fd, SOL_SOCKET, SO_RCVBUF, (const char *)&rcv, sizeof rcv);
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(c->fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    a.sin_addr.s_addr = htonl(0x7f000001u);
    if (connect(c->fd, (struct sockaddr *)&a, sizeof a) != 0) return -1;
#ifdef E2E_TLS
    if (tls) {
        static SSL_CTX *cctx;
        if (!cctx) {
            cctx = SSL_CTX_new(TLS_client_method());
            if (!cctx) return -1;
            SSL_CTX_set_verify(cctx, SSL_VERIFY_NONE, NULL);   /* test certificate */
            static const unsigned char alpn[] = { 2, 'h', '2' };
            SSL_CTX_set_alpn_protos(cctx, alpn, sizeof alpn);
        }
        c->ssl = SSL_new(cctx);
        if (!c->ssl || SSL_set_fd(c->ssl, (int)c->fd) != 1 || SSL_connect(c->ssl) != 1) return -1;
        const unsigned char *p = NULL; unsigned int pl = 0;
        SSL_get0_alpn_selected(c->ssl, &p, &pl);
        if (pl != 2 || memcmp(p, "h2", 2) != 0) return -1;
    }
#else
    if (tls) return -1;
#endif
    nghttp2_session_callbacks *cbs;
    nghttp2_session_callbacks_new(&cbs);
    nghttp2_session_callbacks_set_send_callback(cbs, cli_send_cb);
    nghttp2_session_callbacks_set_on_header_callback(cbs, cli_header_cb);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cbs, cli_data_cb);
    nghttp2_session_callbacks_set_on_frame_recv_callback(cbs, cli_frame_cb);
    nghttp2_session_client_new(&c->ng, cbs, c);
    nghttp2_session_callbacks_del(cbs);
    /* 1 MiB per stream, 16 MiB for the connection: WINDOW_UPDATEs flow while the data does. */
    nghttp2_settings_entry iv[] = { { NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, 1024 * 1024 } };
    nghttp2_submit_settings(c->ng, NGHTTP2_FLAG_NONE, iv, 1);
    nghttp2_session_set_local_window_size(c->ng, NGHTTP2_FLAG_NONE, 0, 16 * 1024 * 1024);
    return 0;
}
static int cli_get(Cli *c, const char *path, int k, size_t want) {
    nghttp2_nv nva[] = {
        { (uint8_t *)":method", (uint8_t *)"GET", 7, 3, NGHTTP2_NV_FLAG_NONE },
        { (uint8_t *)":scheme", (uint8_t *)"http", 7, 4, NGHTTP2_NV_FLAG_NONE },
        { (uint8_t *)":authority", (uint8_t *)"localhost", 10, 9, NGHTTP2_NV_FLAG_NONE },
        { (uint8_t *)":path", (uint8_t *)path, 5, strlen(path), NGHTTP2_NV_FLAG_NONE },
    };
    int32_t id = nghttp2_submit_request(c->ng, NULL, nva, 4, NULL, NULL);
    if (id < 0 || c->nst >= NSTREAMS) return -1;
    c->st[c->nst++] = (CliStream){ .id = id, .k = k, .want = want };
    return 0;
}
/* Send what is queued, then take in at most one 16 KiB read. 1 at the end of stream, -1 on an
 * error (a reset among them), else 0. */
static int cli_step(Cli *c, int wait_ms) {
    if (c->eof) return 1;
    if (nghttp2_session_send(c->ng) != 0) { c->reset = 1; return -1; }   /* the server is gone */
    uint8_t buf[16384];
    int n;
#ifdef E2E_TLS
    if (c->ssl) {
        if (SSL_pending(c->ssl) <= 0 && cli_poll(c->fd, wait_ms) <= 0) return 0;
        n = SSL_read(c->ssl, buf, (int)sizeof buf);
        if (n <= 0) {
            int e = SSL_get_error(c->ssl, n);
            if (e == SSL_ERROR_WANT_READ) return 0;
            c->eof = 1;
            c->tls_clean = e == SSL_ERROR_ZERO_RETURN;   /* close_notify, then the FIN */
            if (e == SSL_ERROR_SYSCALL && sock_err() != 0) c->reset = 1;
            return c->reset ? -1 : 1;
        }
    } else
#endif
    {
        if (cli_poll(c->fd, wait_ms) <= 0) return 0;
        n = (int)recv(c->fd, (char *)buf, (int)sizeof buf, 0);
        if (n == 0) { c->eof = 1; return 1; }
        if (n < 0) { c->reset = 1; fprintf(stderr, "  client recv error %d\n", sock_err()); return -1; }
    }
    if (nghttp2_session_mem_recv(c->ng, buf, (size_t)n) < 0) { c->reset = 1; return -1; }
    return 0;
}
static void cli_free(Cli *c) {
#ifdef E2E_TLS
    if (c->ssl) { SSL_free(c->ssl); c->ssl = NULL; }
#endif
    if (c->ng) nghttp2_session_del(c->ng);
    c->ng = NULL;
    if (c->fd != CLI_BAD) cli_close(c->fd);
    c->fd = CLI_BAD;
}

/* ── cases ────────────────────────────────────────────────────────────────────────────────────── */

static int case_chatty(int tls, KlTlsConfig *tlscfg) {
    const char *name = tls ? "chatty (TLS)" : "chatty";
    KlHttp2ServerConfig h2cfg = { .factory = kl_http2_nghttp2_server_session };
    KlHttpServerConfig cfg = { .port = 0, .bind_addr = "127.0.0.1", .h2 = &h2cfg, .tls = tlscfg,
                               .stream_capacity = 64 * 1024, .drain_timeout_ms = 15000,
                               .read_timeout_ms = 15000 };
    if (srv_start(&cfg) < 0) return fail("server start");
    Cli c;
    if (cli_connect(&c, kl_http_server_bound_port(&g_srv), tls) < 0) return fail("client connect");
    if (cli_get(&c, "/s0", 0, STREAMED_TOTAL) < 0 || cli_get(&c, "/s1", 1, STREAMED_TOTAL) < 0 ||
        cli_get(&c, "/buf", 2, BUFFERED_TOTAL) < 0)
        return fail("submit");

    int rc = 0, stopped = 0;
    uint64_t t0 = now_ms(), last_ping = 0, last_wu = 0, t_stop = 0;
    for (;;) {
        int all_ended = 1;
        for (int i = 0; i < c.nst; i++) all_ended &= c.st[i].ended;
        if (all_ended || now_ms() - t0 > 60000) break;
        uint64_t t = now_ms();
        if (t - last_ping >= 10) { last_ping = t; nghttp2_submit_ping(c.ng, NGHTTP2_FLAG_NONE, NULL); }
        if (t - last_wu >= 25) {
            last_wu = t;
            nghttp2_submit_window_update(c.ng, NGHTTP2_FLAG_NONE, 0, 1);
        }
        int r = cli_step(&c, 5);
        if (r != 0) break;                 /* the end of stream or an error before every stream ended */
        nap(1);                            /* the slow reader: the server's send queue fills */
        if (!stopped && c.total >= STOP_AT) { stopped = 1; t_stop = now_ms(); kl_http_server_stop(&g_srv); }
    }
    /* Keep talking until the server's end of stream (or an error). */
    for (int i = 0; i < 3000 && !c.eof && !c.reset; i++) {
        if (i % 10 == 0) nghttp2_submit_ping(c.ng, NGHTTP2_FLAG_NONE, NULL);
        if (cli_step(&c, 5) < 0) break;
    }
    int eof = c.eof, reset = c.reset, goaway = c.goaway, tls_clean = c.tls_clean;
    uint64_t t_close = now_ms();
    cli_free(&c);                          /* the client's FIN ends the linger */
    int returned = srv_wait_return(10000) == 0;
    int64_t run_after_close = returned ? (int64_t)atomic_load(&g_run_return_ms) - (int64_t)t_close : 0;
    kl_http_server_free(&g_srv);

    printf("  %s: stop after %zu bytes (+%llu ms); ", name, (size_t)STOP_AT,
           (unsigned long long)(t_stop - t0));
    for (int i = 0; i < c.nst; i++)
        printf("stream %d %zu/%zu ended %d bad %d; ", c.st[i].k, c.st[i].bytes, c.st[i].want,
               c.st[i].ended, c.st[i].bad);
    printf("GOAWAY %d eof %d reset %d tls_clean %d; producers completed %d/%d closed %d/%d; "
           "run returned %d, %lld ms after the client closed\n",
           goaway, eof, reset, tls_clean, atomic_load(&g_prod[0].completed),
           atomic_load(&g_prod[1].completed), atomic_load(&g_prod[0].closed),
           atomic_load(&g_prod[1].closed), returned, (long long)run_after_close);
    if (!stopped) rc = fail("the stop was never issued");
    for (int i = 0; i < c.nst; i++)
        if (c.st[i].bytes != c.st[i].want || c.st[i].bad || !c.st[i].ended || c.st[i].status != 200)
            rc = fail("a response did not arrive whole and ended");
    if (reset) rc = fail("the connection failed (a reset: the tail of the responses dropped)");
    if (!eof) rc = fail("no end of stream after the drain");
    if (!goaway) rc = fail("no GOAWAY");
    for (int k = 0; k < 2; k++)
        if (atomic_load(&g_prod[k].closed) != 1 || atomic_load(&g_prod[k].completed) != 1 ||
            atomic_load(&g_prod[k].wrote_after_close))
            rc = fail("a producer did not hear completed=1 exactly once");
    if (!returned || run_after_close < 0 || run_after_close > 3000)
        rc = fail("the run did not return soon after the client closed");
    if (!rc) printf("nghttp2 %s drain delivers every byte, then a FIN OK\n", name);
    return rc;
}

static int case_abandoned(void) {
    KlHttp2ServerConfig h2cfg = { .factory = kl_http2_nghttp2_server_session };
    KlHttpServerConfig cfg = { .port = 0, .bind_addr = "127.0.0.1", .h2 = &h2cfg,
                               .drain_timeout_ms = 20000, .read_timeout_ms = 1500 };
    if (srv_start(&cfg) < 0) return fail("server start");
    Cli c;
    if (cli_connect(&c, kl_http_server_bound_port(&g_srv), 0) < 0) return fail("client connect");
    if (cli_get(&c, "/small", 2, 16384) < 0 || nghttp2_session_send(c.ng) != 0) return fail("submit");
    nap(300);                              /* the response goes out; nobody reads it */
    uint64_t t_stop = now_ms();
    kl_http_server_stop(&g_srv);
    int returned = srv_wait_return(15000) == 0;
    uint64_t took = returned ? atomic_load(&g_run_return_ms) - t_stop : 0;
    KlHttpServerStats stats;
    kl_http_server_stats(&g_srv, &stats);
    int active = stats.active_connections;
    cli_free(&c);
    kl_http_server_free(&g_srv);
    printf("  abandoned: run returned %d, %llu ms after the stop; active connections %d\n",
           returned, (unsigned long long)took, active);
    if (!returned || took > 6000)
        return fail("an abandoned lingering connection was not closed at the linger deadline");
    if (took < 1000)
        return fail("the connection did not linger (closed over the client's unread data)");
    if (active != 0) return fail("a connection slot was left in use");
    printf("nghttp2 abandoned lingering connection closed at its deadline OK\n");
    return 0;
}

#ifdef E2E_TLS
/* Embedded self-signed EC cert, CN=127.0.0.1 (the test material of alpn_server.c). */
static const char CERT_PEM[] =
"-----BEGIN CERTIFICATE-----\n"
"MIIBmjCCAUGgAwIBAgIUOugIDeF4cZCY5f4MN+sk1xFwncIwCgYIKoZIzj0EAwIw\n"
"FDESMBAGA1UEAwwJMTI3LjAuMC4xMCAXDTI2MDcyNjA5MDEwMVoYDzIxMjYwNzAy\n"
"MDkwMTAxWjAUMRIwEAYDVQQDDAkxMjcuMC4wLjEwWTATBgcqhkjOPQIBBggqhkjO\n"
"PQMBBwNCAAQaZEdp4OpJZgbYIuvQiCDZE86uRuj+HQSP88xCcQ17ZSg3dWoDMRGH\n"
"DXznyJNlQ0vtbNr9Wcg4+/DAC/SLNu7Io28wbTAdBgNVHQ4EFgQUUW6CpMp5FGyE\n"
"eHRZT3D2XQUkJwowHwYDVR0jBBgwFoAUUW6CpMp5FGyEeHRZT3D2XQUkJwowDwYD\n"
"VR0TAQH/BAUwAwEB/zAaBgNVHREEEzARhwR/AAABgglsb2NhbGhvc3QwCgYIKoZI\n"
"zj0EAwIDRwAwRAIga5AdkBoyr0QJLwDzXXBKl/S4T7aplF32UlZDC3bkuusCIBNM\n"
"md2wiK5Cszs6q1wjkLLKoGtsrtjkNcFnKFdAw+zB\n"
"-----END CERTIFICATE-----\n";
static const char KEY_PEM[] =
"-----BEGIN PRIVATE KEY-----\n"
"MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgZKkZoDmfdcTJNMty\n"
"gSicJ0RHBVOrlx0ISej/z1cGczKhRANCAAQaZEdp4OpJZgbYIuvQiCDZE86uRuj+\n"
"HQSP88xCcQ17ZSg3dWoDMRGHDXznyJNlQ0vtbNr9Wcg4+/DAC/SLNu7I\n"
"-----END PRIVATE KEY-----\n";

static int case_chatty_tls(void) {
    static KlAllocator alloc;
    alloc = kl_allocator_default();
    KlTlsCtx *ctx = kl_tls_openssl_ctx_create_from_buf(
        (const unsigned char *)CERT_PEM, sizeof CERT_PEM - 1,
        (const unsigned char *)KEY_PEM, sizeof KEY_PEM - 1, NULL, 0, KL_MTLS_NONE, &alloc);
    if (!ctx) return fail("TLS context");
    const char *protos[] = { "h2", NULL };
    if (kl_tls_openssl_ctx_set_alpn(ctx, protos) != 0) return fail("ALPN");
    KlTlsConfig tls = { .ctx = ctx, .factory = kl_tls_openssl_create,
                        .ctx_destroy = kl_tls_openssl_ctx_destroy };
    return case_chatty(1, &tls);
}
#endif

int main(void) {
#if defined(_WIN32)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    g_bigbuf = malloc(BUFFERED_TOTAL);
    if (!g_bigbuf) return fail("memory");
    for (size_t i = 0; i < BUFFERED_TOTAL; i++) g_bigbuf[i] = pattern(2, i);
    int failed = 0;                        /* every case runs, so each one's result is seen */
    failed += case_chatty(0, NULL);
    failed += case_abandoned();
#ifdef E2E_TLS
    failed += case_chatty_tls();
#endif
    free(g_bigbuf);
    if (failed) { fprintf(stderr, "FAIL: %d case(s)\n", failed); return 1; }
    return 0;
}
