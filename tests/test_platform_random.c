/*
 * test_platform_random.c: the last-resort entropy fill (kl_plat_random_weak), used when the OS RNG
 * fails. It is not cryptographic, but it must not repeat: the previous fallback depended only on the
 * buffer's address, so a resolver refilling the same pool drew the same DNS transaction ids every time.
 */
#include "utest.h"
#include "../src/platform.h"
#include <string.h>

UTEST(platform_random, weak_fill_differs_between_calls) {
    static unsigned char a[64], b[64];
    kl_plat_random_weak(a, sizeof a);
    uint64_t t0 = kl_monotonic_ms();
    while (kl_monotonic_ms() == t0) { }          /* let the clock move */
    kl_plat_random_weak(b, sizeof b);            /* same length, a buffer at a different address */
    ASSERT_NE(0, memcmp(a, b, sizeof a));
    unsigned char c[64];
    memcpy(c, a, sizeof c);
    t0 = kl_monotonic_ms();
    while (kl_monotonic_ms() == t0) { }
    kl_plat_random_weak(a, sizeof a);            /* the SAME buffer again: was identical */
    ASSERT_NE(0, memcmp(a, c, sizeof a));
}

UTEST(platform_random, weak_fill_is_not_flat) {
    unsigned char a[256];
    kl_plat_random_weak(a, sizeof a);
    int distinct = 0;
    int seen[256] = {0};
    for (size_t i = 0; i < sizeof a; i++) if (!seen[a[i]]++) distinct++;
    ASSERT_GT(distinct, 100);                    /* ~162 expected for 256 uniform bytes */
}

UTEST(platform_random, os_fill_fills) {
    unsigned char a[32], b[32];
    memset(a, 0, sizeof a); memset(b, 0, sizeof b);
    kl_plat_random(a, sizeof a);
    kl_plat_random(b, sizeof b);
    ASSERT_NE(0, memcmp(a, b, sizeof a));
}

UTEST_MAIN();
