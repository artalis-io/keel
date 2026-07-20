#ifndef KEEL_HANDLE_H
#define KEEL_HANDLE_H

#include <stdint.h>

/**
 * handle.h — portable socket handle (PAL Phase 5).
 *
 * A socket handle is `int` on POSIX, but a Winsock `SOCKET` is pointer-width
 * (`UINT_PTR`), and future providers hand out pointers (lwIP raw `tcp_pcb *`,
 * UEFI protocol pointers). `KlSocketHandle` is therefore `intptr_t`
 * (pointer-width) on every platform, so one handle type holds any provider's
 * native handle and the (API-breaking) handle change is done once, not twice.
 *
 * On POSIX this is behaviorally a no-op: an `int` fd stored in a signed
 * pointer-width integer, `-1` still the invalid sentinel. `INVALID_SOCKET` on
 * Windows is `(SOCKET)(~0)` — i.e. `-1` reinterpreted — so `KL_INVALID_SOCKET`
 * is the shared invalid sentinel for every descriptor-based provider (POSIX,
 * Winsock, lwIP socket API).
 *
 * NOTE: because a Winsock `SOCKET` is UNSIGNED, the POSIX idiom `if (fd < 0)`
 * is unreliable on Windows — always test validity with `kl_handle_valid()` (or
 * `== KL_INVALID_SOCKET`), never `< 0`. The codebase-wide migration of existing
 * `fd < 0` checks + the retyping of the socket-facing API lands with the Winsock
 * provider (Phase 6a), where a native build validates it; this header defines
 * the type up front.
 */
typedef intptr_t KlSocketHandle;

#define KL_INVALID_SOCKET ((KlSocketHandle)-1)

static inline int kl_handle_valid(KlSocketHandle h) {
    return h != KL_INVALID_SOCKET;
}

#endif /* KEEL_HANDLE_H */
