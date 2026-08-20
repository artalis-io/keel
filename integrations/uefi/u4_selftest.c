/*
 * u4_selftest.c — U-4 acceptance self-test (UEFI EFI application).
 *
 * The TLS culmination of Phase 10: a STOCK libkeel_freestanding.a async KlHttpClient
 * performs an HTTPS GET over the EFI_TCP4 completion backend on bare UEFI firmware.
 * TLS is the mbedTLS adapter (integrations/mbedtls/tls_mbedtls.c) built FREESTANDING
 * for the EFI target. mbedTLS's ciphertext BIO routes through the U-2 EFI socket
 * provider (kl_http_client_start sets ev_ctx->sockets = the EFI native provider, and
 * src/client_async.c auto-wires it into the TLS session via set_socket_provider).
 *
 * Flow (all through the PUBLIC KlHttpClient API — the client is model-blind):
 *   kl_uefi_platform_init(bs, st)            → monotonic clock + EFI_RNG
 *   kl_uefi_mbedtls_platform_init(bs)        → EFI heap + entropy for mbedTLS
 *   kl_uefi_event_provider(bs, image)        → completion provider (+ socket provider)
 *   kl_event_ctx_init_ex(&ev, alloc, ep)     → inject the provider on the stock archive
 *   ctx = kl_tls_mbedtls_client_ctx_create_from_buf(NULL,0,&alloc)  → verify-none TLS ctx
 *   kl_http_client_start(GET https://10.0.2.2:PORT/, cfg.tls=&tls)       → handshake + GET
 *   pump: kl_event_ctx_run + kl_timer_fire until done or bounded ticks
 *
 * Prints:
 *   U-4: status = <n>
 *   U-4: GO            (done && status == 200)   |   U-4: NO-GO-YET
 *
 * *** SPIKE SHORTCUTS (loud, deliberate, NOT production-safe): ***
 *   1. VERIFY-NONE — NULL CA => MBEDTLS_SSL_VERIFY_NONE. The self-signed test
 *      server's certificate is NOT validated. This proves the TLS TRANSPORT, not
 *      trust. A real client MUST pass a CA bundle.
 *   2. NO CERT-TIME CHECK — the freestanding mbedTLS config has HAVE_TIME off, so
 *      cert not-before/not-after are not checked either (moot under verify-none).
 *   3. WEAK ENTROPY FALLBACK — if EFI_RNG is absent, mbedtls_hardware_poll uses a
 *      weak counter+pointer seed (see mbedtls_platform_uefi.c). We warn below.
 *
 * Freestanding: clang --target=x86_64-unknown-windows, -nostdlib, lld PE. No libc.
 */

#include "efi_uefi.h"
#include "platform_uefi.h"
#include "mbedtls_platform_uefi.h"
#include "socket_efi_tcp4.h"
#include "event_efi.h"
#include "allocator_uefi.h"

#include <keel_tls_mbedtls.h>     /* client-ctx create + factory + destroy */

#include <keel/http_client.h>
#include <keel/tls.h>
#include <keel/event_ctx.h>
#include <keel/timer.h>
#include <keel/error.h>

#include <stdint.h>
#include <stddef.h>

#ifndef TARGET_PORT
#define TARGET_PORT 18443
#endif
#ifndef TARGET_HOST
#define TARGET_HOST "10.0.2.2"      /* SLIRP host/gateway from the guest */
#endif

/* Build the HTTPS URL literal at compile time. */
#define STR2(x) #x
#define STR(x) STR2(x)

#ifdef KL_U4_PROD
/* Production TLS mode: CA-verified server cert (dNSName SAN) + real EFI_RNG. The
 * client dials a HOSTNAME (resolved to the responder IP by resolve_uefi.c's static
 * hosts entry), so mbedTLS verifies the cert against the dNSName SAN. */
#include "u4_ca_embed.h"          /* g_u4_ca_pem[] + g_u4_ca_pem_len (generated) */
#ifndef KL_U4_PROD_HOST
#define KL_U4_PROD_HOST "server.keel.test"
#endif
#define TARGET_URL "https://" KL_U4_PROD_HOST ":" STR(TARGET_PORT) "/"
#else
#define TARGET_URL "https://" TARGET_HOST ":" STR(TARGET_PORT) "/"
#endif

/* ── tiny ASCII console (no libc) ─────────────────────────────────────────────── */
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *g_out;

static void print(const char *s) {
    CHAR16 buf[256];
    UINTN i = 0;
    while (s[i] && i < 254) { buf[i] = (CHAR16)(UINT8)s[i]; i++; }
    buf[i] = 0;
    if (g_out) g_out->OutputString(g_out, buf);
}
static void print_line(const char *s) { print(s); print("\r\n"); }

