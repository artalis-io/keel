/*
 * raw_caps_test.c — Stage C fail-early capability contract for the lwIP-raw completion backend.
 *
 * lwip-raw is a SERVER-ONLY, IPv4-only, TCP completion backend. This test asserts that the
 * operations it does NOT support fail EARLY and CLEARLY at the provider seam, so a caller never
 * silently hangs on an unsupported op:
 *
 *   K1  connect()  -> -1 (server-only; no outbound client), errno = ENOTSUP/EOPNOTSUPP.
 *   K2  bind() with a non-IPv4 (IPv6) address -> -1 (the loopif is IPv4; IPv6 unsupported).
 *   K3  bind() with an IPv4 address -> 0 (the supported family still works — a control case).
 *   K4  UDP is SUPPORTED (LC-3a): the raw socket provider now exposes datagram ops
 *         (.dgram != NULL), so kl_udp_init() on a ctx wired to this provider SUCCEEDS — KlUdp runs
 *         over the raw completion loop. (This FLIPPED at LC-3a; previously UDP was unsupported.)
 *
 * (The second-simultaneous-ctx rejection + sequential-ctx cases are covered in raw_recv_test.c;
 * they are not duplicated here.)
 *
 * These are pure provider-seam assertions — no server, no lwIP mainloop thread. K4 wires a
 * KlEventCtx's sockets to the raw provider (the same auto-wire a KlServer does via the loop's
 * native_provider()) and confirms kl_udp_init accepts it.
 *
 * SPDX-License-Identifier: MIT
 */
#include <keel/keel.h>
#include <keel/event_ctx.h>
#include <keel/allocator.h>
#include <keel/socket.h>
#include <keel/sockaddr.h>
#include <keel/udp.h>

#include "keel_lwip_raw.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static int fail(const char *msg) { printf("RAW-CAPS FAIL: %s\n", msg); return 1; }

/* K1 — connect() is unsupported (server-only): must return -1 and set errno = ENOTSUP. */
static int test_connect_rejected(void) {
    const KlSocketProvider *p = kl_socket_provider_lwip_raw();
    if (!p || !p->ops || !p->ops->connect) return fail("no connect op on raw provider");

    KlSockAddr peer;
    static const uint8_t lo[4] = { 127, 0, 0, 1 };
    if (kl_sockaddr_from_ipv4(&peer, lo, 8080) != 0) return fail("build peer addr");

    errno = 0;
    /* A dummy non-null handle; connect must reject regardless of the handle. */
    int rc = p->ops->connect(p->context, (KlSocketHandle)1, &peer);
    if (rc != -1) return fail("connect() did not return -1 (server-only expected)");
#ifdef ENOTSUP
    if (errno != ENOTSUP && errno != EOPNOTSUPP)
#else
    if (errno != EOPNOTSUPP)
#endif
        return fail("connect() did not set errno = ENOTSUP/EOPNOTSUPP");
    printf("PASS K1 (connect rejected: -1 / ENOTSUP)\n");
    return 0;
}

/* K2/K3 — bind() rejects a non-IPv4 (IPv6) address but accepts IPv4. */
static int test_bind_family(void) {
    const KlSocketProvider *p = kl_socket_provider_lwip_raw();
    if (!p || !p->ops || !p->ops->socket || !p->ops->bind || !p->ops->close)
        return fail("no socket/bind/close op on raw provider");

    /* K2: IPv6 bind must fail. Try it without allocating a real pcb (bind checks family first). */
    KlSockAddr v6;
    static const uint8_t loopback6[16] = { 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,1 };  /* ::1 */
    if (kl_sockaddr_from_ipv6(&v6, loopback6, 8080, 0) != 0) return fail("build IPv6 addr");
    if (p->ops->bind(p->context, (KlSocketHandle)1, &v6) != -1)
        return fail("IPv6 bind() did not return -1 (IPv6 unsupported)");
    printf("PASS K2 (IPv6 bind rejected: -1)\n");

    /* K3 (control): IPv4 bind on a real raw pcb must succeed. */
    KlSocketHandle fd = p->ops->socket(p->context, 0, 0, 0);
    if (!kl_handle_valid(fd)) return fail("raw tcp_new (socket) failed");
    KlSockAddr v4;
    static const uint8_t any4[4] = { 0, 0, 0, 0 };
    if (kl_sockaddr_from_ipv4(&v4, any4, 0) != 0) { p->ops->close(p->context, fd); return fail("build IPv4 addr"); }
    int rc = p->ops->bind(p->context, fd, &v4);
    p->ops->close(p->context, fd);
    if (rc != 0) return fail("IPv4 bind() failed (supported family must work)");
    printf("PASS K3 (IPv4 bind accepted)\n");
    return 0;
}

/* K4 — UDP is SUPPORTED (LC-3a): the raw provider exposes datagram ops, so kl_udp_init succeeds.
 * `ctx` is a LIVE raw ctx (lwIP already up); we wire its sockets to the raw provider exactly as a
 * KlServer does via the loop's native_provider(). kl_udp_init now creates a udp_pcb through the
 * provider's datagram data-plane (udp_dg() non-NULL). */
static int test_udp_supported(KlEventCtx *ctx) {
    const KlSocketProvider *p = kl_socket_provider_lwip_raw();
    if (!p) return fail("no raw provider");

    /* The support is structural: the provider now advertises a datagram data-plane. */
    if (p->dgram == NULL)
        return fail("raw provider missing datagram ops (.dgram == NULL — LC-3a should expose them)");
    if (!(p->capabilities & KL_SOCK_CAP_DATAGRAM))
        return fail("raw provider missing KL_SOCK_CAP_DATAGRAM");
    printf("PASS K4a (raw provider .dgram != NULL + KL_SOCK_CAP_DATAGRAM — datagram data-plane)\n");

    ctx->sockets = kl_socket_provider_lwip_raw();
    KlUdp udp;
    KlUdpConfig ucfg;
    memset(&ucfg, 0, sizeof(ucfg));
    ucfg.ctx = ctx;
    int rc = kl_udp_init(&udp, &ucfg);
    if (rc != 0)
        return fail("kl_udp_init failed on the raw provider (UDP must be supported at LC-3a)");
    kl_udp_free(&udp);
    printf("PASS K4b (kl_udp_init accepted on the raw provider)\n");
    return 0;
}

int main(void) {
    /* K1/K2 (connect/bind family) are pure family checks — they never touch the lwIP core. But
     * K3's control case (an IPv4 bind on a real raw pcb) needs lwip_init() to have run, and the
     * K4 udp_init runs against a live ctx. So bring up ONE raw ctx (which lwip_init()s the core +
     * loopif) for the whole test, then tear it down. This also keeps the single-active-ctx
     * invariant intact (no nested ctx creation). */
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    if (kl_event_ctx_init_ex(&ctx, &alloc, kl_event_provider_lwip_raw()) != 0) {
        printf("RAW-CAPS FAIL: ctx init (bring up lwIP core)\n");
        return 1;
    }

    int rc = 0;
    if (rc == 0) rc = test_connect_rejected();
    if (rc == 0) rc = test_bind_family();
    if (rc == 0) rc = test_udp_supported(&ctx);

    kl_event_ctx_free(&ctx);
    if (rc != 0) return 1;
    printf("RAW-CAPS PASS\n");
    return 0;
}
