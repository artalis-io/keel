#ifndef KEEL_PROTO_HOOKS_H
#define KEEL_PROTO_HOOKS_H

/*
 * proto_hooks.h — the per-protocol server upgrade seam.
 *
 * The shared HTTP/1.1 server core (connection.c, server_core.c, and the sweep/drain
 * in server.c) dispatches into the WebSocket and HTTP-2 server modules on upgrade,
 * cleanup, and the drain/idle sweeps. Those modules (server_ws.c / server_h2.c) are
 * OPTIONAL — a freestanding HTTP/1.1 server (UEFI, docs/phase10_uefi_server_design.md
 * §6) links neither. So the core never names kl_ws_server_* / kl_h2_server_* directly;
 * it goes through a SEPARATE hook table PER PROTOCOL, registered by that module.
 *
 * One table per protocol (not a merged blob) so each protocol is independently
 * linkable and named for what it is — a third protocol (HTTP/3 over QUIC) adds a
 * KlH3ServerHooks + kl_h3_server_hooks() the same way, touching no existing table.
 *
 * A hosted build calls the installers (kl_ws_server_hooks_install / _h2_) from
 * kl_server_init — an explicit reference that both registers the table AND pulls the
 * protocol object out of the static archive (a self-registering constructor alone
 * would be dropped by the linker now that the core no longer names its symbols). A
 * freestanding build never calls them, so kl_ws_server_hooks()/kl_h2_server_hooks()
 * return NULL and the core stays pure HTTP/1.1 — with NO #ifdef in the shared code.
 */

#include <keel/connection.h>   /* KlConn */
#include <keel/router.h>       /* KlRouter */
#include <keel/h2_server.h>    /* KlH2ServerConfig */
#include <stddef.h>
#include <stdint.h>

/* ── WebSocket server upgrade seam ──────────────────────────────────────────── */
typedef struct KlWsServerHooks {
    /* HTTP/1.1 -> WebSocket upgrade (returns the next KlConnState as int). */
    int  (*upgrade)(KlConn *c, const char *leftover, size_t leftover_len);
    void (*cleanup)(KlConn *c);                       /* per-connection teardown */
    int  (*auto_ping)(KlConn *c, uint64_t now);       /* idle-sweep keepalive */
    int  (*check_close_timeout)(const KlConn *c, uint64_t now);
    void (*drain_close)(KlConn *c);                   /* graceful-drain nudge */
} KlWsServerHooks;

const KlWsServerHooks *kl_ws_server_hooks(void);      /* NULL if server_ws.c absent */
void kl_ws_server_hooks_set(const KlWsServerHooks *hooks);
void kl_ws_server_hooks_install(void);                /* defined in server_ws.c */

/* ── HTTP/2 server upgrade seam ─────────────────────────────────────────────── */
typedef struct KlH2ServerHooks {
    /* ALPN / prior-knowledge upgrade, and the h2c (HTTP/1.1 Upgrade) path. */
    int  (*upgrade)(KlConn *c, KlRouter *router, KlH2ServerConfig *cfg,
                    const char *data, size_t len);
    int  (*upgrade_from_h1)(KlConn *c, KlRouter *router, KlH2ServerConfig *cfg,
                            const char *leftover, size_t leftover_len);
    void (*cleanup)(KlConn *c);                       /* per-connection teardown */
    void (*drain_shutdown)(KlConn *c);                /* graceful-drain GOAWAY */
} KlH2ServerHooks;

const KlH2ServerHooks *kl_h2_server_hooks(void);      /* NULL if server_h2.c absent */
void kl_h2_server_hooks_set(const KlH2ServerHooks *hooks);
void kl_h2_server_hooks_install(void);                /* defined in server_h2.c */

#endif /* KEEL_PROTO_HOOKS_H */
