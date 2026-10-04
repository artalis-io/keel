/*
 * test_platform_random.c: the OS entropy fill (kl_plat_random). It reads the OS cryptographic RNG
 * or fails (-1, buffer zeroed); there is no weak fallback a caller could mistake for entropy.
 */
#include "utest.h"
#include "../src/platform.h"
#include <string.h>

UTEST(platform_random, os_fill_fills) {
    unsigned char a[32], b[32];
    memset(a, 0, sizeof a); memset(b, 0, sizeof b);
    ASSERT_EQ(0, kl_plat_random(a, sizeof a));
    ASSERT_EQ(0, kl_plat_random(b, sizeof b));
    ASSERT_NE(0, memcmp(a, b, sizeof a));
}

UTEST(platform_random, large_fill_is_not_flat) {
    static unsigned char a[4096];                  /* larger than one getrandom(2) atomic read */
    ASSERT_EQ(0, kl_plat_random(a, sizeof a));
    int distinct = 0;
    int seen[256] = {0};
    for (size_t i = 0; i < sizeof a; i++) if (!seen[a[i]]++) distinct++;
    ASSERT_GT(distinct, 200);                      /* all 256 values expected for 4096 bytes */
}

UTEST(platform_random, zero_length_is_ok) {
    unsigned char a[1] = { 0x5A };
    ASSERT_EQ(0, kl_plat_random(a, 0));
    ASSERT_EQ(0x5A, a[0]);
}

#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/resource.h>

/* With the descriptor table full, /dev/urandom cannot be opened. The fill must still come from the
 * OS (getrandom(2) on Linux, arc4random_buf on macOS/BSD), not fail and not degrade. */
static int g_fill[4096];
UTEST(platform_random, fills_without_a_free_descriptor) {
    struct rlimit old_lim, lim;
    int lim_ok = getrlimit(RLIMIT_NOFILE, &old_lim) == 0;
    lim = old_lim;
    if (lim_ok && (lim.rlim_cur == RLIM_INFINITY || lim.rlim_cur > 1024)) {
        lim.rlim_cur = 1024;
        lim_ok = setrlimit(RLIMIT_NOFILE, &lim) == 0;
    }
    int n = 0, exhausted = 0;
    int base = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (base >= 0) {
        while (n < (int)(sizeof(g_fill) / sizeof(g_fill[0]))) {
            int d = fcntl(base, F_DUPFD_CLOEXEC, 0);
            if (d < 0) { exhausted = (errno == EMFILE); break; }
            g_fill[n++] = d;
        }
    }
    unsigned char a[32], b[32];
    int ra = kl_plat_random(a, sizeof a);
    int rb = kl_plat_random(b, sizeof b);
    for (int i = 0; i < n; i++) close(g_fill[i]);
    if (base >= 0) close(base);
    if (lim_ok) (void)setrlimit(RLIMIT_NOFILE, &old_lim);

    ASSERT_TRUE(exhausted);
    ASSERT_EQ(0, ra);
    ASSERT_EQ(0, rb);
    ASSERT_NE(0, memcmp(a, b, sizeof a));
}
#endif

UTEST_MAIN();
