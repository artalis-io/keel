/*
 * net_compat.h — portable network glue for the test harnesses.
 *
 * Tests use raw POSIX socket idioms (socket/socketpair/pipe/close/read/write/
 * fcntl/poll) as ad-hoc clients that drive the server or feed a stream. Those
 * headers and calls don't exist (or mean something else) under Winsock, so this
 * header provides the portable includes plus a small set of `kl_test_*` helpers
 * that map to the POSIX call on POSIX and the Winsock equivalent on Windows.
 * Ported tests call the helpers instead of the raw idioms; behavior on POSIX is
 * unchanged (thin wrappers). Test-only — never compiled into the library.
 *
 * Socket fds are stored in `int` here (as the tests always have). A Windows
 * SOCKET is pointer-width, but loopback test sockets are small kernel handles in
 * practice, so the narrowing is safe for the handful of fds a test opens.
 */
#ifndef KEEL_TESTS_NET_COMPAT_H
#define KEEL_TESTS_NET_COMPAT_H

#include <string.h>   /* memset in kl_test_socketpair */

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  #include <stddef.h>
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <poll.h>
  #include <sys/uio.h>
  #include <sys/un.h>
  #include <unistd.h>
  #include <fcntl.h>
#endif

/* close() a socket fd. */
static inline int kl_test_closesock(int fd) {
#if defined(_WIN32)
    return closesocket((SOCKET)fd);
#else
    return close(fd);
#endif
}

/* Set a socket fd non-blocking (POSIX O_NONBLOCK / Winsock FIONBIO). */
static inline int kl_test_set_nonblock(int fd) {
#if defined(_WIN32)
    u_long m = 1;
    return ioctlsocket((SOCKET)fd, FIONBIO, &m) == 0 ? 0 : -1;
#else
    int fl = fcntl(fd, F_GETFL, 0);
    return fl < 0 ? -1 : fcntl(fd, F_SETFL, fl | O_NONBLOCK);
#endif
}

/* write()/read() over a socket fd (send/recv on Windows — read/write don't work
 * on Winsock sockets). Return value matches the POSIX ssize_t contract. */
static inline long kl_test_sockwrite(int fd, const void *buf, size_t len) {
#if defined(_WIN32)
    return send((SOCKET)fd, (const char *)buf, (int)len, 0);
#else
    return (long)write(fd, buf, len);
#endif
}
static inline long kl_test_sockread(int fd, void *buf, size_t len) {
#if defined(_WIN32)
    return recv((SOCKET)fd, (char *)buf, (int)len, 0);
#else
    return (long)read(fd, buf, len);
#endif
}

/* Poll a single socket fd for readable (for_write=0) or writable (for_write=1).
 * Returns >0 ready, 0 timeout, -1 error — the poll()/WSAPoll() contract. */
static inline int kl_test_poll1(int fd, int for_write, int timeout_ms) {
#if defined(_WIN32)
    WSAPOLLFD p;
    p.fd = (SOCKET)fd;
    p.events = (SHORT)(for_write ? POLLWRNORM : POLLRDNORM);
    p.revents = 0;
    return WSAPoll(&p, 1, timeout_ms);
#else
    struct pollfd p;
    p.fd = fd;
    p.events = (short)(for_write ? POLLOUT : POLLIN);
    p.revents = 0;
    return poll(&p, 1, timeout_ms);
#endif
}

/* Set a receive timeout on a socket fd. Winsock's SO_RCVTIMEO takes a DWORD of
 * milliseconds; POSIX takes a struct timeval — hence the split. */
static inline int kl_test_set_rcvtimeo(int fd, int ms) {
#if defined(_WIN32)
    DWORD tv = (DWORD)ms;
    return setsockopt((SOCKET)fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
#else
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
#endif
}

/* A connected stream fd pair — socketpair(AF_UNIX) on POSIX; a self-connected
 * loopback TCP pair on Windows (which has neither socketpair nor pollable pipes).
 * Also the portable replacement for pipe() in tests that need a pollable byte
 * channel. Returns 0 on success, -1 on error; sv[0]/sv[1] are both usable ends. */
static inline int kl_test_socketpair(int sv[2]) {
#if defined(_WIN32)
    SOCKET listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == INVALID_SOCKET) return -1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    int addrlen = (int)sizeof(addr);
    SOCKET client = INVALID_SOCKET, server = INVALID_SOCKET;
    if (bind(listener, (struct sockaddr *)&addr, addrlen) != 0) goto fail;
    if (listen(listener, 1) != 0) goto fail;
    if (getsockname(listener, (struct sockaddr *)&addr, &addrlen) != 0) goto fail;
    client = socket(AF_INET, SOCK_STREAM, 0);
    if (client == INVALID_SOCKET) goto fail;
    if (connect(client, (struct sockaddr *)&addr, addrlen) != 0) goto fail;
    server = accept(listener, NULL, NULL);
    if (server == INVALID_SOCKET) goto fail;
    closesocket(listener);
    sv[0] = (int)client;
    sv[1] = (int)server;
    return 0;
fail:
    if (listener != INVALID_SOCKET) closesocket(listener);
    if (client != INVALID_SOCKET) closesocket(client);
    if (server != INVALID_SOCKET) closesocket(server);
    return -1;
#else
    return socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
#endif
}

#endif /* KEEL_TESTS_NET_COMPAT_H */
