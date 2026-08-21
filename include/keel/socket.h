#ifndef KEEL_SOCKET_H
#define KEEL_SOCKET_H

/*
 * keel/socket.h — public socket-provider authoring API (PAL Phase 4).
 *
 * A KlSocketProvider is an immutable ops table + context + capability flags that
 * a Keel server/client carries so a non-POSIX stack (Winsock, lwIP) — or a
 * fault-injection mock — can replace the socket syscalls. This is the authoring
 * surface: to bring your own stack, implement KlSocketOps and hand Keel a
 * KlSocketProvider (via KlHttpServerConfig.sockets / KlHttpClientConfig.sockets / KlEventCtx.
 * sockets). The built-in providers (kl_socket_provider_posix/winsock) are the
 * default.
 *
 * Portability contract: this header exposes NO platform-only types. Sizes are
 * kl_ssize_t (pointer-width signed); scatter-gather is the Keel-owned KlIoVec;
 * the sendfile offset is uint64_t; addresses are the Keel-owned KlSockAddr
 * (keel/sockaddr.h), so a provider never sees a platform struct sockaddr on the
 * connect/bind/get_local_addr ops — it marshals KlSockAddr to its native address
 * shape inside its own implementation (see src/sockaddr_native.h for the built-in
 * providers). A provider likewise translates KlIoVec to its native vector (POSIX
 * struct iovec, Winsock WSABUF, …). All address ops — connect/bind/accept/
 * get_local_addr — are KlSockAddr; no platform struct sockaddr crosses this API.
 *
 * The `kl_sock_*` consumer wrappers and `kl_sockdef_*` platform defaults are NOT
 * here — they are Keel's internal consumers of a provider (src/socket.h), not
 * part of the authoring API. A custom provider implements KlSocketOps; it never
 * calls them.
 */

#include <stddef.h>            /* size_t */
#include <stdint.h>            /* intptr_t, uint64_t */

#include <keel/handle.h>       /* KlSocketHandle, KL_INVALID_SOCKET, kl_handle_valid, kl_ssize_t */
#include <keel/sockaddr.h>     /* KlSockAddr (connect/bind/accept/get_local_addr currency) */
#include <keel/error.h>        /* KlError */
#include <keel/socket_dgram.h> /* KlDatagramOps — the optional provider->dgram data-plane vtable */

