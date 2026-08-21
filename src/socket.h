#ifndef KEEL_SRC_SOCKET_H
#define KEEL_SRC_SOCKET_H

/*
 * src/socket.h — Keel's INTERNAL consumers of the socket seam.
 *
 * The public authoring API — KlSocketProvider, KlSocketOps, KlIoVec, the
 * capability flags, the provider factories, the query helpers, and
 * kl_sock_errno_to_error — lives in the installed <keel/socket.h>. This internal
 * header adds the two things a *consumer* (http_server.c, http_connection.c, http_response.c,
 * client.c, …) needs but a provider author does not:
 *
 *   - kl_sockdef_*  : the built-in platform defaults (raw syscall per op),
 *                     DEFINED per-platform in the provider TU (socket_posix.c /
 *                     socket_winsock.c); declared here for the dispatchers below.
 *   - kl_sock_*     : inline dispatchers that route through a provider's op when
 *                     present, else the kl_sockdef_* default.
 *
 * Logic-neutral: no raw syscall appears here. Internal types (ssize_t, off_t)
 * are used only in these internal decls — the public header exposes none of them.
 *
 * INTERNAL header — not installed, no ABI commitment.
 */

#include <keel/socket.h>      /* public: KlSocketProvider/KlSocketOps/KlIoVec/caps/... */
#include "sockcompat.h"       /* ssize_t (+ struct sockaddr / socklen_t on both platforms) */

/* Internal socket-provider capability (PAL Phase 8), reserving bit 3 out of the
 * public KL_SOCK_CAP_* space (bits 0-2 in <keel/socket.h>). Kept OUT of the public
 * header on purpose: completion-mode I/O is an internal event-axis concern — the
 * provider's data plane is driven by the completion loop's overlapped submit path
 * (WSARecv/WSASend) rather than the synchronous send/recv ops. A completion event
 * loop negotiates against this bit (see kl_caps_compatible). No public API change. */
#define KL_SOCK_CAP_OVERLAPPED (1ull << 3)

/* The overlapped socket provider that pairs with the IOCP completion backend
 * (defined in event_iocp.c, Windows/BACKEND=iocp only). Internal — kept out of the
 * public header alongside its capability. Declared unconditionally; only ever
 * defined/called in the IOCP build. */
const KlSocketProvider *kl_socket_provider_iocp(void);

/* The overlapped socket provider that pairs with the portable poll() completion
 * backend (defined in event_pollcomp.c, BACKEND=pollcomp only). Reuses the POSIX
 * control-plane ops + the OVERLAPPED capability the Phase 7 negotiation keys on, so
 * the completion driver can run — and be tested — on Linux/macOS. Internal; declared
 * unconditionally, defined/called only in the pollcomp build. */
const KlSocketProvider *kl_socket_provider_pollcomp(void);

/* The overlapped socket provider that pairs with the completion-native io_uring backend
 * (defined in event_iouring.c, Linux/BACKEND=iouring only). Like the pollcomp
 * provider it reuses the POSIX control-plane ops + the OVERLAPPED capability; io_uring
 * performs the async I/O over real fds. Internal; declared unconditionally, defined/called
 * only in the iouring build. */
const KlSocketProvider *kl_socket_provider_iouring(void);

/*
 * Platform default socket ops — the raw syscall for each operation, DEFINED
 * per-platform in the provider TU (socket_posix.c: POSIX; socket_winsock.c:
 * Winsock). Declared here so the inline dispatchers below can call them for the
 * NULL-provider / NULL-op default WITHOUT putting any syscall in a header.
 * `kl_sockdef_send`/`recv` keep the EINTR-retry + SIGPIPE-suppression behaviour;
 * `kl_sockdef_sendfile`'s `in_fd` is a *file* descriptor and `offset` a byte
 * offset (uint64_t, matching the public op) translated to off_t in the TU.
 */
