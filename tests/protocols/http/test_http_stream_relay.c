/*
 * test_http_stream_relay.c: the end-to-end gate for bounded HTTP response streaming.
 *
 * upstream Keel server -> Keel async client -> relay handler -> Keel server -> slow client.
 *
 * The upstream server streams a long body through kl_http_response_stream_try_write, paced by the
 * writable edge. The relay is a suspended handler on a second server: its Keel client streams the
 * upstream body, and each fragment goes downstream through kl_http_response_stream_try_write. A
 * fragment refused with WOULD_BLOCK is kept (the client already delivered it) and the upstream
 * client is paused (KL_HTTP_CLIENT_BODY_PAUSE); the writable edge retries it and resumes the
 * client. The downstream client reads slowly, stops for a while, and reads on.
 *
 * Asserted: byte-exact, in-order delivery (checked as it arrives, so the test holds no copy);
 * the relay's pending output never above its capacity; peak memory growth bounded far below the
 * transfer size; the upstream producer observably stalls while the downstream client is stopped;
 * a downstream client that leaves while upstream is paused ends both ops exactly once; and a
 * server stop while paused ends the relay cleanly. Every backend; plaintext and (mock) TLS.
 *
 * Built with -DKEEL_RELAY_MBEDTLS (`make relay-tls KEEL_TLS=mbedtls MBEDTLS_DIR=...`), the same cases
 * also run with real mbedTLS on the upstream hop, on the downstream hop, and on both. Downstream,
 * the relay server's bounded response then holds real ciphertext in its output queue and the
 * reader is Keel's sync client over HTTPS (it stops reading by stalling in its body callback);
 * upstream, the relay's client pauses and resumes a real TLS connection.
 */
#include "utest.h"
#include <keel/keel.h>
#include <keel/tls.h>
#include "net_compat.h"
#include "mock_tls.h"
#include "event_caps.h"
#include "http_conn_internal.h"   /* white-box: shrink the server-side send buffer */
#include "platform_thread.h"
#include "kl_atomic.h"       /* the startup flag the test waits on */
#include <stdatomic.h>       /* state the servers write and the test thread reads */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#if defined(KEEL_RELAY_MBEDTLS)
#include <keel_tls_mbedtls.h>
/* The self-signed EC test certificate of integrations/tls/mbedtls/tests/smoke_tls.c. Test-only. */
static const char RL_CERT_PEM[] =
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
static const char RL_KEY_PEM[] =
"-----BEGIN PRIVATE KEY-----\n"
"MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgZKkZoDmfdcTJNMty\n"
"gSicJ0RHBVOrlx0ISej/z1cGczKhRANCAAQaZEdp4OpJZgbYIuvQiCDZE86uRuj+\n"
"HQSP88xCcQ17ZSg3dWoDMRGHDXznyJNlQ0vtbNr9Wcg4+/DAC/SLNu7I\n"
"-----END PRIVATE KEY-----\n";
static KlAllocator g_tls_alloc;
static KlTlsConfig g_up_tls, g_cli_tls, g_dn_tls;
static KlTlsCtx *g_dn_cli_ctx;
#endif
/* Which hops carry TLS. */
#define RL_DOWN_MOCK 1     /* the relay server over mock TLS (a passthrough), raw reader */
#define RL_UP_REAL   2     /* the upstream hop over real mbedTLS */
#define RL_DOWN_REAL 4     /* the relay server over real mbedTLS, read by Keel's sync client */
static int g_mode;
#if defined(_WIN32)
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

#define RL_TOTAL   (32u * 1024u * 1024u)   /* the upstream body */
#define RL_CHUNK   (16u * 1024u)
#define RL_CAP     (64u * 1024u)           /* both servers' stream capacity */

static unsigned char rl_byte(size_t k) { return (unsigned char)((k * 131u) ^ (k >> 10)); }

/* Server startup, ordered. A zero-delay timer added before the server thread starts runs on the
 * loop's first tick, after the listener is bound and listening; it sets the flag with an atomic store,
 * which the test waits on, so the test reads the bound port only after the server thread wrote it
 * (polling the port itself raced with that write, and could connect between bind and listen). */
static void mark_ready(void *ud) { kl_atomic_store_int((int *)ud, 1); }
static int arm_ready(KlHttpServer *s, int *flag) {
    kl_atomic_store_int(flag, 0);
    return kl_timer_add(kl_http_server_event_ctx(s), 0, mark_ready, flag) < 0 ? -1 : 0;
}
static int wait_ready(const int *flag) {
    for (int i = 0; i < 500 && !kl_atomic_load_int(flag); i++) kl_test_sleep_ms(10);
    return kl_atomic_load_int(flag);
}
static int g_up_ready, g_rl_ready;
static void server_thread_fn(void *arg) { kl_http_server_run((KlHttpServer *)arg); }
/* The relay server's run, and when it returned by itself (a drain ends it without a second stop). */
static _Atomic int g_rl_run_done;
static _Atomic uint64_t g_rl_run_done_ms;
static void rl_thread_fn(void *arg) {
    kl_http_server_run((KlHttpServer *)arg);
    g_rl_run_done_ms = kl_monotonic_ms();
    g_rl_run_done = 1;
}
static void shrink_sndbuf(KlHttpConn *c) {
    int sb = 8192;
    (void)setsockopt((int)c->stream.fd, SOL_SOCKET, SO_SNDBUF, (const char *)&sb, sizeof sb);
}
static int connect_rcvbuf(int port, int rcvbuf) {
    int fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (rcvbuf > 0)
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (const char *)&rcvbuf, sizeof rcvbuf);
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        kl_test_closesock(fd);
        return -1;
    }
    return fd;
}
/* Peak resident memory of the process, in bytes (monotonic). */
static size_t peak_rss(void) {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc;
    if (!K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) return 0;
    return (size_t)pmc.PeakWorkingSetSize;
#else
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) != 0) return 0;
#if defined(__APPLE__)
    return (size_t)ru.ru_maxrss;           /* bytes on macOS */
