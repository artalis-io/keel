/*
 * keel/stream_detail.h: Layout of KlStream (opt-in detail).
 *
 * INTERNAL / UNSTABLE. Include this ONLY to embed or stack-allocate a KlStream (e.g. KlHttpConn embeds
 * it); the fields are NOT part of the API and may change without notice. The behavior/ownership
 * contract is in <keel/stream.h>. External code must use accessors, not `stream.*`.
 *
 * This carries RAW transport state only. TLS-wrapper orchestration and HTTP policy stay on KlHttpConn.
 */
#ifndef KEEL_STREAM_DETAIL_H
#define KEEL_STREAM_DETAIL_H

#include <stddef.h>
#include <keel/stream.h>      /* KlStream typedef + the write/read/close contract */
#include <keel/allocator.h>   /* KlAllocator */
#include <keel/handle.h>      /* KlSocketHandle */
#include <keel/sockaddr.h>    /* KlSockAddr */
#include <keel/drain.h>       /* KlDrain */

/* Back-pointer to the owning event context (the internal socket provider reads ctx->sockets).
 * Opaque forward decl keeps the provider seam out of this header. */
struct KlEventCtx;

struct KlStream {
    KlSocketHandle fd;          /**< Socket handle */
    KlAllocator *alloc;         /**< Allocator (set once on pool init) */
    struct KlEventCtx *ctx;     /**< Back-pointer to event ctx (hot path reads ctx->sockets) */

    KlSockAddr peer_addr;       /**< Client address (captured at accept); KL_AF_UNSPEC = unavailable */

    char *read_buf;             /**< Read buffer */
    size_t read_len;            /**< Bytes in read buffer */
    size_t read_cap;            /**< Read buffer capacity */
    int read_paused;            /**< Read-side flow control: 1 = body reads paused */

    /* ── Write machinery (contract: kl_stream_write_* in <keel/stream.h>) ─────────────────── */
    KlDrain            wq;          /**< Bounded write queue (reserved capacity at init) */
    int                wq_inited;   /**< 1 once kl_stream_write_init preallocated wq */
    int                wq_err;      /**< Sticky write-side error */
    int              (*submit_fn)(void *ctx, const char *data, size_t len); /**< NULL = readiness */
    void              *submit_ctx;
    int                submit_copying;   /**< 1 = backend copies on submit */
    int                send_inflight;    /**< a submit is outstanding */
    size_t             inflight_len;     /**< bytes handed to the in-flight submit */
    int                inflight_copying; /**< ownership policy CAPTURED for the in-flight op */

    /* ── Strict read pause/resume (contract: kl_stream_read_* in <keel/stream.h>) ─────────── */
    void             (*read_deliver)(void *ctx, const char *buf, size_t len, int ok);
    int              (*read_arm)(void *ctx);
    void             (*read_disarm)(void *ctx);
    void              *read_ctx;
    int                read_completion_mode;
    int                read_inited;
    int                recv_inflight;
    int                recv_held;
    size_t             held_len;
    int                held_ok;
    int                read_closed;
    int                arming;            /* arm trampoline (bounds stack under sync completion) */
    int                rearm_pending;
    int                completed_inline;

    /* ── Graceful-close / confirmed-detachment (contract: kl_stream_close_* in <keel/stream.h>) */
    int                close_state;      /**< KL_STREAM_STATE_* */
    int                close_abort;      /**< 1 = abortive (cancel ops, drop queue) vs graceful */
    int                in_close_cancel;  /**< DEPTH counter: >0 = inside cancel hooks; defer finalize */
    int                close_notified;   /**< on_close fired (exactly-once) */
    int                close_inited;
    int                wq_closing;       /**< write side closing: reject new writes */
    int                recv_cancel_requested; /**< outstanding recv cancel-requested (once) */
    int                send_cancel_requested; /**< outstanding send cancel-requested (once) */
    void             (*on_close)(void *ctx);
    void              *close_ctx;
    int              (*cancel_recv)(void *ctx);
    int              (*cancel_send)(void *ctx);
    void             (*on_retire)(struct KlStream *s); /**< read/write → close finalize */
};

#endif /* KEEL_STREAM_DETAIL_H */
