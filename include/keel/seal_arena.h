/*
 * seal_arena.h — Page-backed bump arena with one-way "seal" transition.
 *
 * Keel uses this for the router's frozen state — once kl_router_freeze
 * is called, the route table, middleware arrays, and the server's
 * config mirror live in an mprotect-RO mapping.  A heap-write primitive
 * that would otherwise overwrite a route's handler function pointer
 * (or expand the CORS origin allowlist, or relax the body-size cap,
 * or flip TLS verifyMode) then faults instead of pivoting control flow
 * or escalating capability.
 *
 * Lifecycle:
 *
 *   kl_seal_arena_init    — reserve N pages RW, ready to allocate
 *   kl_seal_arena_alloc   — bump-allocate a block (alignment-aware)
 *   kl_seal_arena_strdup  — convenience: copy a NUL-terminated string
 *   kl_seal_arena_seal    — mprotect(RO); after this, alloc fails and
 *                            writes to allocated blocks fault
 *   kl_seal_arena_destroy — munmap; safe on sealed or unsealed arenas
 *
 * After sealing the arena is one-way: there is NO "unseal" — that would
 * defeat the purpose. To rebuild, init a fresh arena into a different
 * pointer.
 *
 * Threading: NOT thread-safe during the init→alloc→seal lifecycle.
 * That's a feature — these operations are single-threaded boot-time
 * activity. AFTER sealing, the OS read-only mapping is trivially
 * thread-safe to read from any number of threads.
 *
 * Portability: pure POSIX (mmap + mprotect + sysconf). Cosmopolitan
 * provides the same calls via its libc shim, so APE builds work
 * unmodified. No Windows path today; a future native MSVC port would
 * add a #ifdef _WIN32 branch using VirtualAlloc/VirtualProtect.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KEEL_SEAL_ARENA_H
#define KEEL_SEAL_ARENA_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Layout is exposed only so the struct can be embedded; do NOT poke
 * `sealed` or `used` from outside the API. */
typedef struct KlSealArena {
    void       *base;        /**< mmap'd base (page-aligned) */
    size_t      capacity;    /**< total bytes (multiple of page size) */
    size_t      used;        /**< current high-water (within capacity) */
    size_t      page_size;   /**< cached at init */
    int         sealed;      /**< 1 after kl_seal_arena_seal */
    const char *name;        /**< for diagnostics; caller-owned, NOT copied */
} KlSealArena;

/** Initialize an arena with at least @p size bytes of capacity.
 *  Actual capacity is rounded up to a multiple of the system page size,
 *  with a minimum of one page. @p name is for logging only.
 *  Returns 0 on success, -1 on failure (mmap failed, NULL arena). */
int    kl_seal_arena_init(KlSealArena *arena, size_t size, const char *name);

/** Bump-allocate @p size bytes with @p align (power of two: 1/2/4/8/16).
 *  Returns NULL on failure (sealed, OOM, bad alignment, NULL arena). */
void  *kl_seal_arena_alloc(KlSealArena *arena, size_t size, size_t align);

/** Copy a NUL-terminated string into the arena, return pointer to copy.
 *  Returns NULL on failure. */
char  *kl_seal_arena_strdup(KlSealArena *arena, const char *s);

/** Copy @p len bytes (binary-safe, NOT NUL-terminated). NULL on failure. */
void  *kl_seal_arena_memdup(KlSealArena *arena, const void *src, size_t len);

/** Seal: mprotect(PROT_READ). After this:
 *    - kl_seal_arena_alloc returns NULL
 *    - reads from already-allocated blocks continue to work
 *    - writes to those blocks fault (SIGSEGV)
 *  Returns 0 on success, -1 on failure (treat as FATAL). */
int    kl_seal_arena_seal(KlSealArena *arena);

/** Returns 1 if sealed, 0 otherwise (including for NULL). */
int    kl_seal_arena_is_sealed(const KlSealArena *arena);

/** Tear down: munmap. Safe on sealed/unsealed/NULL arenas. */
void   kl_seal_arena_destroy(KlSealArena *arena);

/** Diagnostic accessors. */
size_t kl_seal_arena_capacity(const KlSealArena *arena);
size_t kl_seal_arena_used(const KlSealArena *arena);
size_t kl_seal_arena_remaining(const KlSealArena *arena);

#ifdef __cplusplus
}
#endif

#endif /* KEEL_SEAL_ARENA_H */