#else
    return (size_t)ru.ru_maxrss * 1024u;   /* KiB elsewhere */
#endif
#endif
}

/* A counting allocator for the relay server: live and peak bytes of everything it allocates through
 * its KlAllocator (the response's outbound buffer, the connection's output queue, headers). Only the
 * relay server's loop thread allocates through it. */
typedef struct { _Atomic size_t live, peak; } CountAlloc;   /* read by the test thread */
static CountAlloc g_ca;
static void *ca_malloc(void *ctx, size_t n) {
    CountAlloc *c = ctx;
    void *p = malloc(n);
    if (p) { c->live += n; if (c->live > c->peak) c->peak = c->live; }
    return p;
}
static void *ca_realloc(void *ctx, void *ptr, size_t old_n, size_t new_n) {
    CountAlloc *c = ctx;
    void *p = realloc(ptr, new_n);
    if (p || new_n == 0) {
        c->live = c->live - old_n + new_n;
        if (c->live > c->peak) c->peak = c->live;
    }
    return p;
}
static void ca_free(void *ctx, void *ptr, size_t n) {
    CountAlloc *c = ctx;
    if (ptr) c->live -= n;
    free(ptr);
}
static KlAllocator g_rl_alloc = { ca_malloc, ca_realloc, ca_free, &g_ca };

/* ── Upstream: a bounded producer ─────────────────────────────────────────────────────────────── */
typedef struct {
    KlAsyncOp op;
    KlHttpResponse *res;
    /* Written by the upstream server's thread, read by the test thread: atomic. */
    _Atomic size_t sent;
    _Atomic size_t max_pending;   /* the upstream response's own bound (ciphertext with real TLS) */
    _Atomic int would_blocks, cancelled, ended;
} UpStream;
static UpStream g_up;
static KlHttpServer up_srv;

static void up_pump(void) {
    static unsigned char chunk[RL_CHUNK];
    while (g_up.sent < RL_TOTAL) {
        size_t n = RL_TOTAL - g_up.sent < RL_CHUNK ? RL_TOTAL - g_up.sent : RL_CHUNK;
        for (size_t i = 0; i < n; i++) chunk[i] = rl_byte(g_up.sent + i);
        KlHttpStreamStatus st = kl_http_response_stream_try_write(g_up.res, chunk, n);
        if (st == KL_HTTP_STREAM_WOULD_BLOCK) { g_up.would_blocks++; return; }
        if (st != KL_HTTP_STREAM_ACCEPTED) { kl_async_cancel(&up_srv, &g_up.op); return; }
        g_up.sent += n;
        size_t p = kl_http_response_stream_pending(g_up.res);
        if (p > g_up.max_pending) g_up.max_pending = p;
    }
    g_up.ended = 1;
    kl_http_response_end_stream(g_up.res);
    kl_async_complete(&up_srv, &g_up.op);
}
static void up_on_writable(void *ud) { (void)ud; if (!g_up.ended && !g_up.cancelled) up_pump(); }
static void up_resume(KlAsyncOp *op, void *ud) { (void)op; (void)ud; }
static void up_cancel(KlAsyncOp *op, void *ud) { (void)op; (void)ud; g_up.cancelled++; }
static void up_start(void *ud) { (void)ud; up_pump(); }
static void handle_up(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)ctx;
    g_up.res = res;
    KlHttpConn *c = kl_http_request_conn(req);
    shrink_sndbuf(c);
    KlHttpResponseWriteFn w;
    void *wc;
    if (kl_http_response_begin_stream(res, 200, &w, &wc) < 0) return;
    (void)kl_http_response_stream_on_writable(res, up_on_writable, NULL);
    g_up.op.on_resume = up_resume;
    g_up.op.on_cancel = up_cancel;
    if (kl_async_suspend(&up_srv, c, &g_up.op) < 0) return;
    (void)kl_timer_add(kl_http_server_event_ctx(&up_srv), 0, up_start, NULL);
}

/* ── The relay: a suspended handler whose Keel client streams the upstream body ───────────────── */
typedef struct {
    KlAsyncOp op;
    KlHttpResponse *res;
    KlHttpClient *_Atomic cli;    /* atomic: the test thread checks it went away */
    unsigned char held[KL_HTTP_CLIENT_RECV_BUF_SIZE];   /* the fragment refused downstream */
    size_t held_len;
    /* Counters below: written by the relay server's thread, read by the test thread: atomic. */
    _Atomic size_t relayed, max_pending;
    _Atomic size_t max_held;  /* white-box: drain + output queue, read from the structures */
    _Atomic size_t hdr_cap;   /* the response's header buffer, allocated before the transfer */
    KlHttpConn *conn;
    _Atomic int pauses, resumes, edges, edges_after_end, cancelled, finished, completed;
    _Atomic int upstream_err, resume_failed;
    _Atomic KlHttpStreamStatus failed;
} Relay;
static Relay g_rl;
static KlHttpServer rl_srv;
static char g_up_url[64];

