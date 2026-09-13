/*
 * src/kl_atomic.h - Private lock-free integer atomics.
 *
 * Public struct layouts (KlHttpServer.running / .draining) store these flags as a PLAIN int, so the
 * headers stay valid C and C++ with one shared object representation (C11 _Atomic is not valid C++ and
 * its representation need not match a plain int). Atomicity lives here, off the public surface: every
 * read/write of such a field goes through these helpers, seq-cst throughout.
 *
 * This header is the one place in Keel that knows how a COMPILER spells an atomic operation. That is a
 * different axis from the PAL: the PAL abstracts the operating system, this abstracts the instruction
 * the compiler emits. Both branches below are the same operations with the same ordering, not
 * alternative runtime behaviour, which is why the choice belongs here and not in a PAL TU pair.
 */
#ifndef KEEL_KL_ATOMIC_H
#define KEEL_KL_ATOMIC_H

#include <stdatomic.h>   /* ATOMIC_INT_LOCK_FREE, atomic_is_lock_free (C-only header) */

/* THE LOCK-FREE CONTRACT. Keel needs something semantic: the flag the server-stop path touches must
 * be operated on without taking a lock, because that path runs from a SIGTERM/SIGINT handler (POSIX)
 * or a console control handler (Windows), where a libcall into a mutex is not permitted.
 *
 * ATOMIC_INT_LOCK_FREE answers in three values, only two of them at compile time:
 *
 *     0   never lock-free                  cannot meet the requirement; fail the build
 *     2   always lock-free                 guaranteed; no runtime probe needed
 *     1   implementation/object dependent  the standard answer is to ask at runtime, about the
 *                                          actual object, via atomic_is_lock_free()
 *
 * This was a flat _Static_assert(ATOMIC_INT_LOCK_FREE == 2), which reads like "require lock-free
 * atomics" but says "refuse to build wherever the implementation declines to promise it in a macro" -
 * stronger, and different. MSVC reports 1 while atomic_is_lock_free() returns true for the very object
 * Keel operates on, so the assertion rejected a toolchain that meets the requirement completely.
 *
 * Deliberately NOT an _MSC_VER carve-out that trusts MSVC: that would be an assumption with a compiler
 * name on it. This asks the question the standard defines, of the object in question, and would catch
 * a target where the answer is no.
 */
#if ATOMIC_INT_LOCK_FREE == 0
#error "KEEL requires lock-free int atomics for the signal-safe server stop path; this target has none"
#elif ATOMIC_INT_LOCK_FREE == 2
#define KL_ATOMIC_INT_LOCK_FREE_STATIC 1
#else
#define KL_ATOMIC_INT_LOCK_FREE_STATIC 0
#endif

/**
 * Is @p flag safe for lock-free atomic access? Returns 1 if so, 0 if not (and 0 for NULL).
 *
 * @p flag is the ACTUAL int the stop path will operate on, at its real address, because lock-freedom
 * may depend on an object address and alignment and not only on its type. Constant 1 where
 * ATOMIC_INT_LOCK_FREE is 2; the runtime query the standard requires where it is 1.
 *
 * CALL IT FROM ORDINARY INITIALISATION, never first from a handler: the query need not itself be
 * signal-safe, and where it resolves lazily the first call is exactly the one that would not be.
 * kl_http_server_init() calls it before any handler can be installed and refuses to initialise if it
 * fails, so the signal-safe path is never enabled on a flag whose atomicity is unestablished.
 */
int kl_atomic_int_is_lock_free(const int *flag);

#if defined(_MSC_VER) && !defined(__clang__)

/* MSVC has no __atomic builtins. Its interlocked intrinsics are the equivalent and are full
 * (sequentially consistent) barriers on every architecture it targets, which is what these helpers
 * have always promised. <intrin.h> rather than a Windows header, so no API surface comes with it.
 *
 * long rather than int because that is how the intrinsics are typed; on every Windows ABI the two are
 * the same 32-bit object, asserted rather than assumed. */
#include <intrin.h>
_Static_assert(sizeof(long) == sizeof(int),
               "KEEL lowers int atomics onto MSVC 32-bit interlocked intrinsics");

/** Atomic load (seq-cst) of a plain-int flag. */
static inline int kl_atomic_load_int(const int *p) {
    /* Compare-exchange against itself: returns the current value and changes nothing, storing only
     * when the comparand already matches and then writing back the same bits. A plain volatile load
     * would be cheaper and is correct on x86-64, but is NOT sequentially consistent on ARM64 without
     * an explicit barrier whose intrinsic MSVC does not expose portably. This form is correct on every
     * MSVC target for the price of a redundant same-value write, and these flags are read about once
     * per event-loop tick, right beside a blocking wait syscall. */
    volatile long *q = (volatile long *)(int *)p;   /* not really const; the caller view only */
    return (int)_InterlockedCompareExchange(q, 0, 0);
}

/** Atomic store (seq-cst) of a plain-int flag. */
static inline void kl_atomic_store_int(int *p, int v) {
    (void)_InterlockedExchange((volatile long *)p, (long)v);
}

#else

/* GCC / Clang, including MinGW gcc, Cosmopolitan clang and clang-cl. */

/** Atomic load (seq-cst) of a plain-int flag. */
static inline int kl_atomic_load_int(const int *p) {
    return __atomic_load_n(p, __ATOMIC_SEQ_CST);
}

/** Atomic store (seq-cst) of a plain-int flag. */
static inline void kl_atomic_store_int(int *p, int v) {
    __atomic_store_n(p, v, __ATOMIC_SEQ_CST);
}

#endif

#endif /* KEEL_KL_ATOMIC_H */
