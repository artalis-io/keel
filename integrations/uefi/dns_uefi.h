/*
 * dns_uefi.h — synchronous DNS-over-EFI_UDP4 resolver for the freestanding UEFI
 * client (U-5, Phase 10).
 *
 * The freestanding async KlClient falls back to the synchronous kl_resolve_sync when
 * cfg.resolver == NULL (always, under KEEL_FREESTANDING — the built-in DNS-over-UDP
 * resolver is out of the minimal archive). U-3's resolve_uefi.c handles NUMERIC
 * dotted-quad literals only; U-5 extends it (behind -DKL_U5_DNS) to resolve a real
 * hostname by issuing an A-record query over EFI_UDP4 to a configured nameserver and
 * blocking-pumping the transmit/receive tokens to completion — the same synchronous
 * Poll()+CheckEvent pump model every EFI op in U-1..U-4 uses.
 *
 * kl_resolve_sync's signature carries no bs/image, so the app stashes them up front
 * via kl_uefi_dns_init() (mirroring kl_uefi_platform_init).
 *
 * IPv4 / A-record only. Single nameserver. Bounds-safe parse (untrusted network
 * input): every offset checked against the response length, label/pointer traversal
 * capped. Create + teardown of the UDP4 child happens WITHIN the resolve call (a
 * one-shot synchronous query — nothing outlives it, so no generation guard is needed
 * as in socket_efi_tcp4.c).
 *
 * This is a BYO integration injected at runtime — nothing under src/ or include/ or
 * the root Makefile references it. Only build_u5 links it.
 */
#ifndef KEEL_UEFI_DNS_UEFI_H
#define KEEL_UEFI_DNS_UEFI_H

#include "efi_uefi.h"
#include <keel/sockaddr.h>

#include <stdint.h>

/* Stash the boot services + loaded-image handle for the DNS path. Call once, early,
 * after kl_uefi_platform_init and before any client that may resolve a name. */
void kl_uefi_dns_init(EFI_BOOT_SERVICES *bs, EFI_HANDLE image);

/* Resolve @host (a NON-numeric hostname) to an IPv4 address via an A-record query
 * over EFI_UDP4 to the compile-time nameserver, filling @out with the resolved
 * address + @port. Returns 0 on success, -1 on any failure (no init, UDP4 absent,
 * Configure/Transmit/Receive error, timeout, malformed response, or no A record).
 * Synchronous: creates + tears down the UDP4 child within the call. */
int kl_uefi_dns_resolve(const char *host, uint16_t port, KlSockAddr *out);

/* Copy the last successfully-resolved IPv4 (network-order bytes) into @out[4].
 * Returns 1 if a resolution has succeeded since init, 0 otherwise. Lets the selftest
 * print "resolved <name> -> a.b.c.d" without re-parsing the KlSockAddr. */
int kl_uefi_dns_last_ip(uint8_t out[4]);

#endif /* KEEL_UEFI_DNS_UEFI_H */
