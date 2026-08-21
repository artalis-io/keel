/*
 * freestanding_dns_harness.c — RUNTIME proof of the freestanding (UDP-only) DNS
 * resolver's truncation branch (6.4a-2 review, Medium).
 *
 * The freestanding build of src/dns_resolver.c compiles the RFC 7766 TCP fallback
 * out; a truncated (TC) response must instead settle the leg EMPTY so the whole
 * resolution fails PROMPTLY and CLEARLY (KL_ERR_DNS) — never open a TCP socket and
 * never wait for the per-leg timeout. That branch is otherwise only compile/link
 * gated; this harness EXECUTES it on the host, under -DKEEL_FREESTANDING, over a
 * mock datagram socket provider + a mock readiness event loop (no OS sockets, no
 * syscalls, an advanceable mock clock that is NEVER advanced — so a completion can
 * only come from the injected recv, not a timeout).
 *
 * Built + run under ASan+UBSan+LSan by `make freestanding-dns-harness`. It links
 * the SAME freestanding TUs as the freestanding-dns archive (FREESTANDING_DNS_SRC)
 * + the F-5 host platform TU (kl_plat_random / kl_monotonic_ms / the fail-closed
 * sockdef+builtin stubs); the 4 datagram-path sockdef seams udp.c references (never
 * called — the mock provider wins) are defined below, fail-closed.
 */
#include <keel/clock.h>
#include <keel/dns_resolver.h>
#include <keel/resolver.h>
#include <keel/allocator.h>
#include <keel/event_ctx.h>
#include <keel/timer.h>
#include <keel/datagram.h>
#include <keel/sockaddr.h>

#include "../src/socket.h"                /* KlSocketProvider / KlSocketOps / KlIoStatus */
#include "freestanding_platform_test.h"   /* fs_clock_set / fs_clock_now */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ══════════════════════════════════════════════════════════════════════
 * Harness plumbing
 * ══════════════════════════════════════════════════════════════════════ */
static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  FAIL: %s\n", (msg)); } \
} while (0)

/* Counting allocator over host malloc/free — asserts live==0 at teardown (leak
 * check that also runs where LSan can't, e.g. macOS). */
typedef struct { KlAllocator base; long live; } CountAlloc;
static void *ca_malloc(void *c, size_t n) {
    CountAlloc *a = c; void *p = malloc(n); if (p) a->live++; return p;
}
static void *ca_realloc(void *c, void *p, size_t on, size_t nn) {
    (void)on; CountAlloc *a = c; if (!p) { void *q = malloc(nn); if (q) a->live++; return q; }
    return realloc(p, nn);
}
static void ca_free(void *c, void *p, size_t n) {
    (void)n; CountAlloc *a = c; if (p) { a->live--; free(p); }
}
static void ca_init(CountAlloc *a) {
    a->base.malloc = ca_malloc; a->base.realloc = ca_realloc; a->base.free = ca_free;
    a->base.ctx = a; a->live = 0;
}

/* ══════════════════════════════════════════════════════════════════════
 * Mock datagram socket provider (readiness face)
 * ══════════════════════════════════════════════════════════════════════ */
#define MOCK_NS_ADDR "127.0.0.1"
#define MOCK_NS_PORT 53
#define MAX_MSG 4
#define MSG_CAP 600

static struct MockDgram {
    int            stream_sockets;      /* # of SOCK_STREAM socket() calls == TCP-recovery attempts */
    int            next_fd;
    uint8_t        sent[MAX_MSG][MSG_CAP];   /* captured outbound queries (A + AAAA) */
    size_t         sent_len[MAX_MSG];
    int            sent_n;
    uint8_t        inj[MAX_MSG][MSG_CAP];    /* injected inbound responses */
    size_t         inj_len[MAX_MSG];
    int            inj_n, inj_i;
    KlIoStatus     last_status;
    KlSockAddr     ns_src;                    /* source stamped on every injected recv */
} g;

static KlSocketHandle ds_socket(void *c, int d, int t, int p) {
    (void)c; (void)d; (void)p;
    if (t == SOCK_STREAM) g.stream_sockets++;   /* the event we assert NEVER happens */
    return (KlSocketHandle)(1000 + (++g.next_fd));
}
static int  ds_bind(void *c, KlSocketHandle f, const KlSockAddr *a) { (void)c;(void)f;(void)a; return 0; }
static int  ds_close(void *c, KlSocketHandle f) { (void)c;(void)f; return 0; }
static int  ds_set_nonblocking(void *c, KlSocketHandle f) { (void)c;(void)f; return 0; }
static void ds_set_cloexec(void *c, KlSocketHandle f) { (void)c;(void)f; }
static int  ds_get_local_addr(void *c, KlSocketHandle f, KlSockAddr *a) {
    (void)c;(void)f; kl_sockaddr_parse(a, "0.0.0.0", 0); return 0;
}
static int  ds_get_so_error(void *c, KlSocketHandle f, int *e) { (void)c;(void)f; if (e) *e = 0; return 0; }
static KlIoStatus ds_io_status(void *c) { (void)c; return g.last_status; }

