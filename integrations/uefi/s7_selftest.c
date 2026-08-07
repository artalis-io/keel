/*
 * s7_selftest.c — S-7 acceptance: clean KlServer teardown + EFI ExitBootServices lifetime.
 *
 * The server mirror of U-7. A freestanding KlServer serves ONE plaintext GET / -> 200
 * over EFI_TCP4, then tears itself down cleanly from the archive alone and releases the
 * EFI network stack before (a real app would) ExitBootServices:
 *
 *   kl_server_free(&s)                     → closes the listener + every accepted child
 *                                            (draining their EFI_TCP4 tokens), frees the
 *                                            pool/router — the S-7 freestanding carve.
 *   kl_uefi_socket_provider_live_count()   → MUST be 0 now (every socket closed) — the
 *                                            documented precondition for kl_uefi_shutdown.
 *   kl_uefi_shutdown()                     → releases the EFI_TCP4 socket provider, the
 *                                            completion event provider, and the platform
 *                                            timer/EBS event (U-7).
 *
 * Like U-7 we do NOT call the real ExitBootServices() — there is no serial console after
 * it. kl_uefi_after_ebs() is shown as the fail-closed guard (0 pre-EBS; a real
 * ExitBootServices sets it, after which KEEL's EFI providers refuse boot-service I/O).
 *
 * Prints:  S-7: served -> tearing down -> live sockets = 0 -> providers released -> PASS
 *
 * Freestanding: clang --target=x86_64-unknown-windows, -nostdlib, lld PE. No libc.
 */

#include "efi_uefi.h"
#include "platform_uefi.h"             /* kl_uefi_after_ebs */
#include "socket_efi_tcp4.h"           /* kl_uefi_socket_provider_live_count */
#include "event_efi.h"
#include "allocator_uefi.h"
#include "lifecycle_uefi.h"            /* kl_uefi_shutdown */

#include <keel/server.h>
#include <keel/router.h>
#include <keel/request.h>
#include <keel/response.h>
#include <keel/event_ctx.h>
#include <keel/timer.h>
#include <keel/sockaddr.h>
#include <keel/handle.h>
#include "socket.h"                    /* the socket seam (-Isrc) */

#include <stdint.h>
#include <stddef.h>

#define S7_AF_INET     2
#define S7_SOCK_STREAM 1
#define S7_BIND_PORT   80
#define S7_BACKLOG     4
#define S7_DRAIN_TICKS 3000            /* grace ticks after serving: flush the 200, let curl close */

extern int kl_server_run_completion_loop(KlServer *s);

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

/* ── the request handler ──────────────────────────────────────────────────────── */
static int g_served = 0;
static void s7_handler(KlRequest *req, KlResponse *res, void *user_data) {
    (void)req; (void)user_data;
    static const char body[] = "hello from KEEL on UEFI (S-7 teardown test)\n";
    kl_response_status(res, 200);
    kl_response_body_borrow(res, body, sizeof(body) - 1);
    if (!g_served) { g_served = 1; print_line("S-7: GO — served GET / (200)"); }
}

/* ── entry ────────────────────────────────────────────────────────────────────── */
int efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st);

int efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st) {
    g_out = st->ConOut;
    EFI_BOOT_SERVICES *bs = st->BootServices;

    print_line("");
    print_line("=== S-7 KlServer clean teardown + ExitBootServices lifetime ===");

    if (kl_uefi_platform_init(bs, st) != 0)
        print_line("S-7: (warn) platform_init failed — clock stuck, continuing");

    const KlEventProvider *ep = kl_uefi_event_provider(bs, image_handle);
    if (!ep) { print_line("S-7: event provider build failed"); goto park; }

    KlAllocator alloc = kl_uefi_allocator(bs);

    KlConfig cfg;
    { unsigned char *p = (unsigned char *)&cfg; for (size_t i = 0; i < sizeof(cfg); i++) p[i] = 0; }
    cfg.port            = S7_BIND_PORT;
    cfg.bind_addr       = "0.0.0.0";
    cfg.max_connections = S7_BACKLOG;
    cfg.alloc           = &alloc;
    cfg.event_provider  = ep;

    KlServer s;
    if (kl_server_init(&s, &cfg) != 0) {
        print("S-7: kl_server_init failed (err="); print_int((int)s.last_error); print_line(")");
        print_line("S-7: NO-GO-YET (needs network-enabled OVMF)");
        goto park;
    }
    print_line("S-7: server up (efi-tcp4-completion)");

    if (kl_server_route(&s, "GET", "/", s7_handler, NULL, NULL) != 0) {
        print_line("S-7: route registration failed"); goto park;
    }
    KlSockAddr bind_sa;
    if (kl_sockaddr_parse(&bind_sa, "0.0.0.0", (uint16_t)S7_BIND_PORT) != 0) {
        print_line("S-7: bind address parse failed"); goto park;
    }
    s.listen_fd = kl_sock_socket(s.ev.sockets, S7_AF_INET, S7_SOCK_STREAM, 0);
    if (!kl_handle_valid(s.listen_fd)) { print_line("S-7: socket() failed"); goto park; }
    (void)kl_sock_set_reuseaddr(s.ev.sockets, s.listen_fd, 1);
    if (kl_sock_bind(s.ev.sockets, s.listen_fd, &bind_sa) < 0) {
        print_line("S-7: bind(:80) failed"); goto park;
    }
    if (kl_sock_listen(s.ev.sockets, s.listen_fd, S7_BACKLOG) < 0) {
        print_line("S-7: listen() failed"); goto park;
    }
    print_line("S-7: listening on :80");

    /* Serve until one request is handled, then run a bounded grace tail so the 200 is
     * fully flushed and curl can close, and break to the clean-teardown path. */
    int drain = 0;
    for (long tick = 0; tick < 100000000L; tick++) {
        if (kl_server_run_completion_loop(&s) < 0) {
            print_line("S-7: completion loop reported fatal error"); break;
        }
        kl_timer_fire(&s.ev);
        if (g_served && ++drain > S7_DRAIN_TICKS) break;
    }

    /* ── clean teardown (the S-7 acceptance) ──────────────────────────────────── */
    print_line("S-7: tearing down — kl_server_free (close listener + children) ...");
    kl_server_free(&s);

    int live = kl_uefi_socket_provider_live_count();
    print("S-7: live sockets after kl_server_free = "); print_int(live); print_line("");

    print_line("S-7: releasing EFI network stack (kl_uefi_shutdown) ...");
    kl_uefi_shutdown();

    print("S-7: after_ebs guard = "); print_int(kl_uefi_after_ebs());
    print_line(" (0 pre-EBS; a real ExitBootServices sets it -> providers fail-closed)");

    if (g_served && live == 0)
        print_line("S-7: PASS — served GET / -> 200 then clean teardown (0 live sockets)");
    else
        print_line("S-7: FAIL");

park:
    print_line("S-7: (parked; harness will terminate QEMU)");
    for (;;) { bs->Stall(1000000); }
    /* not reached */
}
