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
    void    (*set_cloexec)(void *ctx, KlSocketHandle fd);
    void    (*set_nosigpipe)(void *ctx, KlSocketHandle fd);
    /* lifecycle. `socket`/`accept` return a KlSocketHandle (KL_INVALID_SOCKET on
     * failure) — a Winsock SOCKET is pointer-width, hence the handle type. */
    KlSocketHandle (*socket)(void *ctx, int domain, int type, int protocol);
    int     (*connect)(void *ctx, KlSocketHandle fd, const struct sockaddr *addr, socklen_t len);
    int     (*bind)(void *ctx, KlSocketHandle fd, const struct sockaddr *addr, socklen_t len);
    int     (*listen)(void *ctx, KlSocketHandle fd, int backlog);
    KlSocketHandle (*accept)(void *ctx, KlSocketHandle fd, struct sockaddr *addr, socklen_t *len);
    int     (*close)(void *ctx, KlSocketHandle fd);
    /* I/O */
    ssize_t (*send)(void *ctx, KlSocketHandle fd, const void *buf, size_t len);
    ssize_t (*recv)(void *ctx, KlSocketHandle fd, void *buf, size_t len);
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

/* The built-in POSIX provider (static storage, no allocation). */
const KlSocketProvider *kl_socket_provider_posix(void);

/*
 * Platform default socket ops — the raw syscall for each operation, DEFINED
 * per-platform in the provider TU (socket_posix.c: POSIX; socket_winsock.c:
 * Winsock). Declared here so the inline dispatchers below can call them for the
 * NULL-provider / NULL-op default WITHOUT putting any syscall in this header.
 * `kl_sockdef_send`/`recv` keep the EINTR-retry + SIGPIPE-suppression behaviour;
 * `kl_sockdef_sendfile`'s `in_fd` is a *file* descriptor.
 */
int            kl_sockdef_set_nonblocking(KlSocketHandle fd);
void           kl_sockdef_set_cloexec(KlSocketHandle fd);
void           kl_sockdef_set_nosigpipe(KlSocketHandle fd);
KlSocketHandle kl_sockdef_socket(int domain, int type, int protocol);
int            kl_sockdef_connect(KlSocketHandle fd, const struct sockaddr *addr, socklen_t len);
int            kl_sockdef_bind(KlSocketHandle fd, const struct sockaddr *addr, socklen_t len);
int            kl_sockdef_listen(KlSocketHandle fd, int backlog);
KlSocketHandle kl_sockdef_accept(KlSocketHandle fd, struct sockaddr *addr, socklen_t *len);
int            kl_sockdef_close(KlSocketHandle fd);
ssize_t        kl_sockdef_send(KlSocketHandle fd, const void *buf, size_t len);
ssize_t        kl_sockdef_recv(KlSocketHandle fd, void *buf, size_t len);
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

static inline void kl_sock_set_cloexec(const KlSocketProvider *p, KlSocketHandle fd) {
    if (p && p->ops->set_cloexec) { p->ops->set_cloexec(p->context, fd); return; }
    kl_sockdef_set_cloexec(fd);
}

static inline void kl_sock_set_nosigpipe(const KlSocketProvider *p, KlSocketHandle fd) {
    if (p && p->ops->set_nosigpipe) { p->ops->set_nosigpipe(p->context, fd); return; }
    kl_sockdef_set_nosigpipe(fd);
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