/* Capture each outbound query so we can echo its exact question in a TC response. */
static kl_ssize_t dg_send(void *c, KlSocketHandle f, const void *data, size_t len,
                          const KlSockAddr *dest, const KlSockAddr *src, int tos) {
    (void)c; (void)f; (void)dest; (void)src; (void)tos;
    if (g.sent_n < MAX_MSG && len <= MSG_CAP) {
        memcpy(g.sent[g.sent_n], data, len);
        g.sent_len[g.sent_n] = len;
        g.sent_n++;
    }
    return (kl_ssize_t)len;                       /* fully accepted */
}
/* Deliver the next injected response from the nameserver source; then drain. */
static kl_ssize_t dg_recv(void *c, KlSocketHandle f, void *buf, size_t buflen,
                          KlSockAddr *src, KlDgramRxMeta *meta) {
    (void)c; (void)f;
    if (meta) { memset(meta, 0, sizeof(*meta)); meta->tos = -1; }
    if (g.inj_i < g.inj_n) {
        size_t n = g.inj_len[g.inj_i];
        if (n > buflen) n = buflen;
        memcpy(buf, g.inj[g.inj_i], n);
        if (src) *src = g.ns_src;
        g.inj_i++;
        return (kl_ssize_t)n;
    }
    g.last_status = KL_IO_WOULD_BLOCK;            /* drained */
    return -1;
}
static uint32_t dg_configure(void *c, KlSocketHandle f, int fam, const struct KlDatagramSocketConfig *cfg) {
    (void)c; (void)f; (void)fam; (void)cfg; return 0;   /* no capture caps */
}

static const KlDatagramOps MOCK_DGRAM_OPS = {
    .send = dg_send, .recv = dg_recv, .configure = dg_configure,
};
static const KlSocketOps MOCK_DSOCK_OPS = {
    .socket = ds_socket, .bind = ds_bind, .close = ds_close,
    .set_nonblocking = ds_set_nonblocking, .set_cloexec = ds_set_cloexec,
    .get_local_addr = ds_get_local_addr, .get_so_error = ds_get_so_error,
    .io_status = ds_io_status, .name = "mock-dns-dgram-sock",
};
static KlSocketProvider g_dsock_provider = {
    &MOCK_DSOCK_OPS, &g, KL_SOCK_CAP_NATIVE_FD | KL_SOCK_CAP_DATAGRAM, &MOCK_DGRAM_OPS,
};

/* ══════════════════════════════════════════════════════════════════════
 * Mock readiness event loop — one tagged watcher (the DNS socket's READ)
 * ══════════════════════════════════════════════════════════════════════ */
static struct MockLoop {
    int            armed;
    KlSocketHandle fd;
    KlEventMask    mask;
    void          *udata;    /* tagged KlWatcher pointer */
} gl;

static int mloop_init(KlEventLoop *loop) { (void)loop; memset(&gl, 0, sizeof(gl)); return 0; }
static int mloop_add(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata) {
    (void)loop;
    gl.armed = 1; gl.fd = fd; gl.mask = mask; gl.udata = udata; return 0;
}
static int mloop_mod(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata) {
    (void)loop; gl.fd = fd; gl.mask = mask; gl.udata = udata; gl.armed = 1; return 0;
}
static int mloop_del(KlEventLoop *loop, KlSocketHandle fd) { (void)loop;(void)fd; gl.armed = 0; return 0; }
/* Emit one READ event for the armed watcher while injected responses remain. */
static int mloop_wait(KlEventLoop *loop, KlEvent *out, int max, int timeout_ms) {
    (void)loop; (void)timeout_ms;
    if (max < 1 || !gl.armed || !(gl.mask & KL_EVENT_READ) || g.inj_i >= g.inj_n)
        return 0;
    out[0].udata = gl.udata;
    out[0].ready = KL_EVENT_READ;
    return 1;
}
static void mloop_close(KlEventLoop *loop) { (void)loop; }
static unsigned mloop_caps(const KlEventLoop *loop) {
    (void)loop; return KL_EVENT_CAP_READINESS | KL_EVENT_CAP_NATIVE_FD;
}
static const struct KlSocketProvider *mloop_native_provider(const KlEventLoop *loop) {
    (void)loop; return &g_dsock_provider;
}
static const KlEventOps MOCK_EVENT_OPS = {
    .init = mloop_init, .add = mloop_add, .mod = mloop_mod, .del = mloop_del,
    .wait = mloop_wait, .close = mloop_close, .caps = mloop_caps,
    .native_provider = mloop_native_provider, .completion = NULL,
};
static const KlEventProvider MOCK_EVENT_PROVIDER = { &MOCK_EVENT_OPS, "mock-dns-readiness" };

/* ══════════════════════════════════════════════════════════════════════
 * The scenario
 * ══════════════════════════════════════════════════════════════════════ */
typedef struct { int done; int err; int result_null; int naddrs; } DoneCtx;

