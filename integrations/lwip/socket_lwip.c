/*
 * socket_lwip.c — reference KlSocketProvider over the lwIP socket API.
 *
 * BYO / opt-in: lwIP is NOT vendored — build this against your own lwIP
 * (LWIP_DIR) with your lwipopts.h. This is a reference that validates Keel's
 * public provider API (<keel/socket.h>) is sufficient to run Keel on lwIP; it is
 * not a shipped/default platform. See docs/lwip_platform_design.md.
 *
 * Uses only the public authoring API — kl_ssize_t / KlIoVec / KlSocketHandle,
 * never internal or host-POSIX types. struct iovec here is lwIP's own (from
 * lwip/sockets.h), translated from KlIoVec inside this TU.
 */

#define KEEL_PLATFORM_LWIP 1     /* net.h → lwip/sockets.h, not host <sys/socket.h> */
#include <keel/socket.h>

#include "keel_lwip.h"

#include "lwip/sockets.h"

#include <errno.h>
#include <string.h>

/* lwIP socket descriptors are small ints (possibly LWIP_SOCKET_OFFSET-shifted);
 * they round-trip through KlSocketHandle (intptr_t), -1 == KL_INVALID_SOCKET. */

static int  lw_set_nonblocking(void *c, KlSocketHandle fd) { (void)c; return lwip_fcntl((int)fd, F_SETFL, O_NONBLOCK); }
static int  lw_set_blocking(void *c, KlSocketHandle fd)    { (void)c; return lwip_fcntl((int)fd, F_SETFL, 0); }
static void lw_set_cloexec(void *c, KlSocketHandle fd)     { (void)c; (void)fd; }  /* n/a on lwIP */
static void lw_set_nosigpipe(void *c, KlSocketHandle fd)   { (void)c; (void)fd; }  /* no SIGPIPE */

static int lw_setopt_int(int fd, int level, int opt, int on) {
    return lwip_setsockopt(fd, level, opt, &on, sizeof(on));
}
static int lw_set_reuseaddr(void *c, KlSocketHandle fd, int on)   { (void)c; return lw_setopt_int((int)fd, SOL_SOCKET, SO_REUSEADDR, on); }
static int lw_set_reuseport(void *c, KlSocketHandle fd, int on)   { (void)c; (void)fd; (void)on; return -1; }  /* usually unavailable */
static int lw_set_ipv6only(void *c, KlSocketHandle fd, int on) {
    (void)c;
#if defined(IPPROTO_IPV6) && defined(IPV6_V6ONLY)
    return lw_setopt_int((int)fd, IPPROTO_IPV6, IPV6_V6ONLY, on);
#else
    (void)fd; (void)on; return -1;   /* IPv4-only lwIP build — best-effort */
#endif
}
static int lw_set_tcp_nodelay(void *c, KlSocketHandle fd, int on) { (void)c; return lw_setopt_int((int)fd, IPPROTO_TCP, TCP_NODELAY, on); }
static int lw_set_cork(void *c, KlSocketHandle fd, int on)        { (void)c; (void)fd; (void)on; return -1; }  /* no TCP_CORK */

/* KlSockAddr <-> lwIP sockaddr — the lwIP-local counterpart of the built-in
 * providers' src/sockaddr_native.h (which resolves host sockaddr and so cannot be
 * reused here). This is where, and the only where, lwIP's address layout lives. */
static socklen_t lw_to_native(const KlSockAddr *a, struct sockaddr_storage *ss) {
    memset(ss, 0, sizeof *ss);
    if (a->family == KL_AF_INET) {
        struct sockaddr_in *in = (struct sockaddr_in *)ss;
        in->sin_family = AF_INET;
        in->sin_port = lwip_htons(a->port);
        memcpy(&in->sin_addr, a->u.ip, 4);
        return (socklen_t)sizeof(*in);
    }
#if LWIP_IPV6
    if (a->family == KL_AF_INET6) {
        struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)ss;
        in6->sin6_family = AF_INET6;
        in6->sin6_port = lwip_htons(a->port);
        in6->sin6_scope_id = a->scope_id;
        memcpy(&in6->sin6_addr, a->u.ip, 16);
        return (socklen_t)sizeof(*in6);
    }
#endif
    return 0;
}
static int lw_from_native(KlSockAddr *out, const struct sockaddr *sa) {
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)sa;
        uint8_t ip[4]; memcpy(ip, &in->sin_addr, 4);
        return kl_sockaddr_from_ipv4(out, ip, lwip_ntohs(in->sin_port));
    }
#if LWIP_IPV6
    if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)sa;
        uint8_t ip[16]; memcpy(ip, &in6->sin6_addr, 16);
        return kl_sockaddr_from_ipv6(out, ip, lwip_ntohs(in6->sin6_port), in6->sin6_scope_id);
    }
#endif
    return -1;
}

