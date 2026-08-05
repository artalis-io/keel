/*
 * u1_selftest.c — U-1 acceptance self-test (UEFI EFI application).
 *
 * Proves the two U-1 shims (allocator_uefi.c + platform_uefi.c) work AND that
 * they satisfy the freestanding archive's undefined platform/allocator hooks,
 * by:
 *   1. linking against libkeel_freestanding_selfcontained.a and REFERENCING the
 *      client public API (kl_client_start &co) so the archive objects — which
 *      reference kl_monotonic_ms / kl_plat_random / kl_resolve_sync — are pulled
 *      into the image and their undefined seams resolved by our shims;
 *   2. exercising the allocator (malloc/realloc-preserve/free + a huge-alloc
 *      NULL path), the monotonic clock (Stall 50 ms, assert a monotonic delta),
 *      and kl_plat_random (fill 16 bytes, report GO / fail-closed).
 *
 * Prints, over ConOut:
 *   U-1: allocator OK
 *   U-1: monotonic OK (dt=<ms>)
 *   U-1: rng <available|fail-closed>
 *   U-1: GO           (only if allocator + monotonic both passed)
 *
 * Freestanding: clang --target=x86_64-unknown-windows, -nostdlib, lld PE,
 * subsystem efi_application. No libc.
 */

#include "efi_uefi.h"
#include "allocator_uefi.h"
#include "platform_uefi.h"

#include <keel/allocator.h>   /* kl_malloc / kl_realloc / kl_free */
#include <keel/client.h>      /* referenced only to pull the archive objects */

#include <stdint.h>
#include <stddef.h>

/* The two freestanding platform hooks (src/platform.h contract), defined by
 * platform_uefi.c. Declared here directly so the self-test needs no internal
 * header (src/platform.h drags in keel/handle.h + the socket surface). */
uint64_t kl_monotonic_ms(void);
void     kl_plat_random(void *buf, size_t len);

/* ── tiny ASCII->UTF-16 console print (no libc, no wchar) ──────────────────── */
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *g_out;

static void print(const char *s) {
    CHAR16 buf[256];
    UINTN i = 0;
    while (s[i] && i < 254) {
        CHAR16 c = (CHAR16)(UINT8)s[i];
        buf[i] = c;
        i++;
    }
    buf[i] = 0;
    if (g_out) g_out->OutputString(g_out, buf);
}

static void print_line(const char *s) { print(s); print("\r\n"); }

/* Append the decimal of @v to @dst (bounded), return new length. */
static UINTN u64_to_dec(UINT64 v, char *dst, UINTN cap) {
    char tmp[24];
    UINTN n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v && n < sizeof(tmp)) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    UINTN o = 0;
    while (n && o < cap - 1) dst[o++] = tmp[--n];
    dst[o] = 0;
    return o;
}

/* ── the archive-reference anchor (link-only; NEVER executed) ───────────────
 * A file-scope, `used` table of client-API addresses. Taking their addresses in
 * static DATA (not in a called function) forces the linker to pull the client
 * objects — and, transitively, llhttp — into the image so the -nostdlib link
 * closure exercises the platform/allocator hooks the shims supply. It is pure
 * data: no code path ever CALLS these functions (U-1 issues no request), so the
 * parser never runs. (An earlier version put this in a called helper; at -O0 the
 * linked llhttp state machine ended up on a live path and #PF'd on UEFI's
 * bounded stack — data-only anchoring keeps it strictly link-time.) */
__attribute__((used))
static void *const g_client_refs[] = {
    (void *)&kl_client_start,
    (void *)&kl_client_start_s,
    (void *)&kl_client_response,
    (void *)&kl_client_error,
    (void *)&kl_client_cancel,
    (void *)&kl_client_free,
};

/* ── allocator round-trip over one size ────────────────────────────────────
 * malloc(N) -> fill pattern -> realloc(2N) -> verify first N survived -> free.
 * Returns 1 on pass, 0 on fail. */
static int alloc_roundtrip(KlAllocator *a, size_t n) {
    unsigned char *p = (unsigned char *)kl_malloc(a, n);
    if (!p) return 0;
    for (size_t i = 0; i < n; i++) p[i] = (unsigned char)((i * 31u + 7u) & 0xff);

    unsigned char *q = (unsigned char *)kl_realloc(a, p, n, n * 2);
    if (!q) { kl_free(a, p, n); return 0; }

    int ok = 1;
    for (size_t i = 0; i < n; i++)
        if (q[i] != (unsigned char)((i * 31u + 7u) & 0xff)) { ok = 0; break; }

    kl_free(a, q, n * 2);
    return ok;
}

