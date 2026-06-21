/*
 * test_sh_seal_arena.c — Direct API tests for the shared sealed-arena
 * vendored utility (vendor/sh_seal_arena/).
 *
 * Covers:
 *   1. init / destroy lifecycle (success + failure cases)
 *   2. allocation: alignment, overflow guards, capacity
 *   3. seal transition: read still works, alloc fails
 *   4. process-isolated death test: writes to sealed memory SIGSEGV
 *   5. zero-size + NULL guards
 *   6. round-trip via strdup / memdup
 *
 * Keel uses sh_seal_arena via kl_router_freeze, but the freeze
 * tests in test_router.c exercise the primitive only indirectly.
 * These tests are the direct-API coverage Keel needs so a regression
 * in the shared vendor lib is caught here (not just by downstream
 * consumers).  Mirrors Hull's tests/hull/test_seal_arena.c — keep
 * the two in sync when extending.
 *
 * SPDX-License-Identifier: MIT
 */

#include "utest.h"
#include <sh_seal_arena.h>

#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* ── lifecycle ─────────────────────────────────────────────────────── */

UTEST(seal_arena, init_destroy_roundtrip)
{
    ShSealArena a;
    ASSERT_EQ(0, sh_seal_arena_init(&a, 4096, "test"));
    ASSERT_TRUE(a.base != NULL);
    ASSERT_TRUE(a.capacity >= 4096);
    ASSERT_EQ((size_t)0, a.used);
    ASSERT_EQ(0, a.sealed);
    sh_seal_arena_destroy(&a);
    /* destroy zeroes the struct */
    ASSERT_TRUE(a.base == NULL);
    ASSERT_EQ((size_t)0, a.capacity);
}

UTEST(seal_arena, init_zero_size_rounds_up_to_one_page)
{
    ShSealArena a;
    ASSERT_EQ(0, sh_seal_arena_init(&a, 0, NULL));
    ASSERT_TRUE(a.capacity >= 4096); /* page size on every supported platform */
    sh_seal_arena_destroy(&a);
}

UTEST(seal_arena, init_null_arena_fails)
{
    ASSERT_EQ(-1, sh_seal_arena_init(NULL, 4096, "x"));
}

UTEST(seal_arena, destroy_null_is_safe)
{
    sh_seal_arena_destroy(NULL); /* must not crash */
    ShSealArena zero = {0};
    sh_seal_arena_destroy(&zero); /* must not crash when base==NULL */
    ASSERT_TRUE(zero.base == NULL); /* exercise the assertion macro so
                                       Keel's -Werror=unused-parameter
                                       doesn't flag the utest-injected
                                       `utest_result` arg */
}

/* ── allocation ────────────────────────────────────────────────────── */

UTEST(seal_arena, alloc_basic)
{
    ShSealArena a;
    ASSERT_EQ(0, sh_seal_arena_init(&a, 4096, NULL));
    void *p = sh_seal_arena_alloc(&a, 100, 8);
    ASSERT_TRUE(p != NULL);
    ASSERT_EQ(0u, (uintptr_t)p & 7u); /* 8-aligned */
    sh_seal_arena_destroy(&a);
}

UTEST(seal_arena, alloc_alignment_respected)
{
    ShSealArena a;
    ASSERT_EQ(0, sh_seal_arena_init(&a, 4096, NULL));
    (void)sh_seal_arena_alloc(&a, 1, 1);                /* misalign */
    void *p16 = sh_seal_arena_alloc(&a, 64, 16);
    ASSERT_EQ(0u, (uintptr_t)p16 & 15u);
    void *p32 = sh_seal_arena_alloc(&a, 64, 32);
    ASSERT_EQ(0u, (uintptr_t)p32 & 31u);
    sh_seal_arena_destroy(&a);
}

UTEST(seal_arena, alloc_rejects_non_power_of_two_align)
{
    ShSealArena a;
    ASSERT_EQ(0, sh_seal_arena_init(&a, 4096, NULL));
    ASSERT_TRUE(sh_seal_arena_alloc(&a, 16, 3) == NULL);
    ASSERT_TRUE(sh_seal_arena_alloc(&a, 16, 5) == NULL);
    ASSERT_TRUE(sh_seal_arena_alloc(&a, 16, 0) == NULL);
    sh_seal_arena_destroy(&a);
}

