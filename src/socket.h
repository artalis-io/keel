#ifndef KEEL_SRC_SOCKET_H
#define KEEL_SRC_SOCKET_H

/*
 * socket.h — the platform-neutral socket seam + provider vtable.
 *
 * The interface: a KlSocketProvider (immutable ops table + context + capability
 * flags) that a transport carries so a non-POSIX stack (Winsock, lwIP) — or a
 * test-only fault-injection mock — can replace the syscalls. The analog of
 * event.h: this header DECLARES; the per-platform provider TUs (socket_posix.c,
 * socket_winsock.c, …) DEFINE. See docs/pal_transformation_design.md and
 * docs/phase6_winsock_design.md §B.0.
 *
 * Logic-neutral by construction: no raw syscall appears in this header. The
 * inline wrappers dispatch through a provider's ops when present, else call the
 * platform default `kl_sockdef_*` (defined per-platform in the provider TU) — so
 * the header compiles on any platform. The only platform-specific thing is the
 * system-header selection for socket types, isolated in sockcompat.h.
 *
 * INTERNAL header — not installed, no ABI commitment.
 */

#include <stdint.h>

#include "sockcompat.h"       /* struct sockaddr / socklen_t / struct iovec / ssize_t */
#include <keel/error.h>
#include <keel/handle.h>

/* Provider operation table. `ctx` is the provider's own context (NULL for the
 * built-in POSIX provider). Any op may be NULL, in which case the wrapper falls
 * back to the POSIX implementation. */
typedef struct KlSocketOps {
    /* setup */
    int     (*set_nonblocking)(void *ctx, KlSocketHandle fd);
    int     (*set_blocking)(void *ctx, KlSocketHandle fd);   /* inverse of set_nonblocking */
    void    (*set_cloexec)(void *ctx, KlSocketHandle fd);
    void    (*set_nosigpipe)(void *ctx, KlSocketHandle fd);
    /* Socket options (each maps to a single setsockopt on POSIX/Winsock). `on` is
     * a boolean toggle. Best-effort by contract — a provider/platform that lacks
     * the option returns -1 and the caller ignores it (SO_REUSEPORT, IPV6_V6ONLY,
     * TCP_NODELAY are all tuning knobs, not correctness gates). */
    int     (*set_reuseaddr)(void *ctx, KlSocketHandle fd, int on);
    int     (*set_reuseport)(void *ctx, KlSocketHandle fd, int on);
    int     (*set_ipv6only)(void *ctx, KlSocketHandle fd, int on);
    int     (*set_tcp_nodelay)(void *ctx, KlSocketHandle fd, int on);
    /* Cork/uncork to coalesce header+file into fewer segments during a file
     * send: on=1 before, on=0 (flush) after. Linux TCP_CORK / macOS TCP_NOPUSH;
     * best-effort (-1 where unavailable — Windows TransmitFile coalesces on its
     * own, so it's a no-op there). */
    int     (*set_cork)(void *ctx, KlSocketHandle fd, int on);
    /* lifecycle. `socket`/`accept` return a KlSocketHandle (KL_INVALID_SOCKET on
     * failure) — a Winsock SOCKET is pointer-width, hence the handle type. */
    KlSocketHandle (*socket)(void *ctx, int domain, int type, int protocol);
    int     (*connect)(void *ctx, KlSocketHandle fd, const struct sockaddr *addr, socklen_t len);
    int     (*bind)(void *ctx, KlSocketHandle fd, const struct sockaddr *addr, socklen_t len);
    int     (*listen)(void *ctx, KlSocketHandle fd, int backlog);
    KlSocketHandle (*accept)(void *ctx, KlSocketHandle fd, struct sockaddr *addr, socklen_t *len);
    int     (*close)(void *ctx, KlSocketHandle fd);
    /* Read the local (bound) address — getsockname. Used for ephemeral-port
     * readback and to recover the family of an adopted (socket-activation) fd. */
    int     (*get_local_addr)(void *ctx, KlSocketHandle fd, struct sockaddr *addr, socklen_t *len);
    /* Read + clear the pending socket error — getsockopt(SO_ERROR). Writes the
     * error to *out_err (0 = none) and returns 0, or returns -1 if the query
     * itself fails (leaving *out_err untouched). Used for async connect
     * completion: 0 means the nonblocking connect succeeded. The value is a
     * platform error code (errno / WSA*); callers only test zero vs non-zero. */
    int     (*get_so_error)(void *ctx, KlSocketHandle fd, int *out_err);
    /* I/O */
    ssize_t (*send)(void *ctx, KlSocketHandle fd, const void *buf, size_t len);
    ssize_t (*recv)(void *ctx, KlSocketHandle fd, void *buf, size_t len);
    /* Peek up to @len bytes without consuming them (recv/MSG_PEEK). >0 = bytes
     * available (count returned), 0 = peer closed, <0 = error/would-block. Used
     * to test for a pending byte before TLS/HTTP (len 1) and to peek+parse a
     * whole PROXY-protocol header (len N) before consuming it. */
    ssize_t (*recv_peek)(void *ctx, KlSocketHandle fd, void *buf, size_t len);
    /* Vectored write + zero-copy file send. May be NULL — a provider without
     * them advertises no WRITEV/SENDFILE capability and the caller serializes /
     * pread-sends instead. POSIX fills these; Winsock will use WSASend /
     * TransmitFile. `in_fd` is a *file* descriptor (stays int — a CRT fd on
     * Windows); `out_fd` is a socket handle. `sendfile` advances `*offset`. */
    ssize_t (*writev)(void *ctx, KlSocketHandle fd, const struct iovec *iov, int iovcnt);
    ssize_t (*sendfile)(void *ctx, KlSocketHandle out_fd, int in_fd, off_t *offset, size_t count);
    /* lifecycle: release provider-owned context. May be NULL (nothing to free,
     * e.g. the static POSIX provider). */
    void    (*destroy)(void *ctx);
    const char *name;                 /* provider identity, for diagnostics */
} KlSocketOps;

