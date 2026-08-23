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
 * or, for a KlHttpServer, KlHttpServerConfig.event_provider = kl_event_provider_lwip_raw(). Its
 * native_provider() returns kl_socket_provider_lwip_raw(), so the ctx/server auto-wires
 * the matching overlapped socket provider on the completion loop.
 *
 * lwIP is NOT vendored — build against your own lwIP (LWIP_DIR) with the NO_SYS=1
 * lwipopts_raw.h. See integrations/platform/lwip/Makefile (loopback-raw target).
 *
 * ── Supported / Unsupported (this is the API-facing capability statement) ──────────────
 *
 * SUPPORTED:
 *   - IPv4 TCP *server* (KlHttpServer): accept/recv/send over the loopback netif.
 *   - IPv4 TCP *client* (KlHttpClient): outbound connect via the COMPLETION connect primitive
 *     (kl_comp_post_connect → tcp_connect), plaintext HTTP/1.1 (LC-1), Happy-Eyeballs address
 *     racing (LC-2), and HTTPS (LC-4). The client's send/recv ride an emulated readiness
 *     watcher over the raw loop.
 *   - HTTP/1.1 including keep-alive.
 *   - HTTPS — both directions (LC-4): the client over the mbedTLS socket-BIO routed through
 *     kl_socket_provider_lwip_raw() (kl_sock_send/recv → tcp_write/read), and the server over
 *     the generic memory-BIO completion-TLS leg. Buffered HTTP/1.1 over TLS (no ALPN-h2, no
 *     TLS file/stream body). BYO mbedTLS.
 *   - UDP (KlDatagram / udp_server): the provider exposes datagram ops (.dgram != NULL), so
 *     kl_datagram_socket_init() over the raw completion loop succeeds (LC-3a).
 *   - DNS: KEEL's built-in async resolver (src/protocols/dns/dns_resolver.c) over KlDatagram-on-raw (LC-3) —
 *     one DNS path, no lwIP dns_gethostbyname.
 *   - Buffered, streaming, and file responses of UNBOUNDED size (transmit memory is bounded
 *     by a fixed per-conn window — the response/file size is not).
 *   - Request bodies with bounded per-conn receive flow-control (ERR_MEM backpressure).
 *   - The server-path modules that ride KlHttpServer: router, middleware, CORS, SSE, body
 *     readers, compression.
 *   - Multiple SEQUENTIAL event contexts (create → destroy → create).
 *
 * UNSUPPORTED (fail early + clearly):
 *   - The SYNCHRONOUS socket-provider connect op (p->ops->connect) — returns -1 / ENOTSUP
 *     BY DESIGN: a blocking connect is nonsensical on a NO_SYS=1 single-loop target. Outbound
 *     client connects go through the COMPLETION connect primitive (see SUPPORTED), not this op.
 *   - IPv6 — bind() rejects a non-IPv4 address (the loopif is IPv4).
 *   - A SECOND SIMULTANEOUS raw context — rejected at create (NO_SYS=1 lwIP core is
 *     process-global); sequential contexts are fine.
 *
 * NOT SPECIFICALLY DEMONSTRATED (honest disclosure): server- AND client-side WebSocket /
 *   HTTP-2 ride the generic completion driver / client machine (branches exist for any
 *   completion backend) but are not lwip-raw-specifically tested — no claim of tested support
 *   is made. ALPN-h2 over raw TLS is explicitly out of the LC-4 subset.
 *
 * CONSTRAINTS:
 *   - NO_SYS=1, single-thread: KEEL's event loop IS the lwIP mainloop (no separate lwIP
 *     thread). The thread-pool done_fn still runs on the loop thread, as always.
 *   - One active raw context per process.
 *
 * For IPv6 or the BSD-socket lwIP model, use the readiness socket-API integration
 * (keel_lwip.h: kl_socket_provider_lwip() + kl_event_provider_lwip()).
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
