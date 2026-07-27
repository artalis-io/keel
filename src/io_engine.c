/*
 * io_engine.c — readiness-build stub for the completion tick (PAL Phase 8).
 *
 * Linked on every build EXCEPT the IOCP one (Makefile selection). The server only
 * calls kl_io_engine_run_completion() when its event loop advertises
 * KL_EVENT_CAP_COMPLETION, which no readiness backend does — so on these builds
 * this is never invoked. It exists solely so the shared server loop can reference
 * the symbol without a platform #ifdef. The real tick lives in event_iocp.c.
 */
#include "io_engine.h"

int kl_io_engine_run_completion(struct KlServer *s, int timeout_ms) {
    (void)s; (void)timeout_ms;
    return -1;   /* unreachable on readiness builds */
}
