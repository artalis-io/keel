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
