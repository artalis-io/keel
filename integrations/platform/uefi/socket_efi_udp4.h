/*
 * socket_efi_udp4.h: the datagram half of the unified EFI socket provider.
 *
 * Adds an EFI_UDP4 datagram data-plane to the EFI provider so the STOCK freestanding
 * dns_resolver.c resolves over KlDatagram-over-EFI_UDP4 on bare firmware. It is the
 * datagram sibling of socket_efi_tcp4.c: the SAME single KlSocketProvider serves both
 * SOCK_STREAM (EFI_TCP4 child) and SOCK_DGRAM (EFI_UDP4 child), because KlEventCtx.sockets
 * is singular and a UEFI HTTP client resolves (UDP) then connects (TCP) on one ctx.
 *
 * ── Handle discrimination ─────────────────────────────────────────────
 * A datagram KlSocketHandle is TAGGED so it is disjoint from a stream handle and
 * self-identifying: `handle = KL_EFI_UDP_HANDLE_TAG | (udp_slot + 1)`. UDP slots live in
 * their OWN fixed pool with their OWN magic, so a stream late-completion can never index
 * UDP state and vice-versa. socket_efi_tcp4.c's conn_of() naturally rejects a tagged
 * handle (it is out of the TCP slot range), and udp_of() requires the tag. The even/odd
 * `generation` stale-guard (in slot storage, read from STABLE memory) protects backend ops
 * that CAPTURED the generation at post, NOT arbitrary reused caller handles (same
 * caller-owns-close contract as EFI_TCP4). A datagram completion recovers the KEEL owner
 * (the datagram core) via the stable token (ev->life), and validates the EFI child via this
 * captured-generation guard: two independent guards.
 *
 * ── I/O model ─────────────────────────────────────────────────────────
 * COMPLETION-native. event_efi.c's post_dgram_recv/_send drive the primitives below
 * (post one EFI_UDP4 Receive/Transmit token; drain Polls+CheckEvents them); the KlDatagramOps
 * vtable supplies only `configure` (mandatory at kl_datagram_socket_init) plus a sync `send` fallback
 * for the source-pinned/TOS path the completion fast-path skips; `recv` is NULL (a sync
 * Receive would be a second receive machine violating one-in-flight).
 *
 * ── Cancel / quarantine ───────────────────────────────────────────────
 * Every submitted token reaches ONE terminal state (completed OR cancelled-and-drained)
 * before its storage is freed. On a close/teardown with an outstanding token, Cancel + drain;
 * if the drain cannot confirm retirement within budget, the slot is QUARANTINED; the
 * firmware-reachable EFI storage (token/event/child/Tx payload) is leaked until EBS, the
 * op's token ref is never released (retirement unconfirmed), and the socket fail-closes.
 * A SIGNALLED token's Packet is valid even if the child generation is stale (still recycle
 * RxData); an UNSIGNALLED (quarantined) token's Packet is never inspected. All ops are
 * fail-closed after ExitBootServices.
 *
 * IPv4 / SOCK_DGRAM only. IPv6 (EFI_UDP6) is a later family switch. BYO integration;
 * nothing under src/ or the root Makefile references it; vtable signatures match src/socket.h
 * and include/keel/datagram.h exactly.
 */
#ifndef KEEL_UEFI_SOCKET_EFI_UDP4_H
#define KEEL_UEFI_SOCKET_EFI_UDP4_H

#include "efi_uefi.h"
#include "efi_udp4.h"

#include <keel/allocator.h>
#include <keel/socket.h>       /* KlSocketProvider, KlSocketHandle, KlIoStatus */
#include <keel/datagram.h>     /* KlDatagramOps, KlDgramRxMeta */
#include <keel/sockaddr.h>     /* KlSockAddr */

#include <stddef.h>
#include <stdint.h>

/* ── Handle tag ────────────────────────────────────────────────────────
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
 * sockets can be created. Tolerates absence (a build with no UDP4 stack); returns -1 and
 * the provider simply does not advertise KL_SOCK_CAP_DATAGRAM. Single-instance (file-scope
 * ctx); idempotent. Returns 0 if a UDP4 ServiceBinding is present, -1 otherwise. */
int  kl_uefi_udp_provider_init(EFI_BOOT_SERVICES *bs, EFI_HANDLE image);
/* Free the ServiceBinding handle buffer + clear the file-scope ctx (mirrors the TCP reset).
 * Does NOT close live datagram sockets. Safe on an un-inited provider. */
void kl_uefi_udp_provider_reset(void);

/* TEST SUPPORT ONLY: reclaim the entire UDP slot pool (incl. quarantined slots the production reset
 * preserves) so host-mock quarantine tests don't exhaust the small fixed pool. Never used on firmware. */
void kl_uefi_udp_test_reset_pool(void);

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
 * CloseEvent/Configure(NULL)/CloseProtocol/DestroyChild, or leak under quarantine.
 * Fail-closed post-EBS (mark dead, touch no boot service). Generation bumped. */
int  kl_uefi_udp_close(KlSocketHandle fd);

/* Live (open, not closed, not quarantined) datagram slots: MUST be 0 before
 * kl_uefi_shutdown()/EBS. A quarantined slot counts as NOT live (it is leaked, never
 * reclaimed) but is reported separately below for diagnostics. */
int  kl_uefi_udp_provider_live_count(void);
int  kl_uefi_udp_provider_quarantined_count(void);

/* ── Stale-child guard for the completion backend (captured-at-post) ───────────────── */
unsigned long long kl_uefi_udp_generation_h(KlSocketHandle fd);
int  kl_uefi_udp_valid_h(KlSocketHandle fd, unsigned long long generation);

