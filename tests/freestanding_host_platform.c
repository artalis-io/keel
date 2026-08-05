/*
 * freestanding_host_platform.c — F-5 HOST reference implementations of the
 * freestanding platform-hook contract, for the F-7 host mock harness.
 *
 * ─────────────────────────────────────────────────────────────────────────
 * THE FREESTANDING PLATFORM-HOOK CONTRACT
 * ─────────────────────────────────────────────────────────────────────────
 * libkeel_freestanding.a (the client-only, completion-only archive built by
 * `make freestanding-lib`) leaves a documented set of symbols UNDEFINED — the
 * seams an embedder (UEFI, a unikernel, this test harness) must fill. The
 * enforced whitelist lives in tests/freestanding_symbol_gate.sh. They fall in
 * three classes; F-5 concerns the FIRST two:
 *
 *   1. PLATFORM SEAMS — the tiny per-platform services the design flags
 *      (docs/phase10_uefi_feasibility_design.md §6):
 *        - uint64_t kl_monotonic_ms(void)
 *              A monotonic millisecond clock. Drives kl_timer deadlines, the
 *              Happy-Eyeballs attempt delay, and the overall request deadline.
 *              A real freestanding build backs this with an EFI timer tick /
 *              GetTime; here it is a MOCK, advanceable clock (fs_clock_*) so
 *              timeouts are DETERMINISTIC with no sleeps.
 *        - void kl_plat_random(void *buf, size_t len)
 *              Fills `len` bytes with randomness. In the hosted build this is
 *              the DNS query-ID / cookie entropy source; the freestanding
 *              CLIENT archive does not pull the DNS stack, so this is only a
 *              reserved seam (kept defined so an embedder that later adds DNS
 *              has one). A REAL freestanding build MUST fail-closed or use
 *              EFI_RNG_PROTOCOL — a deterministic PRNG like the one below is
 *              TEST-ONLY and cryptographically worthless.
 *        - int kl_resolve_sync(host, port, socktype, out, max, *n)
 *              Blocking name resolution → KlSockAddr. See the long note at its
 *              definition: the async freestanding client resolves via
 *              cfg->resolver (or a numeric literal), so this is only a
 *              reference NUMERIC-ONLY resolver, never blocking DNS.
 *
 *   2. FALLBACK SOCKET DEFAULTS — kl_sockdef_*: the built-in per-op syscall the
 *      inline dispatchers in src/socket.h call when a provider op is NULL. A
 *      freestanding build supplies its own provider (EFI_TCP4); the harness
 *      supplies a full mock provider. These symbols must still be DEFINED (the
 *      inline dispatchers reference them) even though a complete provider means
 *      they are never CALLED — so here they FAIL-CLOSED (no host syscall).
 *
 *   3. EVENT/COMPLETION BUILTIN HOOKS — kl_event_*_builtin / kl_comp_ops_builtin:
 *      the compiled-in event/completion backend the dispatchers fall back to
 *      when loop->ops == NULL. The harness INJECTS a runtime provider
 *      (kl_event_ctx_init_ex → loop->ops != NULL), so these are never reached;
 *      defined fail-closed so the archive links.
 *
 * WHAT AN EMBEDDER MUST IMPLEMENT (minimal set, plaintext HTTP/1.1 client):
 *   - kl_monotonic_ms          (a real monotonic clock)
 *   - a KlSocketProvider       (socket/connect/send/recv/close/get_so_error/
 *                               io_status/set_nonblocking/set_nosigpipe, +
 *                               KL_SOCK_CAP_OVERLAPPED for a completion loop)
 *   - a KlEventProvider with a KlCompletionOps (drain + post_connect + cancel;
 *                               add/mod/del/wait/caps/native_provider)
 *   Nothing more for a plaintext client. TLS/ws would add a KlTls backend +
 *   its entropy (kl_plat_random / EFI_RNG); DNS (U-5) would add a KlResolver or
 *   a real kl_resolve_sync over EFI_UDP4/EFI_DNS4.
 *
 * This TU is TEST/HARNESS-ONLY. It is compiled with the SAME -DKEEL_FREESTANDING
 * as the archive objects, under the host ASan/UBSan toolchain, and linked into
 * tests/freestanding_harness (see `make freestanding-harness`).
 */

