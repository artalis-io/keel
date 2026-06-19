/*
 * sh_seal_arena.h — Page-backed bump arena with one-way "seal" transition.
 *
 * Vendored shared utility (sh_* family alongside sh_arena, sh_json).
 * Single source of truth for the seal-arena primitive used by both
 * Hull (manifest, wasm host_symbols, CORS config) and Keel (router
 * + middleware tables via kl_router_freeze).
 *
 * Lifecycle:
 *
 *   sh_seal_arena_init    — reserve N pages RW, ready to allocate
 *   sh_seal_arena_alloc   — bump-allocate a block (alignment-aware)
 *   sh_seal_arena_strdup  — copy a NUL-terminated string
 *   sh_seal_arena_memdup  — copy raw bytes
 *   sh_seal_arena_seal    — mprotect(RO); after this, alloc fails and
 *                            writes to allocated blocks fault
 *   sh_seal_arena_destroy — munmap; safe on sealed/unsealed arenas
 *
 * After sealing the arena is one-way: there is NO unseal — that would
 * defeat the purpose.  To rebuild, init a fresh arena.
 *
 * Threading: NOT thread-safe during the init→alloc→seal lifecycle.
 * That's a feature — these operations are by-design single-threaded
 * boot-time activity.  AFTER sealing, the OS read-only mapping is
 * trivially thread-safe to read from any number of threads.
 *
 * Portability: pure POSIX (mmap + mprotect + sysconf).  Cosmopolitan
 * provides the same calls via its libc shim, so APE builds work
 * unmodified.  No Windows path today; a future native MSVC port
 * would add a #ifdef _WIN32 branch using VirtualAlloc/VirtualProtect.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef SH_SEAL_ARENA_H
#define SH_SEAL_ARENA_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Layout is exposed only so the struct can be embedded; do NOT poke
 * `sealed` or `used` from outside the API. */
typedef struct ShSealArena {
    void       *base;        /**< mmap'd base (page-aligned) */
    size_t      capacity;    /**< total bytes (multiple of page size) */
    size_t      used;        /**< current high-water (within capacity) */
    size_t      page_size;   /**< cached at init */
    int         sealed;      /**< 1 after sh_seal_arena_seal */
    const char *name;        /**< for diagnostics; caller-owned, NOT copied */
} ShSealArena;

/** Initialize an arena with at least @p size bytes of capacity.
 *  Actual capacity is rounded up to a multiple of the system page size,
 *  with a minimum of one page.  @p name is for logging only (not copied).
 *  Returns 0 on success, -1 on failure (mmap failed, NULL arena). */
int    sh_seal_arena_init(ShSealArena *arena, size_t size, const char *name);

/** Bump-allocate @p size bytes with @p align (power of two: 1/2/4/8/16).
 *  Returns NULL on failure (sealed, OOM, bad alignment, NULL arena). */
void  *sh_seal_arena_alloc(ShSealArena *arena, size_t size, size_t align);

/** Copy a NUL-terminated string into the arena, return pointer to copy.
 *  Returns NULL on failure. */
char  *sh_seal_arena_strdup(ShSealArena *arena, const char *s);

/** Copy @p len bytes (binary-safe, NOT NUL-terminated). NULL on failure. */
void  *sh_seal_arena_memdup(ShSealArena *arena, const void *src, size_t len);

/** Seal: mprotect(PROT_READ). After this:
 *    - sh_seal_arena_alloc returns NULL
 *    - reads from already-allocated blocks continue to work
 *    - writes to those blocks fault (SIGSEGV)
 *  Returns 0 on success, -1 on failure (treat as FATAL). */
int    sh_seal_arena_seal(ShSealArena *arena);

/** Returns 1 if sealed, 0 otherwise (including for NULL). */
int    sh_seal_arena_is_sealed(const ShSealArena *arena);

/** Tear down: munmap. Safe on sealed/unsealed/NULL arenas. */
void   sh_seal_arena_destroy(ShSealArena *arena);

/** Diagnostic accessors. */
size_t sh_seal_arena_capacity(const ShSealArena *arena);
size_t sh_seal_arena_used(const ShSealArena *arena);
size_t sh_seal_arena_remaining(const ShSealArena *arena);

#ifdef __cplusplus
}
#endif

#endif /* SH_SEAL_ARENA_H */
