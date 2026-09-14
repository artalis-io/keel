/*
 * net_compat.h: portable network glue for the test harnesses (declarations).
 *
 * Tests use raw POSIX socket idioms (socketpair/pipe/close/read/write/fcntl/poll)
 * as ad-hoc clients that drive the server or feed a stream. Those don't exist (or
 * mean something else) under Winsock. Mirroring the library's socket seam
 * (socket.h + socket_posix.c/socket_winsock.c), the platform logic lives in two
 * sibling TUs (net_compat_posix.c / net_compat_win.c, selected by the Makefile)
 * so this header carries no logic #ifdef, only the one unavoidable include-
 * selection boundary (the same boundary src/sockcompat.h owns for the library).
 * Ported tests call the kl_test_* helpers instead of the raw idioms; on POSIX the
 * helpers are thin wrappers, so native behavior is unchanged. Test-only.
 *
 * Tests still STORE socket fds in `int`, as they always have: a Windows SOCKET is
 * pointer-width, but loopback test sockets are small kernel handles in practice, so
 * the narrowing is safe for the handful of fds a test opens. The helpers below now
 * TAKE a KlSocketHandle, though. An int widens to it silently, so no caller changed,
 * and a handle that came from Keel no longer has to be narrowed at each call site
 * (which MSVC /W3 rightly flags). The one narrowing left is inside the two impls,
 * where the platform call actually needs the native type.
 */
#ifndef KEEL_TESTS_NET_COMPAT_H
#define KEEL_TESTS_NET_COMPAT_H

#include <stddef.h>   /* size_t */
#include <stdint.h>   /* uintptr_t */
#include <keel/handle.h>   /* KlSocketHandle */

/* The one contained platform-include boundary: where tests resolve socket types
 * (struct sockaddr_in / htons / inet_pton / ...) per platform. Same shape as
 * src/sockcompat.h; kept separate so tests need no -Isrc. */
#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <poll.h>
  #include <sys/uio.h>
  #include <sys/un.h>
  #include <sys/time.h>   /* struct timeval (not transitive on musl) */
  #include <unistd.h>
  #include <fcntl.h>
#endif

/* Portable test helpers (implemented in tests/net_compat_{posix,win}.c). */

/* The platform's built-in KlSocketProvider: kl_socket_provider_posix() off Windows,
 * kl_socket_provider_winsock() on it. A build compiles exactly one platform socket TU, so
 * exactly one of those factories is DEFINED and naming the wrong one fails to link. Tests that
 * need a real provider go through this instead of hard-coding either name.
 * Returns an opaque pointer; cast to const KlSocketProvider * (tests that use it include
 * src/socket.h themselves, which this header deliberately does not pull in). */
const void *kl_test_builtin_provider(void);

/* close() a socket fd. */
int kl_test_closesock(KlSocketHandle fd);

/* Set a socket fd non-blocking (POSIX O_NONBLOCK / Winsock FIONBIO). */
int kl_test_set_nonblock(KlSocketHandle fd);

/* write()/read() over a socket fd (send/recv on Windows, read/write don't work
 * on Winsock sockets). Return value matches the POSIX ssize_t contract. */
long kl_test_sockwrite(KlSocketHandle fd, const void *buf, size_t len);
long kl_test_sockread(KlSocketHandle fd, void *buf, size_t len);

/* Poll a single socket fd for readable (for_write=0) or writable (for_write=1).
 * Returns >0 ready, 0 timeout, -1 error; the poll()/WSAPoll() contract. */
int kl_test_poll1(KlSocketHandle fd, int for_write, int timeout_ms);

/* Set a receive timeout (ms) on a socket fd (Winsock DWORD / POSIX timeval). */
int kl_test_set_rcvtimeo(KlSocketHandle fd, int ms);

/* Sleep for ms milliseconds. POSIX has nanosleep; Windows does not, and the
 * struct-timespec shape means a macro cannot bridge it. Harness-only: tests
 * wait on a TTL or a timeout, they do not need sub-millisecond precision. */
void kl_test_sleep_ms(unsigned ms);

/* An opaque, comparable identity for the CALLING thread. Harness-only, and deliberately
 * NOT part of the PAL: nothing in Keel asks which thread it is running on, so the
 * production seam has no business growing a primitive only tests want. Used to assert
 * that work_fn runs on a worker and done_fn on the event-loop thread. Compare with ==;
 * the value is meaningful only against another value from the same process. */
typedef uintptr_t KlTestThreadId;
KlTestThreadId kl_test_thread_id(void);

/* A connected stream fd pair: socketpair(AF_UNIX) on POSIX; a self-connected
 * loopback TCP pair on Windows (which has neither socketpair nor pollable pipes).
 * Also the portable replacement for pipe() in tests that need a pollable byte
 * channel. Returns 0 on success, -1 on error; sv[0]/sv[1] are both usable ends. */
int kl_test_socketpair(int sv[2]);

#endif /* KEEL_TESTS_NET_COMPAT_H */
