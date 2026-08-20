/*
 * event_efi.h — a completion-axis KlEventProvider over EFI_TCP4 tokens (U-3 client, S-3..S-7 server).
 *
 * The event/completion half of the real EFI provider. Together with the U-2 socket
 * provider (socket_efi_tcp4.c) it lets a STOCK freestanding libkeel async KlHttpClient AND a
 * freestanding KlHttpServer run HTTP(S) on bare UEFI firmware: no epoll/kqueue/io_uring, no
 * OS sockets, no errno — just EFI_TCP4 completion tokens pumped by the firmware event
 * services.
 *
 * It is a completion backend (KL_EVENT_CAP_COMPLETION) exactly like event_iocp.c /
 * event_pollcomp.c / lwip-raw, injected at RUNTIME on the stock archive via
 * kl_event_ctx_init_ex(&ctx, alloc, kl_uefi_event_provider(bs, image)). The provider
 * carries a KlCompletionOps (completion.h).
 *
 * ── Data-plane asymmetry (client vs server) ──────────────────────────────────────
 * The two directions drive I/O differently, by design:
 *   - CLIENT: completion-based CONNECT (post_connect → KL_COMP_CONNECT), then the actual
 *     send/recv ride the drain WATCHER RELAY (KL_COMP_WATCHER) over the U-2 provider's
 *     synchronous ops — the emulated-readiness model of tests/freestanding_harness.c.
 *   - SERVER: completion-NATIVE accept/recv/send. The generic completion server
 *     (completion_server.c) posts prime_accepts/post_recv/post_send directly, surfaced by
 *     drain as KL_COMP_ACCEPT / KL_COMP_READ / KL_COMP_WRITE. Accept arming is
 *     capacity-gated (kl_uefi_socket_accept_arm, backpressure). No heap in the send path
 *     (the response iovec is snapshotted into a fixed inline op buffer).
 *
 * KlCompletionOps surface: post_connect, cancel, drain (client + shared); prime_accepts,
 * post_accept, post_recv, post_send (server). post_sendfile + post_udp_* stay NULL — file
 * responses and UDP are not yet wired for the EFI server.
 *
 * NOTE (abstraction boundary, matches IOCP/pollcomp): the SERVER recv path is now
 * protocol-blind — post_recv(KlStream*, buf, cap) does raw transport I/O into a caller-chosen
 * buffer and never peeks TLS/connection state. The HTTP completion adapter picks the buffer
 * (plaintext read_buf vs the per-conn TLS ciphertext scratch) and feeds ciphertext to
 * tls->feed_input; the backend interprets no protocol state (Phase-A receive-boundary move).
 *
 * BYO integration: nothing under src/ or include/ or the root Makefile references it.
 * The native socket provider (kl_uefi_socket_provider) is created lazily and returned
 * from native_provider() so kl_event_ctx_sockets_compatible() passes.
 */
#ifndef KEEL_UEFI_EVENT_EFI_H
#define KEEL_UEFI_EVENT_EFI_H

#include "efi_uefi.h"
#include <keel/event.h>   /* KlEventProvider */

/*
 * Build the EFI completion event provider. Stashes @bs/@image so native_provider()
 * can lazily create the U-2 socket provider over them. Returns a borrowed const
 * pointer (static storage) — pass it to kl_event_ctx_init_ex(). Borrows @bs/@image;
 * they must outlive the event ctx (i.e. until ExitBootServices). Single-instance
 * (mirrors the socket provider): a second call before kl_uefi_event_provider_reset()
 * returns the same provider.
 */
const KlEventProvider *kl_uefi_event_provider(EFI_BOOT_SERVICES *bs, EFI_HANDLE image);

/* Clear the file-scope event state so a subsequent kl_uefi_event_provider() re-inits.
 * Does NOT reset the socket provider (call kl_uefi_socket_provider_reset separately).
 * Safe on an un-created provider (no-op). */
void kl_uefi_event_provider_reset(void);

#endif /* KEEL_UEFI_EVENT_EFI_H */
