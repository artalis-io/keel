/*
 * net_compat.h — portable network glue for the test harnesses (declarations).
 *
 * Tests use raw POSIX socket idioms (socketpair/pipe/close/read/write/fcntl/poll)
 * as ad-hoc clients that drive the server or feed a stream. Those don't exist (or
 * mean something else) under Winsock. Mirroring the library's socket seam
 * (socket.h + socket_posix.c/socket_winsock.c), the platform logic lives in two
 * sibling TUs — net_compat_posix.c / net_compat_win.c, selected by the Makefile —
 * so this header carries no logic #ifdef, only the one unavoidable include-
 * selection boundary (the same boundary src/sockcompat.h owns for the library).
 * Ported tests call the kl_test_* helpers instead of the raw idioms; on POSIX the
 * helpers are thin wrappers, so native behavior is unchanged. Test-only.
 *
 * Socket fds are stored in `int` (as the tests always have). A Windows SOCKET is
 * pointer-width, but loopback test sockets are small kernel handles in practice,
 * so the narrowing is safe for the handful of fds a test opens.
 */
#ifndef KEEL_TESTS_NET_COMPAT_H
#define KEEL_TESTS_NET_COMPAT_H

#include <stddef.h>   /* size_t */

/* The one contained platform-include boundary — where tests resolve socket types
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

/* close() a socket fd. */
int kl_test_closesock(int fd);

/* Set a socket fd non-blocking (POSIX O_NONBLOCK / Winsock FIONBIO). */
int kl_test_set_nonblock(int fd);

/* write()/read() over a socket fd (send/recv on Windows — read/write don't work
 * on Winsock sockets). Return value matches the POSIX ssize_t contract. */
long kl_test_sockwrite(int fd, const void *buf, size_t len);
long kl_test_sockread(int fd, void *buf, size_t len);

/* Poll a single socket fd for readable (for_write=0) or writable (for_write=1).
 * Returns >0 ready, 0 timeout, -1 error — the poll()/WSAPoll() contract. */
int kl_test_poll1(int fd, int for_write, int timeout_ms);

/* Set a receive timeout (ms) on a socket fd (Winsock DWORD / POSIX timeval). */
int kl_test_set_rcvtimeo(int fd, int ms);

/* A connected stream fd pair — socketpair(AF_UNIX) on POSIX; a self-connected
 * loopback TCP pair on Windows (which has neither socketpair nor pollable pipes).
 * Also the portable replacement for pipe() in tests that need a pollable byte
 * channel. Returns 0 on success, -1 on error; sv[0]/sv[1] are both usable ends. */
int kl_test_socketpair(int sv[2]);

#endif /* KEEL_TESTS_NET_COMPAT_H */
