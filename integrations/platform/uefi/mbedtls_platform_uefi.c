/*
 * mbedtls_platform_uefi.c — the freestanding platform the mbedTLS build
 * needs beyond what libkeel_freestanding_selfcontained.a (kl_cstr_builtin: the
 * mem-family + strlen) and the EFI shims already supply:
 *
 *   (a) mbedtls_hardware_poll()      — entropy for the CTR_DRBG, over EFI_RNG
 *   (b) EFI calloc/free              — mbedTLS heap through AllocatePool/FreePool
 *   (c) `int errno;` + a few libc    — string helpers mbedTLS references that
 *       string/misc functions          kl_cstr_builtin does not define
 *
 * No hosted libc is pulled in; every symbol here is a small freestanding impl or
 * an EFI Boot Services call.
 *
 * ENTROPY POLICY (fail closed by default):
 *   - With EFI_RNG_PROTOCOL present, (a) draws real randomness.
 *   - Without it, (a) FAILS CLOSED (returns nonzero, *olen=0) so the CTR_DRBG seed
 *     fails and NO TLS context is created — UNLESS the build explicitly defines
 *     -DKL_UEFI_INSECURE_TEST_ENTROPY, in which case it falls back to a WEAK, INSECURE
 *     seed (a counter mixed with a boot-services pointer) so a SPIKE demo can proceed
 *     over weak entropy. That macro proves the TRANSPORT, not security; the selftest
 *     prints a loud warning. The reusable adapter (no macro) never fabricates entropy.
 */

#include "mbedtls_platform_uefi.h"
#include "platform_uefi.h"          /* kl_uefi_have_entropy / kl_uefi_have_trustworthy_wallclock */
#include "clock_snapshot.h"         /* kl_uefi_clock_snapshot[_reset] (cert clock, lifetime) */
#include "../../../src/platform.h"     /* kl_plat_random (freestanding platform hook) */

#include <mbedtls/platform.h>       /* mbedtls_platform_set_calloc_free */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>                  /* FILE, fopen/... prototypes (local shim) */
#include <ws2tcpip.h>               /* socklen_t (local shim) for inet_ntop */

/* ── (c) errno global ────────────────────────────────────────────────────────
 * The `errno` global lives in u1_link_stubs.c: the EFI socket provider
 * WRITES errno=EAGAIN/EIO on its would-block/error -1 returns, which the
 * plaintext build — that does NOT link this TU — references too. Defining
 * it in the shared u1_link_stubs.c gives exactly one `errno` per link set. */

/* ── (c) inert <stdio.h> FILE ops ───────────────────────────────────────────
 * tls_mbedtls.c's read_file (file-based cert loader) references these. This build uses the
 * *_from_buf ctx creators exclusively, so this path is DEAD — but the symbols must
 * resolve at link. fopen fails-closed (NULL) so the loader short-circuits; the
 * others are unreachable no-ops. `stderr` is defined by u1_link_stubs.c already, so
 * it is NOT redefined here. */
FILE *fopen(const char *path, const char *mode) { (void)path; (void)mode; return NULL; }
int   fclose(FILE *f) { (void)f; return 0; }
int   fseek(FILE *f, long off, int whence) { (void)f; (void)off; (void)whence; return -1; }
long  ftell(FILE *f) { (void)f; return -1; }
size_t fread(void *ptr, size_t sz, size_t n, FILE *f) { (void)ptr; (void)sz; (void)n; (void)f; return 0; }

/* ── (b) EFI heap: mbedTLS calloc/free over AllocatePool/FreePool ───────────── */

static EFI_BOOT_SERVICES *g_bs;   /* borrowed; set by kl_uefi_mbedtls_platform_init */

static void *uefi_mbed_calloc(size_t nmemb, size_t size) {
    if (!g_bs) return NULL;
    /* After ExitBootServices the Boot Services heap is gone — AllocatePool must
     * never be called. Fail closed so any post-EBS TLS allocation returns NULL rather
     * than dereferencing g_bs->AllocatePool against a torn-down firmware. */
    if (kl_uefi_after_ebs()) return NULL;
    /* overflow guard on nmemb*size */
    size_t total;
    if (size != 0 && nmemb > (size_t)-1 / size) return NULL;
    total = nmemb * size;
    if (total == 0) total = 1;      /* AllocatePool(0) is impl-defined */
    VOID *buf = NULL;
    EFI_STATUS st = g_bs->AllocatePool(EfiBootServicesData, (UINTN)total, &buf);
    if (EFI_ERROR(st) || buf == NULL) return NULL;
    /* calloc contract: zero the region (AllocatePool does not). */
    volatile UINT8 *b = (volatile UINT8 *)buf;
    for (size_t i = 0; i < total; i++) b[i] = 0;
    return buf;
}