UTEST(seal_arena, alloc_rejects_zero_size)
{
    ShSealArena a;
    ASSERT_EQ(0, sh_seal_arena_init(&a, 4096, NULL));
    ASSERT_TRUE(sh_seal_arena_alloc(&a, 0, 8) == NULL);
    sh_seal_arena_destroy(&a);
}

UTEST(seal_arena, alloc_returns_null_when_oom)
{
    ShSealArena a;
    ASSERT_EQ(0, sh_seal_arena_init(&a, 4096, NULL));
    ASSERT_TRUE(sh_seal_arena_alloc(&a, a.capacity + 1, 8) == NULL);
    /* Filling exactly: should succeed */
    void *p = sh_seal_arena_alloc(&a, a.capacity, 1);
    ASSERT_TRUE(p != NULL);
    /* One more byte: should fail */
    ASSERT_TRUE(sh_seal_arena_alloc(&a, 1, 1) == NULL);
    sh_seal_arena_destroy(&a);
}

UTEST(seal_arena, alloc_rejects_overflow_size)
{
    ShSealArena a;
    ASSERT_EQ(0, sh_seal_arena_init(&a, 4096, NULL));
    ASSERT_TRUE(sh_seal_arena_alloc(&a, SIZE_MAX, 8) == NULL);
    ASSERT_TRUE(sh_seal_arena_alloc(&a, SIZE_MAX - 8, 16) == NULL);
    sh_seal_arena_destroy(&a);
}

UTEST(seal_arena, alloc_null_arena_returns_null)
{
    ASSERT_TRUE(sh_seal_arena_alloc(NULL, 16, 8) == NULL);
}

/* ── strdup / memdup ──────────────────────────────────────────────── */

UTEST(seal_arena, strdup_copies_bytes_and_nul_terminates)
{
    ShSealArena a;
    ASSERT_EQ(0, sh_seal_arena_init(&a, 4096, NULL));
    const char *src = "hello world";
    char *dst = sh_seal_arena_strdup(&a, src);
    ASSERT_TRUE(dst != NULL);
    ASSERT_STREQ(src, dst);
    ASSERT_TRUE(dst != src); /* copy, not alias */
    sh_seal_arena_destroy(&a);
}

UTEST(seal_arena, strdup_null_returns_null)
{
    ShSealArena a;
    ASSERT_EQ(0, sh_seal_arena_init(&a, 4096, NULL));
    ASSERT_TRUE(sh_seal_arena_strdup(&a, NULL) == NULL);
    sh_seal_arena_destroy(&a);
}

UTEST(seal_arena, memdup_binary_safe)
{
    ShSealArena a;
    ASSERT_EQ(0, sh_seal_arena_init(&a, 4096, NULL));
    const unsigned char src[] = { 0x00, 0xff, 0x42, 0x00, 0x13 };
    void *dst = sh_seal_arena_memdup(&a, src, sizeof src);
    ASSERT_TRUE(dst != NULL);
    ASSERT_EQ(0, memcmp(src, dst, sizeof src));
    sh_seal_arena_destroy(&a);
}

UTEST(seal_arena, memdup_nt_appends_nul_and_is_strlen_safe)
{
    ShSealArena a;
    ASSERT_EQ(0, sh_seal_arena_init(&a, 4096, NULL));
    const char src[] = "hello"; /* 5 bytes + the implicit NUL we don't pass */
    char *dst = (char *)sh_seal_arena_memdup_nt(&a, src, 5);
    ASSERT_TRUE(dst != NULL);
    ASSERT_EQ(0, memcmp(src, dst, 5));
    ASSERT_EQ('\0', dst[5]);                /* trailing NUL */
    ASSERT_EQ((size_t)5, strlen(dst));      /* C-string safe */
    sh_seal_arena_destroy(&a);
}

UTEST(seal_arena, memdup_nt_zero_length_yields_empty_string)
{
    ShSealArena a;
    ASSERT_EQ(0, sh_seal_arena_init(&a, 4096, NULL));
    char *dst = (char *)sh_seal_arena_memdup_nt(&a, "anything", 0);
    ASSERT_TRUE(dst != NULL);
    ASSERT_EQ('\0', dst[0]);
    ASSERT_EQ((size_t)0, strlen(dst));
    sh_seal_arena_destroy(&a);
}

UTEST(seal_arena, memdup_nt_null_src_returns_null)
{
    ShSealArena a;
    ASSERT_EQ(0, sh_seal_arena_init(&a, 4096, NULL));
    ASSERT_TRUE(sh_seal_arena_memdup_nt(&a, NULL, 10) == NULL);
    sh_seal_arena_destroy(&a);
}

