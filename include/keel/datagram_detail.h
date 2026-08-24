#ifndef KEEL_DATAGRAM_DETAIL_H
#define KEEL_DATAGRAM_DETAIL_H

/*
 * datagram_detail.h: the OPT-IN, UNSTABLE layout of KlDatagram.
 *
 * Include this ONLY to stack-/embed-allocate a KlDatagram by value; you then opt into the layout and
 * MUST recompile when it changes (it is NOT ABI-stable; see the <keel/datagram.h> banner). A consumer
 * that only holds a `KlDatagram *` created behind the API never needs this header.
 *
 * The heavy machine state (KlDgramCore + its slots/send/recv/close/life machines) is heap-allocated
 * behind an OPAQUE `core` pointer (the full KlDgramCore type is src-only and cannot be installed by
 * an embedder), so this layout stays tiny: the borrowed handles + facade-only state.
 */

#include <keel/handle.h>     /* KlSocketHandle */
#include <keel/error.h>      /* KlError */
#include <keel/datagram.h>   /* KlDatagramRecvFn + the public contract */

#ifdef __cplusplus
extern "C" {
#endif

struct KlEventCtx;
struct KlSocketProvider;
struct KlDgramCore;   /* opaque: the full type is src-only (src/datagram_core.h); init heap-allocates it */

struct KlDatagram {
    struct KlDgramCore     *core;      /* heap; allocated in init from cfg->alloc, freed in free (after CLOSED) */
    struct KlEventCtx      *ctx;       /* borrowed loop (adapters + close/backend-retirement + reuse) */
    const struct KlSocketProvider *sockets;  /* borrowed provider (readiness pull/send + fd close) */
    KlAllocator            *alloc;     /* borrowed (core allocation + rx-holder lifetime) */
    KlSocketHandle          fd;        /* adopted on init success; closed by the close machine (not free) */
    KlDatagramRecvFn        on_recv;   /* facade-only: the user recv callback (deliver adapter forwards) */
    void                   *recv_ud;
    /* batch/GRO receive (facade-only). `on_recv_segments` (+ its ud), when set, receives a
     * whole GRO-coalesced buffer instead of the default per-segment split; `recv_seg_size` is the
     * yield adapter's scratch (the coalesced segment size for the current whole-buffer delivery, 0 for
     * a plain/split datagram): read by the deliver adapter to route on_recv vs on_recv_segments. */
    void (*on_recv_segments)(void *ud, const void *data, size_t len, size_t segment_size,
                             const KlSockAddr *peer, const KlSockAddr *local, unsigned flags);
    void                   *recv_seg_ud;
    size_t                  recv_seg_size;
    KlDatagramCloseFn       on_close_cb;/* facade-only: user terminal callback (dg_on_close forwards) */
    void                   *close_ud;
    KlError                 last_error;
    unsigned                provider_caps; /* KL_DGRAM_CAP_* the provider supports on `fd` (kl_datagram_provider_caps) */
    uint64_t                truncated; /* count of delivered captured-prefix (KL_DGRAM_TRUNCATED) datagrams */
    uint64_t                dropped;   /* RESERVED: 0 until the recv machine surfaces overflow/contract drops */
    int                     completion;/* 1 = completion mode / 0 = readiness mode */
    int                     registered;/* completion: the fd is registered with the loop (kl_event_add):
                                        * the generic fd↔loop association step: inert on
                                        * io_uring/pollcomp, CreateIoCompletionPort on IOCP */
    int                     read_armed;/* readiness: a watcher is installed on `fd` */
    unsigned                want_mask; /* readiness: desired KlEventMask (READ | WRITE) */
};

#ifdef __cplusplus
}
#endif

#endif /* KEEL_DATAGRAM_DETAIL_H */
