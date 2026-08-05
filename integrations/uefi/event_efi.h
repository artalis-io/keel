/*
 * event_efi.h — a completion-axis KlEventProvider over EFI_TCP4 tokens (U-3, F-8).
 *
 * The event/completion half of the real EFI provider. Together with the U-2 socket
 * provider (socket_efi_tcp4.c) it lets a STOCK libkeel_freestanding.a async KlClient
 * run an HTTP/1.1 GET on bare UEFI firmware: no epoll/kqueue/io_uring, no OS sockets,
 * no errno — just EFI_TCP4 completion tokens pumped by the firmware event services.
 *
 * It is a completion backend (KL_EVENT_CAP_COMPLETION) exactly like event_iocp.c /
 * event_pollcomp.c / lwip-raw, injected at RUNTIME on the stock archive via
 * kl_event_ctx_init_ex(&ctx, alloc, kl_uefi_event_provider(bs, image)). The provider
 * carries a KlCompletionOps (completion.h) with the MINIMAL client-driving surface
 * the freestanding harness proved sufficient (F-7):
 *
 *   - post_connect  → Configure + issue the EFI Connect token (non-blocking).
 *   - drain         → Poll()+CheckEvent the pending Connect tokens → KL_COMP_CONNECT,
 *                     then relay each armed tagged watch → KL_COMP_WATCHER (the
 *                     emulated-readiness relay that re-drives the client's SYNC
 *                     send/recv over the U-2 provider — identical model to the mock
 *                     completion loop in tests/freestanding_harness.c).
 *   - cancel        → drop a queued Connect op (stale completion must not fire); the
 *                     generation bump on socket close is the deeper stale guard.
 *
 * Server/UDP completion primitives (post_accept/post_recv/post_send/post_sendfile/
 * post_udp_*) are NULL — unreached by the client-only completion path, and out of the
 * freestanding archive anyway.
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
