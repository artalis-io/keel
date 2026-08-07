/*
 * u7_selftest.c — U-7 acceptance: ExitBootServices lifetime / clean EFI teardown.
 *
 * Does the U-3 plaintext GET, then exercises the boot-services-lifetime path:
 *   kl_uefi_shutdown()  → releases the EFI_TCP4 socket provider, the completion event
 *                         provider, and the platform (periodic monotonic timer + the
 *                         EVT_SIGNAL_EXIT_BOOT_SERVICES event).
 * and OBSERVABLY proves the teardown happened: the monotonic clock, driven by the
 * periodic timer, FREEZES after shutdown (a Stall no longer advances it) — the timer
 * event was really cancelled + closed. A clean run through teardown (no #PF / reset)
 * proves there is no double-free / use-after-free in the release path.
 *
 * The EVT_SIGNAL_EXIT_BOOT_SERVICES notify (platform_uefi.c) + the socket provider's
 * kl_uefi_after_ebs() guard are the automatic fail-closed net: a real
 * ExitBootServices() sets after_ebs, after which KEEL refuses boot-service I/O. We do
 * NOT call the real ExitBootServices() here — there is no serial console after it, so
 * the run would be unobservable; the teardown above is the observable acceptance.
 *
 * Freestanding: clang --target=x86_64-unknown-windows, -nostdlib, lld PE. No libc.
 */

#include "efi_uefi.h"
#include "platform_uefi.h"
#include "socket_efi_tcp4.h"
#include "event_efi.h"
#include "allocator_uefi.h"
#include "lifecycle_uefi.h"

#include <keel/client.h>
#include <keel/event_ctx.h>
#include <keel/timer.h>
#include <keel/error.h>

#include <stdint.h>
#include <stddef.h>

/* The freestanding monotonic-clock hook (src/platform.h), backed by platform_uefi.c's
 * periodic timer. Declared locally to observe the clock freezing after teardown. */
uint64_t kl_monotonic_ms(void);

#ifndef TARGET_PORT
#define TARGET_PORT 18080
#endif
#ifndef TARGET_HOST
#define TARGET_HOST "10.0.2.2"
#endif
#define STR2(x) #x
#define STR(x) STR2(x)
#define TARGET_URL "http://" TARGET_HOST ":" STR(TARGET_PORT) "/"

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

typedef struct { int done; int status; KlError err; } DoneCtx;
static void on_done(KlClient *cl, void *ud) {
    DoneCtx *d = ud;
    const KlClientResponse *r = kl_client_response(cl);
    d->status = r ? r->status : -1;
    d->err = kl_client_last_error(cl);
    d->done = 1;
}

int efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st);
int efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st) {
    g_out = st->ConOut;
    EFI_BOOT_SERVICES *bs = st->BootServices;

    print_line("");
    print_line("=== U-7 ExitBootServices lifetime / clean EFI teardown ===");
    print("U-7: target = "); print_line(TARGET_URL);

    if (kl_uefi_platform_init(bs, st) != 0)
        print_line("U-7: (warn) platform_init failed — continuing");

    const KlEventProvider *ep = kl_uefi_event_provider(bs, image_handle);
    if (!ep) { print_line("U-7: event provider build failed"); goto park; }

    KlAllocator alloc = kl_uefi_allocator(bs);
    KlEventCtx ev;
    if (kl_event_ctx_init_ex(&ev, &alloc, ep) != 0) {
        print_line("U-7: kl_event_ctx_init_ex failed");
        print_line("U-7: NO-GO-YET (needs network-enabled OVMF)");
        goto park;
    }

    KlClientConfig cfg;
    { unsigned char *p = (unsigned char *)&cfg; for (size_t i = 0; i < sizeof(cfg); i++) p[i] = 0; }
    cfg.timeout_ms = 20000;
    DoneCtx d;
    { unsigned char *p = (unsigned char *)&d; for (size_t i = 0; i < sizeof(d); i++) p[i] = 0; }

    KlClient *c = kl_client_start(&ev, &alloc, &cfg, "GET", TARGET_URL,
                                  NULL, 0, NULL, 0, on_done, &d);
    if (!c) { print_line("U-7: kl_client_start failed"); kl_event_ctx_free(&ev); goto park; }
    print_line("U-7: client started; pumping...");
    for (int tick = 0; tick < 200000 && !d.done; tick++) {
        kl_event_ctx_run(&ev, 16, 10);
        kl_timer_fire(&ev);
    }
    if (d.done) { print("U-7: status = "); print_int(d.status); print_line(""); }
    else        print_line("U-7: did not complete");

    kl_client_free(c);
    kl_event_ctx_free(&ev);

    /* ── the U-7 payload: clean boot-services teardown, observably proven ── */
    /* F6: the lifecycle contract is that every opened socket is closed before shutdown.
     * The async client closes its fd on completion, so this must read 0 here — prove it. */
    int live = kl_uefi_socket_provider_live_count();
    print("U-7: live socket slots before shutdown = "); print_int(live);
    print_line(live == 0 ? " (contract OK — all closed)"
                         : " (WARN: contract violation — slots preserved, not reclaimed)");
    print_line("U-7: releasing EFI network stack (kl_uefi_shutdown)...");
    uint64_t t0 = kl_monotonic_ms();
    kl_uefi_shutdown();                 /* socket provider + event provider + platform */
    print_line("U-7: clean teardown OK — socket/event providers + monotonic timer released");

    /* The monotonic clock is driven by the periodic timer we just cancelled+closed;
     * after shutdown it must FREEZE. Stall ~50 ms and confirm it did not advance —
     * observable proof the timer event was truly released, not merely leaked. */
    bs->Stall(50000);
    uint64_t t1 = kl_monotonic_ms();
    int frozen = (t1 == t0);
    print("U-7: monotonic clock after shutdown: dt="); print_int((int)(t1 - t0));
    print_line(frozen ? " (frozen — timer released)" : " (WARN: still advancing)");

    print("U-7: after_ebs guard = "); print_int(kl_uefi_after_ebs());
    print_line(" (0 pre-EBS; a real ExitBootServices sets it → providers fail-closed)");

    if (d.done && d.status == 200 && frozen)
        print_line("U-7: GO");
    else
        print_line("U-7: NO-GO-YET");

park:
    print_line("U-7: (parked; harness will terminate QEMU)");
    for (;;) { bs->Stall(1000000); }
}