#include <keel/sockaddr.h>
#include "../src/socket.h"        /* kl_sockdef_* decls (fail-closed defs below) */
#include "../src/resolve_sync.h"  /* kl_resolve_sync decl */
#include "../src/event_builtin.h" /* kl_event_*_builtin decls (fail-closed defs below) */
#include "../src/completion.h"    /* KlCompletionOps + kl_comp_ops_builtin decl */
#include "freestanding_platform_test.h"  /* fs_clock_* test API */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* ══════════════════════════════════════════════════════════════════════
 * 1a. MOCK MONOTONIC CLOCK — advanceable, deterministic (no sleeps).
 * ══════════════════════════════════════════════════════════════════════
 * A file-scope counter the harness drives with fs_clock_set / fs_clock_advance,
 * so timer deadlines (HE delay, request deadline) fire on demand rather than by
 * wall-clock time. This is the whole reason the timeout scenario is
 * deterministic: the harness advances the clock past the deadline, then pumps
 * the loop, and the deadline timer fires exactly once. */
static uint64_t g_fs_now_ms = 1000;   /* start non-zero (0 is "clock failed" in some paths) */

uint64_t kl_monotonic_ms(void) { return g_fs_now_ms; }

/* Test API (declared in freestanding_platform_test.h). */
void fs_clock_set(uint64_t ms)     { g_fs_now_ms = ms; }
void fs_clock_advance(uint64_t ms) { g_fs_now_ms += ms; }
uint64_t fs_clock_now(void)        { return g_fs_now_ms; }

/* ══════════════════════════════════════════════════════════════════════
 * 1b. DETERMINISTIC PRNG — TEST ONLY (a real build fails-closed / uses EFI_RNG).
 * ══════════════════════════════════════════════════════════════════════
 * A splitmix64-style deterministic fill. NOT cryptographic. The freestanding
 * CLIENT archive does not currently reference kl_plat_random (no DNS/TLS in the
 * manifest), but it is defined as the reserved platform seam so the contract is
 * complete and an embedder adding DNS/TLS sees the shape. */
static uint64_t g_fs_prng = 0x9E3779B97F4A7C15ull;

void kl_plat_random(void *buf, size_t len) {
    unsigned char *p = buf;
    for (size_t i = 0; i < len; i++) {
        g_fs_prng += 0x9E3779B97F4A7C15ull;
        uint64_t z = g_fs_prng;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z = z ^ (z >> 31);
        p[i] = (unsigned char)z;
    }
}

/* ══════════════════════════════════════════════════════════════════════
 * 1c. kl_resolve_sync — NUMERIC-ONLY reference resolver (never blocking DNS).
 * ══════════════════════════════════════════════════════════════════════
 * WHY MOCKED, NOT TRIMMED. The archive references kl_resolve_sync (undefined,
 * whitelisted): client_async.c's sync-DNS FALLBACK path — reached when
 * client_pick_resolver returns NULL — still calls it, and under KEEL_FREESTANDING
 * client_pick_resolver DOES return NULL (the built-in DNS-over-UDP resolver is
 * out of the minimal archive, §8). So the symbol is a genuine link dependency of
 * the compiled client, even though a consumer that passes cfg->resolver never
 * EXECUTES it.
 *
 * Trimming it would mean #ifdef-ing out both call sites AND their surrounding
 * fallback blocks in client_async.c — a real src change — and would leave a
 * freestanding client unable to dial a NUMERIC address literal without a
 * resolver (numeric IPs also flow through this path in the fallback). The design
 * (§F-0) deliberately keeps kl_resolve_sync on the whitelist as "the tiny
 * platform seam"; the right move is to PROVIDE it as a numeric-only reference
 * impl (no getaddrinfo, no host DNS) — which is exactly what a minimal
 * freestanding embedder would ship pre-DNS (U-5). Reported in the deliverable.
 *
 * Parses a dotted-quad IPv4 literal; any non-numeric host fails (a freestanding
 * client with no resolver cannot resolve names — the correct behavior). */
static int parse_ipv4(const char *host, uint8_t out[4]) {
    int oct = 0, val = -1;
    const char *p = host;
    for (;;) {
        if (*p >= '0' && *p <= '9') {
            if (val < 0) val = 0;
            val = val * 10 + (*p - '0');
            if (val > 255) return -1;
        } else if (*p == '.' || *p == '\0') {
            if (val < 0 || oct > 3) return -1;
            out[oct++] = (uint8_t)val;
            val = -1;
            if (*p == '\0') break;
        } else {
            return -1;   /* non-numeric host — no DNS in a freestanding client */
        }
        p++;
    }
    return oct == 4 ? 0 : -1;
}