static void rl_note_pending(void) {
    size_t p = kl_http_response_stream_pending(g_rl.res);
    if (p > g_rl.max_pending) g_rl.max_pending = p;
    size_t held = kl_drain_buffered(&g_rl.res->drain) +
                  (size_t)(g_rl.conn->comp_tlsq_appended - g_rl.conn->comp_tlsq_sent);
    if (held > g_rl.max_held) g_rl.max_held = held;
}
static void rl_finish(void) {
    if (g_rl.finished) return;
    g_rl.finished = 1;
    kl_http_response_end_stream(g_rl.res);
    kl_async_complete(&rl_srv, &g_rl.op);
}
static void rl_drop_client(void) {
    if (!g_rl.cli) return;
    kl_http_client_cancel(g_rl.cli);
    kl_http_client_free(g_rl.cli);
    g_rl.cli = NULL;
}
static int rl_on_body(const char *data, size_t len, void *ud) {
    (void)ud;
    KlHttpStreamStatus st = kl_http_response_stream_try_write(g_rl.res, data, len);
    if (st == KL_HTTP_STREAM_ACCEPTED) {
        g_rl.relayed += len;
        rl_note_pending();
        return KL_HTTP_CLIENT_BODY_CONTINUE;
    }
    if (st == KL_HTTP_STREAM_WOULD_BLOCK && len <= sizeof g_rl.held) {
        memcpy(g_rl.held, data, len);        /* consumed from the client; delivered on the edge */
        g_rl.held_len = len;
        g_rl.pauses++;
        return KL_HTTP_CLIENT_BODY_PAUSE;
    }
    g_rl.failed = st;
    return KL_HTTP_CLIENT_BODY_ABORT;
}
static void rl_on_complete(void *ud) { (void)ud; rl_finish(); }
static void rl_on_writable(void *ud) {
    (void)ud;
    g_rl.edges++;
    if (g_rl.finished || g_rl.cancelled) { g_rl.edges_after_end++; return; }
    if (!g_rl.held_len) return;
    KlHttpStreamStatus st = kl_http_response_stream_try_write(g_rl.res, g_rl.held, g_rl.held_len);
    if (st == KL_HTTP_STREAM_WOULD_BLOCK) return;          /* re-armed: the next edge retries */
    if (st != KL_HTTP_STREAM_ACCEPTED) {
        g_rl.failed = st;
        rl_drop_client();
        kl_async_cancel(&rl_srv, &g_rl.op);
        return;
    }
    g_rl.relayed += g_rl.held_len;
    g_rl.held_len = 0;
    rl_note_pending();
    g_rl.resumes++;
    if (kl_http_client_resume(g_rl.cli) < 0) g_rl.resume_failed++;
}
static void rl_on_done(KlHttpClient *cli, void *ud) {
    (void)ud;
    if (kl_http_client_error(cli)) g_rl.upstream_err = 1;
    kl_http_client_free(cli);
    g_rl.cli = NULL;
    if (!g_rl.finished && !g_rl.cancelled) kl_async_cancel(&rl_srv, &g_rl.op);   /* upstream failed */
}
static void rl_resume(KlAsyncOp *op, void *ud) { (void)op; (void)ud; g_rl.completed++; }
static void rl_cancel(KlAsyncOp *op, void *ud) {
    (void)op; (void)ud;
    g_rl.cancelled++;
    rl_drop_client();                                       /* the downstream client is gone */
}
static void handle_relay(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)ctx;
    g_rl.res = res;
    KlHttpConn *c = kl_http_request_conn(req);
    g_rl.conn = c;
    g_rl.hdr_cap = res->hdr_cap;
    shrink_sndbuf(c);
    KlHttpResponseWriteFn w;
    void *wc;
    if (kl_http_response_begin_stream(res, 200, &w, &wc) < 0) return;
    (void)kl_http_response_stream_on_writable(res, rl_on_writable, NULL);
    g_rl.op.on_resume = rl_resume;
    g_rl.op.on_cancel = rl_cancel;
    if (kl_async_suspend(&rl_srv, c, &g_rl.op) < 0) return;
    static const KlHttpClientStreamCfg scfg = { .on_body = rl_on_body, .on_complete = rl_on_complete };
    static KlAllocator alloc;
    alloc = kl_allocator_default();
    KlHttpClientConfig ccfg = { .timeout_ms = 120000, .max_response_size = (size_t)RL_TOTAL * 2 };
#if defined(KEEL_RELAY_MBEDTLS)
    if (g_mode & RL_UP_REAL) ccfg.tls = &g_cli_tls;
#endif
    g_rl.cli = kl_http_client_start_s(kl_http_server_event_ctx(&rl_srv), &alloc, &ccfg, "GET",
                                      g_up_url, NULL, 0, NULL, 0, &scfg, rl_on_done, NULL);
    if (!g_rl.cli) kl_async_cancel(&rl_srv, &g_rl.op);
}

