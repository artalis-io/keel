/*
 * dgram_dns_selftest.c — 6.4c acceptance self-test (UEFI EFI application).
 *
 * The on-real-firmware validation for the EFI_UDP4 datagram provider (6.4b): the STOCK
 * src/dns_resolver.c resolves a hostname over KlUdp-over-EFI_UDP4 — the unified EFI socket
 * provider (SOCK_DGRAM → EFI_UDP4 child, socket_efi_udp4.c) + the completion-native datagram
 * wiring in event_efi.c (post_dgram_recv/_send + drain, B.6 stable token) — with NO bespoke
 * dns_uefi.c on the path. Contrast U-5, which used the one-shot dns_uefi.c behind
 * kl_resolve_sync; here the async KlResolver vtable runs the real Do53 query engine over the
 * datagram machine, and the resolved address then drives an EFI_TCP4 GET.
 *
 * Frozen 6.4c acceptance (docs/phase10_efi_udp4_provider_design.md §9):
 *   1. stock DNS over EFI_UDP4 → A/AAAA → HTTP GET 200 (the resolver is injected as
 *      cfg.resolver, so kl_client resolves over EFI_UDP4 then connects over EFI_TCP4;
 *      TLS-over-EFI_TCP4 is orthogonal and already proven in U-4/U-6).
 *   2. a truncated-response (TC=1) case: the freestanding resolver has NO TCP fallback
 *      (6.4a-2, #ifndef KEEL_FREESTANDING), so a TC answer settles a clean KL_ERR_DNS
 *      failure — no hang — on real firmware.
 *   3. kl_uefi_udp_provider_live_count()==0 (and quarantined==0) after teardown.
 *
 * The self-test is mode-agnostic: it always resolves→GETs and reports FACTS; the harness
 * oracle (run_dgram_dns.sh) decides PASS per DNS mode. Machine-checkable markers:
 *   6.4c: http status = <n>        (<0 if resolve/connect failed)
 *   6.4c: error = <KlError>
 *   6.4c: udp_live = <n> udp_quarantined = <n>     (asserted 0/0 on the happy path)
 *   6.4c: GO       (status==200 && live==0 && quarantined==0)   |   6.4c: NO-GO
 *   6.4c: DONE     (always — the terminal marker; its absence means crash/hang)
 *
 * Freestanding: clang --target=*-unknown-windows, -nostdlib, lld PE. No libc.
 */

#include "efi_uefi.h"
#include "platform_uefi.h"
#include "socket_efi_tcp4.h"   /* unified provider + kl_efi_sockaddr_to_ipv4 */
#include "socket_efi_udp4.h"   /* kl_uefi_udp_provider_live_count / _quarantined_count */
#include "event_efi.h"
#include "allocator_uefi.h"
#include "../../src/platform.h" /* kl_monotonic_ms — elapsed-time oracle (response- vs timeout-driven) */

#include <keel/event_ctx.h>
#include <keel/timer.h>
#include <keel/dns_resolver.h>
#include <keel/resolver.h>
#include <keel/client.h>
#include <keel/error.h>

#include <stdint.h>
#include <stddef.h>

#ifndef KL_U9_HOSTNAME
#define KL_U9_HOSTNAME "keel.test"
#endif
#ifndef KL_U9_NAMESERVER
#define KL_U9_NAMESERVER "10.0.2.2"
#endif
#ifndef TARGET_PORT
#define TARGET_PORT 18080
#endif

#define STR2(x) #x
#define STR(x) STR2(x)
#define TARGET_URL "http://" KL_U9_HOSTNAME ":" STR(TARGET_PORT) "/"

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

/* ── async GET completion ─────────────────────────────────────────────────────── */
typedef struct { int done; int status; KlError err; } DoneCtx;

static void on_done(KlClient *cl, void *ud) {
    DoneCtx *d = ud;
    const KlClientResponse *r = kl_client_response(cl);
    d->status = r ? r->status : -1;
    d->err = kl_client_last_error(cl);
    d->done = 1;
}

/* ── entry ────────────────────────────────────────────────────────────────────── */
int efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st);

int efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st) {
    g_out = st->ConOut;
    EFI_BOOT_SERVICES *bs = st->BootServices;

    print_line("");
    print_line("=== 6.4c stock dns_resolver over KlUdp-over-EFI_UDP4 → GET ===");
    print("6.4c: target = "); print(TARGET_URL);
    print(" nameserver = "); print_line(KL_U9_NAMESERVER);

    if (kl_uefi_platform_init(bs, st) != 0)
        print_line("6.4c: (warn) platform_init failed — clock/RNG stuck, continuing");

    const KlEventProvider *ep = kl_uefi_event_provider(bs, image_handle);
    if (!ep) { print_line("6.4c: event provider build failed"); goto done; }

    KlAllocator alloc = kl_uefi_allocator(bs);

    KlEventCtx ev;
    if (kl_event_ctx_init_ex(&ev, &alloc, ep) != 0) {
        print_line("6.4c: kl_event_ctx_init_ex failed");
        goto done;
    }
    /* The unified EFI provider serves BOTH SOCK_STREAM (EFI_TCP4) and SOCK_DGRAM (EFI_UDP4):
     * the resolver's KlUdp rides its .dgram data-plane; the GET rides its stream ops. */
    ev.sockets = kl_uefi_socket_provider(bs, image_handle);
    if (!ev.sockets) { print_line("6.4c: no EFI socket provider (no network stack?)"); kl_event_ctx_free(&ev); goto done; }
    print_line("6.4c: event ctx up (efi completion) + unified socket provider");

    KlDnsResolverConfig dcfg;
    { unsigned char *p = (unsigned char *)&dcfg; for (size_t i = 0; i < sizeof(dcfg); i++) p[i] = 0; }
    dcfg.nameserver = KL_U9_NAMESERVER;   /* freestanding REQUIRES an explicit nameserver */
    dcfg.timeout_ms = 6000;
    dcfg.alloc = &alloc;

    KlResolver *res = kl_dns_resolver_create(&ev, &dcfg);
    if (!res) { print_line("6.4c: kl_dns_resolver_create failed"); kl_event_ctx_free(&ev); goto done; }
    print_line("6.4c: stock resolver created (KlUdp over EFI_UDP4, completion)");

    /* Inject the stock resolver: kl_client resolves KL_U9_HOSTNAME over EFI_UDP4 (A+AAAA,
     * RFC 8305) then connects to the resolved address over EFI_TCP4 and GETs. */
    KlClientConfig cfg;
    { unsigned char *p = (unsigned char *)&cfg; for (size_t i = 0; i < sizeof(cfg); i++) p[i] = 0; }
    cfg.timeout_ms = 30000;   /* generous: DNS RTT + TCG + SLIRP */
    cfg.resolver = res;       /* borrowed — destroyed below after kl_client_free */

    print("6.4c: resolving "); print(KL_U9_HOSTNAME); print(" via "); print(KL_U9_NAMESERVER);
    print_line(" over EFI_UDP4, then GET over EFI_TCP4 ...");

    DoneCtx d;
    { unsigned char *p = (unsigned char *)&d; for (size_t i = 0; i < sizeof(d); i++) p[i] = 0; }

    /* Time the whole resolve→GET. On the TC leg the stock resolver fails PROMPTLY (no TCP
     * fallback, no retransmit — src/dns_resolver.c header §"TC-settles-clean"), so completion
     * lands far below the per-leg timeout (dcfg.timeout_ms). If instead the TC responses were
     * dropped, both legs would only settle KL_ERR_DNS after the ≥6000 ms leg timeout. The
     * elapsed marker below therefore distinguishes response-driven settlement from a timeout. */
    uint64_t t0 = kl_monotonic_ms();
    KlClient *c = kl_client_start(&ev, &alloc, &cfg, "GET", TARGET_URL,
                                  NULL, 0, NULL, 0, on_done, &d);
    if (c) {
        for (int tick = 0; tick < 400000 && !d.done; tick++) {
            kl_event_ctx_run(&ev, 16, 10);
            kl_timer_fire(&ev);
        }
        if (!d.done) { d.status = -1; d.err = KL_ERR_TIMEOUT; }
    } else {
        print_line("6.4c: kl_client_start failed");
        d.done = 1; d.status = -1; d.err = KL_ERR_DNS;
    }
    uint64_t elapsed = kl_monotonic_ms() - t0;

    print("6.4c: http status = "); print_int(d.status); print_line("");
    print("6.4c: error = "); print_int((int)d.err); print_line("");
    print("6.4c: leg_timeout_ms = "); print_int(dcfg.timeout_ms); print_line("");
    print("6.4c: elapsed_ms = "); print_int((int)elapsed); print_line("");
    /* Symbolic error marker (robust to KlError reordering vs the numeric above): lets the TC
     * oracle attribute the failure to the DNS rejection specifically — not a timeout, socket
     * error, or malformed-response reject — per the 6.3 negative-test rule. */
    if (d.err == KL_ERR_DNS)     print_line("6.4c: resolve-error = KL_ERR_DNS");
    if (d.err == KL_ERR_TIMEOUT) print_line("6.4c: resolve-error = KL_ERR_TIMEOUT");

    /* Teardown BEFORE the live-count assertion: the resolver's KlUdp (EFI_UDP4 child) must
     * be reaped and no slot leaked/quarantined on the happy path (frozen §9.3). */
    if (c) kl_client_free(c);
    res->destroy(res);
    kl_event_ctx_free(&ev);

    int live = kl_uefi_udp_provider_live_count();
    int quar = kl_uefi_udp_provider_quarantined_count();
    print("6.4c: udp_live = "); print_int(live);
    print(" udp_quarantined = "); print_int(quar); print_line("");

    if (d.status == 200 && live == 0 && quar == 0)
        print_line("6.4c: GO");
    else
        print_line("6.4c: NO-GO");

done:
    /* Terminal marker: its ABSENCE in the serial log is the harness's crash/hang signal.
     * Return (rather than park) so startup.nsh's `reset -s` shuts the guest down promptly —
     * teardown already ran and udp_live/quarantined were asserted above. BOOT_TIMEOUT is the
     * safety net if a firmware ignores the shutdown. */
    print_line("6.4c: DONE");
    return 0;
}