UTEST(seal_arena, memdup_nt_rejects_size_max)
{
    ShSealArena a;
    ASSERT_EQ(0, sh_seal_arena_init(&a, 4096, NULL));
    /* len == SIZE_MAX would overflow len+1; the helper rejects. */
    ASSERT_TRUE(sh_seal_arena_memdup_nt(&a, "x", SIZE_MAX) == NULL);
    sh_seal_arena_destroy(&a);
}

/* ── seal lifecycle ────────────────────────────────────────────────── */

UTEST(seal_arena, seal_blocks_subsequent_allocs)
{
    ShSealArena a;
    ASSERT_EQ(0, sh_seal_arena_init(&a, 4096, NULL));
    char *p = sh_seal_arena_strdup(&a, "before-seal");
    ASSERT_TRUE(p != NULL);
    ASSERT_EQ(0, sh_seal_arena_seal(&a));
    ASSERT_EQ(1, sh_seal_arena_is_sealed(&a));

    /* alloc must fail post-seal */
    ASSERT_TRUE(sh_seal_arena_alloc(&a, 1, 1) == NULL);
    ASSERT_TRUE(sh_seal_arena_strdup(&a, "after") == NULL);

    /* reads must still work */
    ASSERT_STREQ("before-seal", p);

    sh_seal_arena_destroy(&a);
}

UTEST(seal_arena, double_seal_returns_error)
{
    ShSealArena a;
    ASSERT_EQ(0, sh_seal_arena_init(&a, 4096, NULL));
    ASSERT_EQ(0, sh_seal_arena_seal(&a));
    ASSERT_EQ(-1, sh_seal_arena_seal(&a)); /* re-seal is a programming bug */
    sh_seal_arena_destroy(&a);
}

UTEST(seal_arena, is_sealed_handles_null)
{
    ASSERT_EQ(0, sh_seal_arena_is_sealed(NULL));
    ShSealArena zero = {0};
    ASSERT_EQ(0, sh_seal_arena_is_sealed(&zero));
}

UTEST(seal_arena, destroy_after_seal_works)
{
    ShSealArena a;
    ASSERT_EQ(0, sh_seal_arena_init(&a, 4096, NULL));
    ASSERT_EQ(0, sh_seal_arena_seal(&a));
    sh_seal_arena_destroy(&a);
    ASSERT_TRUE(a.base == NULL);
}

/* ── death test: page protection actually works ───────────────────── */

/* Fork-and-segfault test. The child writes to sealed memory; the
 * parent waits and asserts the child died with SIGSEGV (or SIGBUS on
 * some platforms — kernel's choice for protection violations). If the
 * mprotect didn't take effect, the child would write happily and exit
 * 0, and the test would fail. This is the only way to verify that the
 * OS protection is REAL without crashing the test runner. */
UTEST(seal_arena, write_after_seal_faults)
{
    ShSealArena a;
    ASSERT_EQ(0, sh_seal_arena_init(&a, 4096, NULL));
    char *p = sh_seal_arena_strdup(&a, "do-not-overwrite");
    ASSERT_TRUE(p != NULL);
    ASSERT_EQ(0, sh_seal_arena_seal(&a));

    pid_t pid = fork();
    if (pid == 0) {
        /* child: reset SEGV/BUS handlers to SIG_DFL so the signal
         * propagates as a termination (WIFSIGNALED) instead of being
         * caught by a sanitizer runtime (ASan/MSan install their own
         * handlers that print a diagnostic and _exit(1), which would
         * make WIFSIGNALED false). Then write into the sealed page;
         * exit 0 if it somehow succeeded (never expected). */
        signal(SIGSEGV, SIG_DFL);
        signal(SIGBUS,  SIG_DFL);
        p[0] = 'X';
        _exit(0);
    }
    ASSERT_TRUE(pid > 0);

    int status = 0;
    pid_t w = waitpid(pid, &status, 0);
    ASSERT_EQ(pid, w);
    ASSERT_TRUE(WIFSIGNALED(status));
    int sig = WTERMSIG(status);
    /* SIGSEGV on Linux/macOS for write to read-only pages. SIGBUS on
     * some BSDs / older kernels. Either is correct evidence that the
     * mprotect actually took effect. */
    ASSERT_TRUE(sig == SIGSEGV || sig == SIGBUS);

    sh_seal_arena_destroy(&a);
}