/* ── The downstream client: de-chunks and checks every byte as it reads ───────────────────────── */
typedef struct {
    int state;                 /* 0 head, 1 size, 2 size LF, 3 data, 4 data CR, 5 data LF, 6 done, -1 bad */
    unsigned head_match;
    size_t size, left, payload;
    int ended;
    size_t mismatch_at;
} Dechunk;
static void dc_feed(Dechunk *d, const unsigned char *p, size_t n) {
    static const char crlf2[] = "\r\n\r\n";
    for (size_t i = 0; i < n && d->state >= 0 && d->state != 6; i++) {
        unsigned char ch = p[i];
        switch (d->state) {
        case 0:
            d->head_match = ch == (unsigned char)crlf2[d->head_match] ? d->head_match + 1
                                                                     : (ch == '\r' ? 1u : 0u);
            if (d->head_match == 4) { d->state = 1; d->size = 0; }
            break;
        case 1: {
            int v = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 :
                    ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
            if (ch == '\r') d->state = 2;
            else if (v < 0) d->state = -1;
            else d->size = d->size * 16 + (size_t)v;
            break;
        }
        case 2:
            if (ch != '\n') { d->state = -1; break; }
            if (d->size == 0) { d->ended = 1; d->state = 6; break; }
            d->left = d->size;
            d->state = 3;
            break;
        case 3: {
            size_t take = n - i < d->left ? n - i : d->left;
            for (size_t k = 0; k < take; k++)
                if (p[i + k] != rl_byte(d->payload + k) && d->mismatch_at == (size_t)-1)
                    d->mismatch_at = d->payload + k;
            d->payload += take;
            d->left -= take;
            i += take - 1;
            if (d->left == 0) d->state = 4;
            break;
        }
        case 4: d->state = ch == '\r' ? 5 : -1; break;
        case 5: if (ch == '\n') { d->state = 1; d->size = 0; } else d->state = -1; break;
        default: break;
        }
    }
}

#if defined(KEEL_RELAY_MBEDTLS)
static KlTlsCtx *rl_server_ctx(void) {
    return kl_tls_mbedtls_ctx_create_from_buf(
        (const unsigned char *)RL_CERT_PEM, sizeof RL_CERT_PEM,
        (const unsigned char *)RL_KEY_PEM, sizeof RL_KEY_PEM, NULL, 0, KL_MTLS_NONE, &g_tls_alloc);
}
#endif

static int relay_servers_up(KlPlatThread *up_tid, KlPlatThread *rl_tid, int mode, int drain_ms) {
    static KlTlsConfig tls_cfg;
    g_mode = mode;
    /* Each case starts from zero: the handlers reset these only once a request arrives, and a case
     * that waits on them (pauses, would-blocks) before then would read the previous case's values.
     * The run order differs per platform (MinGW runs the cases in reverse). */
    /* The per-case reset, before either server thread starts: the handlers do not reset this
     * state, as the test thread may already be polling it while they run. */
    memset(&g_rl, 0, sizeof g_rl);
    memset(&g_up, 0, sizeof g_up);
    tls_cfg = (KlTlsConfig){ .ctx = NULL, .factory = mock_tls_create };
    KlHttpServerConfig ucfg = { .port = 0, .max_connections = 4, .stream_capacity = RL_CAP };
    KlHttpServerConfig rcfg = { .port = 0, .max_connections = 4, .stream_capacity = RL_CAP,
                                .drain_timeout_ms = drain_ms,
                                .tls = (mode & RL_DOWN_MOCK) ? &tls_cfg : NULL,
                                .alloc = &g_rl_alloc };
#if defined(KEEL_RELAY_MBEDTLS)
    /* Real TLS: fresh contexts per run (a server frees its own through ctx_destroy). */
    g_tls_alloc = kl_allocator_default();
    g_cli_tls.ctx = NULL;
    g_dn_cli_ctx = NULL;
    if (mode & RL_UP_REAL) {
        KlTlsCtx *sctx = rl_server_ctx();
        KlTlsCtx *cctx = kl_tls_mbedtls_client_ctx_create_insecure(&g_tls_alloc);
        if (!sctx || !cctx) return -1;
        g_up_tls = (KlTlsConfig){ .ctx = sctx, .factory = kl_tls_mbedtls_create,
                                  .ctx_destroy = kl_tls_mbedtls_ctx_destroy };
        g_cli_tls = (KlTlsConfig){ .ctx = cctx, .factory = kl_tls_mbedtls_create,
                                   .ctx_destroy = kl_tls_mbedtls_ctx_destroy };
        ucfg.tls = &g_up_tls;
    }
    if (mode & RL_DOWN_REAL) {
        KlTlsCtx *sctx = rl_server_ctx();
        g_dn_cli_ctx = kl_tls_mbedtls_client_ctx_create_insecure(&g_tls_alloc);
        if (!sctx || !g_dn_cli_ctx) return -1;
        g_dn_tls = (KlTlsConfig){ .ctx = sctx, .factory = kl_tls_mbedtls_create,
                                  .ctx_destroy = kl_tls_mbedtls_ctx_destroy };
        rcfg.tls = &g_dn_tls;
    }
#endif
    if (kl_http_server_init(&up_srv, &ucfg) != 0) return -1;
    kl_http_server_route(&up_srv, "GET", "/up", handle_up, NULL, NULL);
    if (kl_http_server_init(&rl_srv, &rcfg) != 0) return -1;
    kl_http_server_route(&rl_srv, "GET", "/relay", handle_relay, NULL, NULL);
    if (arm_ready(&up_srv, &g_up_ready) < 0 || arm_ready(&rl_srv, &g_rl_ready) < 0) return -1;
    kl_plat_thread_create(up_tid, server_thread_fn, &up_srv);
    g_rl_run_done = 0;
    kl_plat_thread_create(rl_tid, rl_thread_fn, &rl_srv);
    if (!wait_ready(&g_up_ready) || !wait_ready(&g_rl_ready)) return -1;
    snprintf(g_up_url, sizeof g_up_url, "%s://127.0.0.1:%d/up",
             (mode & RL_UP_REAL) ? "https" : "http", kl_http_server_bound_port(&up_srv));
    return 0;
}
static void relay_servers_down(KlPlatThread *up_tid, KlPlatThread *rl_tid) {
    kl_http_server_stop(&rl_srv);
    kl_plat_thread_join(rl_tid);
    kl_http_server_free(&rl_srv);
    kl_http_server_stop(&up_srv);
    kl_plat_thread_join(up_tid);
    kl_http_server_free(&up_srv);
#if defined(KEEL_RELAY_MBEDTLS)
    if (g_cli_tls.ctx) kl_tls_mbedtls_ctx_destroy(g_cli_tls.ctx);   /* clients' contexts are ours */
    if (g_dn_cli_ctx) kl_tls_mbedtls_ctx_destroy(g_dn_cli_ctx);
    g_cli_tls.ctx = NULL;
    g_dn_cli_ctx = NULL;
#endif
}

