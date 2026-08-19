#ifndef KEEL_SRC_DATAGRAM_CORE_H
#define KEEL_SRC_DATAGRAM_CORE_H

/*
 * datagram_core.h — INTERNAL fixed-slot datagram assembly (Phase B, step 7A-3).
 *
 * KlDgramCore is the integrated Tier-1 object the public KlDatagram (7B) will be a facade/layout over:
 * it wires the already-built machines into one lifecycle over a prepared (provider/fd) transport —
 *
 *   - OBJECT-owned outbound send pool (KlDgramSlots) + the single-flight FIFO send machine (KlDgramSend);
 *   - LIFE-token-owned inbound slot (KlDgramInbound) + the serial receive machine (KlDgramRecv), held in
 *     a separately-allocated rx-holder freed by the B.6 KlDgramLife on_final (so it outlives any posted
 *     op AND the caller-owned KlDgramCore);
 *   - the confirmed-detachment close coordinator (KlDgramClose) with the §4.3 retirement classifier.
 *
 * The core is provider-NEUTRAL: it takes explicit adapter hooks (send submit, recv arm/disarm/pull,
 * cancel, retire classify) rather than a KlSocketProvider — 7B binds those to a real provider +
 * completion loop; a test scripts them. This is NOT the public API (no kl_datagram_* / ABI here) and
 * does NOT touch KlUdp (its byte-budget transport is untouched — D-COMPAT).
 *
 * INTERNAL header — not installed, no ABI commitment.
 */

#include <keel/allocator.h>
#include <keel/handle.h>

#include "datagram_slots.h"
#include "datagram_send.h"
#include "datagram_recv.h"
#include "datagram_close.h"
#include "datagram_life.h"

/* Physically close the prepared fd (the provider's socket close). Invoked EXACTLY ONCE by the close
 * coordinator's backend-retirement step (after the outbound queue drains). 7B binds it to the socket
 * provider; a test scripts it to assert the exactly-once fd close. */
typedef void (*KlDgramCloseTransportFn)(void *ctx, KlSocketHandle fd);

/* Borrowed neutral adapters + prepared transport state. The `fd` is already socket()'d/configure()'d/
 * bound by the caller through its provider; the core owns it from a SUCCESSFUL init to detachment. */
typedef struct {
    KlAllocator *alloc;        /* event-ctx/backend allocator — MUST outlive posted ops (frees the
                                * life-owned rx holder at on_final). */
    KlSocketHandle fd;
    int          completion;   /* 1 = completion (submit posts; recv arm posts) / 0 = readiness (sync). */
    size_t       send_slots;   /* outbound slot count (count-based backpressure). */
    size_t       send_slot_cap;/* per outbound slot payload capacity. */
    size_t       send_byte_budget;/* M1: 0 = SLOT policy; >0 = BOTH byte-gate budget (bytes). */
    size_t       recv_cap;     /* inbound slot payload capacity. */
    unsigned     caps;         /* KL_DGRAM_CAP_* for UNSUPPORTED mapping on send. */
    unsigned     accepted_rx_caps;/* M5.1: enabled-per-socket KL_DGRAM_RX_* mask (already masked by the
                                * facade). Stored for the GRO-activation predicate; no wiring in M5.1. */

    /* send */
    KlDgramSubmitFn submit; void *submit_ctx;      /* copy-before-accept is the submit adapter's job. */
    /* recv */
    KlDgramRecvArmFn arm; KlDgramRecvDisarmFn disarm; KlDgramRecvPullFn pull; void *recv_hook_ctx;
    KlDgramRecvDeliverFn deliver; void *deliver_ctx;
    /* close */
    KlDgramCancelFn cancel_send, cancel_recv; void *cancel_ctx;
    KlDgramRetireFn retire; void *retire_ctx;      /* per-op RETIRED/QUARANTINED/PENDING (may be NULL). */
    KlDgramCloseTransportFn close_transport; void *transport_ctx;  /* physical fd close (once); REQUIRED —
                                                    * the core adopts the fd, so it must be able to close it. */
    KlDgramCloseFn  on_close; void *close_ctx;     /* terminal classification (fires once). */
    /* Completion-dispatch identity for the B.6 token (7B-3). A live completion facade installs its typed
     * handler (kl_datagram_comp_dispatch) here so the driver routes this token's completions via
     * life->dispatch(target=KlDgramCore, ev). NULL (the default / neutral-adapter tests) → the token
     * carries no dispatch and completions are driven directly (kl_dgram_core_{send,recv}_on_complete). */
    KlDgramDispatchFn dispatch;
    /* Final PRE-ADOPTION hook (7B-7). Called ONCE, as the LAST fallible step — only after EVERY core
     * allocation has succeeded and immediately before the fd is adopted. Returns 0 to commit (fd
     * adopted) or non-0 to abort: the core then unwinds every prepared allocation and returns -1 WITHOUT
     * adopting the fd. Used by the completion facade to register the fd with the loop (kl_event_add →
     * CreateIoCompletionPort on IOCP) as the last step, so a failed/non-committed init NEVER leaves the
     * fd associated with a completion port it cannot be detached from except by closing it. NULL = no
     * pre-adoption step (always commits). */
    int  (*on_prepared)(void *ctx);
    void  *prepared_ctx;
} KlDgramCoreConfig;

