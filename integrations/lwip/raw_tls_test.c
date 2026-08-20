/*
 * raw_tls_test.c — LC-4: HTTPS (TLS) over the lwIP-raw completion backend, in-process over the
 * loopback netif (NO_SYS=1, single-thread).
 *
 * The proof for LC-4 (docs/phase10_lwip_raw_client_design.md §6 + §8 LC-4): the SAME single-loop
 * model as LC-1 (ONE KlEventCtx == the one lwIP mainloop, driving BOTH a raw KlServer and a raw
 * async KlClient on the loop thread), now with TLS on BOTH ends — a genuine mbedTLS handshake +
 * request round-trips over 127.0.0.1 on the loopif with zero lwIP-specific TLS code.
 *
 * TLS over raw rides two EXISTING, generic legs (no new TLS code in the lwIP backend):
 *
 *   1. Client side = the mbedTLS socket-BIO through the CONFIGURED socket provider. tls_mbedtls.c's
 *      bio_send/bio_recv call kl_sock_send/kl_sock_recv(t->sp, t->fd, ...) — the provider wired via
 *      set_socket_provider, which src/client.c sets to ev_ctx->sockets. On raw that is
 *      kl_socket_provider_lwip_raw(), so bio_send/recv -> lwr_sock_send/recv -> tcp_write/read over
 *      the pcb. The WANT_READ/WANT_WRITE handshake churn rides the raw backend's emulated readiness
 *      watcher (writable = sndbuf headroom; readable = retained rx / peer close).
 *
 *   2. Server side = the generic memory-BIO completion-TLS leg. src/completion_driver.c's
 *      comp_tls_drive + comp_tls_drain_output drive the abstract KlTls feed_input/drain_output
 *      vtable with zero mbedTLS knowledge: the raw backend delivers ciphertext via KL_COMP_READ, the
 *      driver feed_input's it, tls->read returns plaintext, drain_output ciphertext is posted via
 *      the send path. The raw KlServer already uses completion_driver.c, so server TLS "just moves
 *      bytes".
 *
 * Case: a raw HTTPS KlClient GETs https://127.0.0.1:PORT/ from a raw TLS KlServer -> handshake +
 * 200 + body BYTE-EXACT. Embedded self-signed EC cert (CN=127.0.0.1); the client skips CA
 * verification. Same test-only cert material as tests/smoke_tls.c / lwip_loopback_test.c.
 *
 * Buffered HTTP/1.1 over TLS only (the 8b-5 subset, §6 caveat): small buffered body, no
 * file/stream body, no ALPN-h2.
 *
 * Must be ASan+UBSan+LSan-clean.
 *
 * SPDX-License-Identifier: MIT
 */
#include <keel/keel.h>
#include <keel/event_ctx.h>
#include <keel/allocator.h>
#include <keel/resolver.h>
#include <keel/sockaddr.h>
#include <keel/timer.h>

#include "keel_lwip_raw.h"
#include <keel_tls_mbedtls.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ── numeric-only KlResolver (no DNS, no UDP — same as LC-1) ────────────────────
 * Parses a numeric IPv4/IPv6 literal via kl_sockaddr_parse and completes SYNCHRONOUSLY inside
 * resolve() — the resolver sync-completion contract (CLAUDE.md). No UDP, no timers, no lwIP: the
 * built-in kl_dns_resolver is unusable on raw (it eagerly kl_datagram_socket_init's, which lwip-raw rejects). */
typedef struct { KlResolver base; KlResolveReq req; } NumResolver;

static KlResolveReq *num_resolve(KlResolver *self, KlEventCtx *ctx, const char *host, int port,
                                 KlResolveDoneFn done_fn, void *ud) {
    (void)ctx;
    NumResolver *nr = (NumResolver *)self;
    KlResolveResult res;
    memset(&res, 0, sizeof(res));
    if (kl_sockaddr_parse(&res.addrs[0], host, (uint16_t)port) != 0) {
        done_fn(&nr->req, NULL, -1, ud);   /* not a numeric literal — synchronous failure */
        return &nr->req;
    }
    res.naddrs = 1;
    done_fn(&nr->req, &res, 0, ud);         /* synchronous success */
    return &nr->req;
}
static void num_cancel(KlResolveReq *req) { (void)req; }
static void num_destroy(KlResolver *self) { (void)self; }

static NumResolver g_numres = {
    { num_resolve, num_cancel, num_destroy }, { NULL }
};

/* ── test-only self-signed EC cert (CN=127.0.0.1) — same material as tests/smoke_tls.c ── */
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

/* ── server handler: a known, non-trivial body (byte-exactness proof) ────────── */
#define WANT_BODY "{\"lc4\":true,\"raw\":\"tls\",\"loop\":\"lwip\",\"n\":424242}"

static void handle_root(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)req; (void)ud;
    kl_http_response_status(res, 200);
    kl_http_response_header(res, "Content-Type", "application/json");
    kl_http_response_body_copy(res, WANT_BODY, sizeof(WANT_BODY) - 1);   /* COPY: survives async send */
}

/* ── client completion state (all fields touched only on the loop thread) ─────── */
typedef struct {
    KlServer   *srv;
    const char *url;
    KlTlsCtx   *client_ctx;   /* client TLS ctx — created on the loop thread, freed after done */
    KlClient   *client;
    atomic_int  done;
    atomic_int  pass;
} ClientCase;