/* ── entry ────────────────────────────────────────────────────────────────── */
int efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st);

int efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st) {
    (void)image_handle;
    g_out = st->ConOut;
    EFI_BOOT_SERVICES *bs = st->BootServices;

    print_line("");
    print_line("=== U-1 UEFI platform+allocator self-test ===");

    /* g_client_refs (file scope, `used`) pulls the client + llhttp objects at
     * LINK time — no runtime call here. */

    kl_uefi_platform_trace(g_out); /* step-trace init to the console */
    if (kl_uefi_platform_init(bs, st) != 0) {
        print_line("U-1: FAIL (platform_init: could not create timer)");
        return 1;
    }

    KlAllocator a = kl_uefi_allocator(bs);
    int alloc_ok = 1;

    /* several sizes incl. a large one */
    static const size_t sizes[] = { 16, 64, 1024, 65536, 262144 };
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        char t[48]; UINTN k = 0; const char *p = "  [alloc] size=";
        while (p[k]) { t[k] = p[k]; k++; }
        char n[24]; u64_to_dec(sizes[i], n, sizeof(n));
        UINTN j = 0; while (n[j] && k < sizeof(t) - 3) t[k++] = n[j++];
        t[k++] = '\r'; t[k++] = '\n'; t[k] = 0; print(t);
        if (!alloc_roundtrip(&a, sizes[i])) { alloc_ok = 0; }
    }

    /* AllocatePool-failure handling: an over-large size must yield NULL, no
     * crash. Use a size that exceeds available pool but does NOT overflow the
     * firmware's internal pool arithmetic (a near-SIZE_MAX value makes EDK2's
     * AllocatePool wrap and #PF rather than return EFI_OUT_OF_RESOURCES). One
     * GiB comfortably exceeds this 512 MiB VM's free pool and is safely
     * representable. */
    print_line("  [alloc] over-large (expect NULL)");
    {
        size_t huge = (size_t)1 << 30; /* 1 GiB */
        void *p = kl_malloc(&a, huge);
        if (p != NULL) { alloc_ok = 0; kl_free(&a, p, huge); }
    }

    print_line(alloc_ok ? "U-1: allocator OK" : "U-1: allocator FAIL");

    /* ---- monotonic clock: read, wait ~50 ms, read again ----
     * Wait in 5 ms Stall slices, reading the clock between slices: this both
     * bounds each Stall (giving the periodic timer notify clean dispatch
     * opportunities at each TPL transition) and totals ~50 ms of real time.
     * With the 10 ms clock resolution, dt lands near 50 ms (phase-dependent);
     * the assertion uses a loose >= 30 ms lower bound for TCG jitter. */
    uint64_t t0 = kl_monotonic_ms();
    for (int i = 0; i < 10; i++) { bs->Stall(5000); (void)kl_monotonic_ms(); }
    uint64_t t1 = kl_monotonic_ms();
    uint64_t dt = (t1 >= t0) ? (t1 - t0) : 0;
    int mono_ok = (t1 >= t0) && (dt >= 30); /* monotonic + loose >=30 ms bound for TCG */

    {
        char msg[64]; UINTN k = 0;
        const char *pfx = "U-1: monotonic OK (dt=";
        while (pfx[k]) { msg[k] = pfx[k]; k++; }
        char num[24]; u64_to_dec(dt, num, sizeof(num));
        UINTN j = 0; while (num[j] && k < sizeof(msg) - 2) msg[k++] = num[j++];
        msg[k++] = ')'; msg[k] = 0;
        if (mono_ok) print_line(msg);
        else {
            /* still print the observed dt for diagnosis */
            print("U-1: monotonic FAIL (dt="); print(num); print_line(")");
        }
    }

    /* ---- rng: fill 16 bytes, report source ---- */
    {
        unsigned char rb[16];
        kl_plat_random(rb, sizeof(rb));
        int have = kl_uefi_have_entropy();
        print_line(have ? "U-1: rng available"
                        : "U-1: rng fail-closed");
        (void)rb;
    }

    if (alloc_ok && mono_ok) print_line("U-1: GO");
    else                     print_line("U-1: NO-GO");

    /* Park here so the markers stay the last thing on the serial line and no
     * post-return shell/firmware code (which on some OVMF builds executes an
     * unsupported instruction on `reset`) muddies the capture. The harness ends
     * QEMU via its timeout. */
    print_line("U-1: (parked; harness will terminate QEMU)");
    for (;;) { bs->Stall(1000000); }
    /* not reached */
}
