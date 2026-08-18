/*
 * udp_cmsg.c — the shared POSIX UDP control-message parsers (udp_cmsg.h).
 *
 * kl_udp_parse_local / kl_udp_parse_gro are used by the POSIX completion backends
 * (event_iouring.c, event_pollcomp.c) to extract the pktinfo local address and the
 * UDP_GRO segment size from a received msghdr's control data. They used to live in
 * udp_io_posix.c; with the datagram data-plane folded onto the socket providers
 * (KlDatagramOps), the readiness recv path no longer needs a separate seam TU, so
 * these shared parsers live here (always linked on POSIX) instead.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE            /* struct in{,6}_pktinfo + IP_PKTINFO/IPV6_PKTINFO (glibc) */
#endif
#if defined(__APPLE__) && !defined(__APPLE_USE_RFC_3542)
#define __APPLE_USE_RFC_3542
#endif

#include "udp_cmsg.h"

#include <string.h>
#include <netinet/in.h>
#include <sys/socket.h>

#if defined(__linux__)
#include <netinet/udp.h>
#ifndef UDP_GRO
#define UDP_GRO 104
#endif
#endif

socklen_t kl_udp_parse_local(struct msghdr *msg, struct sockaddr_storage *out) {
    for (struct cmsghdr *cm = CMSG_FIRSTHDR(msg); cm; cm = CMSG_NXTHDR(msg, cm)) {
#if defined(IP_PKTINFO)
        if (cm->cmsg_level == IPPROTO_IP && cm->cmsg_type == IP_PKTINFO &&
            cm->cmsg_len >= CMSG_LEN(sizeof(struct in_pktinfo))) {
            struct in_pktinfo pi;
            memcpy(&pi, CMSG_DATA(cm), sizeof(pi));
            struct sockaddr_in *s4 = (struct sockaddr_in *)out;
            memset(s4, 0, sizeof(*s4));
            s4->sin_family = AF_INET;
            s4->sin_addr = pi.ipi_addr;
            return sizeof(*s4);
        }
#endif
        if (cm->cmsg_level == IPPROTO_IPV6 && cm->cmsg_type == IPV6_PKTINFO &&
            cm->cmsg_len >= CMSG_LEN(sizeof(struct in6_pktinfo))) {
            struct in6_pktinfo pi;
            memcpy(&pi, CMSG_DATA(cm), sizeof(pi));
            struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)out;
            memset(s6, 0, sizeof(*s6));
            s6->sin6_family = AF_INET6;
            s6->sin6_addr = pi.ipi6_addr;
            return sizeof(*s6);
        }
    }
    return 0;
}

int kl_udp_parse_gro(struct msghdr *msg) {
#if defined(__linux__) && defined(UDP_GRO)
    for (struct cmsghdr *cm = CMSG_FIRSTHDR(msg); cm; cm = CMSG_NXTHDR(msg, cm)) {
        if (cm->cmsg_level == IPPROTO_UDP && cm->cmsg_type == UDP_GRO &&
            cm->cmsg_len >= CMSG_LEN(sizeof(int))) {
            int seg;
            memcpy(&seg, CMSG_DATA(cm), sizeof(seg));
            return seg;
        }
    }
#else
    (void)msg;
#endif
    return 0;
}

/* Overflow-safe capacity check: does a `space`-byte cmsg record fit after `used` bytes in `bufsz`?
 * `used` is a parameter (not a constant here) so the subtraction is guarded without a `0 > bufsz`
 * comparison — `bufsz - used` is evaluated only once `used <= bufsz` holds (short-circuit). */
static int ctrl_fits(size_t used, size_t space, size_t bufsz) {
    return used <= bufsz && space <= bufsz - used;
}

