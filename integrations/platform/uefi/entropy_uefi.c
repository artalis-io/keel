/*
 * entropy_uefi.c: mbedtls_hardware_poll() over EFI_RNG.
 *
 * This is the entropy source mbedTLS draws from (MBEDTLS_ENTROPY_HARDWARE_ALT). It has
 * NO mbedTLS dependency (only the freestanding platform hooks kl_uefi_have_entropy()
 * (EFI_RNG present?) and kl_plat_random() (EFI_RNG bytes)), so the host mock-EFI harness
 * links it directly and exercises the fail-closed contract as a real test rather
 * than "by inspection". mbedtls_platform_uefi.c carries the heap + libc residuals (which
 * DO clash with a hosted libc and cannot be linked into the host harness).
 *
 * ENTROPY POLICY (fail closed by default):
 *   - With EFI_RNG_PROTOCOL present (kl_uefi_have_entropy()==1), draw real randomness.
 *   - Without it, FAIL CLOSED (return nonzero, *olen=0) so the CTR_DRBG seed fails and NO
 *     TLS context is created, UNLESS the build explicitly defines
 *     -DKL_UEFI_INSECURE_TEST_ENTROPY, which enables a WEAK, INSECURE seed so a SPIKE demo
 *     can proceed over weak entropy. That macro proves the TRANSPORT, not security; the
 *     reusable adapter (no macro) never fabricates entropy.
 */

#include "platform_uefi.h"          /* kl_uefi_have_entropy */
#include "../../../src/platform.h"     /* kl_plat_random (freestanding platform hook) */

#include <stddef.h>
#include <stdint.h>

int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len,
                          size_t *olen);

int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len,
                          size_t *olen) {
    (void)data;
    if (kl_uefi_have_entropy()) {
        /* Real EFI_RNG-backed randomness. */
        kl_plat_random(output, len);
        if (olen) *olen = len;
        return 0;
    }

#ifdef KL_UEFI_INSECURE_TEST_ENTROPY
    /* *** INSECURE TEST-ONLY SEED: opt-in via -DKL_UEFI_INSECURE_TEST_ENTROPY ***
     * No EFI_RNG in this firmware. kl_plat_random would fail-closed (zero the buffer),
     * which would make the CTR_DRBG deterministic-but-known. Only when a build EXPLICITLY
     * opts in (the SPIKE demo over weak entropy, build_u4.sh spike mode) do we mix a
     * monotonic counter with a pointer value (ASLR-ish) so the bytes at least vary
     * run-to-run. This is NOT cryptographically secure and MUST NOT ship. The selftest
     * warns loudly when kl_uefi_have_entropy() is 0. */
    static uint64_t ctr;
    uintptr_t p = (uintptr_t)(void *)&ctr;       /* address-derived variability */
    for (size_t i = 0; i < len; i++) {
        uint64_t x = ++ctr;
        x ^= p + (uint64_t)i * 0x9E3779B97F4A7C15ULL;
        x *= 0xD1B54A32D192ED03ULL;               /* splitmix-ish diffusion */
        x ^= x >> 31;
        output[i] = (unsigned char)(x & 0xFF);
    }
    if (olen) *olen = len;
    return 0;
#else
    /* FAIL CLOSED by default. The reusable adapter refuses to fabricate entropy:
     * a non-zero return with *olen=0 makes the CTR_DRBG seed FAIL, so no TLS context is
     * created without a real entropy source. A build that wants the insecure demo must
     * define KL_UEFI_INSECURE_TEST_ENTROPY explicitly. */
    (void)output; (void)len;
    if (olen) *olen = 0;
    return -1;
#endif
}