/* ── Completion-op result: the substrate MUST distinguish a cleanly-retired stale
 * op (the event layer drops it AND releases its KlDgramLife ref) from a QUARANTINED op (the event
 * layer removes it from polling but NEVER releases the ref; retirement was never confirmed). A bare
 * "terminal-drop" conflates the two and would release a quarantined op's life. Every poll/cancel/query
 * returns one of: ─────────────────────────────────────────────────────────────────────────────────── */
typedef enum {
    KL_UEFI_UDP_OP_PENDING = 0,    /* poll/query: still the live op, not yet signalled; keep polling */
    KL_UEFI_UDP_OP_DELIVERED,      /* poll: live op signalled (recv: out_bytes/out_ok set; send likewise) */
    KL_UEFI_UDP_OP_RETIRED,        /* cancel: confirmed retired now → release life */
    KL_UEFI_UDP_OP_STALE_RETIRED,  /* op's slot was cleanly closed/reused (reaped at close) → release life */
    KL_UEFI_UDP_OP_QUARANTINED,    /* op's slot was quarantined at close → RETAIN life forever (unconfirmed) */
    KL_UEFI_UDP_OP_INVALID         /* not a datagram handle: fail safe; NOT a confirmed retirement */
} KlUefiUdpOpResult;

/* Side-effect-free state of the op {fd,@generation}: PENDING (still the live posted op),
 * STALE_RETIRED (cleanly closed/reused), QUARANTINED (leaked at close), or INVALID. The event layer
 * uses this to decide life-release without polling (e.g. after close). */
KlUefiUdpOpResult kl_uefi_udp_op_state(KlSocketHandle fd, unsigned long long generation);

/* ── Completion-native token primitives (driven by event_efi.c post_dgram_* + drain) ──
 *
 * OPERATION IDENTITY: the completion backend polls an operation that was posted
 * EARLIER, and between post and poll the slot may have been closed (dead) or CLOSED-AND-REUSED
 * (a NEW socket at the same handle value). Resolving the op through the live handle alone
 * (udp_of(fd)) is therefore UNSAFE: a dead slot would skip the op, and a reused slot would let
 * the poll touch the NEW socket's token (cross-generation confusion). So every poll/cancel/query
 * below takes the GENERATION captured at post (kl_uefi_udp_generation_h after post_recv/_send) and
 * resolves the EXACT posted record via STABLE slot storage + a generation match. NEVER udp_of(fd).
 * If the generation no longer matches (closed / reused / quarantined), the op is STALE: it was
 * already reaped+recycled at close (single-threaded, close runs before any reuse), so the poll
 * returns a terminal-DROP WITHOUT touching the (possibly reused) token.
 *
 * RECEIVE (serial, at most one Receive token outstanding per socket):
 *   post_recv  → issue EFI_UDP4.Receive(&rx_tok) WITHOUT pumping (on the live slot). 0/-1.
 *   poll_recv  → resolve {fd,@generation}: -1 = not a datagram handle; STALE → 1 with out_ok=0
 *                (terminal-drop, token untouched). LIVE + Poll()+CheckEvent: 0 = pending; 1 =
 *                terminal (signalled) → coalesce the datagram into @copy_into (bounded by @cap;
 *                *out_trunc if it exceeded @cap), fill out_src + out_local from RxData.UdpSession,
 *                set out_bytes + out_ok, and ALWAYS SignalEvent(RxData.RecycleSignal). (@copy_into
 *                NULL on a live signalled token recycles WITHOUT copying, a drain-without-deliver.)
 *                Marks the token no longer posted (close must not re-drain it).
 *
 * TRANSMIT (independent Tx token, completion send):
 *   post_send  → issue EFI_UDP4.Transmit(&tx_tok) with a copied payload + per-datagram @dest. 0/-1.
 *   poll_send  → resolve {fd,@generation} (same stale rule): 0 = pending; 1 = terminal
 *                (out_bytes + out_ok, or out_ok=0 on stale); -1 = not a datagram handle.
 *
 * CANCEL (close / datagram teardown while a token is outstanding):
 *   cancel_recv / cancel_send → resolve {fd,@generation}; Cancel(token) + bounded drain.
 *                1 = retired (recycle a signalled RxData) or already-stale; 0 = UNCONFIRMED →
 *                the caller quarantines. */
int kl_uefi_udp_post_recv(KlSocketHandle fd);
KlUefiUdpOpResult kl_uefi_udp_poll_recv(KlSocketHandle fd, unsigned long long generation,
                          void *copy_into, size_t cap,
                          KlSockAddr *out_src, KlSockAddr *out_local,
                          int *out_trunc, size_t *out_bytes, int *out_ok);
int kl_uefi_udp_post_send(KlSocketHandle fd, const void *data, size_t len,
                          const KlSockAddr *dest);
KlUefiUdpOpResult kl_uefi_udp_poll_send(KlSocketHandle fd, unsigned long long generation,
                          size_t *out_bytes, int *out_ok);
KlUefiUdpOpResult kl_uefi_udp_cancel_recv(KlSocketHandle fd, unsigned long long generation);
KlUefiUdpOpResult kl_uefi_udp_cancel_send(KlSocketHandle fd, unsigned long long generation);
/* 1 iff a Receive/Transmit token is currently outstanding on the EXACT op {fd,@generation}. */
int kl_uefi_udp_recv_posted(KlSocketHandle fd, unsigned long long generation);
int kl_uefi_udp_send_posted(KlSocketHandle fd, unsigned long long generation);

#endif /* KEEL_UEFI_SOCKET_EFI_UDP4_H */
