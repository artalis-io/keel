/*
 * platform_uefi.c — UEFI backing for kl_monotonic_ms + kl_plat_random.
 *
 * ── Monotonic clock ────────────────────────────────────────────────────────
 * SOURCE: a periodic EVT_TIMER (UEFI 2.10 §7.1 CreateEvent / SetTimer). At
 * kl_uefi_platform_init() we CreateEvent(EVT_TIMER | EVT_NOTIFY_SIGNAL) with a
 * TPL_NOTIFY notify function, then SetTimer(TimerPeriodic, 10000) — a 1 ms
 * period (SetTimer TriggerTime is in 100 ns units, so 10000 * 100 ns = 1 ms).
 * The notify function bumps a static uint64_t tick counter; kl_monotonic_ms()
 * returns it. This is genuinely MONOTONIC (a free-running count, never wall-
 * clock) — unlike GetTime/EFI_TIME which is RTC wall-clock and can jump
 * backward (finding 8). It is exempt from ExitBootServices concerns for the
 * pre-boot client lifetime.
 *
 * RESOLUTION: 1 ms nominal. The firmware timer granularity is typically coarser
 * (OVMF under TCG fires the periodic event on the order of the emulated timer
 * tick), so the counter advances in bursts — the clock is monotonic and its
 * mean rate tracks real time, but a single read may lag real elapsed time by a
 * few ms. That is fine for KEEL's use (timeout deadlines, Happy-Eyeballs delay);
 * none of those need sub-tick precision. If the firmware supports a finer
 * period it will simply tick more often.
 *
 * ── Randomness ─────────────────────────────────────────────────────────────
 * SOURCE: EFI_RNG_PROTOCOL->GetRNG (UEFI 2.10 §37.5), located once at init.
 * FAIL-CLOSED POLICY: if EFI_RNG_PROTOCOL is absent (or GetRNG fails at call
 * time), kl_plat_random ZEROES the output buffer and sets an internal
 * "no entropy" flag queryable via kl_uefi_have_entropy() (returns 0). It does
 * NOT invent a weak address-/clock-derived fallback — a caller that needs real
 * entropy (TLS, later) must check kl_uefi_have_entropy() and refuse to proceed.
 * Zeroing (rather than leaving the buffer uninitialized) makes the failure
 * deterministic and obvious rather than silently seeding with stack garbage.
 */

#include "platform_uefi.h"
#include "wallclock_uefi.h"      /* kl_uefi_wallclock_from_efi (cert validity-time) */
#include "../../../src/platform.h"  /* kl_monotonic_ms / kl_plat_random signatures */

/* ── Module state (installed once by kl_uefi_platform_init) ──────────────────
 * Single-threaded pre-boot environment; the tick counter is only written by the
 * timer notify (which runs at TPL_CALLBACK, serialized against TPL_APPLICATION
 * reads) and read by kl_monotonic_ms. `volatile` so the compiler reloads it on
 * every read rather than caching the value across the notify. */
static EFI_BOOT_SERVICES *g_bs;
static EFI_RUNTIME_SERVICES *g_rt;        /* GetTime (cert clock); valid pre- AND post-EBS */
static EFI_RNG_PROTOCOL  *g_rng;          /* NULL => fail-closed */
static EFI_EVENT          g_timer_event;
static EFI_EVENT          g_ebs_event;    /* EVT_SIGNAL_EXIT_BOOT_SERVICES (U-7) */
static volatile UINT64    g_ticks_ms;     /* monotonic millisecond counter */
static volatile int       g_after_ebs;    /* set by the EBS notify — degrade fail-closed */
static int                g_inited;

/* Optional ConOut trace sink (set by kl_uefi_platform_trace) so init can report
 * exactly which boot-service call it reached — invaluable for bring-up on real
 * firmware. NULL = silent. */
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *g_trace;
static void trace(const char *s) {
    if (!g_trace) return;
    CHAR16 b[96]; UINTN i = 0;
    while (s[i] && i < 94) { b[i] = (CHAR16)(UINT8)s[i]; i++; }
    b[i] = 0;
    g_trace->OutputString(g_trace, b);
}
void kl_uefi_platform_trace(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out) { g_trace = out; }

/* SetTimer TriggerTime is in 100 ns units (UEFI 2.10 §7.1). We arm a 10 ms
 * period: EDK2's timer architectural protocol coalesces very short periods up to
 * its own tick granularity (~10 ms under OVMF), so a 1 ms request actually fires
 * only every ~10 ms — counting those as 1 ms each undercounts by ~10x (observed
 * dt=5 for a 50 ms Stall). A 10 ms period is at/above that granularity, so it
 * fires reliably once per ~10 ms; the notify then advances the clock by the
 * period (10 ms), giving an accurate elapsed-ms count. Resolution: 10 ms. */
#define UEFI_TICK_MS       10ULL
#define UEFI_100NS_PER_MS  10000ULL
#define UEFI_TIMER_PERIOD  (UEFI_TICK_MS * UEFI_100NS_PER_MS)  /* 10 ms in 100 ns */

