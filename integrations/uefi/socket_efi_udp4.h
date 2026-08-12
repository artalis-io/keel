/*
 * socket_efi_udp4.h — the datagram half of the unified EFI socket provider (6.4b).
 *
 * Adds an EFI_UDP4 datagram data-plane to the EFI provider so the STOCK freestanding
 * dns_resolver.c resolves over KlUdp-over-EFI_UDP4 on bare firmware. It is the
 * datagram sibling of socket_efi_tcp4.c: the SAME single KlSocketProvider serves both
 * SOCK_STREAM (EFI_TCP4 child) and SOCK_DGRAM (EFI_UDP4 child), because KlEventCtx.sockets
 * is singular and a UEFI HTTP client resolves (UDP) then connects (TCP) on one ctx.
 * Design: docs/phase10_efi_udp4_provider_design.md (FROZEN).
 *
 * ── Handle discrimination (FROZEN #3) ─────────────────────────────────────────────
 * A datagram KlSocketHandle is TAGGED so it is disjoint from a stream handle and
 * self-identifying: `handle = KL_EFI_UDP_HANDLE_TAG | (udp_slot + 1)`. UDP slots live in
 * their OWN fixed pool with their OWN magic, so a stream late-completion can never index
 * UDP state and vice-versa. socket_efi_tcp4.c's conn_of() naturally rejects a tagged
 * handle (it is out of the TCP slot range), and udp_of() requires the tag. The even/odd
 * `generation` stale-guard (in slot storage, read from STABLE memory) protects backend ops
 * that CAPTURED the generation at post — NOT arbitrary reused caller handles (same
 * caller-owns-close contract as EFI_TCP4). A datagram completion recovers the KEEL owner
 * (KlUdp) via the B.6 stable token (ev->life), and validates the EFI child via this
 * captured-generation guard — two independent guards.
 *
 * ── I/O model (FROZEN #1) ─────────────────────────────────────────────────────────
 * COMPLETION-native. event_efi.c's post_dgram_recv/_send drive the primitives below
 * (post one EFI_UDP4 Receive/Transmit token; drain Polls+CheckEvents them); the KlDatagramOps
 * vtable supplies only `configure` (mandatory at kl_udp_init) plus a sync `send` fallback
 * for the source-pinned/TOS path the completion fast-path skips — `recv` is NULL (a sync
 * Receive would be a second receive machine violating one-in-flight).
 *
 * ── Cancel / quarantine (FROZEN #6) ───────────────────────────────────────────────
 * Every submitted token reaches ONE terminal state (completed OR cancelled-and-drained)
 * before its storage is freed. On a close/teardown with an outstanding token, Cancel + drain;
 * if the drain cannot confirm retirement within budget, the slot is QUARANTINED — the
 * firmware-reachable EFI storage (token/event/child/Tx payload) is leaked until EBS, the
 * op's B.6 token ref is never released (retirement unconfirmed), and the socket fail-closes.
 * A SIGNALLED token's Packet is valid even if the child generation is stale (still recycle
 * RxData); an UNSIGNALLED (quarantined) token's Packet is never inspected. All ops are
 * fail-closed after ExitBootServices.
 *
 * IPv4 / SOCK_DGRAM only. IPv6 (EFI_UDP6) is a later family switch. BYO integration —
 * nothing under src/ or the root Makefile references it; vtable signatures match src/socket.h
 * and include/keel/datagram.h exactly.
 */
#ifndef KEEL_UEFI_SOCKET_EFI_UDP4_H
#define KEEL_UEFI_SOCKET_EFI_UDP4_H

#include "efi_uefi.h"
#include "../../spikes/uefi/efi_udp4.h"

#include <keel/allocator.h>
#include <keel/socket.h>       /* KlSocketProvider, KlSocketHandle, KlIoStatus */
#include <keel/datagram.h>     /* KlDatagramOps, KlDgramRxMeta */
#include <keel/sockaddr.h>     /* KlSockAddr */

#include <stddef.h>
#include <stdint.h>

/* ── Handle tag (FROZEN #3) ────────────────────────────────────────────────────────
 * A modest high bit, far above KL_EFI_MAX_UDP, so a UDP handle value can never collide
 * with a TCP slot handle (1..KL_EFI_MAX_CONNS) and the tag is trivially strip/testable. */
#define KL_EFI_UDP_HANDLE_TAG ((intptr_t)1 << 20)
#define KL_EFI_MAX_UDP        8            /* DNS needs 1–2; small fixed pool, no heap */

/* Is @fd a datagram (EFI_UDP4) handle? Used by the unified provider in socket_efi_tcp4.c
 * to route socket-lifecycle ops (close/get_local_addr/set_*) to the UDP variants. */
static inline int kl_efi_is_udp_handle(KlSocketHandle fd) {
    return kl_handle_valid(fd) && (((intptr_t)fd) & KL_EFI_UDP_HANDLE_TAG) != 0;
}

/* ── Provider-lifetime init (called by the unified provider builder) ───────────────
 * Locate the EFI_UDP4 ServiceBinding over @bs and stash bs/image/allocator so datagram
 * sockets can be created. Tolerates absence (a build with no UDP4 stack) — returns -1 and
 * the provider simply does not advertise KL_SOCK_CAP_DATAGRAM. Single-instance (file-scope
 * ctx); idempotent. Returns 0 if a UDP4 ServiceBinding is present, -1 otherwise. */
