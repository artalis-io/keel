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
#include <keel/proxy_protocol.h> /* KlProxyResult / KlCidr / KlSockAddr (PROXY seam) */
#include <stddef.h>
#include <stdint.h>

/* ── WebSocket server upgrade seam ──────────────────────────────────────────── */
typedef struct KlWsServerHooks {
    /* HTTP/1.1 -> WebSocket upgrade (returns the next KlConnState as int). */
    int  (*upgrade)(KlConn *c, const char *leftover, size_t leftover_len);
    /* Readiness data plane (KL_CONN_WEBSOCKET): drive a read/write-ready frame pump;
     * each returns the next KlConnState as int. The completion path uses .drive
     * (KlWsCompHooks); this is its readiness counterpart so server.c dispatches through
     * the same seam instead of naming kl_ws_server_* directly. */
    int  (*on_readable)(KlConn *c);
    int  (*on_writable)(KlConn *c);
    int  (*drain_pending)(const KlConn *c);           /* readiness: want WRITE interest? */
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
    /* Readiness data plane (KL_CONN_HTTP2): drive a read/write-ready frame pump; each
     * returns the next KlConnState as int (readiness counterpart of KlH2CompHooks.drive). */
    int  (*on_readable)(KlConn *c);
    int  (*on_writable)(KlConn *c);
    int  (*want_write)(const KlConn *c);              /* readiness: arm WRITE interest? */
    void (*cleanup)(KlConn *c);                       /* per-connection teardown */
    void (*drain_shutdown)(KlConn *c);                /* graceful-drain GOAWAY */
} KlH2ServerHooks;

const KlH2ServerHooks *kl_h2_server_hooks(void);      /* NULL if server_h2.c absent */
void kl_h2_server_hooks_set(const KlH2ServerHooks *hooks);
void kl_h2_server_hooks_install(void);                /* defined in server_h2.c */

/* ── Completion-mode drive seam ─────────────────────────────────────────────
 * Once a connection has upgraded, the completion server driver (completion_server.c)
 * pumps its frames through the ws/h2 COMPLETION handlers (completion_ws.c /
 * completion_h2.c). Registered from the COMPLETION axis — not server_ws.c/server_h2.c
 * — because those completion TUs share completion_server.c's build axis: a readiness
 * build compiles none of them (so nothing references the drives), and a freestanding
 * HTTP/1.1 server leaves the tables NULL (no upgrade ever occurs). completion_server.c
 * calls the installers, which also pull the completion TUs out of the static archive. */
struct KlServer;

typedef struct KlWsCompHooks {
    void (*drive)(struct KlServer *s, KlConn *c);
} KlWsCompHooks;
const KlWsCompHooks *kl_ws_comp_hooks(void);
void kl_ws_comp_hooks_set(const KlWsCompHooks *hooks);
void kl_ws_comp_hooks_install(void);                  /* defined in completion_ws.c */

typedef struct KlH2CompHooks {
    void (*drive)(struct KlServer *s, KlConn *c);
} KlH2CompHooks;
const KlH2CompHooks *kl_h2_comp_hooks(void);
void kl_h2_comp_hooks_set(const KlH2CompHooks *hooks);
void kl_h2_comp_hooks_install(void);                  /* defined in completion_h2.c */

/* ── PROXY-protocol seam ────────────────────────────────────────────────────
 * Recovering the real client address behind an L4 load balancer (proxy_protocol.c:
 * strtok_r / inet_pton / ntohs) is optional and hosted-only. The server core parses
 * the PROXY header and CIDR-matches the trusted source through this table; a
 * freestanding firmware server links no proxy_protocol.c, so the hooks are NULL and
 * no PROXY handling occurs. Unlike the completion drives, proxy_protocol.c is always
 * compiled in a hosted build, so kl_server_init installs this with no build-axis #ifdef. */
typedef struct KlProxyHooks {
    KlProxyResult (*parse)(const uint8_t *buf, size_t len, size_t *consumed,
                           KlSockAddr *peer);
    int (*cidr_match)(const KlCidr *list, int count, const KlSockAddr *sa);
} KlProxyHooks;
const KlProxyHooks *kl_proxy_hooks(void);
void kl_proxy_hooks_set(const KlProxyHooks *hooks);
void kl_proxy_hooks_install(void);                    /* defined in proxy_protocol.c */

#endif /* KEEL_PROTO_HOOKS_H */
