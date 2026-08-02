/*
 * test_stream_transport.c — Phase 5: a non-HTTP TCP protocol built entirely on
 * Keel's public transport surface, proving a protocol author needs no HTTP
 * machinery and no internal APIs.
 *
 * The server is a length-prefixed framed echo (4-byte big-endian length + payload)
 * built ONLY from public primitives:
 *   - the socket seam:  KlEventCtx.sockets (KlSocketProvider) ops for
 *     socket/bind/listen/accept/recv/send/close/set_nonblocking/set_reuseaddr;
 *   - the event loop:   kl_watcher_add/mod/del + kl_event_ctx_run (readiness);
 *   - backpressure:     KlDrain (buffered writes, flush on writability).
 * No KlServer, no router, no request/response, no kl_* internal headers.
 *
 * A raw-socket client drives it: several frames (tiny, medium, and one larger
 * than the socket buffer to force drain buffering + a writable-driven flush),
 * each echoed back byte-identical.
 */
#include "utest.h"
#include <keel/event_ctx.h>
#include <keel/socket.h>
#include <keel/drain.h>
#include <keel/allocator.h>
#include "net_compat.h"

#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define ST_PORT      18488
#define ST_HDR       4                 /* 4-byte BE length prefix */
#define ST_RBUF      (256 * 1024)      /* holds one whole in-flight frame */
#define ST_MAXFRAME  (200 * 1024)

static void st_nap(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ── framed-echo server, on the public transport surface ────────────── */

typedef struct {
    KlEventCtx             *ev;
    const KlSocketProvider *sp;
    KlSocketHandle          fd;
    KlDrain                 out;
    unsigned char          *rbuf;      /* accumulation buffer */
    size_t                  rlen;
    int                     dead;
} FrameConn;

/* Drain writer: send() over the provider; 0 = would-block (drain buffers). */
static ssize_t fc_write(const char *data, size_t len, void *ctx) {
    FrameConn *fc = ctx;
    kl_ssize_t n = fc->sp->ops->send(fc->sp->context, fc->fd, data, len);
    if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
    return (ssize_t)n;
}

static void fc_arm(FrameConn *fc) {
    /* Want WRITE only while the drain has pending bytes; always want READ. */
    KlEventMask m = KL_EVENT_READ | (kl_drain_pending(&fc->out) ? KL_EVENT_WRITE : 0);
    kl_watcher_mod(fc->ev, fc->fd, m);
}

static void fc_close(FrameConn *fc) {
    if (fc->dead) return;
    fc->dead = 1;
    kl_watcher_del(fc->ev, fc->fd);
    fc->sp->ops->close(fc->sp->context, fc->fd);
    kl_drain_free(&fc->out);
    KlAllocator *a = fc->ev->alloc;
    kl_free(a, fc->rbuf, ST_RBUF);
    kl_free(a, fc, sizeof(*fc));
}

static void fc_on_ready(KlSocketHandle fd, KlEventMask ready, void *ud) {
    (void)fd;
    FrameConn *fc = ud;

    if (ready & KL_EVENT_WRITE) {
        if (kl_drain_flush(&fc->out) < 0) { fc_close(fc); return; }
        fc_arm(fc);
    }

    if (ready & KL_EVENT_READ) {
        for (;;) {
            if (fc->rlen == ST_RBUF) break;    /* full — frame too big; drained below */
            kl_ssize_t n = fc->sp->ops->recv(fc->sp->context, fc->fd,
                                             fc->rbuf + fc->rlen, ST_RBUF - fc->rlen);
            if (n > 0) { fc->rlen += (size_t)n; continue; }
            if (n == 0) { fc_close(fc); return; }    /* peer closed */
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            fc_close(fc); return;                    /* error */
        }
        /* Parse + echo complete frames. */
        size_t off = 0;
        while (fc->rlen - off >= ST_HDR) {
            const unsigned char *p = fc->rbuf + off;
            uint32_t flen = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                            ((uint32_t)p[2] << 8) | (uint32_t)p[3];
            if (flen > ST_MAXFRAME) { fc_close(fc); return; }
            if (fc->rlen - off < ST_HDR + flen) break;   /* need more */
            if (kl_drain_write(&fc->out, (const char *)p, ST_HDR + flen) < 0) {
                fc_close(fc); return;
            }
            off += ST_HDR + flen;
        }
        if (off > 0) {
            memmove(fc->rbuf, fc->rbuf + off, fc->rlen - off);
            fc->rlen -= off;
        }
        fc_arm(fc);
    }
}

static void st_on_accept(KlSocketHandle lfd, KlEventMask ready, void *ud) {
    (void)ready;
    KlEventCtx *ev = ud;
    const KlSocketProvider *sp = ev->sockets ? ev->sockets : kl_socket_provider_posix();
    for (;;) {
        KlSocketHandle cfd = sp->ops->accept(sp->context, lfd, NULL);
        if (!kl_handle_valid(cfd)) break;            /* drained (would-block) */
        sp->ops->set_nonblocking(sp->context, cfd);
        FrameConn *fc = kl_malloc(ev->alloc, sizeof(*fc));
        if (!fc) { sp->ops->close(sp->context, cfd); continue; }
        memset(fc, 0, sizeof(*fc));
        fc->ev = ev; fc->sp = sp; fc->fd = cfd;
        fc->rbuf = kl_malloc(ev->alloc, ST_RBUF);
        if (!fc->rbuf) { sp->ops->close(sp->context, cfd); kl_free(ev->alloc, fc, sizeof(*fc)); continue; }
        kl_drain_init(&fc->out, fc_write, fc, ev->alloc);
        if (kl_watcher_add(ev, cfd, KL_EVENT_READ, fc_on_ready, fc) < 0) { fc_close(fc); continue; }
    }
}

/* ── server thread ──────────────────────────────────────────────────── */

static KlEventCtx       g_ev;
static KlAllocator      g_alloc;
static KlSocketHandle   g_listener;
static volatile int     g_stop;

static int st_listen(KlEventCtx *ev) {
    const KlSocketProvider *sp = kl_socket_provider_posix();
    KlSocketHandle lfd = sp->ops->socket(sp->context, AF_INET, SOCK_STREAM, 0);
    if (!kl_handle_valid(lfd)) return -1;
    sp->ops->set_reuseaddr(sp->context, lfd, 1);
    sp->ops->set_nonblocking(sp->context, lfd);
    const uint8_t lo[4] = { 127, 0, 0, 1 };
    KlSockAddr a;
    kl_sockaddr_from_ipv4(&a, lo, ST_PORT);
    if (sp->ops->bind(sp->context, lfd, &a) < 0) return -1;
    if (sp->ops->listen(sp->context, lfd, 16) < 0) return -1;
    g_listener = lfd;
    return kl_watcher_add(ev, lfd, KL_EVENT_READ, st_on_accept, ev);
}

static void *st_srv_thread(void *arg) {
    (void)arg;
    while (!g_stop)
        if (kl_event_ctx_run(&g_ev, 32, 50) < 0) break;
    return NULL;
}

/* ── client helpers (raw sockets) ───────────────────────────────────── */

static int st_connect(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_port = htons(ST_PORT);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) { kl_test_closesock(fd); return -1; }
    return fd;
}