int            kl_sockdef_set_nonblocking(KlSocketHandle fd);
int            kl_sockdef_set_blocking(KlSocketHandle fd);
void           kl_sockdef_set_cloexec(KlSocketHandle fd);
void           kl_sockdef_set_nosigpipe(KlSocketHandle fd);
int            kl_sockdef_set_reuseaddr(KlSocketHandle fd, int on);
int            kl_sockdef_set_reuseport(KlSocketHandle fd, int on);
int            kl_sockdef_set_ipv6only(KlSocketHandle fd, int on);
int            kl_sockdef_set_tcp_nodelay(KlSocketHandle fd, int on);
int            kl_sockdef_set_cork(KlSocketHandle fd, int on);
KlSocketHandle kl_sockdef_socket(int domain, int type, int protocol);
int            kl_sockdef_connect(KlSocketHandle fd, const KlSockAddr *addr);
int            kl_sockdef_bind(KlSocketHandle fd, const KlSockAddr *addr);
int            kl_sockdef_listen(KlSocketHandle fd, int backlog);
KlSocketHandle kl_sockdef_accept(KlSocketHandle fd, KlSockAddr *peer);
int            kl_sockdef_close(KlSocketHandle fd);
int            kl_sockdef_get_local_addr(KlSocketHandle fd, KlSockAddr *addr);
int            kl_sockdef_get_so_error(KlSocketHandle fd, int *out_err);
ssize_t        kl_sockdef_send(KlSocketHandle fd, const void *buf, size_t len);
ssize_t        kl_sockdef_recv(KlSocketHandle fd, void *buf, size_t len);
/* The built-in provider's datagram ops (POSIX / Winsock per platform) — the default
 * datagram data-plane when KlEventCtx.sockets is NULL, mirroring the kl_sockdef_*
 * stream fallback. Defined in socket_posix.c / socket_winsock.c. */
struct KlDatagramOps;
const struct KlDatagramOps *kl_sockdef_dgram(void);
ssize_t        kl_sockdef_recv_peek(KlSocketHandle fd, void *buf, size_t len);
ssize_t        kl_sockdef_writev(KlSocketHandle fd, const KlIoVec *iov, int iovcnt);
ssize_t        kl_sockdef_sendfile(KlSocketHandle out_fd, int in_fd, uint64_t *offset, size_t count);
/* The hosted default I/O-result classifier: maps the current `errno` to a
 * KlIoStatus. This is the ONLY place the errno mapping lives — the inline
 * dispatcher below never reads errno itself, so this header stays freestanding.
 * Defined per-platform in the provider TU (socket_posix.c / socket_winsock.c). */
KlIoStatus     kl_sockdef_io_status(void);

/*
 * Provider-aware wrappers. Inline dispatch: a non-NULL provider whose op is set
 * goes straight through the ops table; otherwise the platform default
 * `kl_sockdef_*` (one direct call — negligible next to the syscall it wraps).
 * No raw syscall in this header, so it compiles on any platform. Returns are
 * ssize_t internally (== kl_ssize_t == pointer-width) for the consumers' benefit.
 */
static inline int kl_sock_set_nonblocking(const KlSocketProvider *p, KlSocketHandle fd) {
    if (p && p->ops->set_nonblocking) return p->ops->set_nonblocking(p->context, fd);
    return kl_sockdef_set_nonblocking(fd);
}

static inline int kl_sock_set_blocking(const KlSocketProvider *p, KlSocketHandle fd) {
    if (p && p->ops->set_blocking) return p->ops->set_blocking(p->context, fd);
    return kl_sockdef_set_blocking(fd);
}

static inline void kl_sock_set_cloexec(const KlSocketProvider *p, KlSocketHandle fd) {
    if (p && p->ops->set_cloexec) { p->ops->set_cloexec(p->context, fd); return; }
    kl_sockdef_set_cloexec(fd);
}

static inline void kl_sock_set_nosigpipe(const KlSocketProvider *p, KlSocketHandle fd) {
    if (p && p->ops->set_nosigpipe) { p->ops->set_nosigpipe(p->context, fd); return; }
    kl_sockdef_set_nosigpipe(fd);
}

static inline int kl_sock_set_reuseaddr(const KlSocketProvider *p, KlSocketHandle fd, int on) {
    if (p && p->ops->set_reuseaddr) return p->ops->set_reuseaddr(p->context, fd, on);
    return kl_sockdef_set_reuseaddr(fd, on);
}

static inline int kl_sock_set_reuseport(const KlSocketProvider *p, KlSocketHandle fd, int on) {
    if (p && p->ops->set_reuseport) return p->ops->set_reuseport(p->context, fd, on);
    return kl_sockdef_set_reuseport(fd, on);
}

static inline int kl_sock_set_ipv6only(const KlSocketProvider *p, KlSocketHandle fd, int on) {
    if (p && p->ops->set_ipv6only) return p->ops->set_ipv6only(p->context, fd, on);
    return kl_sockdef_set_ipv6only(fd, on);
}

static inline int kl_sock_set_tcp_nodelay(const KlSocketProvider *p, KlSocketHandle fd, int on) {
    if (p && p->ops->set_tcp_nodelay) return p->ops->set_tcp_nodelay(p->context, fd, on);
    return kl_sockdef_set_tcp_nodelay(fd, on);
}