/* The downstream reader's mid-transfer stop: once every buffer on the way is full the upstream
 * producer must stop too, so its progress is sampled twice, well into the stop. A kernel may
 * still grow a loopback socket buffer meanwhile (macOS autosizing), so a little movement is
 * allowed; free-running it would move megabytes in that time. */
static size_t g_up_at_a, g_up_at_b;
static void rl_stall_and_sample(void) {
    kl_test_sleep_ms(1000);
    g_up_at_a = g_up.sent;
    kl_test_sleep_ms(700);
    g_up_at_b = g_up.sent;
}

#if defined(KEEL_RELAY_MBEDTLS)
/* Real TLS downstream: Keel's sync client reads the relay over HTTPS. The body arrives
 * de-chunked; each byte is checked, and the callback stalls once (the client then reads nothing). */
static int dn_on_body(const char *data, size_t len, void *ud) {
    Dechunk *d = ud;
    if (d->payload < RL_TOTAL / 4 && d->payload + len >= RL_TOTAL / 4) rl_stall_and_sample();
    for (size_t k = 0; k < len; k++)
        if ((unsigned char)data[k] != rl_byte(d->payload + k) && d->mismatch_at == (size_t)-1)
            d->mismatch_at = d->payload + k;
    d->payload += len;
    return KL_HTTP_CLIENT_BODY_CONTINUE;
}
static void dn_on_complete(void *ud) { Dechunk *d = ud; d->ended = 1; d->state = 6; }
static int dn_read_tls(Dechunk *d) {
    KlAllocator a = kl_allocator_default();
    KlTlsConfig ct = { .ctx = g_dn_cli_ctx, .factory = kl_tls_mbedtls_create };
    KlHttpClientConfig cfg = { .timeout_ms = 120000, .max_response_size = (size_t)RL_TOTAL * 2,
                               .tls = &ct };
    KlHttpClientStreamCfg sc = { .on_body = dn_on_body, .on_complete = dn_on_complete, .user_data = d };
    char url[64];
    snprintf(url, sizeof url, "https://127.0.0.1:%d/relay", kl_http_server_bound_port(&rl_srv));
    KlHttpClientResponse resp;
    memset(&resp, 0, sizeof resp);
    int rc = kl_http_client_request_s(&a, &cfg, "GET", url, NULL, 0, NULL, 0, &sc, &resp);
    kl_http_client_response_free(&resp);
    return rc;
}
#endif

