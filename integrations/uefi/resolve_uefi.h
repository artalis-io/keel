/*
 * resolve_uefi.h — a NUMERIC-ONLY reference kl_resolve_sync for the freestanding
 * UEFI client (U-3), the pre-DNS resolver seam.
 *
 * libkeel_freestanding.a references kl_resolve_sync (the sync-DNS fallback the async
 * client takes when client_pick_resolver() returns NULL — which it always does under
 * KEEL_FREESTANDING, since the built-in DNS-over-UDP resolver is out of the minimal
 * archive). A client dialing a NUMERIC address literal flows through this same path,
 * so the symbol is a genuine link dependency even with no name resolution.
 *
 * This TU provides it as a dotted-quad IPv4 parser: any non-numeric host FAILS (a
 * pre-DNS freestanding client cannot resolve names — the correct behavior). U-5 will
 * replace this with a real KlResolver over EFI_UDP4 / EFI_DNS4; until then it is what
 * a minimal EFI HTTP client ships. Defining KEEL_UEFI_HAVE_RESOLVE when compiling
 * u1_link_stubs.c drops that file's fail-closed kl_resolve_sync in favor of this one.
 */
#ifndef KEEL_UEFI_RESOLVE_UEFI_H
#define KEEL_UEFI_RESOLVE_UEFI_H
/* kl_resolve_sync itself is declared in src/resolve_sync.h and defined in the .c;
 * this header exists only to document the seam. No public surface of its own. */
#endif /* KEEL_UEFI_RESOLVE_UEFI_H */
