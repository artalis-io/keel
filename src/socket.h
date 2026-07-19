#ifndef KEEL_SRC_SOCKET_H
#define KEEL_SRC_SOCKET_H

/*
 * socket.h — internal POSIX socket seam (PAL Phase 1).
 *
 * Centralizes the socket create/setup/I/O idioms that were duplicated across the
 * client transports (client, h2_client, websocket_client), the DNS resolver, and
 * the server: the O_NONBLOCK / FD_CLOEXEC fcntl dances, the SO_NOSIGPIPE option,
 * and the MSG_NOSIGNAL-or-plain send/recv loop.
 *
 * This is an INTERNAL header — not installed, no ABI commitment. There is exactly
 * one production provider today (POSIX). It exists so these platform assumptions
 * live behind one seam that a future non-POSIX socket provider can replace; see
 * docs/pal_transformation_design.md. Signatures deliberately still use int fds —
 * portable-handle evolution is a later phase.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <errno.h>
#include <unistd.h>

/* Put fd into non-blocking mode. Returns 0 on success, -1 on error (errno set). */
int  kl_sock_set_nonblocking(int fd);

/* Set close-on-exec so the fd is not leaked into exec()'d children. Best-effort;
 * done via fcntl for portability (macOS lacks SOCK_CLOEXEC on socket()). */
void kl_sock_set_cloexec(int fd);

/* Suppress SIGPIPE on writes to this socket where the platform offers a
 * socket-level option (SO_NOSIGPIPE, BSD/macOS). A no-op elsewhere; on those
 * platforms writes carry MSG_NOSIGNAL instead — see kl_sock_send(). Best-effort. */
void kl_sock_set_nosigpipe(int fd);

/*
 * send()/recv() with EINTR retry and SIGPIPE suppression (MSG_NOSIGNAL where the
 * platform defines it). On a connected socket that also has SO_NOSIGPIPE set,
 * these are behaviourally identical to the write()/read() loops they replace.
 * Kept inline so the hot path has no call-overhead delta versus the originals.
 */
static inline ssize_t kl_sock_send(int fd, const void *buf, size_t len) {
    ssize_t r;
#ifdef MSG_NOSIGNAL
    do { r = send(fd, buf, len, MSG_NOSIGNAL); } while (r < 0 && errno == EINTR);
#else
    do { r = send(fd, buf, len, 0); } while (r < 0 && errno == EINTR);
#endif
    return r;
}

static inline ssize_t kl_sock_recv(int fd, void *buf, size_t len) {
    ssize_t r;
    do { r = recv(fd, buf, len, 0); } while (r < 0 && errno == EINTR);
    return r;
}

#endif /* KEEL_SRC_SOCKET_H */
