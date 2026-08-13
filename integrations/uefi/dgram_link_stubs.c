/*
 * dgram_link_stubs.c — datagram/passive socket-seam link residuals for the 6.4c
 * stock-dns_resolver-over-EFI_UDP4 self-test (build_dgram_dns.sh).
 *
 * u1_link_stubs.c covers the CLIENT residuals (connect/recv/send seam fallbacks + the
 * event/completion builtins + kl_resolve_sync). The DATAGRAM path (udp.c) and the shared
 * event_efi.c additionally reference the bind / get_local_addr / set_cloexec / accept /
 * listen / dgram DEFAULT-provider fallbacks: the unified EFI provider always supplies these
 * ops, so the kl_sockdef_* else-branch of the seam inline is never taken — but the symbol
 * must still resolve under -nostdlib (the compiler emits the reference). Fail-closed.
 *
 * This is SEPARATE from s4_link_stubs.c (which also defines bind/listen/accept for the S-4
 * server): the 6.4c client-datagram build links u1_link_stubs.c + this, never s4, so there
 * is no duplicate definition. Adding these to u1_link_stubs.c would collide with s4 in the
 * server build, hence a dedicated TU.
 */

#include "../../src/socket.h"   /* KlSocketHandle / KlSockAddr + kl_sockdef_* + KlDatagramOps */

void           kl_sockdef_set_cloexec(KlSocketHandle f) { (void)f; }
int            kl_sockdef_bind(KlSocketHandle f, const KlSockAddr *a) { (void)f; (void)a; return -1; }
int            kl_sockdef_listen(KlSocketHandle f, int backlog) { (void)f; (void)backlog; return -1; }
KlSocketHandle kl_sockdef_accept(KlSocketHandle f, KlSockAddr *peer) { (void)f; (void)peer; return KL_INVALID_SOCKET; }
int            kl_sockdef_get_local_addr(KlSocketHandle f, KlSockAddr *a) { (void)f; (void)a; return -1; }
const struct KlDatagramOps *kl_sockdef_dgram(void) { return (const struct KlDatagramOps *)0; }
