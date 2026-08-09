/*
 * keel/listener_detail.h — Layout of KlListener (opt-in detail).
 *
 * INTERNAL / UNSTABLE. Include this ONLY to embed or stack-allocate a KlListener; the fields are
 * NOT part of the API and may change without notice. The behavior/ownership contract is in
 * <keel/listener.h>. Do not read or write these fields directly — use the kl_listener_* functions.
 */
#ifndef KEEL_LISTENER_DETAIL_H
#define KEEL_LISTENER_DETAIL_H

#include <keel/listener.h>   /* KlListener typedef, hook typedefs, KlSlotLease */

struct KlListener {
    int  state;               /* KL_LISTENER_* */
    int  inited;
    int  completion_mode;     /* 1 = a posted accept completes even after disarm (→ dispose) */
    int  detached;            /* on_close fired (exactly-once) */
    int  last_error;          /* last accept/arm error (informational) */

    int  reserved;            /* 1 = a slot credit is held, awaiting an accept to commit it */
    int  accept_inflight;     /* an accept is armed/posted */
    int  accept_cancel_requested;

    /* sync-completion trampoline (bounds stack; mirrors stream_read) */
    int  arming;
    int  rearm_pending;
    int  accepted_inline;

    /* reentrancy guards */
    int  in_start;            /* DEPTH: inside an arm hook — defer detachment */
    int  in_dispatch;         /* DEPTH: inside on_accept/dispose/close/cancel/reserve/release */

    KlSlotReserveFn      reserve;
    KlSlotReleaseFn      release;
    void                *credit_ctx;
    const int           *liveness;
    KlListenerArmFn      arm_accept;
    KlListenerDisarmFn   disarm_accept;
    KlListenerCancelFn   cancel_accept;
    KlListenerAcceptFn   on_accept;
    KlListenerDisposeFn  dispose_fd;
    KlListenerCloseFn    on_close;
    void *ctx;
};

#endif /* KEEL_LISTENER_DETAIL_H */
