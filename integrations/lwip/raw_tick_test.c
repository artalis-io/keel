/*
 * raw_tick_test.c — Phase 9 P9-1 test: the lwIP-raw COMPLETION KlEventCtx ticks.
 *
 * Proves the P9-1 bar (docs/phase9_lwip_raw_design.md): "an lwIP-raw KlEventCtx ticks
 * without a server." It inits a KlEventCtx on the lwIP-raw completion backend, asserts
 * the loop's event caps and its socket provider are compatible (a completion loop needs
 * an overlapped provider — the backend auto-wires kl_socket_provider_lwip_raw), then
 * drives the loop ~20 times via kl_event_ctx_run. Each run enters the completion path
 * (kl_event_caps → KL_EVENT_CAP_COMPLETION → kl_comp_run → kl_comp_drain), which does
 * ONE lwIP mainloop tick (sys_check_timeouts + netif_poll) and returns 0 events. A
 * non-negative return every tick + a clean free = the completion loop drives lwIP with
 * no server, no crash. Accept/recv/send land in P9-2..P9-5.
 *
 * Runs in-process over the loopback netif — no tap device, no root — like the spike.
 */
#include <keel/event_ctx.h>
#include <keel/allocator.h>

#include "keel_lwip_raw.h"
#include "event_caps.h"   /* kl_event_ctx_sockets_compatible (internal negotiation) */

#include <stdio.h>

#define TICK_ITERATIONS  20
#define TICK_MAX_EVENTS  16
#define TICK_TIMEOUT_MS  10

int main(void) {
    KlAllocator alloc = kl_allocator_default();

    /* Init the ctx on the lwIP-raw completion backend. init() brings up NO_SYS=1 lwIP
     * (lwip_init + attach the loopback netif) and stashes per-loop state. */
    KlEventCtx ctx;
    if (kl_event_ctx_init_ex(&ctx, &alloc, kl_event_provider_lwip_raw()) != 0) {
        printf("P9-1 FAIL: kl_event_ctx_init_ex(lwip_raw) failed\n");
        return 1;
    }

    /* The backend's native_provider() must auto-wire the overlapped socket provider so
     * the completion loop and its provider are compatible (kl_caps_compatible keys on
     * KL_SOCK_CAP_OVERLAPPED for a COMPLETION loop). Mirror the server/client auto-wire:
     * a completion loop adopts kl_event_native_provider() when none was configured. */
    if (ctx.sockets == NULL)
        ctx.sockets = kl_event_native_provider(&ctx.loop);

    if (!kl_event_ctx_sockets_compatible(&ctx)) {
        printf("P9-1 FAIL: lwip-raw completion loop + socket provider not compatible\n");
        kl_event_ctx_free(&ctx);
        return 1;
    }

    /* Drive the completion loop: each run is one lwIP tick. Assert non-negative every
     * time (>= 0 = drained OK / no fatal loop error) — the P9-1 liveness proof. */
    for (int i = 0; i < TICK_ITERATIONS; i++) {
        int n = kl_event_ctx_run(&ctx, TICK_MAX_EVENTS, TICK_TIMEOUT_MS);
        if (n < 0) {
            printf("P9-1 FAIL: kl_event_ctx_run returned %d on tick %d\n", n, i);
            kl_event_ctx_free(&ctx);
            return 1;
        }
    }

    kl_event_ctx_free(&ctx);
    printf("P9-1 PASS\n");
    return 0;
}