static void write_bytes(const char *b, UINTN n) {
    CHAR16 w[2]; w[1] = 0;
    for (UINTN i = 0; i < n; i++) {
        char c = b[i];
        if (c == '\r') continue;
        if (c == '\n') { CHAR16 cr[2] = { (CHAR16)'\r', 0 }; if (g_out) g_out->OutputString(g_out, cr); }
        w[0] = (CHAR16)(UINT8)c;
        if (g_out) g_out->OutputString(g_out, w);
    }
}

static void print_int(int v) {
    char tmp[16]; int n = 0;
    unsigned u = (v < 0) ? (unsigned)(-v) : (unsigned)v;
    if (v < 0) print("-");
    if (u == 0) { print("0"); return; }
    while (u && n < (int)sizeof(tmp)) { tmp[n++] = (char)('0' + (u % 10)); u /= 10; }
    char out[17]; int o = 0;
    while (n) out[o++] = tmp[--n];
    out[o] = 0;
    print(out);
}

#ifdef U4_MANUAL_DIAG
/* Manual TLS drive: bypass kl_client and step the KlTls vtable directly over the EFI
 * socket provider, printing every handshake/write/read result + the provider io_status.
 * Pinpoints whether the KL_ERR_IO is in the request write or the response read. */
#include <keel/socket.h>
#include <keel/sockaddr.h>
int kl_uefi_socket_connect_now(KlSocketHandle fd);   /* U-2: issue+pump the Connect token */

static void manual_diag(EFI_BOOT_SERVICES *bs, EFI_HANDLE image,
                        KlTlsCtx *tctx, KlAllocator *alloc) {
    const KlSocketProvider *p = kl_uefi_socket_provider(bs, image);
    if (!p) { print_line("DIAG: no socket provider"); return; }
    const KlSocketOps *ops = p->ops; void *cx = p->context;

    KlSocketHandle fd = ops->socket(cx, 2 /*AF_INET*/, 1 /*STREAM*/, 0);
    if (!kl_handle_valid(fd)) { print_line("DIAG: socket() failed"); return; }
    KlSockAddr remote; { uint8_t ip[4] = {10,0,2,2}; kl_sockaddr_from_ipv4(&remote, ip, TARGET_PORT); }
    (void)ops->connect(cx, fd, &remote);            /* Configure (returns pending) */
    if (kl_uefi_socket_connect_now(fd) != 0) { print_line("DIAG: connect_now failed"); ops->close(cx, fd); return; }
    print_line("DIAG: TCP connected");

    KlTls *tls = kl_tls_mbedtls_create(tctx, alloc);
    if (!tls) { print_line("DIAG: tls create failed"); ops->close(cx, fd); return; }
    if (tls->set_socket_provider) tls->set_socket_provider(tls, p);
    if (tls->set_hostname) tls->set_hostname(tls, TARGET_HOST);

    int hs = 0;
    for (int i = 0; i < 64; i++) {
        KlTlsResult r = tls->handshake(tls, fd);
        if (r == KL_TLS_OK) { print("DIAG: handshake OK @step "); print_int(i+1); print_line(""); hs = 1; break; }
        if (r == KL_TLS_ERROR) { print("DIAG: handshake ERROR @step "); print_int(i+1);
            print(" io_status="); print_int((int)ops->io_status(cx)); print_line(""); break; }
        /* WANT_READ/WANT_WRITE: the pump provider gets data on retry. */
    }
    if (!hs) { print_line("DIAG: handshake incomplete"); tls->destroy(tls); ops->close(cx, fd); return; }

    static const char req[] = "GET / HTTP/1.1\r\nHost: 10.0.2.2\r\nConnection: close\r\n\r\n";
    kl_ssize_t w = tls->write(tls, fd, req, sizeof(req) - 1);
    print("DIAG: tls->write ret="); print_int((int)w);
    print(" io_status="); print_int((int)ops->io_status(cx)); print_line("");

    static char rx[2048];
    for (int i = 0; i < 200; i++) {
        kl_ssize_t n = tls->read(tls, fd, rx, sizeof(rx));
        print("DIAG: tls->read["); print_int(i); print("] ret="); print_int((int)n);
        print(" io_status="); print_int((int)ops->io_status(cx)); print_line("");
        if (n > 0) { write_bytes(rx, (UINTN)(n < 160 ? n : 160)); print_line(""); continue; }
        if (n == 0) { print_line("DIAG: read EOF"); break; }
        if (ops->io_status(cx) == KL_IO_WOULD_BLOCK) continue;   /* retry */
        print_line("DIAG: read FATAL"); break;
    }
    tls->destroy(tls); ops->close(cx, fd);
    print_line("DIAG: done");
}
#endif /* U4_MANUAL_DIAG */