static void relay_case(int *utest_result, int mode) {
    KlPlatThread up_tid, rl_tid;
    ASSERT_EQ(relay_servers_up(&up_tid, &rl_tid, mode, 0), 0);
    Dechunk d = { .mismatch_at = (size_t)-1 };
    size_t rss0 = peak_rss();
    kl_test_sleep_ms(20);
    size_t base = g_ca.live;                        /* the relay server, idle: pool, buffers */
    g_ca.peak = g_ca.live;
    g_up_at_a = g_up_at_b = 0;
    int fd = -1;
#if defined(KEEL_RELAY_MBEDTLS)
    if (mode & RL_DOWN_REAL) fd = dn_read_tls(&d) == 0 ? 0 : -1;
    else
#endif
    fd = connect_rcvbuf(kl_http_server_bound_port(&rl_srv), 16384);
    if (fd > 0) {
        const char *rq = "GET /relay HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
        (void)kl_test_sockwrite(fd, rq, strlen(rq));
        static unsigned char buf[64 * 1024];
        int stalled = 0;
        uint64_t start = kl_monotonic_ms();
        while (kl_monotonic_ms() - start < 60000 && d.state >= 0 && d.state != 6) {
            if (!stalled && d.payload >= RL_TOTAL / 4) {
                stalled = 1;                         /* stop reading mid-transfer */
                rl_stall_and_sample();
            }
            if (kl_test_poll1(fd, 0, 500) <= 0) continue;
            kl_ssize_t n = kl_test_sockread(fd, buf, sizeof buf);
            if (n <= 0) break;
            dc_feed(&d, buf, (size_t)n);
        }
        kl_test_closesock(fd);
    }
    size_t rss_growth = peak_rss() - rss0;
    size_t alloc_growth = g_ca.peak - base;         /* what the transfer made the server hold */
    size_t up_at_a = g_up_at_a, up_at_b = g_up_at_b;
    for (int i = 0; i < 200 && !g_rl.completed && !g_rl.cancelled; i++) kl_test_sleep_ms(10);
    relay_servers_down(&up_tid, &rl_tid);
    static const char *names[8] = { "plain", "mock tls down", "real tls up", "?", "real tls down",
                                    "?", "real tls both", "?" };
    printf("  relay (%s): %zu payload bytes, ended %d, mismatch %s, %d pauses, %d resumes, "
           "%d edges, max pending %zu of %u (upstream %zu), upstream stalled %zu -> %zu "
           "(%d would-blocks), relay alloc peak +%zu KiB, held %zu, peak RSS +%zu KiB\n",
           names[mode & 7], d.payload, d.ended,
           d.mismatch_at == (size_t)-1 ? "none" : "YES", g_rl.pauses, g_rl.resumes, g_rl.edges,
           g_rl.max_pending, RL_CAP, g_up.max_pending, up_at_a, up_at_b, g_up.would_blocks,
           alloc_growth / 1024, g_rl.max_held, rss_growth / 1024);
    ASSERT_TRUE(fd >= 0);
    ASSERT_EQ(d.state, 6);                          /* well-formed chunked body, ended */
    ASSERT_EQ(d.payload, (size_t)RL_TOTAL);         /* every byte ... */
    ASSERT_EQ(d.mismatch_at, (size_t)-1);           /* ... exact and in order */
    ASSERT_EQ((int)g_rl.failed, 0);
    ASSERT_EQ(g_rl.upstream_err, 0);
    ASSERT_EQ(g_rl.cancelled, 0);
    ASSERT_TRUE(g_rl.completed);
    ASSERT_GT(g_rl.pauses, 0);                      /* the relay met its bound and paused upstream */
    ASSERT_EQ(g_rl.resumes, g_rl.pauses);           /* every pause was resumed by an edge */
    ASSERT_EQ(g_rl.resume_failed, 0);
    ASSERT_LE(g_rl.max_pending, (size_t)RL_CAP);    /* one bound on the relay's response ... */
    ASSERT_LE(g_rl.max_held, (size_t)RL_CAP + 256u); /* ... also as held (ciphertext with TLS) */
    /* Upstream too. With real TLS its queue holds ciphertext while admission counts plaintext, so
     * pending may pass the capacity by the record overhead of the last admitted chunk. */
    ASSERT_LE(g_up.max_pending, (size_t)RL_CAP + 256u);
    ASSERT_EQ(g_rl.edges_after_end, 0);
    ASSERT_GT(up_at_a, (size_t)0);
    ASSERT_LT(up_at_b - up_at_a, (size_t)(1024u * 1024u));   /* upstream stopped during the stop */
    ASSERT_LT(up_at_b, (size_t)RL_TOTAL);
    ASSERT_GT(g_up.would_blocks, 0);                /* by its own bound, not by luck */
    /* Memory, against the documented bound M for capacity C (docs/contracts/streaming.md):
     * outbound buffer C+5, output queue under 4(C + 5 + O + F) for the TLS record overhead O and the
     * 16 KiB flush chunk F, the backend's send copy min(256 KiB, C + O), and the response headers.
     * The counting allocator also holds the relay server's connection pool, which M does not cover,
     * so its baseline is not added; what the response already held before the transfer is its
     * header buffer, which is added: the response's total is growth + header buffer, and every
     * other term of M starts at zero on this fresh connection. Process RSS is printed only: it is a
     * process-wide high-water mark (later cases show about 0) and sanitizer builds inflate it. */
    {
        const size_t C = RL_CAP, F = 16384, O = 128, B = 16384;   /* O: 3 records of overhead */
        const size_t send_copy = C + O < (size_t)256 * 1024 ? C + O : (size_t)256 * 1024;
        const size_t bound = (C + 5) + 4 * (C + 5 + O + F) + send_copy + B;
        ASSERT_LE(alloc_growth + g_rl.hdr_cap, bound);
    }
}
UTEST(stream_relay, a_relay_is_bounded_end_to_end) { relay_case(utest_result, 0); }
UTEST(stream_relay, a_tls_relay_is_bounded_end_to_end) { relay_case(utest_result, RL_DOWN_MOCK); }
#if defined(KEEL_RELAY_MBEDTLS)
UTEST(stream_relay, a_relay_over_real_tls_upstream_is_bounded) { relay_case(utest_result, RL_UP_REAL); }
UTEST(stream_relay, a_relay_over_real_tls_downstream_is_bounded) { relay_case(utest_result, RL_DOWN_REAL); }
UTEST(stream_relay, a_relay_over_real_tls_both_ways_is_bounded) {
    relay_case(utest_result, RL_UP_REAL | RL_DOWN_REAL);
}
#endif

/* The downstream client leaves while upstream is paused: the relay op is cancelled once, its client
 * is cancelled from on_cancel, and the upstream server then sees its client go: its op ends too. */
