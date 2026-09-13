/*
 * platform_socket.h: INTERNAL platform-services interface, part of the PAL. No ABI commitment.
 *
 * One concern: bringing the platform's SOCKET RUNTIME up before Keel makes its first native socket
 * call. On POSIX there is no such runtime and both operations are trivially successful. On Windows
 * ws2_32 must be started with WSAStartup first, and until it has been, EVERY native call returns
 * WSANOTINITIALISED (10093) instead of the error the call would otherwise produce.
 *
 * THE INVARIANT, stated once so every call site can point at it:
 *
 *     Before any Keel code invokes a native socket operation, the platform socket runtime must have
 *     been initialised successfully.
 *
 * It holds regardless of whether Keel created the descriptor, whether it came from an embedder,
 * whether a custom or mock provider is in play, and whether the call originates in the socket,
 * event, resolver, datagram or wakeup path.
 *
 * WHY THIS EXISTS AT ALL. Windows builds used to establish the invariant with a GCC/Clang
 * __attribute__((constructor)) in socket_winsock.c, which ran WSAStartup at image load. MSVC has no
 * equivalent in C mode, and emulating one with CRT section tricks was rejected, so the invariant had
 * to become something the code states rather than something the compiler arranges. One mechanism for
 * every Windows toolchain: there is no constructor left to fall back to.
 *
 * WHY NOT LAZY INITIALISATION AT SOCKET CREATION, the obvious first guess: socket creation is not the
 * only first use. A custom or mock provider can hand Keel a synthetic handle it never created, and a
 * default helper (kl_sockdef_*) will still enter ws2_32 with it. Uninitialised, that call reports
 * WSANOTINITIALISED where the caller expects WSAENOTSOCK, which changes control flow and error
 * semantics rather than merely losing a diagnostic. Keel's own datagram provider suites exercise
 * exactly that shape, which is how the distinction was found. So the gate sits at the native boundary,
 * not at the point a socket happens to be born.
 *
 * WHY A SEPARATE PAL HEADER rather than declarations in platform.h: the same reason platform_thread.h
 * is separate. Not types here (this header deliberately has none), but HOME: this is a per-concern
 * seam with its own per-OS TU pair, exactly as wakeup and threads already are.
 *
 *   platform_socket_win.c     INIT_ONCE + WSAStartup, no WSACleanup
 *   platform_socket_posix.c   successful no-op
 *
 * INIT_ONCE, WSADATA and WSAStartup stay confined to the Windows TU; nothing above this line names
 * them, and this header pulls in no platform header at all.
 *
 * MONOTONIC BY DESIGN. The runtime is initialised at most once and never torn down: there is no
 * WSACleanup in Keel's teardown. Keel is a library, it does not own the process, and a refcounted
 * startup/cleanup pair would let the last Keel object closing take ws2_32 down under an embedder that
 * is still using sockets of its own. Process-lifetime initialisation is the deliberate choice.
 *
 * INTERNAL header: not installed.
 */
#ifndef KEEL_SRC_PLATFORM_SOCKET_H
#define KEEL_SRC_PLATFORM_SOCKET_H

/**
 * Ensure the platform socket runtime is up. Idempotent, thread-safe, and cheap after the first
 * successful call (on Windows an InitOnce fast path, in front of operations that are entering
 * ws2_32 and the kernel anyway).
 *
 * Returns 0 when the runtime is usable, or -1 with errno set when it is not. A failure is CACHED:
 * the platform is not asked again, so a process whose socket runtime is unavailable fails fast and
 * identically on every subsequent call rather than retrying a startup that already failed.
 *
 * Callers must not proceed into a native socket call after -1. Each call site maps the failure into
 * whatever its own contract already uses to report "this socket operation did not happen"; errno is
 * set so the POSIX-shaped error checks throughout Keel (and kl_sock_errno_to_error) stay coherent.
 */
int kl_plat_socket_runtime_init(void);

/**
 * TEST SEAM. Reports what has happened to the one-time initialisation so far, so a test can assert
 * the invariant directly instead of inferring it from error codes:
 *
 *     0   not yet attempted
 *    >0   attempted and succeeded
 *    <0   attempted and failed; the magnitude is the platform's own startup error code
 *
 * This is the whole reason the facility needs no public API: the PAL header is internal, tests
 * reach it with -Isrc, and embedders acquire no initialisation obligation.
 *
 * Deliberately NOT synchronised against a concurrent first initialisation: it reports a point-in-time
 * observation for a single-threaded assertion, and callers that need the runtime up call
 * kl_plat_socket_runtime_init() instead.
 */
int kl_plat_socket_runtime_status(void);

#endif /* KEEL_SRC_PLATFORM_SOCKET_H */