/* ── reset cycle: RO → RW → fill → RO → reset → fill → RO ──────────── */

UTEST(seal_arena, reset_on_unsealed_rewinds_bump)
{
    ShSealArena a;
    ASSERT_EQ(0, sh_seal_arena_init(&a, 4096, NULL));
    (void)sh_seal_arena_alloc(&a, 100, 8);
    ASSERT_EQ((size_t)100, sh_seal_arena_used(&a));

    ASSERT_EQ(0, sh_seal_arena_reset(&a));
    ASSERT_EQ((size_t)0, sh_seal_arena_used(&a));
    ASSERT_EQ(0, sh_seal_arena_is_sealed(&a));

    void *p = sh_seal_arena_alloc(&a, 50, 8);
    ASSERT_TRUE(p != NULL); /* alloc works after reset */
    sh_seal_arena_destroy(&a);
}

UTEST(seal_arena, reset_on_sealed_unprotects_and_rewinds)
{
    ShSealArena a;
    ASSERT_EQ(0, sh_seal_arena_init(&a, 4096, NULL));
    char *p1 = sh_seal_arena_strdup(&a, "first-cycle");
    ASSERT_TRUE(p1 != NULL);
    ASSERT_EQ(0, sh_seal_arena_seal(&a));
    ASSERT_EQ(1, sh_seal_arena_is_sealed(&a));

    ASSERT_EQ(0, sh_seal_arena_reset(&a));
    ASSERT_EQ(0, sh_seal_arena_is_sealed(&a));
    ASSERT_EQ((size_t)0, sh_seal_arena_used(&a));

    /* alloc must work again */
    char *p2 = sh_seal_arena_strdup(&a, "second-cycle");
    ASSERT_TRUE(p2 != NULL);
    ASSERT_STREQ("second-cycle", p2);

    /* the previous allocation's memory is reused at the same address */
    ASSERT_EQ((void *)p1, (void *)p2);

    sh_seal_arena_destroy(&a);
}

UTEST(seal_arena, reset_null_returns_error)
{
    ASSERT_EQ(-1, sh_seal_arena_reset(NULL));
    ShSealArena zero = {0};
    ASSERT_EQ(-1, sh_seal_arena_reset(&zero)); /* no base */
}

UTEST(seal_arena, full_cycle_seal_reset_seal_again)
{
    ShSealArena a;
    ASSERT_EQ(0, sh_seal_arena_init(&a, 4096, NULL));

    /* first cycle: fill + seal */
    (void)sh_seal_arena_strdup(&a, "cycle-1");
    ASSERT_EQ(0, sh_seal_arena_seal(&a));

    /* reset + second cycle: fill + seal */
    ASSERT_EQ(0, sh_seal_arena_reset(&a));
    char *p = sh_seal_arena_strdup(&a, "cycle-2");
    ASSERT_TRUE(p != NULL);
    ASSERT_EQ(0, sh_seal_arena_seal(&a));
    ASSERT_EQ(1, sh_seal_arena_is_sealed(&a));

    /* writes still fault after re-seal */
    pid_t pid = fork();
    if (pid == 0) {
        signal(SIGSEGV, SIG_DFL);
        signal(SIGBUS,  SIG_DFL);
        p[0] = 'X';
        _exit(0);
    }
    ASSERT_TRUE(pid > 0);
    int status = 0;
    ASSERT_EQ(pid, waitpid(pid, &status, 0));
    ASSERT_TRUE(WIFSIGNALED(status));
    int sig = WTERMSIG(status);
    ASSERT_TRUE(sig == SIGSEGV || sig == SIGBUS);

    sh_seal_arena_destroy(&a);
}

/* ── diagnostics ──────────────────────────────────────────────────── */

UTEST(seal_arena, diagnostics_report_usage)
{
    ShSealArena a;
    ASSERT_EQ(0, sh_seal_arena_init(&a, 4096, NULL));
    size_t cap = sh_seal_arena_capacity(&a);
    ASSERT_TRUE(cap >= 4096);
    ASSERT_EQ((size_t)0, sh_seal_arena_used(&a));
    ASSERT_EQ(cap, sh_seal_arena_remaining(&a));

    (void)sh_seal_arena_alloc(&a, 100, 8);
    ASSERT_EQ((size_t)100, sh_seal_arena_used(&a));
    ASSERT_EQ(cap - 100, sh_seal_arena_remaining(&a));

    sh_seal_arena_destroy(&a);
}

UTEST_MAIN()