/* The life-owned receive holder: inbound slot + recv machine, freed as a unit by on_final so a posted
 * recv op's storage outlives the (caller-owned) core. */
typedef struct KlDgramCoreRx {
    KlAllocator   *alloc;
    KlDgramLife   *life;       /* back-ref (the token that owns THIS holder) */
    KlDgramInbound inbound;
    KlDgramRecv    recv;
} KlDgramCoreRx;

typedef struct KlDgramCore {   /* tagged so <keel/datagram_detail.h> can forward-declare it opaquely (7B-3) */
    KlAllocator   *alloc;
    KlSocketHandle fd;
    int            completion;
    unsigned       caps;
    /* object-owned outbound (freed at detachment once send retired) */
    KlDgramSlots   out;
    /* caller-owned send + close machines */
    KlDgramSend    send;
    KlDgramClose   close;
    /* life-owned rx holder (inbound + recv), pinned by `life` */
    KlDgramCoreRx *rx;
    KlDgramLife   *life;
    /* physical fd close, driven once by the coordinator's backend-retirement step */
    KlDgramCloseTransportFn close_transport; void *transport_ctx;
    int            fd_closed;
    /* the caller's terminal callback — the core interposes its own on_close to drop the owner life ref
     * (releasing the rx storage on DETACHED, or leaving it pinned on QUARANTINE) before forwarding. */
    KlDgramCloseFn user_on_close; void *user_close_ctx;
    int            inited;
    /* M5.1 scaffolding (no wiring yet). `ext` = the core-adopted batch extension (KlDgramBatchExt*,
     * opaque here; NULL when no batch attached — the single-flight paths run untouched). `gso_unsupported`
     * = the per-fd first-use GSO latch (§6.3). `accepted_rx_caps` = the enabled-per-socket KL_DGRAM_RX_*
     * mask (masked by the facade) for the GRO-activation predicate. */
    void          *ext;
    int            gso_unsupported;
    unsigned       accepted_rx_caps;
    /* Owner-destruction (abandon) reclamation (docs/datagram_sync_teardown_design.md §4a). When
     * `abandoning`, the coordinator's SILENT terminal (deferred to the outermost busy-frame leave) skips
     * user_on_close and instead runs `abandon_reclaim` as its destructive tail — the facade frees the
     * object-owned machines + this heap core + memsets the handle, then invokes `abandon_owner` to free
     * the embedding owner (e.g. the resolver). All of this runs at the outermost leave, so it is
     * reentrancy-safe when kl_datagram_teardown is called from within on_recv/on_writable/on_drain. */
    int            abandoning;
    void         (*abandon_reclaim)(void *ctx); void *abandon_reclaim_ctx;   /* facade: free core + handle */
    void         (*abandon_owner)(void *ctx);   void *abandon_owner_ctx;     /* owner: free the container */
} KlDgramCore;

/* Assemble the core over the prepared transport + neutral adapters. Overflow-/failure-safe: on any
 * failure the object is left zeroed (fd NOT adopted — caller retains it) and re-init-able. Requires
 * alloc, valid fd, send_slots/send_slot_cap/recv_cap > 0, submit, arm, deliver, AND close_transport
 * (the core adopts the fd on success, so it must own a way to close it). Returns 0 / -1. */
int  kl_dgram_core_init(KlDgramCore *core, const KlDgramCoreConfig *cfg);

/* One whole datagram out (atomic accept; count-based backpressure). See KlDatagramSendStatus. */
KlDatagramSendStatus kl_dgram_core_send(KlDgramCore *core, const KlDatagramMessage *m);

/* Async send retirement (completion) / writable flush (readiness). 0 / -1. */
int  kl_dgram_core_send_on_complete(KlDgramCore *core, int ok);
int  kl_dgram_core_send_flush(KlDgramCore *core);

/* Receive control (delegates to KlDgramRecv). pause is STRICT: readiness drops interest at the backend
 * seam (disarm); a completion recv already posted stays in flight and is HELD on completion, delivered
 * exactly once on resume then re-armed. recv_stop discards any held datagram + drops interest. */
