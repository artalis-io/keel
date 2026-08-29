/*
 * src/kl_atomic.h - Private lock-free integer atomics.
 *
 * Public struct layouts (e.g. KlHttpServer.running / .draining) store these flags as a PLAIN int, so
 * the headers are valid C and C++ with one shared object representation (the C11 `_Atomic` keyword is
 * not valid C++ and its representation is not guaranteed to match a plain int). Atomicity lives here,
 * off the public surface: every read/write of such a field goes through these helpers, which lower to
 * the GCC/Clang `__atomic` builtins (also provided by MinGW gcc and Cosmopolitan's clang; the project
 * targets no MSVC). SEQ_CST matches the prior `_Atomic int` default.
 *
 * Signal-safety: the server stop path runs from a SIGTERM/SIGINT handler, so the operation MUST be
 * lock-free (no libcall). The static assert below fails the build on any target where an int atomic is
 * not always lock-free.
 */
#ifndef KEEL_KL_ATOMIC_H
#define KEEL_KL_ATOMIC_H

#include <stdatomic.h>   /* ATOMIC_INT_LOCK_FREE: compile-time lock-free query (C-only header) */

/* ATOMIC_INT_LOCK_FREE == 2 means "always lock-free". It is a standard C11 constant expression, valid
 * in _Static_assert under -Wpedantic (unlike the __atomic_always_lock_free builtin, which GCC rejects
 * there as not an integer constant expression). This guards the signal-safe stop path: an int atomic
 * that is not always lock-free would fall into a libcall in the SIGTERM/SIGINT handler. */
_Static_assert(ATOMIC_INT_LOCK_FREE == 2,
               "KEEL requires always-lock-free int atomics (signal-safe server stop path)");

/** Atomic load (seq-cst) of a plain-int flag. */
static inline int kl_atomic_load_int(const int *p) {
    return __atomic_load_n(p, __ATOMIC_SEQ_CST);
}

/** Atomic store (seq-cst) of a plain-int flag. */
static inline void kl_atomic_store_int(int *p, int v) {
    __atomic_store_n(p, v, __ATOMIC_SEQ_CST);
}

#endif /* KEEL_KL_ATOMIC_H */
