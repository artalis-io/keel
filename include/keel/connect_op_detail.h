/*
 * keel/connect_op_detail.h — Layout of KlConnectOp (opt-in detail).
 *
 * INTERNAL / UNSTABLE. Include this ONLY to embed or stack-allocate a KlConnectOp; the fields are
 * NOT part of the API and may change without notice. The behavior/ownership contract is in
 * <keel/connect_op.h>. Do not read or write these fields directly — use the kl_connect_op_* functions.
 */
#ifndef KEEL_CONNECT_OP_DETAIL_H
#define KEEL_CONNECT_OP_DETAIL_H

#include <keel/connect_op.h>   /* KlConnectOp typedef, hook typedefs, KlConnectResult */

struct KlConnectOp {
    int  state;               /* KL_CONNECT_STATE_* */
    int  inited;              /* 1 once kl_connect_op_init ran */
    int  terminal;            /* on_done fired (exactly-once guard) */
    int  detached;            /* on_detach fired (exactly-once guard) */
    KlConnectResult result;   /* terminal result */
    KlSocketHandle  win_fd;   /* winning fd on SUCCESS, else KL_INVALID_SOCKET */
    int  error;               /* terminal error code (0 on success) */
    int  last_error;          /* most recent failure code (reported on exhaustion) */

    /* resolve op */
    int  resolve_inflight;
    int  resolve_cancel_requested;

    /* racing connect attempts */
    int  naddrs;              /* usable addresses after resolve (<= KL_CONNECT_MAX_ADDRS) */
    int  next_idx;            /* cursor: next address to try */
    int  pending;             /* active attempts count */
    unsigned char attempt_active[KL_CONNECT_MAX_ADDRS];
    unsigned char attempt_cancel_requested[KL_CONNECT_MAX_ADDRS];

    /* timers as first-class outstanding resources (retired synchronously at terminal) */
    int  delay_armed;
    int  deadline_armed;

    /* sync-completion / reentrancy sentinels */
    int  in_start;            /* DEPTH: inside a start/arm hook — defer detachment */
    int  in_dispatch;         /* DEPTH: inside the terminal dispatch / cancel — defer detachment */

    KlConnectResolveFn        start_resolve;
    KlConnectResolveCancelFn  cancel_resolve;
    KlConnectAttemptFn        start_attempt;
    KlConnectAttemptCancelFn  cancel_attempt;
    KlConnectDisposeFn        dispose_fd;
    KlConnectArmDelayFn       arm_delay;
    KlConnectCancelDelayFn    cancel_delay;
    KlConnectArmDeadlineFn    arm_deadline;
    KlConnectCancelDeadlineFn cancel_deadline;
    KlConnectDoneFn           on_done;
    KlConnectDetachFn         on_detach;
    void *ctx;
};

#endif /* KEEL_CONNECT_OP_DETAIL_H */
