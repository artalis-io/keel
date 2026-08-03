/*
 * lwip_raw_glue.h — INTERNAL seam between the KEEL completion backend and lwIP raw.
 *
 * Why a seam at all: lwIP's headers (lwip/def.h) redefine htons/ntohs as macros and its
 * arch.h typedefs ssize_t — both clash with the host <sys/socket.h>/<netinet/in.h> that
 * KEEL's internal socket seam (src/socket.h → sockcompat.h) and keel/net.h pull in. A
 * single TU cannot include BOTH the lwIP raw headers AND the KEEL socket headers. So the
 * lwIP-touching code lives in lwip_raw_glue.c (lwIP headers ONLY, no KEEL socket headers)
 * and the completion backend lives in event_lwip_raw.c (KEEL headers only, no lwIP). They
 * meet across this tiny, type-neutral header — the "put platform-only helpers behind a
 * seam, keep #ifdef/foreign types out of the shared TU" pattern.
 *
 * The lwIP netif is passed as an opaque `void *` so this header pulls no lwIP type.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef KEEL_LWIP_RAW_GLUE_H
#define KEEL_LWIP_RAW_GLUE_H

/* Bring up NO_SYS=1 lwIP (once per process) and attach the loopback netif; return the
 * loop netif as an opaque handle, or NULL if the build created no loopif. Idempotent:
 * a second call reuses the already-initialised stack and returns the same netif. */
void *kl_lwr_lwip_up(void);

/* Drive one lwIP mainloop tick: sys_check_timeouts() (lwIP timers) + netif_poll(loopif)
 * (drain the loopback TX queue into RX so raw tcp_* callbacks fire). `loopif` is the
 * opaque handle from kl_lwr_lwip_up(); a NULL handle only fires the timers. */
void kl_lwr_lwip_tick(void *loopif);

#endif /* KEEL_LWIP_RAW_GLUE_H */
