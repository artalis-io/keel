#ifndef KEEL_SRC_SOCKADDR_NATIVE_H
#define KEEL_SRC_SOCKADDR_NATIVE_H

/*
 * sockaddr_native.h — the ONLY bridge between KlSockAddr and a platform
 * `struct sockaddr`. Included exclusively by socket providers (socket_posix.c,
 * socket_winsock.c, the overlapped providers in the completion backends, and a
 * foreign stack's own provider e.g. integrations/lwip/socket_lwip.c). Core /
 * protocol code never includes this — it speaks KlSockAddr only.
 *
 * Because the conversion is `static inline` and pulls the platform sockaddr via
 * sockcompat.h, each including TU gets a copy compiled against *its* sockaddr
 * layout. That is deliberate: an lwIP provider compiled with lwIP's headers gets
 * lwIP-layout marshalling (with sin_len) for free, with no core recompile — this
 * is what makes the address ABI runtime-injectable (docs/keel_sockaddr_design.md).
 */
#include <keel/sockaddr.h>
#include "sockcompat.h"   /* struct sockaddr{,_in,_in6,_un}, AF_*, htons/ntohs */

#include <string.h>

/**
 * KlSockAddr -> platform sockaddr (for bind/connect/sendto). Writes into @p out
 * and returns the socklen to pass to the syscall, or 0 on unsupported family.
 */
static inline socklen_t kl_sockaddr_to_native(const KlSockAddr *a,
                                              struct sockaddr_storage *out) {
    memset(out, 0, sizeof *out);
    if (!a) return 0;

    switch (a->family) {
    case KL_AF_INET: {
        struct sockaddr_in *in = (struct sockaddr_in *)out;
        in->sin_family = AF_INET;
        in->sin_port = htons(a->port);
        memcpy(&in->sin_addr, a->u.ip, 4);   /* both network order */
        return (socklen_t)sizeof(*in);
    }
    case KL_AF_INET6: {
        struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)out;
        in6->sin6_family = AF_INET6;
        in6->sin6_port = htons(a->port);
        in6->sin6_scope_id = a->scope_id;
        memcpy(&in6->sin6_addr, a->u.ip, 16);
        return (socklen_t)sizeof(*in6);
    }
#ifdef AF_UNIX
    case KL_AF_UNIX: {
        struct sockaddr_un *un = (struct sockaddr_un *)out;
        if (a->addr_len >= sizeof(un->sun_path)) return 0;
        un->sun_family = AF_UNIX;
        memcpy(un->sun_path, a->u.path, a->addr_len);
        un->sun_path[a->addr_len] = '\0';
        return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + a->addr_len + 1);
    }
#endif
    default:
        return 0;
    }
}

/**
 * platform sockaddr -> KlSockAddr (for accept/recvfrom/getsockname). Returns 0
 * on success, -1 on unsupported family.
 */
static inline int kl_sockaddr_from_native(KlSockAddr *out,
                                          const struct sockaddr *sa,
                                          socklen_t len) {
    if (!out || !sa) return -1;

    /* Cast to int: some platforms (cosmo) type sa_family narrowly (uint8_t),
     * which trips GCC -Werror=switch-outside-range on the wider AF_* labels. */
    switch ((int)sa->sa_family) {
    case AF_INET: {
        /* `len` is untrusted (accept/recvfrom peer): reject a short sockaddr
         * before reading sin_addr/sin_port past the caller's buffer. */
        if (len < (socklen_t)sizeof(struct sockaddr_in)) return -1;
        const struct sockaddr_in *in = (const struct sockaddr_in *)sa;
        uint8_t ip[4];
        memcpy(ip, &in->sin_addr, 4);
        return kl_sockaddr_from_ipv4(out, ip, ntohs(in->sin_port));
    }
    case AF_INET6: {
        if (len < (socklen_t)sizeof(struct sockaddr_in6)) return -1;
        const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)sa;
        uint8_t ip[16];
        memcpy(ip, &in6->sin6_addr, 16);
        return kl_sockaddr_from_ipv6(out, ip, ntohs(in6->sin6_port),
                                     in6->sin6_scope_id);
    }
#ifdef AF_UNIX
    case AF_UNIX: {
        const struct sockaddr_un *un = (const struct sockaddr_un *)sa;
        /* an unnamed/abstract peer (len at/below the header) has no path */
        size_t plen = (len > (socklen_t)offsetof(struct sockaddr_un, sun_path))
                      ? strnlen(un->sun_path, sizeof(un->sun_path)) : 0;
        memset(out, 0, sizeof *out);
        out->family = KL_AF_UNIX;
        if (plen >= KL_UNIX_PATH_MAX) plen = KL_UNIX_PATH_MAX - 1;
        out->addr_len = (uint8_t)plen;
        memcpy(out->u.path, un->sun_path, plen);
        return 0;
    }
#endif
    default:
        return -1;
    }
}

#endif /* KEEL_SRC_SOCKADDR_NATIVE_H */
