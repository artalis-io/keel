/*
 * keel_lwip.h — lwIP platform providers for Keel (socket + event backend).
 *
 * A bring-your-own lwIP platform: a matched pair of a KlSocketProvider (over the
 * lwIP BSD socket API) and a KlEventProvider (over lwip_poll), so Keel's server
 * and client run on an lwIP TCP/IP stack with no kernel sockets and no core
 * recompile. Install both on a KlHttpServerConfig / KlEventCtx:
 *
 *   KlHttpServerConfig cfg = {
 *       .port = 8080, .bind_addr = "127.0.0.1",
 *       .sockets        = kl_socket_provider_lwip(),
 *       .event_provider = kl_event_provider_lwip(),
 *   };
 *
 * (The event provider's native_provider() returns the lwIP socket provider, so
 * setting only .event_provider auto-wires .sockets to match.)
 *
 * lwIP is NOT vendored — build against your own lwIP (LWIP_DIR) + lwipopts.h.
 * The providers use only Keel's public authoring API (<keel/socket.h>,
 * <keel/event.h>); no internal Keel or host-POSIX types. lwIP is readiness-only
 * (its sockets layer is lwip_poll). See docs/lwip_platform_design.md.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef KEEL_LWIP_H
#define KEEL_LWIP_H

#include <keel/socket.h>
#include <keel/event.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The lwIP socket provider (KlHttpServerConfig.sockets / KlEventCtx.sockets). */
const KlSocketProvider *kl_socket_provider_lwip(void);

/** @brief The lwIP readiness event backend (KlHttpServerConfig.event_provider). Pairs with
 *  kl_socket_provider_lwip(); its native_provider() returns that provider. */
const KlEventProvider *kl_event_provider_lwip(void);

#ifdef __cplusplus
}
#endif

#endif /* KEEL_LWIP_H */
