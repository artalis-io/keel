/*
 * keel_lwip_raw.h — Phase 9 lwIP raw-API COMPLETION event backend factories.
 *
 * The completion-model counterpart of keel_lwip.h (the readiness lwIP platform).
 * This backend drives lwIP's native raw `tcp_*` callback API in NO_SYS=1 mode: no
 * tcpip thread, no sockets/netconn — KEEL's event loop IS the lwIP mainloop.
 *
 * CRUCIAL (docs/phase9_lwip_raw_design.md): unlike kl_event_provider_lwip() (a
 * runtime-injectable READINESS provider that rides a STOCK libkeel), this is a
 * COMPLETION backend — it requires a libkeel BUILT for the completion axis
 * (Makefile BACKEND=lwipraw links completion_driver.c). Install it via:
 *
 *   KlEventCtx ctx;
 *   kl_event_ctx_init_ex(&ctx, &alloc, kl_event_provider_lwip_raw());
 *
 * (Its native_provider() returns kl_socket_provider_lwip_raw(), so the ctx
 * auto-wires the matching overlapped socket provider on a completion loop.)
 *
 * lwIP is NOT vendored — build against your own lwIP (LWIP_DIR) with the NO_SYS=1
 * lwipopts_raw.h. See integrations/lwip/Makefile (loopback-raw target).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef KEEL_LWIP_RAW_H
#define KEEL_LWIP_RAW_H

#include <keel/event.h>
#include <keel/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The lwIP raw-API completion event backend (KlEventCtx.event_provider).
 *  Advertises KL_EVENT_CAP_COMPLETION; pairs with kl_socket_provider_lwip_raw(). */
const KlEventProvider *kl_event_provider_lwip_raw(void);

/** @brief The overlapped socket provider paired with the lwIP raw completion loop
 *  (KL_SOCK_CAP_OVERLAPPED). The event backend's native_provider() returns this. */
const KlSocketProvider *kl_socket_provider_lwip_raw(void);

#ifdef __cplusplus
}
#endif

#endif /* KEEL_LWIP_RAW_H */