static KlSocketHandle lw_socket(void *c, int d, int t, int p) { (void)c; return (KlSocketHandle)lwip_socket(d, t, p); }
static int lw_connect(void *c, KlSocketHandle fd, const KlSockAddr *a) {
    (void)c;
    struct sockaddr_storage ss; socklen_t l = lw_to_native(a, &ss);
    if (l == 0) { errno = EAFNOSUPPORT; return -1; }
    return lwip_connect((int)fd, (struct sockaddr *)&ss, l);
}
static int lw_bind(void *c, KlSocketHandle fd, const KlSockAddr *a) {
    (void)c;
    struct sockaddr_storage ss; socklen_t l = lw_to_native(a, &ss);
    if (l == 0) { errno = EAFNOSUPPORT; return -1; }
    return lwip_bind((int)fd, (struct sockaddr *)&ss, l);
}
static int lw_listen(void *c, KlSocketHandle fd, int backlog)   { (void)c; return lwip_listen((int)fd, backlog); }
static KlSocketHandle lw_accept(void *c, KlSocketHandle fd, KlSockAddr *peer) {
    (void)c;
    struct sockaddr_storage ss; socklen_t sl = sizeof ss;
    struct sockaddr *sa = peer ? (struct sockaddr *)&ss : NULL;
    socklen_t *slp = peer ? &sl : NULL;
    KlSocketHandle cfd = (KlSocketHandle)lwip_accept((int)fd, sa, slp);
    if (kl_handle_valid(cfd) && peer && lw_from_native(peer, sa) != 0)
        memset(peer, 0, sizeof(*peer));
    return cfd;
}
static int lw_close(void *c, KlSocketHandle fd) { (void)c; return lwip_close((int)fd); }
static int lw_get_local_addr(void *c, KlSocketHandle fd, KlSockAddr *out) {
    (void)c;
    struct sockaddr_storage ss; socklen_t l = sizeof ss;
    if (lwip_getsockname((int)fd, (struct sockaddr *)&ss, &l) != 0) return -1;
    return lw_from_native(out, (struct sockaddr *)&ss);
}
static int lw_get_so_error(void *c, KlSocketHandle fd, int *out_err) {
    (void)c;
    socklen_t l = sizeof(int);
    return lwip_getsockopt((int)fd, SOL_SOCKET, SO_ERROR, out_err, &l) == 0 ? 0 : -1;
}

static kl_ssize_t lw_send(void *c, KlSocketHandle fd, const void *b, size_t n)      { (void)c; return lwip_send((int)fd, b, n, 0); }
static kl_ssize_t lw_recv(void *c, KlSocketHandle fd, void *b, size_t n)            { (void)c; return lwip_recv((int)fd, b, n, 0); }
static kl_ssize_t lw_recv_peek(void *c, KlSocketHandle fd, void *b, size_t n)       { (void)c; return lwip_recv((int)fd, b, n, MSG_PEEK); }

static kl_ssize_t lw_writev(void *c, KlSocketHandle fd, const KlIoVec *iov, int iovcnt) {
    (void)c;
    if (iovcnt <= 0 || iovcnt > KL_SOCK_IOV_MAX) { errno = EINVAL; return -1; }
    /* Translate the Keel-owned vector into lwIP's struct iovec here, so the
     * platform vector type never escapes this provider TU. */
    struct iovec v[KL_SOCK_IOV_MAX];
    for (int i = 0; i < iovcnt; i++) {
        v[i].iov_base = iov[i].base;
        v[i].iov_len  = iov[i].len;
    }
    return lwip_writev((int)fd, v, iovcnt);
}
/* No sendfile op — lwIP has none; without KL_SOCK_CAP_SENDFILE Keel pread-sends. */

static const KlSocketOps lwip_ops = {
    .set_nonblocking = lw_set_nonblocking, .set_blocking = lw_set_blocking,
    .set_cloexec = lw_set_cloexec, .set_nosigpipe = lw_set_nosigpipe,
    .set_reuseaddr = lw_set_reuseaddr, .set_reuseport = lw_set_reuseport,
    .set_ipv6only = lw_set_ipv6only, .set_tcp_nodelay = lw_set_tcp_nodelay,
    .set_cork = lw_set_cork,
    .socket = lw_socket, .connect = lw_connect, .bind = lw_bind,
    .listen = lw_listen, .accept = lw_accept, .close = lw_close,
    .get_local_addr = lw_get_local_addr, .get_so_error = lw_get_so_error,
    .send = lw_send, .recv = lw_recv, .recv_peek = lw_recv_peek, .writev = lw_writev,
    .name = "lwip",
};

/* Native-fd: the lwIP socket is pollable by the paired lwIP event backend
 * (event_lwip.c, lwip_poll). WRITEV: lwip_writev is available. No SENDFILE. */
static const KlSocketProvider lwip_provider = {
    &lwip_ops, NULL, KL_SOCK_CAP_NATIVE_FD | KL_SOCK_CAP_WRITEV,
    NULL,   /* dgram — datagram ops added in the lwIP datagram-provider stage */
};

const KlSocketProvider *kl_socket_provider_lwip(void) { return &lwip_provider; }
