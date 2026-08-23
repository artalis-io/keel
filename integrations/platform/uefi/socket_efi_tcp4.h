/*
 * socket_efi_tcp4.h — a KlSocketProvider over EFI_TCP4.
 *
 * The socket / data-plane half of the EFI provider: a KlSocketProvider whose
 * KlSocketHandle points to a heap KlUefiConn (NOT the raw EFI_TCP4_PROTOCOL*). The
 * connection object owns everything a stale-completion-safe UEFI socket needs:
 *   - the child EFI_HANDLE (CreateChild result) + the opened EFI_TCP4_PROTOCOL*,
 *   - preallocated connect / rx / tx / close token+event records,
 *   - a generation number bumped on close (so a completion delivered after the
 *     handle is freed and its memory reused is rejected — no stale UAF),
 *   - the pinned EFI_TCP4_CONFIG_DATA + a close/dead flag.
 *
 * Responsibility split (mirrors the lwip-raw precedent):
 *   - socket()  → CreateChild + OpenProtocol → a fresh KlUefiConn (generation++).
 *   - connect() → the SYNC socket op does the Configure (active mode, DHCP, remote
 *                 addr) and returns KL_IO_PENDING-classified -1. A blocking connect
 *                 is nonsensical over token completions; the ACTUAL async connect
 *                 (issue + pump the Connect token) is the completion post_connect.
 *   - send/recv → Transmit/Receive + pump (Poll()+CheckEvent) — the emulated-
 *                 readiness data plane, exactly like lwr_sock_send/_recv.
 *   - close()   → Close(await) → CloseEvent all → CloseProtocol → DestroyChild →
 *                 kl_free (generation bumped).
 *
 * IPv4 / SOCK_STREAM only. IPv6 / KL_AF_INET6 is rejected (EFI_TCP6 = a later
 * family switch). Everything is allocated through kl_uefi_allocator (no libc heap).
 *
 * This is a BYO integration injected at runtime — nothing under src/ or include/ or
 * the root Makefile references it. The vtable signatures match src/socket.h exactly.
 */
#ifndef KEEL_UEFI_SOCKET_EFI_TCP4_H
#define KEEL_UEFI_SOCKET_EFI_TCP4_H

#include "efi_uefi.h"
#include "efi_tcp4.h"

#include <keel/allocator.h>
#include <keel/socket.h>       /* KlSocketProvider, KlIoStatus */
#include <keel/sockaddr.h>     /* KlSockAddr */
#include <keel/error.h>        /* KlError */

/* ── pure EFI_STATUS → Keel mappings (host-unit-testable, no firmware) ────────── */

/*
 * Classify an EFI_STATUS into a portable KlIoStatus. Pure — no firmware, no errno.
 * The mapping:
 *   EFI_SUCCESS            → KL_IO_OK
 *   EFI_NOT_READY          → KL_IO_WOULD_BLOCK   (keep pumping)
 *   EFI_TIMEOUT            → KL_IO_WOULD_BLOCK    (a pump deadline is a retry, not a
 *                                                  fatal — the caller re-arms; the
 *                                                  overall request deadline is the
 *                                                  hard stop, owned above the seam)
 *   EFI_NO_MAPPING         → KL_IO_WOULD_BLOCK   (DHCP not settled — retry/pump)
 *   EFI_CONNECTION_FIN     → KL_IO_CLOSED        (orderly peer close / EOF)
 *   EFI_CONNECTION_RESET   → KL_IO_RESET
 *   EFI_CONNECTION_REFUSED → KL_IO_RESET         (connect refused — a reset-class
 *                                                  failure the client surfaces as
 *                                                  KL_ERR_CONNECT)
 *   anything else (error)  → KL_IO_FATAL
 *   any other success      → KL_IO_OK
 */
KlIoStatus kl_efi_status_to_io(EFI_STATUS st);

/* Map an EFI_STATUS to a coarse KlError category (diagnostics). Pure. */
KlError kl_efi_status_to_error(EFI_STATUS st);

/* Unified provider: publish the last op's EFI_STATUS into the provider-global channel that
 * KlSocketProvider.ops->io_status reads. The datagram TU calls this on its synchronous send so a
 * datagram failure is classified from datagram state, not stale stream state. */
void kl_uefi_provider_set_last_status(EFI_STATUS st);

/* ── KlSockAddr ⇄ EFI_IPv4_ADDRESS (IPv4 only) ────────────────────────────────── */

/* Marshal an IPv4 KlSockAddr into an EFI_IPv4_ADDRESS (4 network-order bytes).
 * Returns 0 on success, -1 if @a is not KL_AF_INET (IPv6/unix rejected). The port
 * is returned separately (host order) via @out_port when non-NULL. */
int kl_efi_sockaddr_to_ipv4(const KlSockAddr *a, EFI_IPv4_ADDRESS *out, UINT16 *out_port);

