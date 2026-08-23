#ifndef KEEL_SRC_DGRAM_RECV_CLASSIFY_H
#define KEEL_SRC_DGRAM_RECV_CLASSIFY_H

/*
 * dgram_recv_classify.h — INTERNAL, platform-agnostic classification of a COMPLETED overlapped
 * datagram receive into the neutral (ok / bytes / truncated / metadata) result the datagram machine
 * expects. Factored out of the IOCP completion path so the classification contract can be unit-tested
 * on ANY host (the live Winsock path only runs on Windows CI).
 *
 * The caller supplies the platform's completion result: whether the op SUCCEEDED, its error code and
 * that platform's "message too long" sentinel (WSAEMSGSIZE / EMSGSIZE), the bytes transferred, the
 * message flags + that platform's truncation bit (MSG_TRUNC), and the receive buffer size. This
 * encodes the frozen datagram receive contract's provider side (docs/contracts/datagram.md §4/§8):
 *
 *  - Success, INCLUDING a zero-length datagram (xfer == 0): ok, bytes = xfer, source + control
 *    metadata parsed regardless of byte count, truncated iff the truncation flag is set (so an
 *    exact-capacity or zero-captured-prefix truncation is still surfaced).
 *  - "Message too long" (WSARecvFrom reports failure with WSAEMSGSIZE): a captured-prefix success —
 *    ok, bytes = the full buffer, truncated; source parsed but the control message is indeterminate,
 *    so the local (pktinfo) address is NOT parsed.
 *  - Any other failure (cancellation / genuine error): NOT ok; no datagram, no metadata parsed.
 *
 * INTERNAL header — not installed, no ABI commitment. No platform headers here (only <stddef.h>).
 */

#include <stddef.h>

typedef struct {
    int    ok;             /* 1 = a datagram to deliver (incl. zero-length); 0 = failed/cancelled */
    size_t bytes;          /* captured-prefix byte count (0 for a zero-length datagram) */
    int    truncated;      /* 1 = captured prefix of an oversized datagram */
    int    parse_source;   /* 1 = parse the peer (source) address */
    int    parse_local;    /* 1 = parse the pktinfo local (destination) address */
} KlDgramRecvClass;

/* Pure decision; performs no I/O and touches no platform state. `msg_flags & trunc_flag` tests the
 * completed message's truncation bit; `wsa_err == emsgsize_err` detects the report-as-failure form. */
static inline KlDgramRecvClass kl_dgram_recv_classify(int wsa_ok, int wsa_err, int emsgsize_err,
                                                      size_t xfer, unsigned msg_flags,
                                                      unsigned trunc_flag, size_t buf_size) {
    KlDgramRecvClass c = { 0, 0, 0, 0, 0 };
    if (wsa_ok) {
        c.ok = 1;
        c.bytes = xfer;                                  /* may be 0 — a real zero-length datagram */
        c.truncated = (msg_flags & trunc_flag) ? 1 : 0;
        c.parse_source = 1;
        c.parse_local = 1;
    } else if (wsa_err == emsgsize_err) {
        c.ok = 1;
        c.bytes = buf_size;                              /* datagram filled the buffer (captured prefix) */
        c.truncated = 1;
        c.parse_source = 1;
        c.parse_local = 0;                               /* control indeterminate on EMSGSIZE */
    }
    /* else: cancelled/failed — leave everything zero (ok = 0, no metadata). */
    return c;
}

#endif /* KEEL_SRC_DGRAM_RECV_CLASSIFY_H */