/* Timer notify: fires every ~10 ms at TPL_NOTIFY, advancing the monotonic
 * counter by the period. EFIAPI (ms_abi) because the firmware calls it. Kept
 * trivial (single add, tiny frame) so it is safe on UEFI's bounded stack. */
static VOID EFIAPI uefi_timer_tick(EFI_EVENT event, VOID *context) {
    (void)event; (void)context;
    g_ticks_ms += UEFI_TICK_MS;
}

/* ExitBootServices notify (U-7): fires once, while boot services are still
 * momentarily usable, when the app calls ExitBootServices(). MUST be minimal — no
 * allocation, no protocol calls — so just latch the degraded state: mark post-EBS
 * (kl_uefi_after_ebs) and drop the RNG pointer (its driver is going away). After
 * this, KEEL's EFI providers refuse further boot-service I/O (fail-closed) and
 * kl_plat_random zeroes. EFIAPI (ms_abi) because the firmware calls it. */
static VOID EFIAPI uefi_exit_boot_services(EFI_EVENT event, VOID *context) {
    (void)event; (void)context;
    g_after_ebs = 1;
    g_rng = NULL;
}

int kl_uefi_platform_init(EFI_BOOT_SERVICES *bs, EFI_SYSTEM_TABLE *st) {
    if (g_inited) return 0;
    if (!bs) return -1;         /* Boot Services table is required (timer/RNG/events) */
    g_bs = bs;
    /* @st may be NULL: non-TLS use runs without Runtime Services GetTime (no cert clock).
     * TLS bring-up then fails closed later (kl_uefi_mbedtls_platform_init snapshots the clock). */
    g_rt = st ? st->RuntimeServices : NULL;   /* GetTime for cert validity-time */
    g_ticks_ms = 0;

    /* ---- monotonic clock: periodic 10 ms EVT_TIMER + NOTIFY_SIGNAL callback --
     * The callback (uefi_timer_tick) fires once per period and advances a
     * counter by the period-in-ms, so kl_monotonic_ms() reads a true elapsed-
     * millisecond count even across a blocking Stall (a CheckEvent-polled bare
     * timer only carries a single signaled bit and cannot count a gap). */
    trace("  [plat] CreateEvent...\r\n");
    EFI_STATUS s = bs->CreateEvent(EVT_TIMER | EVT_NOTIFY_SIGNAL,
                                   TPL_CALLBACK,
                                   (EFI_EVENT_NOTIFY)uefi_timer_tick,
                                   NULL,
                                   &g_timer_event);
    if (EFI_ERROR(s)) { trace("  [plat] CreateEvent FAILED\r\n"); g_timer_event = NULL; goto fail; }
    trace("  [plat] SetTimer...\r\n");
    s = bs->SetTimer(g_timer_event, TimerPeriodic, UEFI_TIMER_PERIOD);
    if (EFI_ERROR(s)) { trace("  [plat] SetTimer FAILED\r\n"); goto fail; }

    /* ---- randomness: locate EFI_RNG_PROTOCOL (optional; fail-closed) ---- */
    trace("  [plat] LocateProtocol(RNG)...\r\n");
    EFI_GUID rng_guid = EFI_RNG_PROTOCOL_GUID;
    VOID *iface = NULL;
    if (!EFI_ERROR(bs->LocateProtocol(&rng_guid, NULL, &iface)) && iface)
        g_rng = (EFI_RNG_PROTOCOL *)iface;
    else
        g_rng = NULL; /* fail-closed */

    /* ---- ExitBootServices lifetime (U-7): register the EBS notify so networking
     * degrades fail-closed the moment the app leaves boot services. Non-fatal if it
     * can't be created (the shutdown/teardown path is the primary guard). ---- */
    trace("  [plat] CreateEvent(EBS)...\r\n");
    s = bs->CreateEvent(EVT_SIGNAL_EXIT_BOOT_SERVICES, TPL_CALLBACK,
                        (EFI_EVENT_NOTIFY)uefi_exit_boot_services, NULL, &g_ebs_event);
    if (EFI_ERROR(s)) { g_ebs_event = NULL; trace("  [plat] EBS event (warn) not registered\r\n"); }
    trace("  [plat] init done\r\n");

    g_inited = 1;
    return 0;

fail:
    /* A failed init must NOT leave a half-installed platform that LOOKS usable: the monotonic
     * timer is what advances kl_monotonic_ms, and the TLS cert clock (a snapshot advanced by the
     * monotonic tick) depends on it. If the timer could not be created/armed, GetTime might still
     * work (g_rt is set) and a snapshot would look valid but FROZEN — accepting a cert past its
     * expiry for the whole lifetime. So tear down every installed handle + pointer; after this
     * kl_uefi_platform_ready() is 0, GetTime is unreachable (g_rt cleared), and TLS bring-up
     * (which snapshots the clock) refuses. */
    if (g_timer_event) { bs->SetTimer(g_timer_event, TimerCancel, 0); bs->CloseEvent(g_timer_event); }
    g_timer_event = NULL;
    g_bs = NULL;
    g_rt = NULL;
    g_rng = NULL;
    g_inited = 0;
    return -1;
}

