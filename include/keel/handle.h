#ifndef KEEL_HANDLE_H
#define KEEL_HANDLE_H

#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif


/**
 * handle.h: portable socket handle.
 *
 * A socket handle is `int` on POSIX, but a Winsock `SOCKET` is pointer-width
 * (`UINT_PTR`), and future providers hand out pointers (lwIP raw `tcp_pcb *`,
 * UEFI protocol pointers). `KlSocketHandle` is therefore `intptr_t`
 * (pointer-width) on every platform, so one handle type holds any provider's
 * native handle and the (API-breaking) handle change is done once, not twice.
 *
 * On POSIX this is behaviorally a no-op: an `int` fd stored in a signed
 * pointer-width integer, `-1` still the invalid sentinel. `INVALID_SOCKET` on
 * Windows is `(SOCKET)(~0)` (i.e. `-1` reinterpreted), so `KL_INVALID_SOCKET`
 * is the shared invalid sentinel for every descriptor-based provider (POSIX,
 * Winsock, lwIP socket API).
 *
 * NOTE: because a Winsock `SOCKET` is UNSIGNED, the POSIX idiom `if (fd < 0)`
 * is unreliable on Windows; always test validity with `kl_handle_valid()` (or
 * `== KL_INVALID_SOCKET`), never `< 0`.
 */
typedef intptr_t KlSocketHandle;

/**
 * kl_ssize_t: pointer-width signed size, the SINGLE definition for the whole
 * tree. It is the return type of every byte-count I/O op/callback in the public
 * API (socket/datagram send/recv, TLS read/write, drain writer, client body
 * pull, file-I/O result, h2 send/recv). `intptr_t` is exactly as wide as POSIX
 * `ssize_t` (both pointer-width signed), so this is source- and ABI-compatible
 * with the old `ssize_t`-based signatures, but it needs no `<sys/types.h>`, so
 * the public headers stay freestanding (no hosted libc types). Defined here,
 * next to KlSocketHandle, because handle.h already pulls only <stdint.h> and is
 * the natural home for the pointer-width integer types the seam speaks.
 */
typedef intptr_t kl_ssize_t;

/* printf-style format checking where the compiler offers it. GCC and Clang diagnose mismatched
 * varargs through the format attribute; MSVC has no equivalent in C mode, so it expands to nothing
 * there. A diagnostic aid only: nothing depends on it for correctness, which is why losing it under
 * MSVC is acceptable where losing a semantic attribute would not be. */
#if defined(__GNUC__) || defined(__clang__)
#define KL_PRINTF_FMT(fmt_idx, first_arg) __attribute__((format(printf, fmt_idx, first_arg)))
#else
#define KL_PRINTF_FMT(fmt_idx, first_arg)
#endif

#define KL_INVALID_SOCKET ((KlSocketHandle)-1)

static inline int kl_handle_valid(KlSocketHandle h) {
    return h != KL_INVALID_SOCKET;
}

#ifdef __cplusplus
}
#endif

#endif /* KEEL_HANDLE_H */