static void on_done(KlResolveReq *req, const KlResolveResult *result, int error, void *ud) {
    (void)req;
    DoneCtx *d = ud;
    d->done = 1;
    d->err = error;
    d->result_null = (result == NULL);
    d->naddrs = result ? result->naddrs : 0;
}

/* Craft a TC (truncated) response by echoing a captured query verbatim, then
 * setting QR|TC and zeroing ANCOUNT. Echoing the question guarantees both the
 * txid match (dns_find_leg) and the question match (dns_question_matches). */
static void make_tc_response(const uint8_t *query, size_t qlen, int slot) {
    memcpy(g.inj[slot], query, qlen);
    g.inj[slot][2] |= 0x80;      /* QR = response */
    g.inj[slot][2] |= 0x02;      /* TC = truncated */
    g.inj[slot][6] = 0;          /* ANCOUNT hi */
    g.inj[slot][7] = 0;          /* ANCOUNT lo */
    g.inj_len[slot] = qlen;
}

int main(void) {
    printf("== freestanding DNS harness: truncated response settles clean (no TCP, no timeout) ==\n");

    CountAlloc a; ca_init(&a);
    memset(&g, 0, sizeof(g));
    memset(&gl, 0, sizeof(gl));
    g.last_status = KL_IO_OK;
    kl_sockaddr_parse(&g.ns_src, MOCK_NS_ADDR, MOCK_NS_PORT);
    fs_clock_set(1000);                 /* fixed; NEVER advanced → no timeout can fire */

    KlEventCtx ev;
    CHECK(kl_event_ctx_init_ex(&ev, &a.base, &MOCK_EVENT_PROVIDER) == 0, "ctx init");
    ev.sockets = &g_dsock_provider;     /* the KlEventCtx.sockets seam: our mock datagram provider */

    /* Freestanding resolver: explicit nameserver is MANDATORY (no resolv.conf). */
    KlDnsResolverConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.nameserver = MOCK_NS_ADDR;
    cfg.port = MOCK_NS_PORT;
    cfg.disable_edns = 1;               /* keep the query minimal (no OPT to echo) */
    cfg.disable_cookies = 1;
    cfg.alloc = &a.base;
    KlResolver *r = kl_dns_resolver_create(&ev, &cfg);
    CHECK(r != NULL, "resolver create (explicit nameserver honored)");
    if (!r) { printf("  ABORT\n"); return 1; }

    DoneCtx d; memset(&d, 0, sizeof(d));
    KlResolveReq *req = r->resolve(r, &ev, "example.com", 80, on_done, &d);
    CHECK(req != NULL, "resolve started");

    /* Both families (A + AAAA) were transmitted synchronously through the mock. */
    CHECK(g.sent_n == 2, "both A+AAAA queries sent");

    /* Injecting a TC response for BOTH legs makes the whole resolution fail once
     * both settle empty (no leg is left to stall on the un-advanced clock). */
    for (int i = 0; i < g.sent_n && i < MAX_MSG; i++)
        make_tc_response(g.sent[i], g.sent_len[i], i);
    g.inj_n = g.sent_n;

    /* One readiness drive drains both injected datagrams (serial recv machine). */
    for (int tick = 0; tick < 8 && !d.done; tick++) {
        kl_event_ctx_run(&ev, 16, 0);
        kl_timer_fire(&ev);             /* no timer is due at the fixed clock */
    }

    CHECK(g.inj_i == g.inj_n, "both TC responses were delivered (input actually exercised)");
    CHECK(d.done == 1, "resolution completed (did not hang on the leg timeout)");
    CHECK(d.result_null == 1, "completion carried no result");
    CHECK(d.err == KL_ERR_DNS, "completion error is KL_ERR_DNS");
    CHECK(d.naddrs == 0, "no addresses resolved");
    CHECK(g.stream_sockets == 0, "no SOCK_STREAM socket opened (no RFC 7766 TCP recovery)");
    CHECK(fs_clock_now() == 1000, "clock never advanced (completion was recv-driven, not a timeout)");

    r->destroy(r);
    kl_event_ctx_free(&ev);
    CHECK(a.live == 0, "no leaks (allocator balanced)");

    printf("== freestanding DNS harness: %d passed, %d failed ==\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

/* ── Datagram-path sockdef seams referenced by udp.c (never called — the mock
 *    provider supplies the live datagram ops; these are fail-closed link fodder,
 *    mirroring the KEEL_FS_LINK_DGRAM stubs in freestanding_link_main.c). The
 *    stream sockdef seams (socket/connect/close/...) come from the host platform TU. */
void kl_sockdef_set_cloexec(KlSocketHandle f) { (void)f; }
int  kl_sockdef_bind(KlSocketHandle f, const KlSockAddr *a) { (void)f;(void)a; return -1; }
int  kl_sockdef_get_local_addr(KlSocketHandle f, KlSockAddr *a) { (void)f;(void)a; return -1; }
const struct KlDatagramOps *kl_sockdef_dgram(void) { return NULL; }