static void on_done(KlClient *client, void *ud) {
    ClientCase *cc = ud;
    int err = kl_client_error(client);
    int ok = 0;
    if (err == 0) {
        const KlClientResponse *r = kl_client_response(client);
        ok = (r && r->status == 200 && r->body_len == sizeof(WANT_BODY) - 1 &&
              r->body && memcmp(r->body, WANT_BODY, sizeof(WANT_BODY) - 1) == 0);
        if (!ok && r) printf("   [diag] status=%d body_len=%zu\n", r->status, r->body_len);
    } else {
        printf("   [diag] unexpected client error=%d last_error=%d\n",
               err, (int)kl_client_last_error(client));
    }
    atomic_store(&cc->pass, ok);
    atomic_store(&cc->done, 1);
    kl_server_stop(cc->srv);
}

/* Fired on the loop thread: start the async HTTPS KlClient on the server's shared ctx (== the one
 * lwIP mainloop). The mbedTLS socket-BIO is auto-wired to the loop's socket provider
 * (kl_socket_provider_lwip_raw) via the KlTls.set_socket_provider hook — no explicit call. */
static void start_client(void *ud) {
    ClientCase *cc = ud;
    static KlAllocator alloc;   /* stable storage: the response stores the allocator by value */
    alloc = kl_allocator_default();

    /* Client TLS ctx: NULL CA = skip verification (test self-signed cert). */
    cc->client_ctx = kl_tls_mbedtls_client_ctx_create(NULL, &alloc);
    if (!cc->client_ctx) {
        atomic_store(&cc->pass, 0);
        atomic_store(&cc->done, 1);
        kl_server_stop(cc->srv);
        return;
    }
    static KlTlsConfig ctls;   /* stable storage: referenced by the client for its lifetime */
    ctls = (KlTlsConfig){ .ctx = cc->client_ctx, .factory = kl_tls_mbedtls_create };

    KlClientConfig ccfg = { .timeout_ms = 10000, .resolver = &g_numres.base, .tls = &ctls };
    cc->client = kl_client_start(&cc->srv->ev, &alloc, &ccfg, "GET", cc->url,
                                 NULL, 0, NULL, 0, on_done, cc);
    if (!cc->client) {
        atomic_store(&cc->pass, 0);
        atomic_store(&cc->done, 1);
        kl_server_stop(cc->srv);
    }
}

static void *server_thread(void *a) { kl_server_run((KlServer *)a); return NULL; }

int main(void) {
    KlAllocator alloc = kl_allocator_default();

    /* Server TLS ctx: cert + key from buffers (mTLS off). */
    KlTlsCtx *sctx = kl_tls_mbedtls_ctx_create_from_buf(
        (const unsigned char *)CERT_PEM, sizeof(CERT_PEM),
        (const unsigned char *)KEY_PEM,  sizeof(KEY_PEM),
        NULL, 0, KL_MTLS_NONE, &alloc);
    if (!sctx) { printf("LC-4 FAIL: server tls ctx create\n"); return 1; }
    KlTlsConfig stls = { .ctx = sctx, .factory = kl_tls_mbedtls_create };  /* destroy manually */

    KlConfig cfg = { .port = 7860, .bind_addr = "127.0.0.1",
                     .event_provider = kl_event_provider_lwip_raw(),
                     .tls = &stls };
    KlServer srv;
    if (kl_server_init(&srv, &cfg) != 0) {
        printf("LC-4 FAIL: server_init\n");
        kl_tls_mbedtls_ctx_destroy(sctx);
        return 1;
    }
    kl_server_route(&srv, "GET", "/", handle_root, NULL, NULL);

    ClientCase cc;
    memset(&cc, 0, sizeof(cc));
    cc.srv = &srv;
    cc.url = "https://127.0.0.1:7860/";
    atomic_store(&cc.done, 0);
    atomic_store(&cc.pass, 0);

    /* Marshal the client start onto the loop thread (single-thread NO_SYS=1 discipline): the timer
     * fires inside kl_server_run's tick, so the client is created + driven on the loop thread. */
    kl_timer_add(&srv.ev, 20, start_client, &cc);

    /* Run the shared loop (server + client) on a bg thread; the main thread is a bounded watchdog
     * that stops the loop if the case hangs (a TLS handshake stall is a FAIL, never a wedge). */
    pthread_t srv_th;
    if (pthread_create(&srv_th, NULL, server_thread, &srv) != 0) {
        printf("LC-4 FAIL: pthread_create\n");
        kl_server_free(&srv);
        kl_tls_mbedtls_ctx_destroy(sctx);
        return 1;
    }
    /* Generous watchdog: a TLS handshake is many WANT_READ/WANT_WRITE round-trips. 10 s. */
    for (int i = 0; i < 1000 && !atomic_load(&cc.done); i++) {
        struct timespec sl = { 0, 10 * 1000000L };
        nanosleep(&sl, NULL);
    }
    if (!atomic_load(&cc.done)) { kl_server_stop(&srv); }   /* hang guard */
    pthread_join(srv_th, NULL);

    int rc = 0;
    if (!atomic_load(&cc.done)) { printf("LC-4 FAIL: HANG (handshake stalled)\n"); rc = 1; }
    else if (!atomic_load(&cc.pass)) { printf("LC-4 FAIL\n"); rc = 1; }
    else { printf("PASS LC-4 (HTTPS GET / -> 200, body byte-exact over raw)\n"); }

    if (cc.client) kl_client_free(cc.client);
    if (cc.client_ctx) kl_tls_mbedtls_ctx_destroy(cc.client_ctx);
    kl_server_free(&srv);
    kl_tls_mbedtls_ctx_destroy(sctx);

    if (rc == 0) printf("LC-4 PASS\n");
    return rc;
}
