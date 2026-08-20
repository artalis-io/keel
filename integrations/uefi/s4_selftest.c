/*
 * s4_selftest.c — S-4 acceptance self-test (UEFI EFI application).
 *
 * The server mirror of U-3: a STOCK freestanding KlServer serves an HTTP/1.1
 * "GET / -> 200" on bare UEFI firmware, driven by the EFI completion backend
 * (event_efi.c) over the EFI_TCP4 socket provider (socket_efi_tcp4.c), with the
 * U-1 platform/allocator shims. No epoll / kqueue / io_uring, no OS sockets, no
 * errno, no libc — just the firmware's event services pumping EFI_TCP4 tokens and
 * relaying accepted children into the model-blind completion server driver.
 *
 * Flow (all through the PUBLIC KlServer API — the server core is model-blind):
 *   kl_uefi_platform_init(bs, st)          → monotonic clock
 *   kl_uefi_event_provider(bs, image)      → the completion provider (+ lazy socket provider)
 *   kl_server_init(&s, cfg{event_provider}) → construct the pool/router/event ctx from
 *                                             the freestanding archive alone (S-4 carve);
 *                                             cfg.sockets == NULL → the ctx adopts the
 *                                             backend's native EFI socket provider
 *   kl_server_route(GET "/", handler)       → 200 + a short body (borrowed static)
 *   manual socket/bind(:80)/listen          → kl_server_bind_listener is hosted-only, so
 *                                             the entry drives the socket seam directly
 *   loop: kl_server_run_completion_loop(&s) → auto-primes accepts, drains KL_COMP_ACCEPT
 *                                             into comp_on_accept, pumps recv/send/close —
 *                                             all model-blind (the SAME loop io_uring/
 *                                             IOCP/pollcomp run)
 *
 * Prints (over the EFI console, echoed to the QEMU serial log):
 *   S-4: listening on :80
 *   S-4: GO — served GET / (200)      (once, from the handler)
 *
 * Freestanding: clang --target=x86_64-unknown-windows, -nostdlib, lld PE. No libc.
 */

#include "efi_uefi.h"
#include "platform_uefi.h"
#include "socket_efi_tcp4.h"
#include "event_efi.h"
#include "allocator_uefi.h"

#include <keel/server.h>
#include <keel/router.h>
#include <keel/request.h>
#include <keel/http_response.h>
#include <keel/event_ctx.h>
#include <keel/timer.h>
#include <keel/sockaddr.h>
#include <keel/handle.h>
#include "socket.h"            /* the socket seam: kl_sock_socket/bind/listen (-Isrc) */

#include <stdint.h>
#include <stddef.h>

/* The EFI socket provider honors only SOCK_STREAM and ignores the domain (EFI_TCP4
 * is IPv4-only); pass the Keel/POSIX numeric literals without a POSIX header. */
#define S4_AF_INET     2
#define S4_SOCK_STREAM 1
#define S4_BIND_PORT   80
#define S4_BACKLOG     4

/* kl_server_run_completion_loop is the internal completion-loop tick (internal.h,
 * not a public header) — declare it here to avoid pulling the hosted internal.h. */
extern int kl_server_run_completion_loop(KlServer *s);

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

/* ── the request handler (model-blind — the same handler a hosted server runs) ── */
static int g_served = 0;

static void s4_handler(KlRequest *req, KlHttpResponse *res, void *user_data) {
    (void)req; (void)user_data;
    static const char body[] = "hello from KEEL on UEFI (EFI_TCP4 server)\n";
    kl_http_response_status(res, 200);
    kl_http_response_body_borrow(res, body, sizeof(body) - 1);
    if (!g_served) {
        g_served = 1;
        print_line("S-4: GO — served GET / (200)");
    }
}

/* ── entry ────────────────────────────────────────────────────────────────────── */
int efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st);

int efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st) {
    g_out = st->ConOut;
    EFI_BOOT_SERVICES *bs = st->BootServices;

    print_line("");
    print_line("=== S-4 KlServer GET / -> 200 over EFI_TCP4 completion backend ===");

    if (kl_uefi_platform_init(bs, st) != 0)
        print_line("S-4: (warn) platform_init failed — clock stuck, continuing");

    /* Inject the EFI completion provider on the stock freestanding SERVER archive. The
     * provider's native_provider() lazily builds the EFI_TCP4 socket provider; if this
     * OVMF has no TCP4 stack that returns NULL and kl_server_init fails below. */
    const KlEventProvider *ep = kl_uefi_event_provider(bs, image_handle);
    if (!ep) { print_line("S-4: event provider build failed"); goto park; }

    KlAllocator alloc = kl_uefi_allocator(bs);

    KlConfig cfg;
    { unsigned char *p = (unsigned char *)&cfg; for (size_t i = 0; i < sizeof(cfg); i++) p[i] = 0; }
    cfg.port = S4_BIND_PORT;
    cfg.bind_addr = "0.0.0.0";
    cfg.max_connections = S4_BACKLOG;
    cfg.alloc = &alloc;
    cfg.event_provider = ep;
    /* cfg.sockets == NULL → kl_server_init adopts kl_event_native_provider (the EFI
     * socket provider) for the completion loop. Same masking the client relies on. */

    KlServer s;
    if (kl_server_init(&s, &cfg) != 0) {
        print("S-4: kl_server_init failed (err="); print_int((int)s.last_error); print_line(")");
        print_line("S-4: NO-GO-YET (needs network-enabled OVMF)");
        goto park;
    }
    print_line("S-4: server up (efi-tcp4-completion)");

    /* NB: kl_server_free (like kl_server_bind_listener) lives in the hosted server.c
     * and is archive-excluded — proper listener teardown + EBS-clean shutdown is S-7
     * (kl_uefi_shutdown). On any error here the firmware simply parks; QEMU is
     * power-cycled by the harness, so there is no leak to reclaim. */
    if (kl_server_route(&s, "GET", "/", s4_handler, NULL, NULL) != 0) {
        print_line("S-4: route registration failed");
        goto park;
    }

    /* Manual listener bring-up: kl_server_bind_listener (TCP socket()+bind()+listen())
     * lives in the hosted server.c and is archive-excluded, so drive the socket seam
     * directly against the adopted EFI provider (s.ev.sockets). */
    KlSockAddr bind_sa;
    if (kl_sockaddr_parse(&bind_sa, "0.0.0.0", (uint16_t)S4_BIND_PORT) != 0) {
        print_line("S-4: bind address parse failed"); goto park;
    }
    s.listen_fd = kl_sock_socket(s.ev.sockets, S4_AF_INET, S4_SOCK_STREAM, 0);
    if (!kl_handle_valid(s.listen_fd)) {
        print_line("S-4: socket() failed"); goto park;
    }
    (void)kl_sock_set_reuseaddr(s.ev.sockets, s.listen_fd, 1);   /* best-effort */
    if (kl_sock_bind(s.ev.sockets, s.listen_fd, &bind_sa) < 0) {
        print_line("S-4: bind(:80) failed"); goto park;
    }
    if (kl_sock_listen(s.ev.sockets, s.listen_fd, S4_BACKLOG) < 0) {
        print_line("S-4: listen() failed"); goto park;
    }
    print_line("S-4: listening on :80");

    /* Run the completion loop. Each tick auto-primes accepts (kl_comp_prime_accepts),
     * drains KL_COMP_ACCEPT into the model-blind comp_on_accept, then pumps recv/send/
     * close + timers. The handler prints "S-4: GO" on the first served request; the
     * loop keeps serving (a real server runs until power-off — the harness times out
     * QEMU once curl has its 200). Bounded so a network-less OVMF still parks. */
    for (long tick = 0; tick < 100000000L; tick++) {
        if (kl_server_run_completion_loop(&s) < 0) {
            print_line("S-4: completion loop reported fatal error");
            break;
        }
        kl_timer_fire(&s.ev);
    }

    /* (No kl_server_free — hosted-only; teardown is S-7.) */

park:
    print_line("S-4: (parked; harness will terminate QEMU)");
    for (;;) { bs->Stall(1000000); }
    /* not reached */
}