/* ── the async completion callback ────────────────────────────────────────────── */
typedef struct { int done; int status; KlError err; const char *body; size_t body_len; } DoneCtx;

static void on_done(KlHttpClient *cl, void *ud) {
    DoneCtx *d = ud;
    const KlHttpClientResponse *r = kl_http_client_response(cl);
    if (r) {
        d->status = r->status;
        d->body = r->body;
        d->body_len = r->body_len;
    } else {
        d->status = -1;
    }
    d->err = kl_http_client_last_error(cl);
    d->done = 1;
}

/* ── entry ────────────────────────────────────────────────────────────────────── */
int efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st);

int efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st) {
    g_out = st->ConOut;
    EFI_BOOT_SERVICES *bs = st->BootServices;

    print_line("");
    print_line("=== U-4 KlHttpClient HTTPS GET over EFI_TCP4 + mbedTLS (freestanding) ===");
    print("U-4: target = "); print_line(TARGET_URL);
#ifdef KL_U4_PROD
    print_line("U-4: PROD mode — CA-verified TLS (dNSName SAN) + real EFI_RNG required");
#else
    print_line("U-4: *** SPIKE: TLS verify-none (self-signed server, no CA check) ***");
#endif

    /* FATAL, not a warning: a failed platform_init means the monotonic timer is not armed, so the
     * TLS cert clock would be frozen (accepting expired certs). platform_init tears its state down
     * on failure, so TLS bring-up would refuse anyway — stop here with a clear signal. */
    if (kl_uefi_platform_init(bs, st) != 0) {
        print_line("U-4: platform_init FAILED (monotonic clock unavailable) — cannot run TLS");
        print_line("U-4: NO-GO-YET (no monotonic clock; cert validity-time cannot advance)");
        goto park;
    }

    /* U-8: the cert-clock gate is INSIDE kl_uefi_mbedtls_platform_init() — it validates + snapshots
     * the wall clock and FAILS if the clock is untrustworthy, so TLS cannot come up without one and
     * the app cannot bypass it. We deliberately do NOT pre-check the clock here: run_u4.sh
     * U4_CLOCK=bad must reach this mandatory gate to prove it rejects the bad clock. */
    if (kl_uefi_mbedtls_platform_init(bs) != 0) {
        print_line("U-4: mbedtls platform init FAILED — untrustworthy wall clock or heap registration");
        print_line("U-4: NO-GO-YET (TLS refused: cert validity-time cannot be enforced)");
        goto park;
    }
    if (!kl_uefi_have_entropy()) {
#ifdef KL_U4_PROD
        /* Production is fail-closed: no real entropy => no handshake. Provision it in
         * QEMU with `-device virtio-rng-pci` (run_u4.sh U4_VIRTIO_RNG=1). */
        print_line("U-4: PROD requires EFI_RNG_PROTOCOL — none present (add virtio-rng)");
        print_line("U-4: NO-GO-YET (no hardware entropy)");
        goto park;
#else
        print_line("U-4: *** WARN: no EFI_RNG — using WEAK entropy fallback (INSECURE) ***");
#endif
    }

    /* Inject the EFI completion provider on the stock freestanding archive. */
    const KlEventProvider *ep = kl_uefi_event_provider(bs, image_handle);
    if (!ep) { print_line("U-4: event provider build failed"); goto park; }

    KlAllocator alloc = kl_uefi_allocator(bs);

    KlEventCtx ev;
    if (kl_event_ctx_init_ex(&ev, &alloc, ep) != 0) {
        print_line("U-4: kl_event_ctx_init_ex failed (no compatible socket provider?)");
        print_line("U-4: NO-GO-YET (needs network-enabled OVMF)");
        goto park;
    }
    print_line("U-4: event ctx up (efi-tcp4-completion)");

    /* U-8: the cert-clock snapshot + fail-closed gate were already taken INSIDE
     * kl_uefi_mbedtls_platform_init() above (no per-session choreography); mbedtls_time() reads
     * that lifetime snapshot during the handshake, never GetTime. Nothing to do here. */

#ifdef KL_U4_PROD
    /* Production: verify-REQUIRED against the embedded CA (the responder's self-signed
     * cert, whose dNSName SAN is KL_U4_PROD_HOST). The client dials KL_U4_PROD_HOST —
     * resolved to the responder IP by resolve_uefi.c's static hosts entry — so mbedTLS
     * checks the cert hostname (SNI + SAN) as a real HTTPS client does. */
    KlTlsCtx *tctx = kl_tls_mbedtls_client_ctx_create_from_buf(
        g_u4_ca_pem, g_u4_ca_pem_len, &alloc);
    if (!tctx) {
        print_line("U-4: PROD TLS client ctx create failed (CA parse / entropy)");
        kl_event_ctx_free(&ev);
        goto park;
    }
#else
    /* Verify-none client TLS context. NOTE: the *_from_buf(NULL,0) entry point REJECTS
     * a NULL buffer (it requires an in-memory CA bundle); the verify-none path is the
     * FILE-path variant with a NULL path — kl_tls_mbedtls_client_ctx_create(NULL,...)
     * short-circuits to client_ctx_create_from_mem(NULL,0,alloc) => MBEDTLS_SSL_VERIFY_NONE
     * WITHOUT touching the filesystem (fopen is a fail-closed stub here anyway). */
    KlTlsCtx *tctx = kl_tls_mbedtls_client_ctx_create(NULL, &alloc);
    if (!tctx) {
        print_line("U-4: TLS client ctx create failed (entropy seed / config)");
        kl_event_ctx_free(&ev);
        goto park;
    }
#endif
    KlTlsConfig tls;
    { unsigned char *p = (unsigned char *)&tls; for (size_t i = 0; i < sizeof(tls); i++) p[i] = 0; }
    tls.ctx         = tctx;
    tls.factory     = kl_tls_mbedtls_create;
    tls.ctx_destroy = kl_tls_mbedtls_ctx_destroy;
#ifdef KL_U4_PROD
    print_line("U-4: TLS client ctx up (mbedtls, CA-verified)");
#else
    print_line("U-4: TLS client ctx up (mbedtls, verify-none)");
#endif

#ifdef U4_MANUAL_DIAG
    manual_diag(bs, image_handle, tctx, &alloc);
    kl_tls_mbedtls_ctx_destroy(tctx);
    kl_event_ctx_free(&ev);
    goto park;
#endif

    KlHttpClientConfig cfg;
    { unsigned char *p = (unsigned char *)&cfg; for (size_t i = 0; i < sizeof(cfg); i++) p[i] = 0; }
    cfg.timeout_ms = 60000;   /* generous: TCG + SLIRP + a full RSA/ECDHE handshake */
    cfg.tls = &tls;
    /* No cfg.resolver → numeric kl_resolve_sync (resolve_uefi.c) resolves the host. */

    DoneCtx d;
    { unsigned char *p = (unsigned char *)&d; for (size_t i = 0; i < sizeof(d); i++) p[i] = 0; }

    KlHttpClient *c = kl_http_client_start(&ev, &alloc, &cfg, "GET", TARGET_URL,
                                  NULL, 0, NULL, 0, on_done, &d);
    if (!c) {
        print_line("U-4: kl_http_client_start failed");
        kl_tls_mbedtls_ctx_destroy(tctx);
        kl_event_ctx_free(&ev);
        goto park;
    }
    print_line("U-4: client started; pumping the completion loop (TLS handshake)...");

    /* Drive the loop until the request completes (or a generous tick bound). The
     * socket provider's send/recv pump the EFI tokens; the mbedTLS non-blocking BIO
     * runs effectively synchronously over that pump. TLS handshake under TCG is slow
     * (RSA/ECDHE), so the bound is large. */
    for (int tick = 0; tick < 2000000 && !d.done; tick++) {
        kl_event_ctx_run(&ev, 16, 10);
        kl_timer_fire(&ev);
    }

    if (d.done) {
        print("U-4: status = "); print_int(d.status); print_line("");
        if (d.status >= 0 && d.body && d.body_len > 0) {
            print_line("U-4: --- body begin ---");
            write_bytes(d.body, d.body_len < 512 ? d.body_len : 512);
            print_line("");
            print_line("U-4: --- body end ---");
        }
        if (d.err != KL_ERR_NONE) { print("U-4: error = "); print_int((int)d.err); print_line(""); }
    } else {
        print_line("U-4: did not complete within the tick bound");
    }

    kl_http_client_free(c);
    kl_tls_mbedtls_ctx_destroy(tctx);   /* destroy every TLS object FIRST ... */
    kl_uefi_mbedtls_platform_shutdown();/* F5: ... then drop the Boot Services heap ptr so
                                         * no later mbedTLS calloc/free can touch firmware */
    kl_event_ctx_free(&ev);

    if (d.done && d.status == 200)
        print_line("U-4: GO");
    else
        print_line("U-4: NO-GO-YET");

park:
    print_line("U-4: (parked; harness will terminate QEMU)");
    for (;;) { bs->Stall(1000000); }
    /* not reached */
}