int  kl_uefi_udp_provider_init(EFI_BOOT_SERVICES *bs, EFI_HANDLE image);
/* Free the ServiceBinding handle buffer + clear the file-scope ctx (mirrors the TCP reset).
 * Does NOT close live datagram sockets. Safe on an un-inited provider. */
void kl_uefi_udp_provider_reset(void);

/* The datagram data-plane vtable (KlSocketProvider.dgram). `configure` maps to
 * EFI_UDP4.Configure (unconnected); `send` is the sync source-pin/TOS fallback (independent
 * Tx token under the same quarantine discipline); `recv` is NULL. */
const KlDatagramOps *kl_uefi_udp_dgram_ops(void);

/* ── Socket lifecycle (dispatched from the unified provider by handle tag) ─────────── */
KlSocketHandle kl_uefi_udp_socket(int domain, int type, int protocol);
int  kl_uefi_udp_bind(KlSocketHandle fd, const KlSockAddr *addr);
int  kl_uefi_udp_get_local_addr(KlSocketHandle fd, KlSockAddr *addr);
void kl_uefi_udp_set_cloexec(KlSocketHandle fd);       /* no-op on EFI (no fd inheritance) */
int  kl_uefi_udp_set_nonblocking(KlSocketHandle fd);   /* no-op success (EFI is non-blocking) */
KlIoStatus kl_uefi_udp_io_status(KlSocketHandle fd);   /* last op's EFI_STATUS → KlIoStatus */
/* close(): Cancel + drain any outstanding Rx/Tx token (quarantine on unconfirmed), then
 * CloseEvent/Configure(NULL)/CloseProtocol/DestroyChild, or leak under quarantine. F3
 * fail-closed post-EBS (mark dead, touch no boot service). Generation bumped. */
int  kl_uefi_udp_close(KlSocketHandle fd);

/* F6: live (open, not closed, not quarantined) datagram slots — MUST be 0 before
 * kl_uefi_shutdown()/EBS. A quarantined slot counts as NOT live (it is leaked, never
 * reclaimed) but is reported separately below for diagnostics. */
int  kl_uefi_udp_provider_live_count(void);
int  kl_uefi_udp_provider_quarantined_count(void);

/* ── Stale-child guard for the completion backend (captured-at-post) ───────────────── */
unsigned long long kl_uefi_udp_generation_h(KlSocketHandle fd);
int  kl_uefi_udp_valid_h(KlSocketHandle fd, unsigned long long generation);

/* ── Completion-native token primitives (driven by event_efi.c post_dgram_* + drain) ──
 *
 * RECEIVE (serial — at most one Receive token outstanding per socket):
 *   post_recv  → issue EFI_UDP4.Receive(&rx_tok) WITHOUT pumping. 0 = posted, -1 = error.
 *   poll_recv  → Poll()+CheckEvent(rx_tok): 0 = pending; 1 = terminal (signalled). On a
 *                terminal, if @copy_into != NULL coalesce the datagram into it (bounded by
 *                @cap; *out_trunc set if the datagram exceeded @cap), fill out_src + out_local
 *                from RxData.UdpSession, and set out_bytes + out_ok; if @copy_into == NULL
 *                (stale-child no-event drop) recycle WITHOUT copying. RxData.RecycleSignal is
 *                ALWAYS signalled on a signalled terminal with non-NULL RxData. -1 = invalid fd.
 *                Marks the token no longer posted (close must not re-drain it).
 *
 * TRANSMIT (independent Tx token — completion send):
 *   post_send  → issue EFI_UDP4.Transmit(&tx_tok) with a copied payload + per-datagram @dest
 *                (UdpSessionData). 0 = posted, -1 = error.
 *   poll_send  → Poll()+CheckEvent(tx_tok): 0 = pending; 1 = terminal (out_bytes + out_ok);
 *                -1 = invalid fd.
 *
 * CANCEL (close / kl_udp_free while a token is outstanding):
 *   cancel_recv / cancel_send → Cancel(token) + bounded drain. 1 = retired (recycle a
 *                signalled RxData), 0 = UNCONFIRMED → the caller quarantines. */
int kl_uefi_udp_post_recv(KlSocketHandle fd);
int kl_uefi_udp_poll_recv(KlSocketHandle fd, void *copy_into, size_t cap,
                          KlSockAddr *out_src, KlSockAddr *out_local,
                          int *out_trunc, size_t *out_bytes, int *out_ok);
int kl_uefi_udp_post_send(KlSocketHandle fd, const void *data, size_t len,
                          const KlSockAddr *dest);
int kl_uefi_udp_poll_send(KlSocketHandle fd, size_t *out_bytes, int *out_ok);
int kl_uefi_udp_cancel_recv(KlSocketHandle fd);
int kl_uefi_udp_cancel_send(KlSocketHandle fd);
/* 1 iff a Receive token is currently outstanding on @fd (drain iterates only posted ops). */
int kl_uefi_udp_recv_posted(KlSocketHandle fd);
int kl_uefi_udp_send_posted(KlSocketHandle fd);

#endif /* KEEL_UEFI_SOCKET_EFI_UDP4_H */
