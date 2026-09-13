/*
 * src/kl_atomic.c - the runtime half of the lock-free contract in kl_atomic.h.
 *
 * Out of line, in one TU, so the _Atomic reinterpretation exists exactly once and so the compile-time
 * and runtime cases present one identical signature to callers.
 */
#include "kl_atomic.h"

#if !KL_ATOMIC_INT_LOCK_FREE_STATIC

/* The query must be asked ABOUT THE OBJECT THE STOP PATH TOUCHES, which is a plain int, so that int is
 * reinterpreted as the C11 atomic type rather than a separate _Atomic temporary being probed in its
 * place. A temporary would answer about a different object at a different address, and address and
 * alignment are exactly what the standard leaves open when ATOMIC_INT_LOCK_FREE is 1.
 *
 * These assertions make the reinterpretation sound: same width, same alignment requirement. If a
 * toolchain ever gave _Atomic int a wider or stricter representation than int, the honest response is a
 * failed build here, not a probe that quietly answers about the wrong thing. */
_Static_assert(sizeof(_Atomic int) == sizeof(int),
               "KEEL probes the plain int the stop path uses; _Atomic int must match its width");
_Static_assert(_Alignof(_Atomic int) == _Alignof(int),
               "KEEL probes the plain int the stop path uses; _Atomic int must match its alignment");

#endif

int kl_atomic_int_is_lock_free(const int *flag) {
    if (!flag) return 0;
#if KL_ATOMIC_INT_LOCK_FREE_STATIC
    /* Guaranteed for every int object, so there is nothing left to ask. */
    return 1;
#else
    return atomic_is_lock_free((const volatile _Atomic int *)flag) ? 1 : 0;
#endif
}