int kl_udp_build_control(unsigned char *buf, size_t bufsz,
                         const struct sockaddr *src, int tos, int family, size_t *out_len) {
    *out_len = 0;
    size_t used = 0;   /* bytes written so far; the next cmsg starts at buf + used (CMSG_SPACE-aligned) */

    if (src) {                                           /* source-pin REQUESTED (family from src) */
#if defined(IP_PKTINFO) || defined(IPV6_PKTINFO)
        int ok = 0;
#if defined(IP_PKTINFO)
        if (src->sa_family == AF_INET) {
            size_t space = CMSG_SPACE(sizeof(struct in_pktinfo));
            if (!ctrl_fits(used, space, bufsz)) return -1;   /* overflow-safe capacity check */
            struct cmsghdr *cm = (struct cmsghdr *)(buf + used);
            cm->cmsg_level = IPPROTO_IP; cm->cmsg_type = IP_PKTINFO;
            cm->cmsg_len = CMSG_LEN(sizeof(struct in_pktinfo));
            struct in_pktinfo pi; memset(&pi, 0, sizeof(pi));
            pi.ipi_spec_dst = ((const struct sockaddr_in *)src)->sin_addr;
            memcpy(CMSG_DATA(cm), &pi, sizeof(pi));
            used += space; ok = 1;
        }
#endif
#if defined(IPV6_PKTINFO)
        if (src->sa_family == AF_INET6) {
            size_t space = CMSG_SPACE(sizeof(struct in6_pktinfo));
            if (!ctrl_fits(used, space, bufsz)) return -1;   /* overflow-safe capacity check */
            struct cmsghdr *cm = (struct cmsghdr *)(buf + used);
            cm->cmsg_level = IPPROTO_IPV6; cm->cmsg_type = IPV6_PKTINFO;
            cm->cmsg_len = CMSG_LEN(sizeof(struct in6_pktinfo));
            struct in6_pktinfo pi; memset(&pi, 0, sizeof(pi));
            pi.ipi6_addr = ((const struct sockaddr_in6 *)src)->sin6_addr;
            memcpy(CMSG_DATA(cm), &pi, sizeof(pi));
            used += space; ok = 1;
        }
#endif
        if (!ok) return -1;                              /* unknown family */
#else
        return -1;                                       /* no pktinfo support compiled */
#endif
    }

    if (tos >= 0) {                                      /* TOS REQUESTED (family from the CALLER) */
#if defined(IP_TOS) || defined(IPV6_TCLASS)
        int v = tos & 0xff, ok = 0;
        size_t space = CMSG_SPACE(sizeof(int));
#if defined(IP_TOS)
        if (family == AF_INET) {
            if (!ctrl_fits(used, space, bufsz)) return -1;   /* overflow-safe capacity check */
            struct cmsghdr *cm = (struct cmsghdr *)(buf + used);
            cm->cmsg_level = IPPROTO_IP; cm->cmsg_type = IP_TOS;
            cm->cmsg_len = CMSG_LEN(sizeof(int));
            memcpy(CMSG_DATA(cm), &v, sizeof(v));
            used += space; ok = 1;
        }
#endif
#if defined(IPV6_TCLASS)
        if (family == AF_INET6) {
            if (!ctrl_fits(used, space, bufsz)) return -1;   /* overflow-safe capacity check */
            struct cmsghdr *cm = (struct cmsghdr *)(buf + used);
            cm->cmsg_level = IPPROTO_IPV6; cm->cmsg_type = IPV6_TCLASS;
            cm->cmsg_len = CMSG_LEN(sizeof(int));
            memcpy(CMSG_DATA(cm), &v, sizeof(v));
            used += space; ok = 1;
        }
#endif
        if (!ok) return -1;                              /* unknown family */
#else
        return -1;                                       /* no TOS support compiled */
#endif
    }
    *out_len = used;
    return 0;
}

int kl_udp_send_family(int fd, const struct sockaddr *dest, const struct sockaddr *src) {
    if (dest && (dest->sa_family == AF_INET || dest->sa_family == AF_INET6))
        return dest->sa_family;
    if (src && (src->sa_family == AF_INET || src->sa_family == AF_INET6))
        return src->sa_family;
    struct sockaddr_storage ss;
    socklen_t sl = sizeof(ss);
    if (getsockname(fd, (struct sockaddr *)&ss, &sl) == 0 &&
        (ss.ss_family == AF_INET || ss.ss_family == AF_INET6))
        return (int)ss.ss_family;
    return -1;   /* undeterminable — caller fails the send rather than guessing IPv4 */
}
