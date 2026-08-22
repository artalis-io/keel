/*
 * dgram_public_selftest.c — 7B-9 acceptance self-test (UEFI EFI application).
 *
 * The PUBLIC KlDatagram facade (keel/datagram.h) over EFI_UDP4 on bare OVMF firmware —
 * the end-to-end gate the host-mock cannot provide. Unlike dgram_dns_selftest.c (which
 * drives a KlDatagram via the stock dns_resolver), this exercises the public kl_datagram_*
 * surface directly: init → recv_start → send → (round-trip) → graceful close → DETACHED.
 *
 * It is the runtime proof for 7B-9 (the confirmed-detachment close): at close the armed
 * recv is EMPTY (its terminal is surfaced from the post-backend_close STALE_RETIRED
 * observation) and the send has drained — so close must confirm DETACHED with ZERO live
 * or quarantined EFI_UDP4 children.
 *
 * Scenario (one KlDatagram over a DHCP-default EFI_UDP4 socket):
 *   1. recv_start arms a receive; on_recv counts + matches the echo.
 *   2. send a probe datagram to KL_U9_ECHO_HOST:KL_U9_ECHO_PORT (a host UDP echo over
 *      SLIRP 10.0.2.2). The firmware Transmit completes; the echo is delivered back to the
 *      armed recv — proving the full public send + recv datagram path on real firmware.
 *   3. kl_datagram_close_begin (graceful): the send has drained and the re-armed recv is
 *      EMPTY → the confirmed-detachment close cancels it and settles DETACHED.
 *   4. free + kl_event_ctx_free, then assert kl_uefi_udp_provider_live_count()==0 and
 *      _quarantined_count()==0 (no leaked/quarantined EFI_UDP4 child).
 *
 * The self-test reports FACTS; run_dgram_public.sh is the marker-based oracle. Markers:
 *   7b9: send = <status>            (0 == KL_DATAGRAM_ACCEPTED)
 *   7b9: recv_bytes = <n>           (echo payload length delivered to on_recv)
 *   7b9: recv_matched = <0|1>       (delivered payload == the probe)
 *   7b9: close_state = <n>          (2 == KL_DGRAM_CLOSE_CLOSED)
 *   7b9: close_result = <n>         (1 == KL_DGRAM_DETACHED)
 *   7b9: udp_live = X udp_quarantined = Y
 *   7b9: GO | NO-GO
 *   7b9: DONE                       (its ABSENCE = crash/hang)
 *
 * Freestanding: clang --target=*-unknown-windows, -nostdlib, lld PE. No libc.
 */

#include "efi_uefi.h"
#include "platform_uefi.h"
#include "socket_efi_tcp4.h"   /* unified EFI socket provider builder */
#include "socket_efi_udp4.h"   /* kl_uefi_udp_socket / dgram_ops / live/quarantined counts */
#include "event_efi.h"
#include "allocator_uefi.h"

#include <keel/event_ctx.h>
#include <keel/timer.h>
#include <keel/datagram.h>
#include <keel/datagram_detail.h>   /* opt-in KlDatagram layout — stack-allocate the handle */
#include <keel/sockaddr.h>
#include <keel/error.h>

#include <stdint.h>
#include <stddef.h>

#ifndef KL_U9_ECHO_HOST_B0
#define KL_U9_ECHO_HOST_B0 10
#define KL_U9_ECHO_HOST_B1 0
#define KL_U9_ECHO_HOST_B2 2
#define KL_U9_ECHO_HOST_B3 2      /* the SLIRP host/gateway */
#endif
#ifndef KL_U9_ECHO_PORT
#define KL_U9_ECHO_PORT 17777
#endif

#define PROBE     "keel-7b9-probe"
#define PROBE_LEN 15              /* strlen("keel-7b9-probe") */

/* Freestanding: no <sys/socket.h>. The EFI provider takes the same numeric domain/type the
 * unified socket seam uses (AF_INET/SOCK_DGRAM == 2); kl_uefi_udp_socket ignores domain and
 * checks type == SOCK_DGRAM, configure() takes the IPv4 family. */
#ifndef AF_INET
#define AF_INET 2
#endif
#ifndef SOCK_DGRAM
#define SOCK_DGRAM 2
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

