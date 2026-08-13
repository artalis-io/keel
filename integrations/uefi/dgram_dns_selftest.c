/*
 * dgram_dns_selftest.c — 6.4c acceptance self-test (UEFI EFI application).
 *
 * The on-real-firmware validation for the EFI_UDP4 datagram provider (6.4b): the STOCK
 * src/dns_resolver.c resolves a hostname over KlUdp-over-EFI_UDP4 — i.e. through the
 * unified EFI socket provider (SOCK_DGRAM → EFI_UDP4 child, socket_efi_udp4.c) and the
 * completion-native datagram wiring in event_efi.c (post_dgram_recv/_send + drain, B.6
 * stable token) — with NO bespoke dns_uefi.c on the path. Contrast U-5, which used the
 * one-shot dns_uefi.c behind kl_resolve_sync; here the async KlResolver vtable runs the
 * real Do53 query engine over the datagram machine.
 *
 * Flow:
 *   kl_uefi_platform_init(bs, st)          → monotonic clock + EFI_RNG (query id entropy)
 *   kl_uefi_event_provider(bs, image)      → the completion provider (lazy unified socket)
 *   kl_event_ctx_init_ex(&ev, alloc, ep)   → inject the provider on the stock archive
 *   ev.sockets = kl_uefi_socket_provider   → the unified stream+datagram provider
 *   kl_dns_resolver_create(&ev, {.nameserver=NS})  → KlUdp over EFI_UDP4 (completion)
 *   resolver->resolve(NAME) → post_dgram_send (A + AAAA) → post_dgram_recv → dns_on_recv
 *   pump kl_event_ctx_run until the KlResolveDoneFn fires
 *
 * Prints:
 *   6.4c: resolving <NAME> via <NS> over EFI_UDP4 ...
 *   6.4c: resolved <NAME> -> a.b.c.d           (first address)
 *   6.4c: GO            (resolved >= 1 address) | 6.4c: NO-GO-YET
 *
 * Freestanding: clang --target=*-unknown-windows, -nostdlib, lld PE. No libc.
 */

#include "efi_uefi.h"
#include "platform_uefi.h"
#include "socket_efi_tcp4.h"   /* kl_uefi_socket_provider (unified) + kl_efi_sockaddr_to_ipv4 */
#include "event_efi.h"
#include "allocator_uefi.h"

#include <keel/event_ctx.h>
#include <keel/timer.h>
#include <keel/dns_resolver.h>
#include <keel/resolver.h>
#include <keel/error.h>

#include <stdint.h>
#include <stddef.h>

#ifndef KL_U9_HOSTNAME
#define KL_U9_HOSTNAME "keel.test"
#endif
#ifndef KL_U9_NAMESERVER
#define KL_U9_NAMESERVER "10.0.2.2"
#endif

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
    out[o] = 0; print(out);
}
static void print_ipv4(const uint8_t ip[4]) {
    for (int i = 0; i < 4; i++) { print_int((int)ip[i]); if (i < 3) print("."); }
}

/* ── the async resolve callback ───────────────────────────────────────────────── */
typedef struct {
    int  done;
    int  naddrs;
    int  error;
    int  have_ipv4;
    uint8_t ip[4];
} ResolveCtx;

static void on_resolved(KlResolveReq *req, const KlResolveResult *result, int error, void *ud) {
    (void)req;
    ResolveCtx *r = ud;
    r->done = 1;
    r->error = error;
    if (result) {
        r->naddrs = result->naddrs;
        /* Extract the first IPv4 (if any) for the serial report. */
        for (int i = 0; i < result->naddrs; i++) {
            EFI_IPv4_ADDRESS a; UINT16 p = 0;
            if (kl_efi_sockaddr_to_ipv4(&result->addrs[i], &a, &p) == 0) {
                for (int b = 0; b < 4; b++) r->ip[b] = a.Addr[b];
                r->have_ipv4 = 1;
                break;
            }
        }
    }
}

/* ── entry ────────────────────────────────────────────────────────────────────── */
int efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st);

int efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st) {
    g_out = st->ConOut;
    EFI_BOOT_SERVICES *bs = st->BootServices;

    print_line("");
    print_line("=== 6.4c stock dns_resolver over KlUdp-over-EFI_UDP4 ===");
    print("6.4c: hostname = "); print(KL_U9_HOSTNAME);
    print(" nameserver = "); print_line(KL_U9_NAMESERVER);

    if (kl_uefi_platform_init(bs, st) != 0)
        print_line("6.4c: (warn) platform_init failed — clock/RNG stuck, continuing");

    const KlEventProvider *ep = kl_uefi_event_provider(bs, image_handle);
    if (!ep) { print_line("6.4c: event provider build failed"); goto park; }

    KlAllocator alloc = kl_uefi_allocator(bs);

    KlEventCtx ev;
    if (kl_event_ctx_init_ex(&ev, &alloc, ep) != 0) {
        print_line("6.4c: kl_event_ctx_init_ex failed");
        print_line("6.4c: NO-GO-YET (needs network-enabled OVMF)");
        goto park;
    }
    /* The unified EFI provider serves BOTH SOCK_STREAM (EFI_TCP4) and SOCK_DGRAM (EFI_UDP4);
     * the resolver's KlUdp is created over its .dgram data-plane. */
    ev.sockets = kl_uefi_socket_provider(bs, image_handle);
    if (!ev.sockets) {
        print_line("6.4c: no EFI socket provider (no network stack in this OVMF?)");
        print_line("6.4c: NO-GO-YET");
        kl_event_ctx_free(&ev); goto park;
    }
    print_line("6.4c: event ctx up (efi completion) + unified socket provider");

    KlDnsResolverConfig dcfg;
    { unsigned char *p = (unsigned char *)&dcfg; for (size_t i = 0; i < sizeof(dcfg); i++) p[i] = 0; }
    dcfg.nameserver = KL_U9_NAMESERVER;   /* freestanding build REQUIRES an explicit nameserver */
    dcfg.timeout_ms = 8000;
    dcfg.alloc = &alloc;

    KlResolver *res = kl_dns_resolver_create(&ev, &dcfg);
    if (!res) {
        print_line("6.4c: kl_dns_resolver_create failed (KlUdp-over-EFI_UDP4 init?)");
        kl_event_ctx_free(&ev); goto park;
    }
    print_line("6.4c: stock resolver created (KlUdp over EFI_UDP4, completion)");

    print("6.4c: resolving "); print(KL_U9_HOSTNAME); print(" via "); print(KL_U9_NAMESERVER);
    print_line(" over EFI_UDP4 ...");

    ResolveCtx rc;
    { unsigned char *p = (unsigned char *)&rc; for (size_t i = 0; i < sizeof(rc); i++) p[i] = 0; }
    KlResolveReq *req = res->resolve(res, &ev, KL_U9_HOSTNAME, 80, on_resolved, &rc);
    if (!req) { print_line("6.4c: resolve() start failed"); res->destroy(res); kl_event_ctx_free(&ev); goto park; }

    for (int tick = 0; tick < 200000 && !rc.done; tick++) {
        kl_event_ctx_run(&ev, 16, 10);
        kl_timer_fire(&ev);
    }

    if (rc.done) {
        if (rc.naddrs > 0) {
            print("6.4c: resolved "); print(KL_U9_HOSTNAME); print(" -> ");
            if (rc.have_ipv4) print_ipv4(rc.ip); else print("(non-IPv4)");
            print(" ("); print_int(rc.naddrs); print_line(" address(es))");
        } else {
            print("6.4c: resolution failed, error = "); print_int(rc.error); print_line("");
        }
    } else {
        print_line("6.4c: did not complete within the tick bound");
    }

    res->destroy(res);
    kl_event_ctx_free(&ev);

    if (rc.done && rc.naddrs > 0)
        print_line("6.4c: GO");
    else
        print_line("6.4c: NO-GO-YET");

park:
    print_line("6.4c: (parked; harness will terminate QEMU)");
    for (;;) { bs->Stall(1000000); }
    /* not reached */
}
