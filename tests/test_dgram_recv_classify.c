/*
 * test_dgram_recv_classify.c: the platform-agnostic completed-recv classification the IOCP drain
 * relies on (src/dgram_recv_classify.h). Proves the Windows success classification WITHOUT a Windows
 * host: a zero-length datagram is a real receive (not a failure), truncation (flag OR EMSGSIZE) is a
 * captured-prefix success (not fatal), and cancellation/failure yields no datagram + no metadata.
 * These distinctions cannot be shown by the backend-agnostic KlDgramRecv tests.
 */
#include "utest.h"

#include "../src/dgram_recv_classify.h"

/* Synthetic stand-ins for the Winsock constants the live IOCP path passes (values are arbitrary; the
 * classifier only compares/masks them). */
#define WIN_MSG_TRUNC        0x0100u
#define WIN_WSAEMSGSIZE      10040
#define WIN_WSA_OP_ABORTED   995
#define WIN_WSAECONNRESET    10054
#define BUFSZ                ((size_t)2048)

/* a normal, non-empty datagram: ok, exact bytes, not truncated, both addresses parsed */
UTEST(dgram_classify, success_nonempty) {
    KlDgramRecvClass c = kl_dgram_recv_classify(/*ok*/1, 0, WIN_WSAEMSGSIZE,
                                                512, 0, WIN_MSG_TRUNC, BUFSZ);
    ASSERT_EQ(c.ok, 1);
    ASSERT_EQ((int)c.bytes, 512);
    ASSERT_EQ(c.truncated, 0);
    ASSERT_EQ(c.parse_source, 1);
    ASSERT_EQ(c.parse_local, 1);
}

/* a genuine ZERO-LENGTH datagram is a real receive (ok, 0 bytes) with valid source/control */
UTEST(dgram_classify, success_zero_length_is_a_datagram) {
    KlDgramRecvClass c = kl_dgram_recv_classify(1, 0, WIN_WSAEMSGSIZE,
                                                0, 0, WIN_MSG_TRUNC, BUFSZ);
    ASSERT_EQ(c.ok, 1);                 /* NOT a failure */
    ASSERT_EQ((int)c.bytes, 0);
    ASSERT_EQ(c.truncated, 0);
    ASSERT_EQ(c.parse_source, 1);       /* source parsed regardless of byte count */
    ASSERT_EQ(c.parse_local, 1);
}

/* success with MSG_TRUNC in the message flags (WSARecvMsg) at EXACT capacity → captured prefix */
UTEST(dgram_classify, success_flag_truncated_exact_capacity) {
    KlDgramRecvClass c = kl_dgram_recv_classify(1, 0, WIN_WSAEMSGSIZE,
                                                BUFSZ, WIN_MSG_TRUNC, WIN_MSG_TRUNC, BUFSZ);
    ASSERT_EQ(c.ok, 1);
    ASSERT_EQ((int)c.bytes, (int)BUFSZ);
    ASSERT_EQ(c.truncated, 1);          /* detected via the flag, not the byte count */
    ASSERT_EQ(c.parse_source, 1);
    ASSERT_EQ(c.parse_local, 1);
}

/* success with MSG_TRUNC even at a ZERO captured prefix → still surfaced as truncated */
UTEST(dgram_classify, success_flag_truncated_zero_prefix) {
    KlDgramRecvClass c = kl_dgram_recv_classify(1, 0, WIN_WSAEMSGSIZE,
                                                0, WIN_MSG_TRUNC, WIN_MSG_TRUNC, BUFSZ);
    ASSERT_EQ(c.ok, 1);
    ASSERT_EQ((int)c.bytes, 0);
    ASSERT_EQ(c.truncated, 1);
}

/* WSARecvFrom reports truncation as FAILURE with WSAEMSGSIZE → captured-prefix success (buffer
 * full), source parsed but local skipped (control indeterminate) */
UTEST(dgram_classify, emsgsize_failure_is_truncated_success) {
    KlDgramRecvClass c = kl_dgram_recv_classify(/*ok*/0, WIN_WSAEMSGSIZE, WIN_WSAEMSGSIZE,
                                                0, 0, WIN_MSG_TRUNC, BUFSZ);
    ASSERT_EQ(c.ok, 1);                 /* not fatal */
    ASSERT_EQ((int)c.bytes, (int)BUFSZ);/* whole buffer captured */
    ASSERT_EQ(c.truncated, 1);
    ASSERT_EQ(c.parse_source, 1);
    ASSERT_EQ(c.parse_local, 0);        /* control indeterminate on EMSGSIZE */
}

/* a cancelled receive (WSA_OPERATION_ABORTED) delivers nothing and parses no metadata */
UTEST(dgram_classify, cancelled_yields_no_datagram) {
    KlDgramRecvClass c = kl_dgram_recv_classify(0, WIN_WSA_OP_ABORTED, WIN_WSAEMSGSIZE,
                                                0, 0, WIN_MSG_TRUNC, BUFSZ);
    ASSERT_EQ(c.ok, 0);
    ASSERT_EQ((int)c.bytes, 0);
    ASSERT_EQ(c.truncated, 0);
    ASSERT_EQ(c.parse_source, 0);
    ASSERT_EQ(c.parse_local, 0);
}

/* a genuine receive failure (e.g. ECONNRESET) is likewise not delivered */
UTEST(dgram_classify, failure_yields_no_datagram) {
    KlDgramRecvClass c = kl_dgram_recv_classify(0, WIN_WSAECONNRESET, WIN_WSAEMSGSIZE,
                                                0, 0, WIN_MSG_TRUNC, BUFSZ);
    ASSERT_EQ(c.ok, 0);
    ASSERT_EQ(c.parse_source, 0);
    ASSERT_EQ(c.parse_local, 0);
}

UTEST_MAIN();
