/*
 * resolve_uefi.h — a NUMERIC-ONLY reference kl_resolve_sync for the freestanding
 * UEFI client, the pre-DNS resolver seam.
 *
 * libkeel_freestanding.a references kl_resolve_sync (the sync-DNS fallback the async
 * client takes when client_pick_resolver() returns NULL — which it always does under
 * KEEL_FREESTANDING, since the built-in DNS-over-UDP resolver is out of the minimal
 * archive). A client dialing a NUMERIC address literal flows through this same path,
 * so the symbol is a genuine link dependency even with no name resolution.
 *
 * This TU provides it as a dotted-quad IPv4 parser: any non-numeric host FAILS — and that
 * is the PERMANENT contract, not a stopgap. DNS is an ASYNC protocol consumer, not a sync
 * platform-seam capability: a freestanding client that must resolve a hostname injects
 * cfg.resolver (a stock kl_dns_resolver_create over the EFI_UDP4 provider — proven end-to-end
 * by integrations/platform/uefi/tests/dgram_dns_selftest.c). This keeps the axes clean (EFI_UDP4 =
 * platform provider; DNS = protocol) and avoids a sync-over-async adapter. Defining
 * KEEL_UEFI_HAVE_RESOLVE when compiling u1_link_stubs.c drops that file's fail-closed
 * kl_resolve_sync in favor of this one.
 */
#ifndef KEEL_UEFI_RESOLVE_UEFI_H
#define KEEL_UEFI_RESOLVE_UEFI_H
/* kl_resolve_sync itself is declared in src/resolve_sync.h and defined in the .c;
 * this header exists only to document the seam. No public surface of its own. */
#endif /* KEEL_UEFI_RESOLVE_UEFI_H */
