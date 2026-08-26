/*
 * test_kl_cstr_builtin.c: the OPTIONAL reference mem* and strlen impls
 * (src/kl_cstr_builtin.c) linked only into the self-contained freestanding
 * archive. They carry the standard libc names, so to unit-test them on a hosted
 * build without a duplicate-symbol clash we pull the .c in under klb_* names and
 * cross-check each against the platform libc (the oracle).
 */
#include "utest.h"
#include <string.h>   /* libc mem* and strlen: the reference oracle (declared before the renames) */
#include <stdint.h>

/* Rename the reference impls so they don't collide with libc, then compile them
 * into this TU. libc/FORTIFY may already macro-define mem* (e.g. __memcpy_chk),
 * so #undef first to avoid -Wmacro-redefined. After the include, klb_* are
 * defined and libc mem* is restored. */
#undef memcpy
#undef memmove
#undef memset
#undef memcmp
#undef strlen
#define memcpy  klb_memcpy
#define memmove klb_memmove
#define memset  klb_memset
#define memcmp  klb_memcmp
#define strlen  klb_strlen
#include "../src/kl_cstr_builtin.c"
#undef memcpy
#undef memmove
#undef memset
#undef memcmp
#undef strlen

static int sgn(int v) { return (v > 0) - (v < 0); }

UTEST(kl_cstr_builtin, memcpy_basic) {
    unsigned char src[64], a[64], b[64];
    for (int i = 0; i < 64; i++) src[i] = (unsigned char)(i * 7 + 1);
    memset(a, 0xAA, sizeof(a)); memset(b, 0xAA, sizeof(b));   /* libc memset here */
    for (size_t n = 0; n <= 40; n++) {
        void *r = klb_memcpy(a, src, n);
        memcpy(b, src, n);                                    /* libc */
        ASSERT_EQ(r, (void *)a);                              /* returns dst */
        ASSERT_EQ(memcmp(a, b, sizeof(a)), 0);               /* incl. bytes past n untouched */
    }
}

UTEST(kl_cstr_builtin, memset_basic) {
    unsigned char a[64], b[64];
    for (int c = 0; c < 260; c += 37) {
        for (size_t n = 0; n <= 48; n++) {
            memset(a, 0x11, sizeof(a)); memset(b, 0x11, sizeof(b));  /* libc baseline */
            void *r = klb_memset(a, c, n);
            memset(b, c, n);                                          /* libc */
            ASSERT_EQ(r, (void *)a);
            ASSERT_EQ(memcmp(a, b, sizeof(a)), 0);
        }
    }
}

UTEST(kl_cstr_builtin, memcmp_ordering) {
    ASSERT_EQ(klb_memcmp("abc", "abc", 3), 0);
    ASSERT_EQ(klb_memcmp("abc", "abd", 0), 0);                /* n=0 → equal */
    ASSERT_EQ(sgn(klb_memcmp("abc", "abd", 3)), sgn(memcmp("abc", "abd", 3))); /* <0 */
    ASSERT_EQ(sgn(klb_memcmp("abd", "abc", 3)), sgn(memcmp("abd", "abc", 3))); /* >0 */
    /* High-bit bytes must compare as UNSIGNED (0x80 > 0x7f), like libc. */
    unsigned char x[1] = {0x80}, y[1] = {0x7f};
    ASSERT_EQ(sgn(klb_memcmp(x, y, 1)), sgn(memcmp(x, y, 1)));   /* > 0 */
    ASSERT_GT(klb_memcmp(x, y, 1), 0);
}

/* The correctness-critical case: overlapping moves in BOTH directions. */
UTEST(kl_cstr_builtin, memmove_overlap) {
    for (size_t n = 0; n <= 8; n++) {
        for (int shift = -4; shift <= 4; shift++) {
            unsigned char base[32];
            for (int i = 0; i < 32; i++) base[i] = (unsigned char)(i + 1);
            unsigned char a[32], b[32];
            memcpy(a, base, 32); memcpy(b, base, 32);       /* libc */
            size_t srcpos = 12;
            size_t dstpos = (size_t)((int)srcpos + shift);
            void *r = klb_memmove(a + dstpos, a + srcpos, n);
            memmove(b + dstpos, b + srcpos, n);              /* libc oracle */
            ASSERT_EQ(r, (void *)(a + dstpos));
            ASSERT_EQ(memcmp(a, b, 32), 0);                  /* whole buffer identical */
        }
    }
    /* dst == src is a no-op returning dst. */
    unsigned char buf[4] = {1,2,3,4};
    ASSERT_EQ(klb_memmove(buf, buf, 4), (void *)buf);
    ASSERT_EQ(buf[0], 1); ASSERT_EQ(buf[3], 4);
}

UTEST(kl_cstr_builtin, strlen_basic) {
    const char *cases[] = { "", "a", "hello", "GET / HTTP/1.1\r\n", "\0hidden" };
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++)
        ASSERT_EQ(klb_strlen(cases[i]), strlen(cases[i]));   /* incl. "" → 0 */
}

UTEST_MAIN();