/* Build an IPv4 KlSockAddr from an EFI_IPv4_ADDRESS + host-order port. Returns 0. */
int kl_efi_ipv4_to_sockaddr(const EFI_IPv4_ADDRESS *ip, UINT16 port, KlSockAddr *out);

/* ── the provider ─────────────────────────────────────────────────────────────── */

/*
 * Build the EFI_TCP4 socket provider. Locates the TCP4 ServiceBinding protocol
 * (LocateHandleBuffer + HandleProtocol on handle 0) and stores it, @bs, @image, and
 * an allocator (kl_uefi_allocator over @bs) in an internal context. Returns a
 * borrowed const KlSocketProvider* (static storage + the ctx) or NULL if no TCP4
 * ServiceBinding is present (this OVMF build has no network stack).
 *
 * The returned provider advertises KL_SOCK_CAP_OVERLAPPED (the completion-driven
 * data plane, paired with the completion backend). It borrows @bs / @image —
 * they must outlive every socket created through it (i.e. until ExitBootServices).
 * Single-instance: a second call before kl_uefi_socket_provider_reset() returns the
 * same provider (the ctx is file-scope, mirroring lwip-raw's one-active-ctx model).
 */
const KlSocketProvider *kl_uefi_socket_provider(EFI_BOOT_SERVICES *bs, EFI_HANDLE image);

/* Release the provider's ServiceBinding handle buffer + clear the file-scope ctx so
 * a subsequent kl_uefi_socket_provider() re-locates. Does NOT close live sockets
 * (each KlUefiConn is closed via the provider's close op). Safe to call on an
 * un-created provider (no-op). */
void kl_uefi_socket_provider_reset(void);

/* How many connection slots are still LIVE (opened, not yet closed). The lifecycle
 * contract (lifecycle_uefi.h) is that this MUST be 0 before kl_uefi_shutdown()/
 * ExitBootServices — every socket the app opened must be closed first. kl_uefi_shutdown()
 * reads this to detect a contract violation. Pure; reads the stable slot pool. */
int kl_uefi_socket_provider_live_count(void);

/* ── Async-connect primitives (driven by the EFI completion backend) ────────
 *
 * The completion path (event_efi.c) splits connect into three NON-BLOCKING steps
 * its drain can pump, instead of the all-in-one blocking kl_uefi_socket_connect_now:
 *   configure     → Configure() (active/DHCP/remote). May block briefly settling
 *                   DHCP (EFI_NO_MAPPING retry). 0 = configured, -1 = failed
 *                   (conn io_status carries the class). Idempotent once configured.
 *   connect_post  → issue tcp->Connect(&conn_tok) WITHOUT pumping — returns as soon
 *                   as the token is accepted. 0 = posted, -1 = immediate error.
 *   connect_poll  → Poll()+CheckEvent(conn_tok): 1 = terminal (*out_ok = connected),
 *                   0 = still pending (keep pumping), -1 = invalid handle.
 * All three operate on the KlUefiConn behind @fd. */
int kl_uefi_socket_configure(KlSocketHandle fd, const KlSockAddr *a);
int kl_uefi_socket_connect_post(KlSocketHandle fd);
int kl_uefi_socket_connect_poll(KlSocketHandle fd, int *out_ok);

/* Capacity-gated Accept-token arming — the server-side analogue of connect_post
 * (backpressure). listen() creates the Accept-token pool events but arms NONE;
 * the completion event layer calls this every tick with @want = the number of FREE Keel
 * connection slots, so the number of armed EFI Accept tokens never exceeds Keel's spare
 * capacity (a connection Keel can't service waits in the TCP backlog, not in a firmware
 * child handle). accept() harvests a completed token WITHOUT re-arming; this is the only
 * re-arm path. Returns the resulting posted (armed-or-completed-unharvested) token count.
 * Idempotent. */
int kl_uefi_socket_accept_arm(KlSocketHandle fd, int want);

/* Handle-based stale-completion guard for the completion backend (which holds a
 * KlSocketHandle + a captured generation, not the opaque KlUefiConn*). A completion
 * delivered after the conn was closed (generation bumped) or its memory reused
 * (magic cleared) is rejected. kl_uefi_conn_generation_h reads the live generation
 * to capture at post time; 0 if @fd is not a live conn. */
unsigned long long kl_uefi_conn_generation_h(KlSocketHandle fd);
int kl_uefi_conn_valid_h(KlSocketHandle fd, unsigned long long generation);

/* ── Non-blocking-recv readiness probe ──────────────────────────────────
 * The completion drain's READ-readiness test: return 1 iff recv() on @fd can
 * return something NOW (buffered data, EOF, or a latched error), else 0. A
 * single call is non-blocking — it posts a Receive token if none is outstanding
 * and Polls+CheckEvents it exactly once; it never pumps/Stalls. A drain-polled
 * receive keeps the completion loop from stalling for a whole recv (timers fire,
 * ops interleave). */
int kl_uefi_socket_recv_ready(KlSocketHandle fd);

#endif /* KEEL_UEFI_SOCKET_EFI_TCP4_H */