static void uefi_mbed_free(void *ptr) {
    if (!ptr) return;
    /* Post-EBS FreePool is undefined — the heap it managed no longer exists.
     * Skip it. Any live mbedTLS object destroyed after ExitBootServices (e.g. a TLS
     * ctx torn down on the shutdown path) thus becomes a deliberate no-op leak rather
     * than a call into torn-down firmware. The correct contract is to destroy all TLS
     * objects BEFORE kl_uefi_shutdown()/ExitBootServices; this guard only makes a
     * contract violation safe instead of fatal. */
    if (kl_uefi_after_ebs()) return;
    if (g_bs) g_bs->FreePool(ptr);   /* FreePool needs no size */
}

int kl_uefi_mbedtls_platform_init(EFI_BOOT_SERVICES *bs) {
    if (g_bs) return 0;         /* idempotent */
    if (!bs) return -1;         /* the EFI heap needs a valid Boot Services table */

    /* Clock gate + capture (fail-closed, enforced HERE so it never depends on application
     * choreography): validate + snapshot UTC once, for the whole TLS-platform lifetime. If the
     * clock is untrustworthy, TLS platform init FAILS → no KlTlsCtx can be created. mbedtls_time()
     * then reads this one snapshot (monotonic-advanced), never GetTime, and it is never refreshed
     * while TLS is active — concurrency-safe across interleaved sessions and immune to a later
     * GetTime failure. (kl_uefi_platform_init must run first so Runtime Services GetTime exists.) */
    if (kl_uefi_clock_snapshot() != 0)
        return -1;

    g_bs = bs;
    if (mbedtls_platform_set_calloc_free(uefi_mbed_calloc, uefi_mbed_free) != 0) {
        g_bs = NULL;
        kl_uefi_clock_snapshot_reset();   /* roll back the snapshot — init did not complete */
        return -1;
    }
    /* The cert-validity clock (Runtime Services GetTime, fail-closed) is bound at compile time
     * via MBEDTLS_PLATFORM_TIME_MACRO (mbedtls_config_uefi.h) + our mbedtls_platform_gmtime_r
     * (time_uefi.c). With MBEDTLS_HAVE_TIME_DATE this makes x509 enforce notBefore/notAfter. */
    return 0;
}

void kl_uefi_mbedtls_platform_shutdown(void) {
    /* Drop the borrowed Boot Services pointer so no later mbedTLS calloc/free can
     * reach firmware. After this, uefi_mbed_calloc returns NULL (no new TLS objects)
     * and uefi_mbed_free is a no-op. Call from the app's shutdown path AFTER every
     * KlTlsCtx/KlTls has been destroyed and BEFORE ExitBootServices. Idempotent. */
    g_bs = NULL;
    kl_uefi_clock_snapshot_reset();   /* clear the cert-clock snapshot (lifetime ends here) */
}

/* mbedTLS's platform.c initializes its default heap function pointers to `calloc`
 * and `free` BY NAME (MBEDTLS_PLATFORM_STD_CALLOC/FREE), so those symbols must link
 * even though kl_uefi_mbedtls_platform_init() overrides the pointers at runtime via
 * mbedtls_platform_set_calloc_free. Route them to the same EFI heap so they are also
 * correct if ever reached before the override. Named exactly `calloc`/`free`. */
void *calloc(size_t nmemb, size_t size);
void *calloc(size_t nmemb, size_t size) { return uefi_mbed_calloc(nmemb, size); }
void free(void *ptr);
void free(void *ptr) { uefi_mbed_free(ptr); }

/* ── (a) entropy: mbedtls_hardware_poll over EFI_RNG (with weak fallback) ──────
 * Lives in entropy_uefi.c so the host mock-EFI harness can link + test the fail-closed
 * contract WITHOUT this TU's libc-clashing residuals (calloc/free/strcmp/snprintf/…).
 * entropy_uefi.c has no mbedTLS dependency; both TUs are compiled by the mbedTLS build. */

/* ── (c) libc string helpers mbedTLS uses that kl_cstr_builtin does not define ─
 * kl_cstr_builtin.c (in the self-contained archive) provides memcpy/memmove/
 * memset/memcmp/strlen. mbedTLS also references the following; simple, correct,
 * freestanding implementations. Compiled with -fno-builtin so they are not lowered
 * back into self-calls. */

