/*
 * lwip_raw_glue.c — the lwIP-touching half of the Phase 9 raw completion backend.
 *
 * This TU includes ONLY lwIP's NO_SYS=1 raw headers — never KEEL's socket/net headers —
 * so lwIP's htons/ntohs macros and its ssize_t typedef cannot clash with the host
 * <sys/socket.h>/<netinet/in.h> that event_lwip_raw.c pulls in through the KEEL socket
 * seam. The two halves meet across lwip_raw_glue.h (opaque void* netif; no lwIP type
 * escapes). See docs/phase9_lwip_raw_design.md and lwip_raw_glue.h for the rationale.
 *
 * P9-1: brings up NO_SYS=1 lwIP + the loopback netif (mirroring raw_loopback_spike.c)
 * and drives one mainloop tick. The raw tcp_* callback wiring (accept/recv/send) is
 * P9-2..P9-5 and will grow here alongside the completion-event collection.
 *
 * SPDX-License-Identifier: MIT
 */
#include "lwip_raw_glue.h"

#include "lwip/init.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"
#include "lwip/netif.h"
#include "lwip/ip_addr.h"
#include "lwip/sys.h"

#include <time.h>

/* ── NO_SYS=1 requires the port to supply sys_now() (u32 ms, monotonic) ──────────
 * Same source as the spike: clock_gettime(CLOCK_MONOTONIC). lwIP calls this from
 * sys_check_timeouts(). Lives here (the lwIP-only TU) so it links with liblwip_raw.a. */
#define KL_LWR_NS_PER_MS 1000000L
u32_t sys_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u32_t)((u32_t)ts.tv_sec * 1000u + (u32_t)(ts.tv_nsec / KL_LWR_NS_PER_MS));
}

/* lwip_init() must run exactly once per process (it seeds global pools + the loop netif).
 * NO_SYS=1 is single-threaded, so a plain flag is safe. */
static int g_lwip_inited = 0;

void *kl_lwr_lwip_up(void) {
    if (!g_lwip_inited) {
        lwip_init();          /* auto-creates the loop netif via LWIP_HAVE_LOOPIF */
        g_lwip_inited = 1;
    }
    /* Find the loopback netif by its 127.0.0.1 address (the loopif symbol is not public),
     * then bring it up + default so 127.0.0.1 routes to it — exactly as the spike does. */
    struct netif *lo = NULL, *nif;
    NETIF_FOREACH(nif) {
        if (ip4_addr_isloopback(netif_ip4_addr(nif))) { lo = nif; break; }
    }
    if (!lo) return NULL;

    ip4_addr_t ip, nm, gw;
    IP4_ADDR(&ip, 127, 0, 0, 1);
    IP4_ADDR(&nm, 255, 0, 0, 0);
    IP4_ADDR(&gw, 127, 0, 0, 1);
    netif_set_addr(lo, &ip, &nm, &gw);
    netif_set_up(lo);
    netif_set_link_up(lo);
    netif_set_default(lo);
    return lo;
}

void kl_lwr_lwip_tick(void *loopif) {
    sys_check_timeouts();                          /* fire lwIP timers (retransmit, etc.) */
    if (loopif) netif_poll((struct netif *)loopif); /* drain loopback TX → RX */
}
