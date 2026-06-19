/*
 * seal_arena.c — see include/keel/seal_arena.h.
 *
 * POSIX-only (mmap/mprotect/sysconf). Cosmopolitan's libc shim
 * provides the same surface, so APE builds work unmodified.
 *
 * SPDX-License-Identifier: MIT
 */

#include "keel/seal_arena.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* ── internals ─────────────────────────────────────────────────────── */

static size_t cached_page_size(void)
{
    static size_t cached = 0;
    if (cached == 0) {
        long ps = sysconf(_SC_PAGESIZE);
        cached = (ps > 0) ? (size_t)ps : 4096u;
    }
    return cached;
}

static size_t align_up(size_t n, size_t align)
{
    if (align == 0) return 0;
    size_t mask = align - 1;
    if (n > SIZE_MAX - mask) return 0; /* overflow */
    return (n + mask) & ~mask;
}

static int is_power_of_two(size_t x)
{
    return x != 0 && (x & (x - 1)) == 0;
}

/* ── lifecycle ─────────────────────────────────────────────────────── */

int kl_seal_arena_init(KlSealArena *arena, size_t size, const char *name)
{
    if (!arena) return -1;
    arena->base      = NULL;
    arena->capacity  = 0;
    arena->used      = 0;
    arena->page_size = 0;
    arena->sealed    = 0;
    arena->name      = name;

    size_t ps = cached_page_size();
    size_t cap = (size == 0) ? ps : align_up(size, ps);
    if (cap == 0 || cap < size) return -1;

    void *base = mmap(NULL, cap, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) return -1;

    arena->base      = base;
    arena->capacity  = cap;
    arena->page_size = ps;
    return 0;
}

void kl_seal_arena_destroy(KlSealArena *arena)
{
    if (!arena || !arena->base) return;
    (void)munmap(arena->base, arena->capacity);
    arena->base      = NULL;
    arena->capacity  = 0;
    arena->used      = 0;
    arena->page_size = 0;
    arena->sealed    = 0;
    arena->name      = NULL;
}

/* ── allocation ────────────────────────────────────────────────────── */

void *kl_seal_arena_alloc(KlSealArena *arena, size_t size, size_t align)
{
    if (!arena || !arena->base) return NULL;
    if (arena->sealed) return NULL;
    if (!is_power_of_two(align)) return NULL;
    if (size == 0) return NULL;

    size_t start = align_up(arena->used, align);
    if (start == 0 && arena->used != 0) return NULL;
    if (start > SIZE_MAX - size) return NULL;
    size_t end = start + size;
    if (end > arena->capacity) return NULL;

    char *p = (char *)arena->base + start;
    arena->used = end;
    return p;
}

char *kl_seal_arena_strdup(KlSealArena *arena, const char *s)
{
    if (!s) return NULL;
    size_t len = strlen(s);
    if (len == SIZE_MAX) return NULL;
    char *dst = (char *)kl_seal_arena_alloc(arena, len + 1, 1);
    if (!dst) return NULL;
    memcpy(dst, s, len + 1);
    return dst;
}

void *kl_seal_arena_memdup(KlSealArena *arena, const void *src, size_t len)
{
    if (!src) return NULL;
    void *dst = kl_seal_arena_alloc(arena, len, 1);
    if (!dst) return NULL;
    memcpy(dst, src, len);
    return dst;
}

/* ── seal ─────────────────────────────────────────────────────────── */

int kl_seal_arena_seal(KlSealArena *arena)
{
    if (!arena || !arena->base) return -1;
    if (arena->sealed) return -1;
    if (mprotect(arena->base, arena->capacity, PROT_READ) != 0) {
        return -1;
    }
    arena->sealed = 1;
    return 0;
}

int kl_seal_arena_is_sealed(const KlSealArena *arena)
{
    return (arena && arena->sealed) ? 1 : 0;
}

/* ── diagnostics ──────────────────────────────────────────────────── */

size_t kl_seal_arena_capacity(const KlSealArena *arena)
{
    return arena ? arena->capacity : 0;
}

size_t kl_seal_arena_used(const KlSealArena *arena)
{
    return arena ? arena->used : 0;
}

size_t kl_seal_arena_remaining(const KlSealArena *arena)
{
    if (!arena) return 0;
    return arena->capacity - arena->used;
}