/* 1 iff kl_uefi_platform_init() FULLY succeeded (monotonic timer armed, so kl_monotonic_ms
 * advances). TLS bring-up requires this because the cert clock is a snapshot advanced by the
 * monotonic tick — a frozen clock would silently accept expired certs. */
int kl_uefi_platform_ready(void) {
    return g_inited;
}

int kl_uefi_have_entropy(void) {
    return g_rng != NULL && !g_after_ebs;
}

/* U-7: 1 once ExitBootServices() has fired — KEEL's EFI providers must stop calling
 * boot services (the TCP4/UDP4 protocols + AllocatePool/events are gone). */
int kl_uefi_after_ebs(void) {
    return g_after_ebs;
}

/* Cert validity-time clock (U-8): read the RTC via Runtime Services GetTime and return UTC
 * seconds since the Unix epoch. GetTime is a RUNTIME service — valid before AND after
 * ExitBootServices — so this is deliberately NOT gated on kl_uefi_after_ebs().
 *
 * Returns 0 with *out_unix set, or -1 FAIL-CLOSED. The trust policy lives in
 * kl_uefi_wallclock_from_efi (wallclock_uefi.c): -1 if there is no RuntimeServices/GetTime,
 * GetTime errors, the EFI_TIME fields are malformed, the year is below KL_UEFI_TIME_FLOOR_YEAR
 * (an unset/stuck RTC — e.g. 1970/2000), or the TimeZone is UNSPECIFIED (which per UEFI §8.3 means
 * LOCAL time with an unknown offset) with no configured offset and no KL_UEFI_ASSUME_UNSPECIFIED_UTC
 * test flag. On a specified zone we normalise local→UTC (UTC = local + TimeZone).
 *
 * How -1 is CONSUMED (do not weaken this): a -1 at kl_uefi_clock_snapshot() (called inside
 * kl_uefi_mbedtls_platform_init) makes TLS platform init FAIL — no KlTlsCtx can be created, so
 * HTTPS is refused up front. mbedtls_time() then reads the captured lifetime SNAPSHOT (advanced by
 * the monotonic clock), NOT GetTime — the epoch-0 return of an *empty* snapshot is only a
 * defence-in-depth backstop, not the primary guard. */
int kl_uefi_wallclock(int64_t *out_unix) {
    EFI_TIME t;
    ZeroMem(&t, sizeof(t));
    int ok = (g_rt && g_rt->GetTime && !EFI_ERROR(g_rt->GetTime(&t, NULL)));
    /* All the trust logic (field validation, unspecified-TZ policy, sanity floor, local→UTC)
     * lives in the host-testable kl_uefi_wallclock_from_efi; here we only make the GetTime call. */
    return kl_uefi_wallclock_from_efi(ok, &t, out_unix);
}

int kl_uefi_have_trustworthy_wallclock(void) {
    int64_t t;
    return kl_uefi_wallclock(&t) == 0;
}

/* U-7: release the platform's boot-services resources (the periodic timer + the EBS
 * event). Call BEFORE ExitBootServices for a clean teardown; idempotent. After this,
 * kl_monotonic_ms freezes at its last value and kl_plat_random fail-closes. */
void kl_uefi_platform_shutdown(void) {
    if (g_bs) {
        if (g_timer_event) {
            g_bs->SetTimer(g_timer_event, TimerCancel, 0);
            g_bs->CloseEvent(g_timer_event);
        }
        if (g_ebs_event)
            g_bs->CloseEvent(g_ebs_event);
    }
    g_timer_event = NULL;
    g_ebs_event = NULL;
    g_rng = NULL;
    g_inited = 0;
}

/* ── kl_monotonic_ms (src/platform.h contract) ──────────────────────────────
 * Return the periodic-timer tick counter (incremented by uefi_timer_tick every
 * 1 ms). Monotonic by construction. */
uint64_t kl_monotonic_ms(void) {
    return (uint64_t)g_ticks_ms;
}

/* ── kl_plat_random (src/platform.h contract) — fail-closed ─────────────── */
void kl_plat_random(void *buf, size_t len) {
    if (len == 0) return;
    if (g_rng != NULL) {
        /* RNGAlgorithm = NULL -> platform default algorithm (UEFI 2.10 §37.5). */
        EFI_STATUS s = g_rng->GetRNG(g_rng, NULL, (UINTN)len, (UINT8 *)buf);
        if (!EFI_ERROR(s)) return;
        /* GetRNG failed at call time: fall through to fail-closed and forget
         * the source so kl_uefi_have_entropy() reports the degraded state. */
        g_rng = NULL;
    }
    /* FAIL-CLOSED: no entropy — zero the buffer, do NOT fabricate weak bytes. */
    {
        volatile UINT8 *b = (volatile UINT8 *)buf;
        for (size_t i = 0; i < len; i++) b[i] = 0;
    }
}
