/*
 * kl_cstr_builtin.c: OPTIONAL reference C-runtime memory/string primitives
 * (memcpy / memmove / memset / memcmp / strlen) for a BARE freestanding target
 * that has NEITHER a hosted libc NOR EDK2 BaseMemoryLib to supply them.
 *
 * By default KEEL relies on the platform's C runtime for these: the compiler
 * emits calls to `memcpy`/`memset`/… by name (struct copies, array init) even
 * under -ffreestanding -fno-builtin, and the platform provides them: libc on a
 * hosted build, EDK2 BaseMemoryLib on UEFI, gnu-efi on gnu-efi. That is the
 * documented "small required C-runtime surface" (docs/archive/phases/phase10_uefi_feasibility_
 * design.md), verified by tests/freestanding_symbol_gate.sh.
 *
 * This TU is the OPTIONAL counterpart for a target that has none of those. It is
 * NOT in CORE_SRC and NOT in the default FREESTANDING_CLIENT_SRC; it is linked
 * ONLY into the self-contained archive (`make freestanding-lib-selfcontained`)
 * or by a bare embedder that lacks these symbols. Linking it on a hosted or EDK2
 * build would DUPLICATE-DEFINE the libc/BaseMemoryLib symbols; don't.
 *
 * It is the mem* and strlen analogue of allocator_default_stdlib.c vs a UEFI
 * allocator: KEEL relies on the platform by default and ships a reference impl
 * to swap in where the platform can't.
 *
 * MUST be compiled with -fno-builtin AND -fno-tree-loop-distribute-patterns (GCC)
 * so the compiler does not "recognize" these byte loops and lower them back into
 * calls to memcpy/memset, i.e. into infinite self-recursion. The
 * freestanding-lib-selfcontained target sets both. These are correctness-first
 * reference implementations, not tuned; a bare target that cares about throughput
 * supplies arch-optimized versions instead.
 */
#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d == s || n == 0) return dst;
    if (d < s) {                              /* forward copy: no overlap hazard */
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else {                                  /* backward copy: dst overlaps ahead of src */
        for (size_t i = n; i-- > 0; ) d[i] = s[i];
    }
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    for (size_t i = 0; i < n; i++) d[i] = (unsigned char)c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    for (size_t i = 0; i < n; i++)
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    return 0;
}

size_t strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}