/* Send one framed message and read its echo back, asserting byte-equality. */
static int st_roundtrip(int fd, const unsigned char *payload, uint32_t len) {
    unsigned char hdr[ST_HDR] = {
        (unsigned char)(len >> 24), (unsigned char)(len >> 16),
        (unsigned char)(len >> 8),  (unsigned char)len };
    if (kl_test_sockwrite(fd, hdr, ST_HDR) != ST_HDR) return -1;
    /* Write payload in chunks (large frames exceed one send). */
    size_t sent = 0;
    while (sent < len) {
        long w = kl_test_sockwrite(fd, payload + sent, len - sent);
        if (w <= 0) return -1;
        sent += (size_t)w;
    }
    /* Read back ST_HDR + len bytes and compare. */
    unsigned char rh[ST_HDR]; size_t got = 0;
    while (got < ST_HDR) {
        if (kl_test_poll1(fd, 0, 3000) <= 0) return -1;
        long n = kl_test_sockread(fd, rh + got, ST_HDR - got);
        if (n <= 0) return -1;
        got += (size_t)n;
    }
    uint32_t rlen = ((uint32_t)rh[0]<<24)|((uint32_t)rh[1]<<16)|((uint32_t)rh[2]<<8)|rh[3];
    if (rlen != len) return -1;
    unsigned char *rb = malloc(len ? len : 1);
    if (!rb) return -1;
    got = 0;
    while (got < len) {
        if (kl_test_poll1(fd, 0, 3000) <= 0) { free(rb); return -1; }
        long n = kl_test_sockread(fd, rb + got, len - got);
        if (n <= 0) { free(rb); return -1; }
        got += (size_t)n;
    }
    int ok = (len == 0) || (memcmp(rb, payload, len) == 0);
    free(rb);
    return ok ? 0 : -1;
}

UTEST(stream_transport, framed_echo_roundtrip) {
    g_stop = 0; g_listener = KL_INVALID_SOCKET;
    g_alloc = kl_allocator_default();
    ASSERT_EQ(kl_event_ctx_init(&g_ev, &g_alloc), 0);
    ASSERT_EQ(st_listen(&g_ev), 0);

    pthread_t tid;
    ASSERT_EQ(pthread_create(&tid, NULL, st_srv_thread, NULL), 0);

    int fd = -1;
    for (int i = 0; i < 100 && fd < 0; i++) { fd = st_connect(); if (fd < 0) st_nap(5); }
    ASSERT_TRUE(fd >= 0);

    /* tiny */
    ASSERT_EQ(st_roundtrip(fd, (const unsigned char *)"hello", 5), 0);
    /* empty frame */
    ASSERT_EQ(st_roundtrip(fd, (const unsigned char *)"", 0), 0);
    /* medium (8 KiB) */
    static unsigned char med[8192];
    for (size_t i = 0; i < sizeof(med); i++) med[i] = (unsigned char)(i * 31 + 7);
    ASSERT_EQ(st_roundtrip(fd, med, sizeof(med)), 0);
    /* large (128 KiB) — exceeds the socket buffer, forces drain buffering + a
     * writable-driven flush on the server side */
    static unsigned char big[128 * 1024];
    for (size_t i = 0; i < sizeof(big); i++) big[i] = (unsigned char)(i * 131 + 17);
    ASSERT_EQ(st_roundtrip(fd, big, sizeof(big)), 0);
    /* several back-to-back small frames (pipelining) */
    for (int i = 0; i < 20; i++)
        ASSERT_EQ(st_roundtrip(fd, (const unsigned char *)"ping", 4), 0);

    kl_test_closesock(fd);
    st_nap(120);          /* let the server observe the peer close and reap the conn */
    g_stop = 1;
    pthread_join(tid, NULL);

    /* Server-side teardown: close listener, then free ctx (frees any watchers). */
    if (kl_handle_valid(g_listener)) {
        const KlSocketProvider *sp = kl_socket_provider_posix();
        kl_watcher_del(&g_ev, g_listener);
        sp->ops->close(sp->context, g_listener);
    }
    kl_event_ctx_free(&g_ev);
}

UTEST_MAIN();
