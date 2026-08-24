/*
 * dgram_link_stubs.c: datagram/passive socket-seam link residuals for the
 * stock-dns_resolver-over-EFI_UDP4 self-test (build_dgram_dns.sh).
 *
 * u1_link_stubs.c covers the CLIENT residuals (connect/recv/send seam fallbacks + the
 * event/completion builtins + kl_resolve_sync + kl_sockdef_accept). The DATAGRAM path (the datagram machine)
 * and the shared event_efi.c additionally reference the bind / get_local_addr / set_cloexec /
 * listen / dgram DEFAULT-provider fallbacks: the unified EFI provider always supplies these
 * ops, so the kl_sockdef_* else-branch of the seam inline is never taken; but the symbol
 * must still resolve under -nostdlib (the compiler emits the reference). Fail-closed.
 *
 * kl_sockdef_accept is NOT here: it is defined in u1_link_stubs.c, which this build
 * ALSO links; defining it here too would duplicate it. The freestanding servers get accept from
 * u1 as well (they link u1+s4); s4_link_stubs.c keeps only the server-only bind/listen.
 */

#include "../../../../src/socket.h"   /* KlSocketHandle / KlSockAddr + kl_sockdef_* + KlDatagramOps */

void           kl_sockdef_set_cloexec(KlSocketHandle f) { (void)f; }
int            kl_sockdef_bind(KlSocketHandle f, const KlSockAddr *a) { (void)f; (void)a; return -1; }
int            kl_sockdef_listen(KlSocketHandle f, int backlog) { (void)f; (void)backlog; return -1; }
int            kl_sockdef_get_local_addr(KlSocketHandle f, KlSockAddr *a) { (void)f; (void)a; return -1; }
const struct KlDatagramOps *kl_sockdef_dgram(void) { return (const struct KlDatagramOps *)0; }