int  kl_dgram_core_recv_start(KlDgramCore *core);
int  kl_dgram_core_recv_on_complete(KlDgramCore *core, size_t len, int ok);
int  kl_dgram_core_recv_on_readable(KlDgramCore *core);
void kl_dgram_core_pause(KlDgramCore *core);
int  kl_dgram_core_resume(KlDgramCore *core);
void kl_dgram_core_recv_stop(KlDgramCore *core);
void kl_dgram_core_set_view_pull(KlDgramCore *core, KlDgramRecvViewFn view_pull, void *ctx);   /* M5.3 */
int  kl_dgram_core_recv_started(const KlDgramCore *core);   /* M5.3: recv_start latched (attach refused) */
static inline unsigned kl_dgram_core_accepted_rx_caps(const KlDgramCore *core) {
    return core ? core->accepted_rx_caps : 0u;   /* M5.3: the GRO two-part-gate half stored at init */
}
int  kl_dgram_core_recv_held(const KlDgramCore *core);      /* 1 = a paused completion datagram is held */

/* Close (graceful/abortive) + progress hook + terminal classification. */
int  kl_dgram_core_close_begin(KlDgramCore *core);
int  kl_dgram_core_close_cancel(KlDgramCore *core);
int  kl_dgram_core_close_retry(KlDgramCore *core);
KlDgramCloseState     kl_dgram_core_close_state(const KlDgramCore *core);
KlDatagramCloseResult kl_dgram_core_close_result(const KlDgramCore *core);

/* Owner-destruction SILENT teardown (docs/datagram_sync_teardown_design.md, Option A + §4a). Requests a
 * forced SILENT terminal via the close coordinator's existing busy handshake: mark dead → cancel/close
 * backend → release owner ref → run `reclaim` (the facade's free-core-and-handle) → `owner` (free the
 * container). If no busy frame is active it all runs synchronously before returning; if invoked from
 * within a send/recv delivery frame (on_recv/on_writable/on_drain) the terminal + reclamation are
 * DEFERRED to the outermost leave — still within the same top-level dispatch, so no caller pump is
 * needed and no machine state is freed under an active frame. The rx holder + any in-flight/EFI-
 * quarantined op's ref outlive, reclaimed at the token's final release. The ordinary confirmed-detachment
 * close (begin/cancel + free) is UNCHANGED. `reclaim`/`owner` may be NULL. 0 / -1. */
int  kl_dgram_core_abandon(KlDgramCore *core,
                           void (*reclaim)(void *), void *reclaim_ctx,
                           void (*owner)(void *),   void *owner_ctx);

/* Free the OBJECT-owned machines (close + send-via-abandon + slots). Used by the abandon reclaim as its
 * destructive tail; does NOT touch the life-owned rx holder or the heap core block. */
void kl_dgram_core_free_object_owned(KlDgramCore *core);

/* Frame bracket for a DISPATCHER that runs send/recv ops then touches the owning handle afterwards (the
 * readiness watcher). Hold on entry, release as the LAST action: a teardown requested from within a
 * delivery callback then defers to `dispatch_end` instead of freeing the handle mid-dispatch. `dispatch_end`
 * may run the (abandon) terminal and free the core — so it must be the caller's last touch of the core. */
void kl_dgram_core_dispatch_begin(KlDgramCore *core);
void kl_dgram_core_dispatch_end(KlDgramCore *core);

/* Registered send edges (optional). */
void kl_dgram_core_on_writable(KlDgramCore *core, KlDgramWritableFn cb, void *ctx);
void kl_dgram_core_on_drain(KlDgramCore *core, KlDgramDrainFn cb, void *ctx);
void kl_dgram_core_on_drop(KlDgramCore *core, KlDgramDropFn cb, void *ctx);   /* M5.2a recoverable drop */
void kl_dgram_core_set_gso_cbs(KlDgramCore *core, KlDgramSubmitGsoFn submit_gso, void *submit_ctx,
                               KlDgramGsoDoneFn on_gso_done, void *done_ctx);   /* M5.2b GSO */

/* The inbound slot (recv storage) — a submit/pull adapter writes the received datagram here. */
KlDgramSlot *kl_dgram_core_inbound_slot(KlDgramCore *core);

/* The B.6 stable-liveness token. A completion backend adapter RETAINS one ref for EVERY posted
 * datagram op — recv AND send alike (uniform with pollcomp/io_uring/IOCP/EFI) — and releases it at
 * that op's terminal completion (kl_dgram_life_retain/_release). The token carries operation-identity
 * / owner-lifetime for both directions; that a send's PAYLOAD lives in the object-owned outbound pool
 * (copy-before-accept) is a SEPARATE concern and does not exempt a send from the token. A QUARANTINED
 * op (recv or send) intentionally never releases its ref, so on_final — which frees the life-owned rx
 * storage — is deferred until that ref is reclaimed. Capture the pointer at POST time (it stays valid
 * via the refcount); it returns NULL once detached. */
KlDgramLife *kl_dgram_core_life(KlDgramCore *core);

/* Free the OBJECT-owned parts (outbound pool + send + close) and drop the owner life ref (its final
 * release runs on_final → frees the rx holder + inbound). REFUSED with -1 before CLOSED. */
int  kl_dgram_core_free(KlDgramCore *core);

#endif /* KEEL_SRC_DATAGRAM_CORE_H */