UTEST(stream_relay, a_downstream_client_that_leaves_while_upstream_is_paused_ends_both) {
    KlPlatThread up_tid, rl_tid;
    ASSERT_EQ(relay_servers_up(&up_tid, &rl_tid, 0, 0), 0);
    int fd = connect_rcvbuf(kl_http_server_bound_port(&rl_srv), 4096);
    size_t got = 0;
    if (fd >= 0) {
        const char *rq = "GET /relay HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
        (void)kl_test_sockwrite(fd, rq, strlen(rq));
        static char buf[64 * 1024];
        while (got < 256 * 1024) {
            if (kl_test_poll1(fd, 0, 2000) <= 0) break;
            kl_ssize_t n = kl_test_sockread(fd, buf, sizeof buf);
            if (n <= 0) break;
            got += (size_t)n;
        }
        for (int i = 0; i < 200 && g_rl.pauses == 0; i++) kl_test_sleep_ms(10);   /* paused */
        kl_test_sleep_ms(100);
        kl_test_closesock(fd);                       /* unread data: the close resets */
        for (int i = 0; i < 500 && (!g_rl.cancelled || !g_up.cancelled); i++) kl_test_sleep_ms(10);
        kl_test_sleep_ms(100);
    }
    int rl_cancelled = g_rl.cancelled, up_cancelled = g_up.cancelled, pauses = g_rl.pauses;
    int after = g_rl.edges_after_end, cli_left = g_rl.cli != NULL;
    relay_servers_down(&up_tid, &rl_tid);
    ASSERT_TRUE(got > 0);
    ASSERT_GT(pauses, 0);
    ASSERT_EQ(rl_cancelled, 1);
    ASSERT_EQ(after, 0);
    ASSERT_FALSE(cli_left);                          /* the upstream client was cancelled, freed */
    ASSERT_EQ(up_cancelled, 1);                      /* and the upstream op ended with it */
    ASSERT_FALSE(g_rl.completed);
}

/* A server stop while the relay is paused ends it cleanly: the relay op is cancelled once and its
 * client freed, with nothing left behind (sanitizer builds check the rest). */
UTEST(stream_relay, a_server_stop_while_upstream_is_paused_ends_the_relay) {
    KlPlatThread up_tid, rl_tid;
    ASSERT_EQ(relay_servers_up(&up_tid, &rl_tid, 0, 0), 0);
    int fd = connect_rcvbuf(kl_http_server_bound_port(&rl_srv), 4096);
    if (fd >= 0) {
        const char *rq = "GET /relay HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
        (void)kl_test_sockwrite(fd, rq, strlen(rq));
        for (int i = 0; i < 300 && g_rl.pauses == 0; i++) kl_test_sleep_ms(10);   /* not reading */
    }
    int pauses = g_rl.pauses;
    relay_servers_down(&up_tid, &rl_tid);           /* the relay server stops first */
    if (fd >= 0) kl_test_closesock(fd);
    ASSERT_TRUE(fd >= 0);
    ASSERT_GT(pauses, 0);
    ASSERT_EQ(g_rl.cancelled, 1);
    ASSERT_TRUE(g_rl.cli == NULL);
    ASSERT_FALSE(g_rl.completed);
}

/* ── A graceful drain over a backpressured relay ──────────────────────────────────────────────────
 * The relay server is stopped with a drain while its response is paused on a reader that stopped
 * reading. The listener closes at once (a new connect is refused) and the suspended relay holds the
 * drain. Then either the reader resumes, and the body completes byte-exact and the run returns by
 * itself before the deadline; or it never does, and the run returns at the deadline and the server's
 * free cancels the relay once. */

/* Request the relay, read `first` payload bytes, then stop reading until the relay has paused on its
 * bound and its upstream has stopped on its own. Returns the socket, or -1. */
static int rl_read_then_stall(Dechunk *d, size_t first) {
    int fd = connect_rcvbuf(kl_http_server_bound_port(&rl_srv), 16384);
    if (fd < 0) return -1;
    const char *rq = "GET /relay HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    (void)kl_test_sockwrite(fd, rq, strlen(rq));
    static unsigned char buf[64 * 1024];
    uint64_t t0 = kl_monotonic_ms();
    while (d->payload < first && d->state >= 0 && kl_monotonic_ms() - t0 < 30000) {
        if (kl_test_poll1(fd, 0, 500) <= 0) continue;
        kl_ssize_t n = kl_test_sockread(fd, buf, sizeof buf);
        if (n <= 0) break;
        dc_feed(d, buf, (size_t)n);
    }
    for (int i = 0; i < 300 && (g_rl.pauses == 0 || g_up.would_blocks == 0); i++) kl_test_sleep_ms(10);
    kl_test_sleep_ms(200);
    return fd;
}

/* Read until the connection ends. 1 when it ended (EOF or reset), 0 when `timeout_ms` passed. */
static int rl_read_to_end(int fd, Dechunk *d, int timeout_ms) {
    static unsigned char buf[64 * 1024];
    uint64_t t0 = kl_monotonic_ms();
    while (kl_monotonic_ms() - t0 < (uint64_t)timeout_ms) {
        if (kl_test_poll1(fd, 0, 500) <= 0) continue;
        kl_ssize_t n = kl_test_sockread(fd, buf, sizeof buf);
        if (n <= 0) return 1;
        dc_feed(d, buf, (size_t)n);
    }
    return 0;
}

