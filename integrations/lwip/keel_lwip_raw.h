/*
 * keel_lwip_raw.h — Phase 9 lwIP raw-API COMPLETION event backend factories.
 *
 * The completion-model counterpart of keel_lwip.h (the readiness lwIP platform).
 * This backend drives lwIP's native raw `tcp_*` callback API in NO_SYS=1 mode: no
 * tcpip thread, no sockets/netconn — KEEL's event loop IS the lwIP mainloop.
 *
 * RUNTIME PROVIDER over a STOCK libkeel (RC-3): like kl_event_provider_lwip() (the
 * readiness lwIP provider), this completion backend rides a STOCK libkeel — no bespoke
 * library build. The always-linked completion driver + dispatch (completion_driver.c /
 * completion_dispatch.c, present on every default build since RC-1) reach this backend's
 * primitives through the injected loop->ops->completion. BACKEND=lwipraw is retired.
 * Install it via:
 *
 *   KlEventCtx ctx;
 *   kl_event_ctx_init_ex(&ctx, &alloc, kl_event_provider_lwip_raw());
 *
 * or, for a KlServer, KlConfig.event_provider = kl_event_provider_lwip_raw(). Its
 * native_provider() returns kl_socket_provider_lwip_raw(), so the ctx/server auto-wires
 * the matching overlapped socket provider on the completion loop.
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