#ifdef __cplusplus
extern "C" {
#endif

/* Keel-owned scatter-gather vector — the seam's I/O-vector currency. A provider
 * translates it to its native vector (POSIX struct iovec / Winsock WSABUF) inside
 * its own TU, so no platform vector type appears in this API. */
typedef struct KlIoVec {
    void  *base;
    size_t len;
} KlIoVec;

/* Portable classification of the most recent -1 I/O result, so a consumer never
 * reads a hosted `errno` to decide would-block / re-arm / connect-pending / fail.
 * This keeps the readiness/completion control flow freestanding: a non-POSIX or
 * freestanding stack (UEFI, lwIP) has no hosted errno, so classification lives on
 * the provider (KlSocketOps.io_status, additive/optional). The built-in POSIX /
 * Winsock providers leave that op NULL and Keel falls back to its hosted errno
 * mapping — behaviour-neutral for every current provider. */
typedef enum {
    KL_IO_OK = 0,        /* no error (the last op succeeded) */
    KL_IO_WOULD_BLOCK,   /* EAGAIN / EWOULDBLOCK — retry when ready (re-arm) */
    KL_IO_INTERRUPTED,   /* EINTR — retry immediately (loop continue) */
    KL_IO_PENDING,       /* EINPROGRESS — async connect in progress */
    KL_IO_CLOSED,        /* peer closed / EPIPE */
    KL_IO_RESET,         /* ECONNRESET — peer reset */
    KL_IO_FATAL,         /* any other error */
    KL_IO_UNSUPPORTED    /* EOPNOTSUPP / ENOTSUP — op unavailable on this fd (e.g. UDP GSO); caller may
                          * fall back. Appended (datagram M5.1) — values above are unchanged. */
} KlIoStatus;

/* Upper bound on scatter-gather segments a provider must handle in one writev.
 * Response assembly uses <= 7 (status line + headers + body); a provider
 * translates into a stack vector of this size and fails EINVAL beyond it. */
#define KL_SOCK_IOV_MAX 16

/* Provider operation table. `ctx` is the provider's own context (NULL for the
 * built-in POSIX provider). Any op may be NULL, in which case Keel falls back to
 * its built-in default (native syscall) for that operation. */
typedef struct KlSocketOps {
    /* setup */
    int     (*set_nonblocking)(void *ctx, KlSocketHandle fd);
    int     (*set_blocking)(void *ctx, KlSocketHandle fd);   /* inverse of set_nonblocking */
    void    (*set_cloexec)(void *ctx, KlSocketHandle fd);
    void    (*set_nosigpipe)(void *ctx, KlSocketHandle fd);
    /* Socket options (each maps to a single setsockopt on POSIX/Winsock). `on` is
     * a boolean toggle. Best-effort by contract — a provider/platform that lacks
     * the option returns -1 and the caller ignores it (SO_REUSEPORT, IPV6_V6ONLY,
     * TCP_NODELAY are tuning knobs, not correctness gates). */
    int     (*set_reuseaddr)(void *ctx, KlSocketHandle fd, int on);
    int     (*set_reuseport)(void *ctx, KlSocketHandle fd, int on);
    int     (*set_ipv6only)(void *ctx, KlSocketHandle fd, int on);
    int     (*set_tcp_nodelay)(void *ctx, KlSocketHandle fd, int on);
    /* Cork/uncork to coalesce header+file into fewer segments during a file send:
     * on=1 before, on=0 (flush) after. Best-effort (-1 where unavailable). */
    int     (*set_cork)(void *ctx, KlSocketHandle fd, int on);
    /* lifecycle. `socket`/`accept` return a KlSocketHandle (KL_INVALID_SOCKET on
     * failure). */
    KlSocketHandle (*socket)(void *ctx, int domain, int type, int protocol);
    int     (*connect)(void *ctx, KlSocketHandle fd, const KlSockAddr *addr);
    int     (*bind)(void *ctx, KlSocketHandle fd, const KlSockAddr *addr);
    int     (*listen)(void *ctx, KlSocketHandle fd, int backlog);
    KlSocketHandle (*accept)(void *ctx, KlSocketHandle fd, KlSockAddr *peer);
    int     (*close)(void *ctx, KlSocketHandle fd);
    /* Read the local (bound) address — getsockname — into a KlSockAddr. */
    int     (*get_local_addr)(void *ctx, KlSocketHandle fd, KlSockAddr *addr);
    /* Read + clear the pending socket error — getsockopt(SO_ERROR). Writes it to
     * *out_err (0 = none) and returns 0, or returns -1 if the query itself fails.
     * Used for async-connect completion. The value is a platform error code;
     * callers only test zero vs non-zero. */
    int     (*get_so_error)(void *ctx, KlSocketHandle fd, int *out_err);
    /* I/O — return bytes moved (>=0) or -1 (errno-style error on the caller side). */
    kl_ssize_t (*send)(void *ctx, KlSocketHandle fd, const void *buf, size_t len);
    kl_ssize_t (*recv)(void *ctx, KlSocketHandle fd, void *buf, size_t len);
    /* Peek up to @len bytes without consuming them (recv/MSG_PEEK). >0 = bytes,
     * 0 = peer closed, <0 = error/would-block. */
    kl_ssize_t (*recv_peek)(void *ctx, KlSocketHandle fd, void *buf, size_t len);
    /* Vectored write + zero-copy file send. May be NULL — a provider without them
     * advertises no WRITEV/SENDFILE capability and Keel serializes / pread-sends
     * instead. `in_fd` is a *file* descriptor; `out_fd` is a socket handle.
     * `sendfile` advances `*offset` (byte offset into the file). */
    kl_ssize_t (*writev)(void *ctx, KlSocketHandle fd, const KlIoVec *iov, int iovcnt);
    kl_ssize_t (*sendfile)(void *ctx, KlSocketHandle out_fd, int in_fd, uint64_t *offset, size_t count);
    /* Classify the most recent -1 I/O result on this provider into a portable
     * KlIoStatus (would-block / interrupted / connect-pending / closed / reset /
     * fatal). Valid immediately after a send/recv/connect on this provider
     * returned -1 — the provider reports how that failure should be handled
     * WITHOUT the consumer reading a hosted `errno`, so the client's
     * would-block/re-arm/connect-pending control flow stays freestanding.
     *
     * OPTIONAL (may be NULL). When NULL, Keel falls back to its hosted errno
     * mapping (kl_sockdef_io_status). A provider that already translates its
     * native errors into `errno` (the built-in POSIX/Winsock providers, and
     * lwIP which maps ERR_MEM→EAGAIN) needs no io_status op; a freestanding
     * provider with no hosted errno MUST supply it. Appended to the ops table
     * (additive ABI): older ops tables leave it zero-initialized (NULL). */
    KlIoStatus (*io_status)(void *ctx);
    /* Release provider-owned context. May be NULL (nothing to free). */
    void    (*destroy)(void *ctx);
    const char *name;                 /* provider identity, for diagnostics */
} KlSocketOps;

typedef struct KlSocketProvider {
    const KlSocketOps *ops;
    void              *context;
    uint64_t           capabilities;
    /* Optional datagram data-plane (keel/datagram.h), or NULL for a stream-only
     * provider. Present iff capabilities & KL_SOCK_CAP_DATAGRAM. Folding datagram
     * I/O onto the provider means one runtime object owns all of a stack's socket
     * I/O (no separately-linked udp_io artifact to keep in sync). */
    const struct KlDatagramOps *dgram;
} KlSocketProvider;

/* Capability flags. A provider advertises what it supports; Keel falls back for
 * anything it does not (e.g. serialize when WRITEV is absent). A native-fd
 * provider's handle is a real OS descriptor the readiness event loop can poll —
 * required for the server (see KlHttpServerConfig.sockets). */
#define KL_SOCK_CAP_NATIVE_FD  (1ull << 0)  /* fd is a real OS descriptor */
#define KL_SOCK_CAP_WRITEV     (1ull << 1)  /* vectored writev usable on this fd */
#define KL_SOCK_CAP_SENDFILE   (1ull << 2)  /* zero-copy sendfile usable on this fd */
#define KL_SOCK_CAP_DATAGRAM   (1ull << 3)  /* provider->dgram datagram ops present */

/* Built-in provider factories (static storage, no allocation). POSIX is defined
 * everywhere; the Winsock factory is defined only on Windows (the declaration is
 * unconditional but only ever called there). */
const KlSocketProvider *kl_socket_provider_posix(void);
const KlSocketProvider *kl_socket_provider_winsock(void);

/* Release a provider's own context. A NULL provider (built-in POSIX) and a NULL
 * destroy op are both no-ops. Providers are borrowed by transports and must
 * outlive them. */
static inline void kl_socket_provider_destroy(const KlSocketProvider *p) {
    if (p && p->ops->destroy) p->ops->destroy(p->context);
}

/* Capability query. A NULL provider is the built-in POSIX provider (native-fd). */
static inline int kl_socket_provider_has_cap(const KlSocketProvider *p, uint64_t cap) {
    uint64_t caps = p ? p->capabilities : KL_SOCK_CAP_NATIVE_FD;
    return (caps & cap) != 0;
}

/* Native-descriptor escape hatch: when the provider advertises native fds, the
 * handle IS a real OS descriptor and is returned as-is; otherwise
 * KL_INVALID_SOCKET (the caller must not treat it as an OS fd). */
static inline KlSocketHandle kl_sock_native_fd(const KlSocketProvider *p, KlSocketHandle fd) {
    return kl_socket_provider_has_cap(p, KL_SOCK_CAP_NATIVE_FD) ? fd : KL_INVALID_SOCKET;
}

/* Map a platform socket error code to a stable KlError category (the coarse
 * network codes; the raw code is preserved by the caller for diagnostics). */
KlError kl_sock_errno_to_error(int err);

#ifdef __cplusplus
}
#endif

#endif /* KEEL_SOCKET_H */