/* ── KlDatagram callbacks ─────────────────────────────────────────────────────── */
typedef struct {
    int      recv_count;
    size_t   recv_bytes;
    int      recv_matched;
    int      close_fired;
    KlDatagramCloseResult close_result;
} DgCtx;

static int mem_eq(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;
    for (size_t i = 0; i < n; i++) if (x[i] != y[i]) return 0;
    return 1;
}

static void on_recv(void *ud, const void *data, size_t len,
                    const KlSockAddr *peer, const KlSockAddr *local, unsigned flags) {
    (void)peer; (void)local; (void)flags;
    DgCtx *c = ud;
    c->recv_count++;
    c->recv_bytes = len;
    if (len == PROBE_LEN && mem_eq(data, PROBE, PROBE_LEN)) c->recv_matched = 1;
}
static void on_close(void *ud, KlDatagramCloseResult r) {
    DgCtx *c = ud;
    c->close_fired++;
    c->close_result = r;
}

/* ── entry ────────────────────────────────────────────────────────────────────── */
int efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st);

int efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st) {
    g_out = st->ConOut;
    EFI_BOOT_SERVICES *bs = st->BootServices;

    print_line("");
    print_line("=== 7b9 public KlDatagram close over EFI_UDP4 (round-trip + DETACHED) ===");

    if (kl_uefi_platform_init(bs, st) != 0)
        print_line("7b9: (warn) platform_init failed — clock/RNG stuck, continuing");

    const KlEventProvider *ep = kl_uefi_event_provider(bs, image_handle);
    if (!ep) { print_line("7b9: event provider build failed"); goto done; }

    KlAllocator alloc = kl_uefi_allocator(bs);
    KlEventCtx ev;
    if (kl_event_ctx_init_ex(&ev, &alloc, ep) != 0) {
        print_line("7b9: kl_event_ctx_init_ex failed");
        goto done;
    }
    ev.sockets = kl_uefi_socket_provider(bs, image_handle);
    if (!ev.sockets) { print_line("7b9: no EFI socket provider (no network stack?)"); kl_event_ctx_free(&ev); goto done; }
    print_line("7b9: event ctx up (efi completion) + unified socket provider");

    /* Prepare a DHCP-default EFI_UDP4 datagram socket (ephemeral source), then hand it to the
     * public KlDatagram — fd ownership transfers to KlDatagram on a successful init. */
    KlSocketHandle fd = kl_uefi_udp_socket(AF_INET, SOCK_DGRAM, 0);
    if (!kl_handle_valid(fd)) { print_line("7b9: kl_uefi_udp_socket failed"); kl_event_ctx_free(&ev); goto done; }
    {
        KlDatagramSocketConfig ucfg;
        { unsigned char *p = (unsigned char *)&ucfg; for (size_t i = 0; i < sizeof(ucfg); i++) p[i] = 0; }
        (void)kl_uefi_udp_dgram_ops()->configure(NULL, fd, AF_INET, &ucfg);   /* DHCP default */
    }

    DgCtx c;
    { unsigned char *p = (unsigned char *)&c; for (size_t i = 0; i < sizeof(c); i++) p[i] = 0; }
    c.close_result = KL_DGRAM_CLOSE_NONE;

    KlDatagram dg;
    { unsigned char *p = (unsigned char *)&dg; for (size_t i = 0; i < sizeof(dg); i++) p[i] = 0; }
    KlDatagramConfig dcfg;
    { unsigned char *p = (unsigned char *)&dcfg; for (size_t i = 0; i < sizeof(dcfg); i++) p[i] = 0; }
    dcfg.ctx = &ev; dcfg.alloc = &alloc; dcfg.sockets = ev.sockets; dcfg.fd = fd;
    dcfg.send_slots = 2; dcfg.send_slot_cap = 1500; dcfg.recv_cap = 1500;
    if (kl_datagram_init(&dg, &dcfg) != 0) {
        print_line("7b9: kl_datagram_init failed");
        (void)kl_uefi_udp_close(fd);   /* init did NOT adopt the fd — reclaim it */
        kl_event_ctx_free(&ev);
        goto done;
    }
    kl_datagram_on_close(&dg, on_close, &c);
    print_line("7b9: kl_datagram_init OK (public facade over EFI_UDP4)");

    if (kl_datagram_recv_start(&dg, on_recv, &c) != 0) print_line("7b9: (warn) recv_start failed");

    uint8_t dip[4] = { KL_U9_ECHO_HOST_B0, KL_U9_ECHO_HOST_B1, KL_U9_ECHO_HOST_B2, KL_U9_ECHO_HOST_B3 };
    KlSockAddr dest;
    kl_sockaddr_from_ipv4(&dest, dip, (uint16_t)KL_U9_ECHO_PORT);
    KlDatagramMessage m;
    { unsigned char *p = (unsigned char *)&m; for (size_t i = 0; i < sizeof(m); i++) p[i] = 0; }
    m.data = PROBE; m.len = PROBE_LEN; m.peer = &dest; m.tos = -1;
    KlDatagramSendStatus ss = kl_datagram_send(&dg, &m);
    print("7b9: send = "); print_int((int)ss); print_line("");

    /* Pump the completion loop: the Transmit completes and (over SLIRP) the host echoes back
     * to the armed recv. Stop early once the echo is delivered; bound the wait either way. */
    for (int tick = 0; tick < 200000 && c.recv_count == 0; tick++) {
        kl_event_ctx_run(&ev, 16, 5);
        kl_timer_fire(&ev);
    }
    print("7b9: recv_bytes = "); print_int((int)c.recv_bytes); print_line("");
    print("7b9: recv_matched = "); print_int(c.recv_matched); print_line("");

    /* Graceful close: the send has drained and the re-armed recv is EMPTY → confirmed-detachment
     * close cancels it and settles DETACHED (the 7B-9 path on real firmware). */
    if (kl_datagram_close_begin(&dg) != 0) print_line("7b9: (warn) close_begin failed");
    for (int tick = 0; tick < 200000 && kl_datagram_close_state(&dg) != KL_DGRAM_CLOSE_CLOSED; tick++) {
        kl_event_ctx_run(&ev, 16, 5);
        kl_timer_fire(&ev);
    }
    int cstate  = (int)kl_datagram_close_state(&dg);
    int cresult = (int)kl_datagram_close_result(&dg);
    print("7b9: close_state = "); print_int(cstate); print_line("");
    print("7b9: close_result = "); print_int(cresult); print_line("");
    print("7b9: on_close_fired = "); print_int(c.close_fired); print_line("");

    /* Teardown BEFORE the live-count assertion: a DETACHED close must leave no EFI_UDP4 child
     * live or quarantined. */
    if (cstate == KL_DGRAM_CLOSE_CLOSED) (void)kl_datagram_free(&dg);
    kl_event_ctx_free(&ev);

    int live = kl_uefi_udp_provider_live_count();
    int quar = kl_uefi_udp_provider_quarantined_count();
    print("7b9: udp_live = "); print_int(live);
    print(" udp_quarantined = "); print_int(quar); print_line("");

    if (cstate == KL_DGRAM_CLOSE_CLOSED && cresult == KL_DGRAM_DETACHED &&
        c.recv_matched == 1 && live == 0 && quar == 0)
        print_line("7b9: GO");
    else
        print_line("7b9: NO-GO");

done:
    /* Release the platform's boot-services resources (the periodic monotonic timer + the EBS
     * notify) BEFORE returning — a still-armed periodic EVT_TIMER notify left running as the app
     * returns to the shell / reset -s is a hazard. Idempotent + safe on every (incl. early) exit. */
    kl_uefi_platform_shutdown();
    /* Terminal marker: its ABSENCE in the serial log is the harness's crash/hang signal. */
    print_line("7b9: DONE");
    /* Power the guest off cleanly. BDS boots this image DIRECTLY off the ESP (\EFI\BOOT\
     * BOOTX64.EFI), so returning falls through to the firmware Boot Manager rather than any
     * startup.nsh `reset -s`; ResetSystem(EfiResetShutdown) shuts down deterministically. If the
     * firmware lacks it, fall through to return (the harness BOOT_TIMEOUT is the safety net). */
    if (st->RuntimeServices && st->RuntimeServices->ResetSystem)
        st->RuntimeServices->ResetSystem(EfiResetShutdown, 0 /*EFI_SUCCESS*/, 0, (void *)0);
    return 0;
}
