#ifndef KEEL_SRC_DGRAM_SEND_CLASSIFY_H
#define KEEL_SRC_DGRAM_SEND_CLASSIFY_H

/*
 * dgram_send_classify.h: INTERNAL classification of a failed datagram send by its errno.
 *
 * A UDP send can fail for a reason that concerns THAT datagram and says nothing about the socket:
 * no route to its destination, an ICMP report about an earlier datagram that a connected socket
 * returns from its next send, a device queue or kernel buffer briefly full, a firewall or broadcast
 * refusal, a path MTU it does not fit. The datagram is lost (UDP is unreliable anyway); the socket
 * keeps working, so the next send must be attempted. Every other errno (a closed or invalid handle,
 * a socket shut down for writing, anything unknown) means the send side cannot be trusted and stays
 * failed.
 *
 * Shared by the hosted provider classifiers (socket_posix.c, socket_winsock.c: Winsock errors are
 * translated to these errno values on the seam first) and the POSIX completion backends
 * (event_iouring.c, event_pollcomp.c), so the readiness and completion paths agree. The IOCP backend
 * classifies Winsock codes with kl_udp_win_send_err_is_per_datagram (udp_cmsg_win.h).
 *
 * An if-chain, not a switch: some libcs expose errno values as objects rather than constants.
 *
 * INTERNAL header: not installed, no ABI commitment.
 */

#include <errno.h>

static inline int kl_dgram_send_err_is_per_datagram(int err) {
    if (err == ENETUNREACH || err == EHOSTUNREACH || err == ECONNREFUSED || err == ECONNRESET)
        return 1;   /* no route, or an ICMP report about an earlier datagram */
#ifdef ENETRESET
    if (err == ENETRESET) return 1;
#endif
#ifdef EHOSTDOWN
    if (err == EHOSTDOWN) return 1;
#endif
#ifdef ENETDOWN
    if (err == ENETDOWN) return 1;
#endif
    if (err == ENOBUFS || err == ENOMEM)
        return 1;   /* a full queue or a short buffer: this datagram only */
    if (err == EPERM || err == EACCES)
        return 1;   /* a firewall refusal, or a broadcast destination without SO_BROADCAST */
    if (err == EMSGSIZE)
        return 1;   /* larger than the path takes */
    if (err == EADDRNOTAVAIL)
        return 1;   /* a pinned source address that is not (or no longer) local */
    if (err == EINTR)
        return 1;
    return 0;
}

#endif /* KEEL_SRC_DGRAM_SEND_CLASSIFY_H */