typedef struct KlSocketProvider {
    const KlSocketOps *ops;
    void              *context;
    uint64_t           capabilities;
} KlSocketProvider;

/* Capability flags. */
#define KL_SOCK_CAP_NATIVE_FD  (1ull << 0)  /* fd is a real OS descriptor */
#define KL_SOCK_CAP_WRITEV     (1ull << 1)  /* POSIX writev() usable on this fd */
#define KL_SOCK_CAP_SENDFILE   (1ull << 2)  /* POSIX sendfile() usable on this fd */

/* Built-in provider factories (static storage, no allocation). Each is defined
 * in its own platform TU: kl_socket_provider_posix() in socket_posix.c,
 * kl_socket_provider_winsock() in socket_winsock.c (defined only on Windows;
 * the declaration is unconditional but only ever called on Windows). */
const KlSocketProvider *kl_socket_provider_posix(void);
const KlSocketProvider *kl_socket_provider_winsock(void);

/*
 * Platform default socket ops — the raw syscall for each operation, DEFINED
 * per-platform in the provider TU (socket_posix.c: POSIX; socket_winsock.c:
 * Winsock). Declared here so the inline dispatchers below can call them for the
 * NULL-provider / NULL-op default WITHOUT putting any syscall in this header.
 * `kl_sockdef_send`/`recv` keep the EINTR-retry + SIGPIPE-suppression behaviour;
 * `kl_sockdef_sendfile`'s `in_fd` is a *file* descriptor.
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
int            kl_sockdef_connect(KlSocketHandle fd, const struct sockaddr *addr, socklen_t len);
int            kl_sockdef_bind(KlSocketHandle fd, const struct sockaddr *addr, socklen_t len);
int            kl_sockdef_listen(KlSocketHandle fd, int backlog);
KlSocketHandle kl_sockdef_accept(KlSocketHandle fd, struct sockaddr *addr, socklen_t *len);
int            kl_sockdef_close(KlSocketHandle fd);
int            kl_sockdef_get_local_addr(KlSocketHandle fd, struct sockaddr *addr, socklen_t *len);
int            kl_sockdef_get_so_error(KlSocketHandle fd, int *out_err);
ssize_t        kl_sockdef_send(KlSocketHandle fd, const void *buf, size_t len);
ssize_t        kl_sockdef_recv(KlSocketHandle fd, void *buf, size_t len);
ssize_t        kl_sockdef_recv_peek(KlSocketHandle fd, void *buf, size_t len);
ssize_t        kl_sockdef_writev(KlSocketHandle fd, const struct iovec *iov, int iovcnt);
ssize_t        kl_sockdef_sendfile(KlSocketHandle out_fd, int in_fd, off_t *offset, size_t count);

/*
 * Provider-aware wrappers. Inline dispatch: a non-NULL provider whose op is set
 * goes straight through the ops table; otherwise the platform default
 * `kl_sockdef_*` (one direct call — negligible next to the syscall it wraps).
 * No raw syscall in this header, so it compiles on any platform.
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
                                  const struct sockaddr *addr, socklen_t len) {
    if (p && p->ops->connect) return p->ops->connect(p->context, fd, addr, len);
    return kl_sockdef_connect(fd, addr, len);
}

static inline int kl_sock_bind(const KlSocketProvider *p, KlSocketHandle fd,
                               const struct sockaddr *addr, socklen_t len) {
    if (p && p->ops->bind) return p->ops->bind(p->context, fd, addr, len);
    return kl_sockdef_bind(fd, addr, len);
}

static inline int kl_sock_listen(const KlSocketProvider *p, KlSocketHandle fd, int backlog) {
    if (p && p->ops->listen) return p->ops->listen(p->context, fd, backlog);
    return kl_sockdef_listen(fd, backlog);
}

static inline KlSocketHandle kl_sock_accept(const KlSocketProvider *p, KlSocketHandle fd,
                                            struct sockaddr *addr, socklen_t *len) {
    if (p && p->ops->accept) return p->ops->accept(p->context, fd, addr, len);
    return kl_sockdef_accept(fd, addr, len);
}

static inline int kl_sock_close(const KlSocketProvider *p, KlSocketHandle fd) {
    if (p && p->ops->close) return p->ops->close(p->context, fd);
    return kl_sockdef_close(fd);
}

static inline int kl_sock_get_local_addr(const KlSocketProvider *p, KlSocketHandle fd,
                                         struct sockaddr *addr, socklen_t *len) {
    if (p && p->ops->get_local_addr) return p->ops->get_local_addr(p->context, fd, addr, len);
    return kl_sockdef_get_local_addr(fd, addr, len);
}

static inline int kl_sock_get_so_error(const KlSocketProvider *p, KlSocketHandle fd, int *out_err) {
    if (p && p->ops->get_so_error) return p->ops->get_so_error(p->context, fd, out_err);
    return kl_sockdef_get_so_error(fd, out_err);
}

static inline ssize_t kl_sock_recv_peek(const KlSocketProvider *p, KlSocketHandle fd,
                                        void *buf, size_t len) {
    if (p && p->ops->recv_peek) return p->ops->recv_peek(p->context, fd, buf, len);
    return kl_sockdef_recv_peek(fd, buf, len);
}

static inline ssize_t kl_sock_writev(const KlSocketProvider *p, KlSocketHandle fd,
                                     const struct iovec *iov, int iovcnt) {
    if (p && p->ops->writev) return p->ops->writev(p->context, fd, iov, iovcnt);
    return kl_sockdef_writev(fd, iov, iovcnt);
}

static inline ssize_t kl_sock_sendfile(const KlSocketProvider *p, KlSocketHandle out_fd,
                                       int in_fd, off_t *offset, size_t count) {
    if (p && p->ops->sendfile)
        return p->ops->sendfile(p->context, out_fd, in_fd, offset, count);
    return kl_sockdef_sendfile(out_fd, in_fd, offset, count);
}

/* ── Lifecycle / capabilities / error taxonomy (Phase 3 semantics) ──────── */