UTEST(stream_relay, a_drain_while_backpressured_completes_once_the_reader_resumes) {
    KlPlatThread up_tid, rl_tid;
    ASSERT_EQ(relay_servers_up(&up_tid, &rl_tid, 0, 30000), 0);
    Dechunk d = { .mismatch_at = (size_t)-1 };
    int fd = rl_read_then_stall(&d, RL_TOTAL / 8);
    int pauses_at_stop = g_rl.pauses, up_blocked = g_up.would_blocks;

    uint64_t t_stop = kl_monotonic_ms();
    kl_http_server_stop(&rl_srv);
    kl_test_sleep_ms(150);                                   /* a loop tick has run */
    int draining = kl_http_server_draining(&rl_srv);
    int late = connect_rcvbuf(kl_http_server_bound_port(&rl_srv), 0);         /* refused: no new connections */
    if (late >= 0) kl_test_closesock(late);
    kl_test_sleep_ms(300);
    int held = !g_rl_run_done && !g_rl.cancelled && !g_rl.completed;   /* the relay holds the drain */

    int ended = fd >= 0 ? rl_read_to_end(fd, &d, 60000) : 0;  /* the reader resumes */
    if (fd >= 0) kl_test_closesock(fd);
    for (int i = 0; i < 500 && !g_rl_run_done; i++) kl_test_sleep_ms(10);
    int run_done = g_rl_run_done;
    uint64_t run_ms = run_done ? g_rl_run_done_ms - t_stop : 0;
    kl_test_sleep_ms(100);                                   /* nothing may fire after the end */
    int completed = g_rl.completed, cancelled = g_rl.cancelled, after = g_rl.edges_after_end;
    int cli_left = g_rl.cli != NULL, up_ended = g_up.ended, up_cancelled = g_up.cancelled;
    relay_servers_down(&up_tid, &rl_tid);
    printf("  drain, reader resumes: %zu payload bytes, %d pauses, %d resumes, run returned %llu ms "
           "after the stop\n", d.payload, g_rl.pauses, g_rl.resumes, (unsigned long long)run_ms);

    ASSERT_TRUE(fd >= 0);
    ASSERT_GT(pauses_at_stop, 0);                            /* backpressured at the stop ... */
    ASSERT_GT(up_blocked, 0);                                /* ... all the way upstream */
    EXPECT_EQ(draining, 1);
    EXPECT_TRUE(late < 0);
    EXPECT_TRUE(held);
    EXPECT_TRUE(ended);                                      /* the connection closed after the body */
    ASSERT_EQ(d.state, 6);                                   /* well-formed chunked body, ended */
    ASSERT_EQ(d.payload, (size_t)RL_TOTAL);                  /* every byte ... */
    ASSERT_EQ(d.mismatch_at, (size_t)-1);                    /* ... exact and in order */
    EXPECT_EQ(completed, 1);                                 /* completed once ... */
    EXPECT_EQ(cancelled, 0);                                 /* ... and never cancelled */
    EXPECT_EQ((int)g_rl.failed, 0);
    EXPECT_EQ(g_rl.upstream_err, 0);
    EXPECT_EQ(g_rl.resumes, g_rl.pauses);
    EXPECT_EQ(g_rl.resume_failed, 0);
    EXPECT_EQ(after, 0);
    EXPECT_FALSE(cli_left);                                  /* the upstream client finished, freed */
    EXPECT_EQ(up_ended, 1);
    EXPECT_EQ(up_cancelled, 0);
    EXPECT_TRUE(run_done);                                   /* the run returned by itself ... */
    EXPECT_LT(run_ms, (uint64_t)30000);                      /* ... before the deadline */
}

UTEST(stream_relay, a_drain_past_its_deadline_ends_the_backpressured_relay_cleanly) {
    size_t live0 = g_ca.live;                                /* the relay server allocates through g_ca */
    KlPlatThread up_tid, rl_tid;
    ASSERT_EQ(relay_servers_up(&up_tid, &rl_tid, 0, 600), 0);
    Dechunk d = { .mismatch_at = (size_t)-1 };
    int fd = rl_read_then_stall(&d, RL_TOTAL / 8);
    int pauses_at_stop = g_rl.pauses;

    uint64_t t_stop = kl_monotonic_ms();
    kl_http_server_stop(&rl_srv);                            /* the reader never resumes */
    for (int i = 0; i < 1000 && !g_rl_run_done; i++) kl_test_sleep_ms(10);
    int run_done = g_rl_run_done;
    uint64_t run_ms = run_done ? g_rl_run_done_ms - t_stop : 0;
    int completed_in_run = g_rl.completed;
    relay_servers_down(&up_tid, &rl_tid);                    /* the free cancels what remains */
    int ended = fd >= 0 ? rl_read_to_end(fd, &d, 5000) : 0;
    if (fd >= 0) kl_test_closesock(fd);
    kl_test_sleep_ms(100);
    size_t live_after = g_ca.live;
    printf("  drain past its deadline: %zu payload bytes, %d pauses, run returned %llu ms after the "
           "stop, relay server live allocation %zu -> %zu\n", d.payload, pauses_at_stop,
           (unsigned long long)run_ms, live0, live_after);

    ASSERT_TRUE(fd >= 0);
    ASSERT_GT(pauses_at_stop, 0);
    EXPECT_TRUE(run_done);                                   /* the run returned at the deadline */
    EXPECT_GE(run_ms, (uint64_t)500);
    EXPECT_LT(run_ms, (uint64_t)5000);
    EXPECT_EQ(completed_in_run, 0);
    EXPECT_EQ(g_rl.cancelled, 1);                            /* cancelled exactly once ... */
    EXPECT_EQ(g_rl.completed, 0);                            /* ... and never completed */
    EXPECT_EQ(g_rl.edges_after_end, 0);                      /* no edge after the end */
    EXPECT_TRUE(g_rl.cli == NULL);                           /* its upstream client released */
    EXPECT_EQ(g_up.cancelled, 1);                            /* and the upstream op ended with it */
    EXPECT_EQ(g_up.ended, 0);
    EXPECT_TRUE(ended);                                      /* the reader sees the connection end ... */
    EXPECT_NE(d.state, 6);                                   /* ... without a body end it never got */
    EXPECT_LT(d.payload, (size_t)RL_TOTAL);
    EXPECT_EQ(d.mismatch_at, (size_t)-1);                    /* what did arrive was exact */
    EXPECT_EQ(live_after, live0);                            /* everything the server allocated returned */
}

UTEST_MAIN();