static inline int kl_sock_set_cork(const KlSocketProvider *p, KlSocketHandle fd, int on) {
    if (p && p->ops->set_cork) return p->ops->set_cork(p->context, fd, on);
    return kl_sockdef_set_cork(fd, on);
}

static inline ssize_t kl_sock_send(const KlSocketProvider *p, KlSocketHandle fd,
                                   const void *buf, size_t len) {
    if (p && p->ops->send) return p->ops->send(p->context, fd, buf, len);
    return kl_sockdef_send(fd, buf, len);
}

static inline ssize_t kl_sock_recv(const KlSocketProvider *p, KlSocketHandle fd,
                                   void *buf, size_t len) {
    if (p && p->ops->recv) return p->ops->recv(p->context, fd, buf, len);
    return kl_sockdef_recv(fd, buf, len);
}

static inline KlSocketHandle kl_sock_socket(const KlSocketProvider *p, int domain,
                                            int type, int protocol) {
    if (p && p->ops->socket) return p->ops->socket(p->context, domain, type, protocol);
    return kl_sockdef_socket(domain, type, protocol);
}

static inline int kl_sock_connect(const KlSocketProvider *p, KlSocketHandle fd,
                                  const KlSockAddr *addr) {
    if (p && p->ops->connect) return p->ops->connect(p->context, fd, addr);
    return kl_sockdef_connect(fd, addr);
}

static inline int kl_sock_bind(const KlSocketProvider *p, KlSocketHandle fd,
                               const KlSockAddr *addr) {
    if (p && p->ops->bind) return p->ops->bind(p->context, fd, addr);
    return kl_sockdef_bind(fd, addr);
}

static inline int kl_sock_listen(const KlSocketProvider *p, KlSocketHandle fd, int backlog) {
    if (p && p->ops->listen) return p->ops->listen(p->context, fd, backlog);
    return kl_sockdef_listen(fd, backlog);
}

static inline KlSocketHandle kl_sock_accept(const KlSocketProvider *p, KlSocketHandle fd,
                                            KlSockAddr *peer) {
    if (p && p->ops->accept) return p->ops->accept(p->context, fd, peer);
    return kl_sockdef_accept(fd, peer);
}

static inline int kl_sock_close(const KlSocketProvider *p, KlSocketHandle fd) {
    if (p && p->ops->close) return p->ops->close(p->context, fd);
    return kl_sockdef_close(fd);
}

static inline int kl_sock_get_local_addr(const KlSocketProvider *p, KlSocketHandle fd,
                                         KlSockAddr *addr) {
    if (p && p->ops->get_local_addr) return p->ops->get_local_addr(p->context, fd, addr);
    return kl_sockdef_get_local_addr(fd, addr);
}

static inline int kl_sock_get_so_error(const KlSocketProvider *p, KlSocketHandle fd, int *out_err) {
    if (p && p->ops->get_so_error) return p->ops->get_so_error(p->context, fd, out_err);
    return kl_sockdef_get_so_error(fd, out_err);
}

/* Classify the provider's most recent -1 I/O result. Op-or-sockdef dispatch,
 * exactly like kl_sock_get_so_error: a provider that supplies io_status reports
 * the category itself; otherwise the hosted errno mapping (kl_sockdef_io_status).
 * This inline reads NO errno of its own — the errno mapping is confined to
 * kl_sockdef_io_status — so it compiles freestanding. */
static inline KlIoStatus kl_sock_io_status(const KlSocketProvider *p) {
    if (p && p->ops->io_status) return p->ops->io_status(p->context);
    return kl_sockdef_io_status();
}

static inline ssize_t kl_sock_recv_peek(const KlSocketProvider *p, KlSocketHandle fd,
                                        void *buf, size_t len) {
    if (p && p->ops->recv_peek) return p->ops->recv_peek(p->context, fd, buf, len);
    return kl_sockdef_recv_peek(fd, buf, len);
}

static inline ssize_t kl_sock_writev(const KlSocketProvider *p, KlSocketHandle fd,
                                     const KlIoVec *iov, int iovcnt) {
    if (p && p->ops->writev) return p->ops->writev(p->context, fd, iov, iovcnt);
    return kl_sockdef_writev(fd, iov, iovcnt);
}

static inline ssize_t kl_sock_sendfile(const KlSocketProvider *p, KlSocketHandle out_fd,
                                       int in_fd, uint64_t *offset, size_t count) {
    if (p && p->ops->sendfile)
        return p->ops->sendfile(p->context, out_fd, in_fd, offset, count);
    return kl_sockdef_sendfile(out_fd, in_fd, offset, count);
}

#endif /* KEEL_SRC_SOCKET_H */