/* Release a provider's own context. NULL provider (POSIX) and a NULL destroy op
 * are both no-ops. The KlSocketProvider struct's storage is the owner's; this
 * only tears down provider-owned state. Providers are borrowed by transports
 * and must outlive them. */
static inline void kl_socket_provider_destroy(const KlSocketProvider *p) {
    if (p && p->ops->destroy) p->ops->destroy(p->context);
}

/* Capability query. A NULL provider is the built-in POSIX provider, which is
 * native-fd. */
static inline int kl_socket_provider_has_cap(const KlSocketProvider *p,
                                             uint64_t cap) {
    uint64_t caps = p ? p->capabilities : KL_SOCK_CAP_NATIVE_FD;
    return (caps & cap) != 0;
}

/* Native-descriptor escape hatch: when the provider advertises native fds, the
 * handle IS a real OS descriptor (usable with syscalls, poll, etc.) and is
 * returned as-is; otherwise KL_INVALID_SOCKET (the caller must not treat it as
 * an OS fd). */
static inline KlSocketHandle kl_sock_native_fd(const KlSocketProvider *p, KlSocketHandle fd) {
    return kl_socket_provider_has_cap(p, KL_SOCK_CAP_NATIVE_FD) ? fd : KL_INVALID_SOCKET;
}

/* Map a socket errno to a stable KlError category (errno itself is preserved by
 * the caller for diagnostics). Uses the existing coarse KlError network codes;
 * finer public categories are deferred to the Phase 4 public error taxonomy. */
KlError kl_sock_errno_to_error(int err);

#endif /* KEEL_SRC_SOCKET_H */
