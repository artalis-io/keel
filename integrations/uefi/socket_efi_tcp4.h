/*
 * socket_efi_tcp4.h — a KlSocketProvider over EFI_TCP4 (U-2, Phase 10 F-8).
 *
 * The socket / data-plane half of the real EFI provider: a KlSocketProvider whose
 * KlSocketHandle points to a heap KlUefiConn (NOT the raw EFI_TCP4_PROTOCOL*). The
 * connection object owns everything a stale-completion-safe UEFI socket needs:
 *   - the child EFI_HANDLE (CreateChild result) + the opened EFI_TCP4_PROTOCOL*,
 *   - preallocated connect / rx / tx / close token+event records,
 *   - a generation number bumped on close (so a completion delivered after the
 *     handle is freed and its memory reused is rejected — no stale UAF),
 *   - the pinned EFI_TCP4_CONFIG_DATA + a close/dead flag.
 *
 * Split of responsibility (matches the lwip-raw precedent + the U-0 findings):
 *   - socket()  → CreateChild + OpenProtocol → a fresh KlUefiConn (generation++).
 *   - connect() → the SYNC socket op does the Configure (active mode, DHCP, remote
 *                 addr) and returns KL_IO_PENDING-classified -1. A blocking connect
 *                 is nonsensical over token completions; the ACTUAL async connect
 *                 (issue + pump the Connect token) is U-3's completion post_connect.
 *   - send/recv → Transmit/Receive + pump (Poll()+CheckEvent) — the emulated-
 *                 readiness data plane, exactly like lwr_sock_send/_recv.
 *   - close()   → Close(await) → CloseEvent all → CloseProtocol → DestroyChild →
 *                 kl_free (generation bumped). Teardown order per U-0 finding #6.
 *
 * IPv4 / SOCK_STREAM only (U-2). IPv6 / KL_AF_INET6 is rejected (EFI_TCP6 = a later
 * family switch). Everything is allocated through kl_uefi_allocator (no libc heap).
 *
 * This is a BYO integration injected at runtime — nothing under src/ or include/ or
 * the root Makefile references it. The vtable signatures match src/socket.h exactly.
 */
#ifndef KEEL_UEFI_SOCKET_EFI_TCP4_H
#define KEEL_UEFI_SOCKET_EFI_TCP4_H

#include "efi_uefi.h"
#include "../../spikes/uefi/efi_tcp4.h"

#include <keel/allocator.h>
#include <keel/socket.h>       /* KlSocketProvider, KlIoStatus */
#include <keel/sockaddr.h>     /* KlSockAddr */
#include <keel/error.h>        /* KlError */

/* ── pure EFI_STATUS → Keel mappings (host-unit-testable, no firmware) ────────── */

/*
 * Classify an EFI_STATUS into a portable KlIoStatus. Pure — no firmware, no errno.
 * The mapping (U-0 finding #5):
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
 * data plane, paired with U-3's completion backend). It borrows @bs / @image —
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

#endif /* KEEL_UEFI_SOCKET_EFI_TCP4_H */