char *strchr(const char *s, int c) {
    char ch = (char)c;
    for (;; s++) {
        if (*s == ch) return (char *)s;
        if (*s == '\0') return NULL;
    }
}

char *strrchr(const char *s, int c) {
    char ch = (char)c;
    const char *last = NULL;
    for (;; s++) {
        if (*s == ch) last = s;
        if (*s == '\0') return (char *)last;
    }
}

int strcmp(const char *a, const char *b) {
    while (*a && (*a == *b)) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a[i], cb = (unsigned char)b[i];
        if (ca != cb) return (int)ca - (int)cb;
        if (ca == '\0') return 0;
    }
    return 0;
}

char *strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
    return dst;
}

char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++)) { }
    return dst;
}

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = (const unsigned char *)s;
    unsigned char ch = (unsigned char)c;
    for (size_t i = 0; i < n; i++)
        if (p[i] == ch) return (void *)(p + i);
    return NULL;
}

char *strstr(const char *haystack, const char *needle) {
    if (!*needle) return (char *)haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (char *)haystack;
    }
    return NULL;
}

/* ── (c) inet_ntop — inert stub ─────────────────────────────────────────────
 * tls_mbedtls.c references it only from mTLS peer-cert SAN formatting, which the
 * verify-none client never reaches. Provided so the link resolves. */
const char *inet_ntop(int af, const void *src, char *dst, socklen_t size);
const char *inet_ntop(int af, const void *src, char *dst, socklen_t size) {
    (void)af; (void)src;
    if (dst && size > 0) dst[0] = '\0';
    return NULL;
}

/* ── (c) snprintf — minimal freestanding formatter ──────────────────────────
 * With MBEDTLS_ERROR_C / DEBUG_C off, mbedTLS's snprintf use is minimal (and the
 * paths that use it — error-string / SAN formatting — are unreached here). This
 * supports just the conversions those call sites use: %s, %d/%u, %x, %c, %%, and
 * literal text, always NUL-terminating within @n. Return value follows C99 (the
 * length that WOULD have been written), best-effort. Not a general printf. */
static int snf_emit(char *buf, size_t n, size_t *pos, char c) {
    if (*pos + 1 < n) buf[*pos] = c;
    (*pos)++;
    return 0;
}
static void snf_num(char *buf, size_t n, size_t *pos,
                    unsigned long long v, int base, int is_neg) {
    char tmp[24]; int t = 0;
    static const char digs[] = "0123456789abcdef";
    if (is_neg) snf_emit(buf, n, pos, '-');
    if (v == 0) tmp[t++] = '0';
    while (v && t < (int)sizeof(tmp)) { tmp[t++] = digs[v % (unsigned)base]; v /= (unsigned)base; }
    while (t) snf_emit(buf, n, pos, tmp[--t]);
}

int snprintf(char *buf, size_t n, const char *fmt, ...);
int snprintf(char *buf, size_t n, const char *fmt, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    size_t pos = 0;
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { snf_emit(buf, n, &pos, *p); continue; }
        p++;
        /* skip flags/width/precision/length modifiers we don't honor */
        while (*p && (*p == '-' || *p == '+' || *p == ' ' || *p == '#' || *p == '0' ||
                      (*p >= '1' && *p <= '9') || *p == '.' || *p == 'l' ||
                      *p == 'h' || *p == 'z')) p++;
        switch (*p) {
        case 's': {
            const char *s = __builtin_va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (*s) snf_emit(buf, n, &pos, *s++);
            break;
        }
        case 'd': case 'i': {
            int v = __builtin_va_arg(ap, int);
            unsigned long long u = (v < 0) ? (unsigned long long)(-(long long)v)
                                           : (unsigned long long)v;
            snf_num(buf, n, &pos, u, 10, v < 0);
            break;
        }
        case 'u': snf_num(buf, n, &pos, (unsigned long long)__builtin_va_arg(ap, unsigned), 10, 0); break;
        case 'x':
        case 'X': snf_num(buf, n, &pos, (unsigned long long)__builtin_va_arg(ap, unsigned), 16, 0); break;
        case 'c': snf_emit(buf, n, &pos, (char)__builtin_va_arg(ap, int)); break;
        case '%': snf_emit(buf, n, &pos, '%'); break;
        case '\0': p--; break;   /* trailing % */
        default:  snf_emit(buf, n, &pos, '%'); snf_emit(buf, n, &pos, *p); break;
        }
    }
    __builtin_va_end(ap);
    if (n > 0) buf[(pos < n) ? pos : n - 1] = '\0';
    return (int)pos;
}
