#ifndef KEEL_HTTP_PROTO_HOOKS_H
#define KEEL_HTTP_PROTO_HOOKS_H

/*
 * http_proto_hooks.h — the per-protocol server upgrade seam.
 *
 * The shared HTTP/1.1 server core (http_connection.c, http_server_core.c, and the sweep/drain
 * in http_server.c) dispatches into the WebSocket and HTTP-2 server modules on upgrade,
 * cleanup, and the drain/idle sweeps. Those modules (http_server_ws.c / http2_server.c) are
 * OPTIONAL — a freestanding HTTP/1.1 server (UEFI, docs/archive/phases/phase10_uefi_server_design.md
 * §6) links neither. So the core never names kl_ws_server_* / kl_http2_server_* directly;
 * it goes through a SEPARATE hook table PER PROTOCOL, registered by that module.
 *
 * One table per protocol (not a merged blob) so each protocol is independently
 * linkable and named for what it is — a third protocol (HTTP/3 over QUIC) adds a
 * KlH3ServerHooks + kl_h3_server_hooks() the same way, touching no existing table.
 *
 * A hosted build calls the installers (kl_ws_server_hooks_install / _h2_) from
 * kl_http_server_init — an explicit reference that both registers the table AND pulls the
 * protocol object out of the static archive (a self-registering constructor alone
 * would be dropped by the linker now that the core no longer names its symbols). A
 * freestanding build never calls them, so kl_ws_server_hooks()/kl_http2_server_hooks()
 * return NULL and the core stays pure HTTP/1.1 — with NO #ifdef in the shared code.
 */

#include <keel/http_connection.h>   /* KlHttpConn */
#include <keel/http_router.h>       /* KlHttpRouter */
#include <keel/http2_server.h>    /* KlHttp2ServerConfig */
#include <keel/proxy_protocol.h> /* KlProxyResult / KlCidr / KlSockAddr (PROXY seam) */
#include <stddef.h>
#include <stdint.h>

/* ── WebSocket server upgrade seam ──────────────────────────────────────────── */
typedef struct KlWsServerHooks {
    /* HTTP/1.1 -> WebSocket upgrade (returns the next KlHttpConnState as int). */
    int  (*upgrade)(KlHttpConn *c, const char *leftover, size_t leftover_len);
    /* Readiness data plane (KL_HTTP_CONN_WEBSOCKET): drive a read/write-ready frame pump;
     * each returns the next KlHttpConnState as int. The completion path uses .drive
     * (KlWsCompHooks); this is its readiness counterpart so http_server.c dispatches through
     * the same seam instead of naming kl_ws_server_* directly. */
    int  (*on_readable)(KlHttpConn *c);
    int  (*on_writable)(KlHttpConn *c);
    int  (*drain_pending)(const KlHttpConn *c);           /* readiness: want WRITE interest? */
    void (*cleanup)(KlHttpConn *c);                       /* per-connection teardown */
    int  (*auto_ping)(KlHttpConn *c, uint64_t now);       /* idle-sweep keepalive */
    int  (*check_close_timeout)(const KlHttpConn *c, uint64_t now);
    void (*drain_close)(KlHttpConn *c);                   /* graceful-drain nudge */
} KlWsServerHooks;

const KlWsServerHooks *kl_ws_server_hooks(void);      /* NULL if http_server_ws.c absent */
void kl_ws_server_hooks_set(const KlWsServerHooks *hooks);
void kl_ws_server_hooks_install(void);                /* defined in http_server_ws.c */

/* ── HTTP/2 server upgrade seam ─────────────────────────────────────────────── */
typedef struct KlHttp2ServerHooks {
    /* ALPN / prior-knowledge upgrade, and the h2c (HTTP/1.1 Upgrade) path. */
    int  (*upgrade)(KlHttpConn *c, KlHttpRouter *router, KlHttp2ServerConfig *cfg,
                    const char *data, size_t len);
    int  (*upgrade_from_h1)(KlHttpConn *c, KlHttpRouter *router, KlHttp2ServerConfig *cfg,
                            const char *leftover, size_t leftover_len);
    /* Readiness data plane (KL_HTTP_CONN_HTTP2): drive a read/write-ready frame pump; each
     * returns the next KlHttpConnState as int (readiness counterpart of KlHttp2CompHooks.drive). */
    int  (*on_readable)(KlHttpConn *c);
    int  (*on_writable)(KlHttpConn *c);
    int  (*want_write)(const KlHttpConn *c);              /* readiness: arm WRITE interest? */
    void (*cleanup)(KlHttpConn *c);                       /* per-connection teardown */
    void (*drain_shutdown)(KlHttpConn *c);                /* graceful-drain GOAWAY */
} KlHttp2ServerHooks;

const KlHttp2ServerHooks *kl_http2_server_hooks(void);      /* NULL if http2_server.c absent */
void kl_http2_server_hooks_set(const KlHttp2ServerHooks *hooks);
void kl_http2_server_hooks_install(void);                /* defined in http2_server.c */

/* ── Completion-mode drive seam ─────────────────────────────────────────────
 * Once a connection has upgraded, the completion server driver (completion_http_server.c)
 * pumps its frames through the ws/h2 COMPLETION handlers (completion_ws.c /
 * completion_http2.c). Registered from the COMPLETION axis — not http_server_ws.c/http2_server.c
 * — because those completion TUs share completion_http_server.c's build axis: a readiness
 * build compiles none of them (so nothing references the drives), and a freestanding
 * HTTP/1.1 server leaves the tables NULL (no upgrade ever occurs). completion_http_server.c
 * calls the installers, which also pull the completion TUs out of the static archive. */
struct KlHttpServer;

typedef struct KlWsCompHooks {
    void (*drive)(struct KlHttpServer *s, KlHttpConn *c);
} KlWsCompHooks;
const KlWsCompHooks *kl_ws_comp_hooks(void);
void kl_ws_comp_hooks_set(const KlWsCompHooks *hooks);
void kl_ws_comp_hooks_install(void);                  /* defined in completion_ws.c */

typedef struct KlHttp2CompHooks {
    void (*drive)(struct KlHttpServer *s, KlHttpConn *c);
} KlHttp2CompHooks;
const KlHttp2CompHooks *kl_http2_comp_hooks(void);
void kl_http2_comp_hooks_set(const KlHttp2CompHooks *hooks);
void kl_http2_comp_hooks_install(void);                  /* defined in completion_http2.c */

/* ── PROXY-protocol seam ────────────────────────────────────────────────────
 * Recovering the real client address behind an L4 load balancer (proxy_protocol.c:
 * strtok_r / inet_pton / ntohs) is optional and hosted-only. The server core parses
 * the PROXY header and CIDR-matches the trusted source through this table; a
 * freestanding firmware server links no proxy_protocol.c, so the hooks are NULL and
 * no PROXY handling occurs. Unlike the completion drives, proxy_protocol.c is always
 * compiled in a hosted build, so kl_http_server_init installs this with no build-axis #ifdef. */
typedef struct KlProxyHooks {
    KlProxyResult (*parse)(const uint8_t *buf, size_t len, size_t *consumed,
                           KlSockAddr *peer);
    int (*cidr_match)(const KlCidr *list, int count, const KlSockAddr *sa);
} KlProxyHooks;
const KlProxyHooks *kl_proxy_hooks(void);
void kl_proxy_hooks_set(const KlProxyHooks *hooks);
void kl_proxy_hooks_install(void);                    /* defined in proxy_protocol.c */

#endif /* KEEL_HTTP_PROTO_HOOKS_H */
