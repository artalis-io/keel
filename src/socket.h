#ifndef KEEL_SRC_SOCKET_H
#define KEEL_SRC_SOCKET_H

/*
 * socket.h — internal socket seam + provider vtable (PAL Phase 2).
 *
 * Phase 1 centralized the POSIX socket idioms behind free functions. Phase 2
 * turns the seam into a pluggable provider: a KlSocketProvider (immutable ops
 * table + context + capability flags) that a transport can carry so a
 * non-POSIX stack — or, today, a test-only fault-injection mock — can replace
 * the syscalls. See docs/pal_transformation_design.md.
 *
 * INTERNAL header — not installed, no ABI commitment. The one production
 * provider is POSIX. The provider-aware wrappers below take a
 * `const KlSocketProvider *`; a NULL provider selects the inline POSIX fast
 * path with NO indirect call, so production hot paths are unchanged. Signatures
 * still use int fds — portable-handle evolution is a later phase.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <errno.h>
#include <unistd.h>
#include <stdint.h>

#include <keel/error.h>

/* Provider operation table. `ctx` is the provider's own context (NULL for the
 * built-in POSIX provider). Any op may be NULL, in which case the wrapper falls
 * back to the POSIX implementation. */
typedef struct KlSocketOps {
    /* setup */
    int     (*set_nonblocking)(void *ctx, int fd);
    void    (*set_cloexec)(void *ctx, int fd);
    void    (*set_nosigpipe)(void *ctx, int fd);
    /* lifecycle */
    int     (*socket)(void *ctx, int domain, int type, int protocol);
    int     (*connect)(void *ctx, int fd, const struct sockaddr *addr, socklen_t len);
    int     (*bind)(void *ctx, int fd, const struct sockaddr *addr, socklen_t len);
    int     (*listen)(void *ctx, int fd, int backlog);
    int     (*accept)(void *ctx, int fd, struct sockaddr *addr, socklen_t *len);
    int     (*close)(void *ctx, int fd);
    /* I/O */
    ssize_t (*send)(void *ctx, int fd, const void *buf, size_t len);
    ssize_t (*recv)(void *ctx, int fd, void *buf, size_t len);
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

/* Raw POSIX implementations — the inline fast path and the POSIX ops share
 * these. Declared here so the header wrappers can call them when p == NULL. */
int  kl_posix_set_nonblocking(int fd);
void kl_posix_set_cloexec(int fd);
void kl_posix_set_nosigpipe(int fd);

/*
 * Provider-aware wrappers. A NULL provider (production default) takes the inline
 * POSIX path with no indirect call; a non-NULL provider dispatches through its
 * ops (with a per-op POSIX fallback when that op is NULL). send/recv keep the
 * EINTR retry + MSG_NOSIGNAL behaviour of the code they replaced.
 */
static inline int kl_sock_set_nonblocking(const KlSocketProvider *p, int fd) {
    if (p && p->ops->set_nonblocking) return p->ops->set_nonblocking(p->context, fd);
    return kl_posix_set_nonblocking(fd);
}

static inline void kl_sock_set_cloexec(const KlSocketProvider *p, int fd) {
    if (p && p->ops->set_cloexec) { p->ops->set_cloexec(p->context, fd); return; }
    kl_posix_set_cloexec(fd);
}

static inline void kl_sock_set_nosigpipe(const KlSocketProvider *p, int fd) {
    if (p && p->ops->set_nosigpipe) { p->ops->set_nosigpipe(p->context, fd); return; }
    kl_posix_set_nosigpipe(fd);
}

static inline ssize_t kl_sock_send(const KlSocketProvider *p, int fd,
                                   const void *buf, size_t len) {
    if (p && p->ops->send) return p->ops->send(p->context, fd, buf, len);
    ssize_t r;
#ifdef MSG_NOSIGNAL
    do { r = send(fd, buf, len, MSG_NOSIGNAL); } while (r < 0 && errno == EINTR);
#else
    do { r = send(fd, buf, len, 0); } while (r < 0 && errno == EINTR);
#endif
    return r;
}

static inline ssize_t kl_sock_recv(const KlSocketProvider *p, int fd,
                                   void *buf, size_t len) {
    if (p && p->ops->recv) return p->ops->recv(p->context, fd, buf, len);
    ssize_t r;
    do { r = recv(fd, buf, len, 0); } while (r < 0 && errno == EINTR);
    return r;
}

/* Lifecycle wrappers. NULL provider / NULL op → the raw POSIX syscall. These
 * are one-shot (connect/accept/bind/listen) so they are not on any per-byte hot
 * path; the branch is negligible. */
static inline int kl_sock_socket(const KlSocketProvider *p, int domain,
                                 int type, int protocol) {
    if (p && p->ops->socket) return p->ops->socket(p->context, domain, type, protocol);
    return socket(domain, type, protocol);
}

static inline int kl_sock_connect(const KlSocketProvider *p, int fd,
                                  const struct sockaddr *addr, socklen_t len) {
    if (p && p->ops->connect) return p->ops->connect(p->context, fd, addr, len);
    return connect(fd, addr, len);
}

static inline int kl_sock_bind(const KlSocketProvider *p, int fd,
                               const struct sockaddr *addr, socklen_t len) {
    if (p && p->ops->bind) return p->ops->bind(p->context, fd, addr, len);
    return bind(fd, addr, len);
}

static inline int kl_sock_listen(const KlSocketProvider *p, int fd, int backlog) {
    if (p && p->ops->listen) return p->ops->listen(p->context, fd, backlog);
    return listen(fd, backlog);
}

static inline int kl_sock_accept(const KlSocketProvider *p, int fd,
                                 struct sockaddr *addr, socklen_t *len) {
    if (p && p->ops->accept) return p->ops->accept(p->context, fd, addr, len);
    return accept(fd, addr, len);
}

static inline int kl_sock_close(const KlSocketProvider *p, int fd) {
    if (p && p->ops->close) return p->ops->close(p->context, fd);
    return close(fd);
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
 * int handle IS a real OS descriptor (usable with syscalls, poll, etc.) and is
 * returned as-is; otherwise -1 (the caller must not treat it as an OS fd). */
static inline int kl_sock_native_fd(const KlSocketProvider *p, int fd) {
    return kl_socket_provider_has_cap(p, KL_SOCK_CAP_NATIVE_FD) ? fd : -1;
}

/* Map a socket errno to a stable KlError category (errno itself is preserved by
 * the caller for diagnostics). Uses the existing coarse KlError network codes;
 * finer public categories are deferred to the Phase 4 public error taxonomy. */
KlError kl_sock_errno_to_error(int err);

#endif /* KEEL_SRC_SOCKET_H */