int kl_resolve_sync(const char *host, uint16_t port, int socktype,
                    KlSockAddr *out, int max, int *n) {
    (void)socktype;
    if (!host || !out || max <= 0 || !n)
        return -1;
    uint8_t ip[4];
    if (parse_ipv4(host, ip) != 0)
        return -1;
    if (kl_sockaddr_from_ipv4(&out[0], ip, port) != 0)
        return -1;
    *n = 1;
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════
 * 2. FALLBACK SOCKET DEFAULTS — fail-closed (the mock provider sets all ops).
 * ══════════════════════════════════════════════════════════════════════
 * These are the kl_sockdef_* the src/socket.h inline dispatchers call when a
 * provider op is NULL. The harness's mock provider supplies every op it uses, so
 * these are never CALLED — but they must be DEFINED for the archive to link. Any
 * accidental reach here (a provider op left NULL) fails loudly rather than
 * silently doing a host syscall on a "freestanding" build. */
KlSocketHandle kl_sockdef_socket(int d, int t, int p) { (void)d;(void)t;(void)p; return KL_INVALID_SOCKET; }
int  kl_sockdef_connect(KlSocketHandle f, const KlSockAddr *a) { (void)f;(void)a; return -1; }
int  kl_sockdef_close(KlSocketHandle f) { (void)f; return -1; }
int  kl_sockdef_set_nonblocking(KlSocketHandle f) { (void)f; return -1; }
void kl_sockdef_set_nosigpipe(KlSocketHandle f) { (void)f; }
int  kl_sockdef_get_so_error(KlSocketHandle f, int *e) { (void)f; if (e) *e = 0; return -1; }
KlIoStatus kl_sockdef_io_status(void) { return KL_IO_FATAL; }

/* recv/send/recv_peek return ssize_t (src/socket.h → sockcompat.h); fail-closed. */
ssize_t kl_sockdef_send(KlSocketHandle f, const void *b, size_t n) { (void)f;(void)b;(void)n; return -1; }
ssize_t kl_sockdef_recv(KlSocketHandle f, void *b, size_t n) { (void)f;(void)b;(void)n; return -1; }
ssize_t kl_sockdef_recv_peek(KlSocketHandle f, void *b, size_t n) { (void)f;(void)b;(void)n; return -1; }

/* ══════════════════════════════════════════════════════════════════════
 * 3. EVENT/COMPLETION BUILTIN HOOKS — fail-closed (harness injects a provider).
 * ══════════════════════════════════════════════════════════════════════
 * The compiled-in event/completion backend the dispatchers fall back to on the
 * loop->ops == NULL path. The harness ALWAYS installs a runtime provider via
 * kl_event_ctx_init_ex (→ loop->ops != NULL), so none of these are reached; they
 * exist only so the archive links. */
int  kl_event_init_builtin(KlEventLoop *loop) { (void)loop; return -1; }
int  kl_event_add_builtin(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata) {
    (void)loop;(void)fd;(void)mask;(void)udata; return -1;
}
int  kl_event_mod_builtin(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata) {
    (void)loop;(void)fd;(void)mask;(void)udata; return -1;
}
int  kl_event_del_builtin(KlEventLoop *loop, KlSocketHandle fd) { (void)loop;(void)fd; return -1; }
int  kl_event_wait_builtin(KlEventLoop *loop, KlEvent *out, int max, int timeout_ms) {
    (void)loop;(void)out;(void)max;(void)timeout_ms; return -1;
}
void kl_event_close_builtin(KlEventLoop *loop) { (void)loop; }
unsigned kl_event_caps_builtin(const KlEventLoop *loop) { (void)loop; return 0; }
const struct KlSocketProvider *kl_event_native_provider_builtin(const KlEventLoop *loop) {
    (void)loop; return NULL;
}

/* The compiled-in completion backend's vtable. NULL is safe here: the dispatchers
 * only consult kl_comp_ops_builtin() on the loop->ops == NULL path, which the
 * harness never takes (it injects a provider carrying its own completion ops). */
const KlCompletionOps *kl_comp_ops_builtin(void) { return NULL; }
