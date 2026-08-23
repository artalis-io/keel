/*
 * datagram_test_util.h — shared helpers for the KlDatagram tests and smokes.
 *
 * Exercises the PUBLIC lifecycle — kl_datagram_close_cancel/close_begin -> drive the loop until CLOSED ->
 * kl_datagram_free — instead of the internal kl_datagram_teardown escape hatch, so consumers
 * prove the canonical public teardown contract on both readiness and completion backends.
 */
#ifndef KEEL_TESTS_DATAGRAM_TEST_UTIL_H
#define KEEL_TESTS_DATAGRAM_TEST_UTIL_H

#include <keel/datagram.h>
#include <keel/event_ctx.h>

/* Abortive close (discard queued output + cancel any posted recv), then drive `ctx` until the datagram
 * reaches CLOSED (immediate on readiness; a later tick on a completion backend), then free. Returns 0 on
 * a clean free, -1 if it never reached CLOSED within the tick budget. NULL-safe. */
static inline int kl_dg_close_free(struct KlEventCtx *ctx, KlDatagram *dg) {
    if (!dg) return 0;
    if (kl_datagram_close_state(dg) != KL_DGRAM_CLOSE_CLOSED)
        (void)kl_datagram_close_cancel(dg);
    for (int i = 0; i < 300 && kl_datagram_close_state(dg) != KL_DGRAM_CLOSE_CLOSED; i++)
        kl_event_ctx_run(ctx, 16, 5);
    if (kl_datagram_close_state(dg) != KL_DGRAM_CLOSE_CLOSED) return -1;
    return kl_datagram_free(dg);
}

/* Graceful variant: close_begin (drain queued output first), then drive until CLOSED, then free. */
static inline int kl_dg_close_free_graceful(struct KlEventCtx *ctx, KlDatagram *dg) {
    if (!dg) return 0;
    if (kl_datagram_close_state(dg) != KL_DGRAM_CLOSE_CLOSED)
        (void)kl_datagram_close_begin(dg);
    for (int i = 0; i < 300 && kl_datagram_close_state(dg) != KL_DGRAM_CLOSE_CLOSED; i++)
        kl_event_ctx_run(ctx, 16, 5);
    if (kl_datagram_close_state(dg) != KL_DGRAM_CLOSE_CLOSED) return -1;
    return kl_datagram_free(dg);
}

#endif /* KEEL_TESTS_DATAGRAM_TEST_UTIL_H */
