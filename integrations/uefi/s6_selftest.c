/*
 * s6_selftest.c — S-6 acceptance self-test (UEFI EFI application): HTTPS server.
 *
 * The TLS mirror of S-4: a STOCK freestanding KlHttpServer answers `GET / -> 200` over
 * HTTPS (EFI_TCP4 + freestanding mbedTLS) on bare UEFI firmware, driven by the EFI
 * completion backend. The completion-mode TLS server is entirely in the model-blind
 * core (completion_server.c: comp_tls_drive / kl_comp_tls_flush / comp_tls_send_response);
 * the EFI backend supplies only the two documented obligations:
 *   1. post_recv does raw transport I/O into the caller-chosen buffer (event_efi.c) — the HTTP
 *      completion adapter (comp_on_read) feeds received CIPHERTEXT to tls->feed_input,
 *   2. a synchronous send on the accepted socket for kl_comp_tls_flush (efi_sock_send).
 * The response ciphertext rides the same post_send as S-4 (content-agnostic).
 *
 * Flow (all through the PUBLIC KlHttpServer API — zero protocol edits):
 *   kl_uefi_platform_init(bs, st)            → monotonic clock + EFI_RNG
 *   kl_uefi_mbedtls_platform_init(bs)        → EFI heap + entropy + cert clock for mbedTLS
 *   kl_tls_mbedtls_ctx_create_from_buf(cert,key,...)  → server TLS ctx (embedded PEM)
 *   kl_http_server_init(cfg{event_provider, tls}) → construct from the freestanding archive
 *   kl_http_server_route(GET "/", handler); bind(:443)/listen; run_completion_loop
 *
 * Prints:  S-6: server up → listening on :443 → GO (served GET / 200)
 *
 * Freestanding: clang --target=x86_64-unknown-windows, -nostdlib, lld PE. No libc.
 */

#include "efi_uefi.h"
#include "platform_uefi.h"
#include "mbedtls_platform_uefi.h"     /* kl_uefi_mbedtls_platform_init */
#include "socket_efi_tcp4.h"
#include "event_efi.h"
#include "allocator_uefi.h"

#include <keel/http_server.h>
#include <keel/http_router.h>
#include <keel/http_request.h>
#include <keel/http_response.h>
#include <keel/event_ctx.h>
#include <keel/timer.h>
#include <keel/sockaddr.h>
#include <keel/handle.h>
#include <keel/tls.h>
#include "keel_tls_mbedtls.h"          /* kl_tls_mbedtls_ctx_create_from_buf + factory */
#include "s6_cert_embed.h"             /* g_s6_cert_pem / g_s6_key_pem (run_s6.sh generates) */
#include "socket.h"                    /* the socket seam: kl_sock_socket/bind/listen (-Isrc) */

#include <stdint.h>
#include <stddef.h>

#define S6_AF_INET     2
#define S6_SOCK_STREAM 1
#define S6_BIND_PORT   443
#define S6_BACKLOG     4

extern int kl_http_server_run_completion_loop(KlHttpServer *s);

/* ── tiny ASCII console (no libc) ─────────────────────────────────────────────── */
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *g_out;
static void print(const char *s) {
    CHAR16 buf[256]; UINTN i = 0;
    while (s[i] && i < 254) { buf[i] = (CHAR16)(UINT8)s[i]; i++; }
    buf[i] = 0;
    if (g_out) g_out->OutputString(g_out, buf);
}
static void print_line(const char *s) { print(s); print("\r\n"); }
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

/* ── the request handler (the SAME one a hosted HTTPS server runs) ────────────── */
static int g_served = 0;
static void s6_handler(KlHttpRequest *req, KlHttpResponse *res, void *user_data) {
    (void)req; (void)user_data;
    static const char body[] = "hello from KEEL on UEFI (HTTPS over EFI_TCP4)\n";
    kl_http_response_status(res, 200);
    kl_http_response_body_borrow(res, body, sizeof(body) - 1);
    if (!g_served) { g_served = 1; print_line("S-6: GO — served GET / (200 over TLS)"); }
}

/* ── entry ────────────────────────────────────────────────────────────────────── */
int efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st);

int efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st) {
    g_out = st->ConOut;
    EFI_BOOT_SERVICES *bs = st->BootServices;

    print_line("");
    print_line("=== S-6 KlHttpServer HTTPS GET / -> 200 over EFI_TCP4 + freestanding mbedTLS ===");

    if (kl_uefi_platform_init(bs, st) != 0)
        print_line("S-6: (warn) platform_init failed — clock stuck, continuing");
    /* EFI heap + entropy + cert clock for mbedTLS (mandatory TLS bring-up). */
    if (kl_uefi_mbedtls_platform_init(bs) != 0) {
        print_line("S-6: mbedtls_platform_init failed (entropy/clock)");
        print_line("S-6: NO-GO-YET");
        goto park;
    }
    if (!kl_uefi_have_entropy())
        print_line("S-6: *** WARN: no EFI_RNG — using WEAK entropy fallback (INSECURE) ***");

    const KlEventProvider *ep = kl_uefi_event_provider(bs, image_handle);
    if (!ep) { print_line("S-6: event provider build failed"); goto park; }

    KlAllocator alloc = kl_uefi_allocator(bs);

    /* Server TLS context from the embedded PEM cert + key (no filesystem). */
    KlTlsCtx *tctx = kl_tls_mbedtls_ctx_create_from_buf(
        g_s6_cert_pem, g_s6_cert_pem_len, g_s6_key_pem, g_s6_key_pem_len,
        NULL, 0, KL_MTLS_NONE, &alloc);
    if (!tctx) {
        print_line("S-6: server TLS ctx create failed (cert/key parse / entropy)");
        goto park;
    }
    KlTlsConfig tls;
    { unsigned char *p = (unsigned char *)&tls; for (size_t i = 0; i < sizeof(tls); i++) p[i] = 0; }
    tls.ctx         = tctx;
    tls.factory     = kl_tls_mbedtls_create;
    tls.ctx_destroy = kl_tls_mbedtls_ctx_destroy;

    KlHttpServerConfig cfg;
    { unsigned char *p = (unsigned char *)&cfg; for (size_t i = 0; i < sizeof(cfg); i++) p[i] = 0; }
    cfg.port            = S6_BIND_PORT;
    cfg.bind_addr       = "0.0.0.0";
    cfg.max_connections = S6_BACKLOG;
    cfg.alloc           = &alloc;
    cfg.event_provider  = ep;
    cfg.tls             = &tls;

    KlHttpServer s;
    if (kl_http_server_init(&s, &cfg) != 0) {
        print("S-6: kl_http_server_init failed (err="); print_int((int)s.last_error); print_line(")");
        print_line("S-6: NO-GO-YET (needs network-enabled OVMF)");
        goto park;
    }
    print_line("S-6: server up (efi-tcp4-completion + mbedTLS)");

    /* NB: kl_http_server_free is hosted-only (teardown is S-7). On error the firmware parks;
     * QEMU is power-cycled by the harness, so there is no leak to reclaim. */
    if (kl_http_server_route(&s, "GET", "/", s6_handler, NULL, NULL) != 0) {
        print_line("S-6: route registration failed"); goto park;
    }

    KlSockAddr bind_sa;
    if (kl_sockaddr_parse(&bind_sa, "0.0.0.0", (uint16_t)S6_BIND_PORT) != 0) {
        print_line("S-6: bind address parse failed"); goto park;
    }
    s.listen_fd = kl_sock_socket(s.ev.sockets, S6_AF_INET, S6_SOCK_STREAM, 0);
    if (!kl_handle_valid(s.listen_fd)) { print_line("S-6: socket() failed"); goto park; }
    (void)kl_sock_set_reuseaddr(s.ev.sockets, s.listen_fd, 1);
    if (kl_sock_bind(s.ev.sockets, s.listen_fd, &bind_sa) < 0) {
        print_line("S-6: bind(:443) failed"); goto park;
    }
    if (kl_sock_listen(s.ev.sockets, s.listen_fd, S6_BACKLOG) < 0) {
        print_line("S-6: listen() failed"); goto park;
    }
    print_line("S-6: listening on :443");

    /* Run the completion loop. Each tick primes accepts, drains KL_COMP_ACCEPT →
     * comp_on_accept (which enters KL_HTTP_CONN_TLS_HANDSHAKE + memory-BIO mode), then the
     * EFI post_recv delivers raw ciphertext and the HTTP adapter (comp_on_read) feeds it to
     * feed_input while comp_tls_drive handshakes / decrypts / responds — all model-blind. The handler prints GO on the first served
     * request; the loop keeps serving (the harness times out QEMU after curl's 200). */
    for (long tick = 0; tick < 100000000L; tick++) {
        if (kl_http_server_run_completion_loop(&s) < 0) {
            print_line("S-6: completion loop reported fatal error");
            break;
        }
        kl_timer_fire(&s.ev);
    }

    /* (No kl_http_server_free — hosted-only; teardown is S-7.) */

park:
    print_line("S-6: (parked; harness will terminate QEMU)");
    for (;;) { bs->Stall(1000000); }
    /* not reached */
}
