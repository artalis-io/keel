/*
 * http_proto_hooks.c — storage for the per-protocol server upgrade seam (http_proto_hooks.h).
 *
 * Freestanding-safe: two file-scope pointers + getters/setters, nothing else.
 * Linked into BOTH the hosted core and the freestanding server archive. In a
 * freestanding build nothing calls the *_set() installers, so the pointers stay
 * NULL and the shared core runs pure HTTP/1.1.
 */

#include "http_proto_hooks.h"

/* Install-once guard (makes the "install-once global registration" invariant executable).
 * The hook tables are process-wide compiled-in capability registrations, not per-server
 * config — installed by a load-time constructor and/or kl_http_server_init, always with the
 * SAME canonical static table. Accept: the first install, an idempotent re-install of the
 * identical table, or a reset to NULL. Silently keep the first table if a DIFFERENT
 * non-NULL table is offered (a programming error) rather than allowing live replacement. */
static const void *hooks_set_once(const void *cur, const void *next) {
    return (cur && next && cur != next) ? cur : next;
}

static const KlWsServerHooks *g_ws_hooks = NULL;
static const KlHttp2ServerHooks *g_h2_hooks = NULL;

const KlWsServerHooks *kl_ws_server_hooks(void) { return g_ws_hooks; }
void kl_ws_server_hooks_set(const KlWsServerHooks *hooks) {
    g_ws_hooks = (const KlWsServerHooks *)hooks_set_once(g_ws_hooks, hooks);
}

const KlHttp2ServerHooks *kl_http2_server_hooks(void) { return g_h2_hooks; }
void kl_http2_server_hooks_set(const KlHttp2ServerHooks *hooks) {
    g_h2_hooks = (const KlHttp2ServerHooks *)hooks_set_once(g_h2_hooks, hooks);
}

static const KlWsCompHooks *g_ws_comp_hooks = NULL;
static const KlHttp2CompHooks *g_h2_comp_hooks = NULL;

const KlWsCompHooks *kl_ws_comp_hooks(void) { return g_ws_comp_hooks; }
void kl_ws_comp_hooks_set(const KlWsCompHooks *hooks) {
    g_ws_comp_hooks = (const KlWsCompHooks *)hooks_set_once(g_ws_comp_hooks, hooks);
}

const KlHttp2CompHooks *kl_http2_comp_hooks(void) { return g_h2_comp_hooks; }
void kl_http2_comp_hooks_set(const KlHttp2CompHooks *hooks) {
    g_h2_comp_hooks = (const KlHttp2CompHooks *)hooks_set_once(g_h2_comp_hooks, hooks);
}

static const KlProxyHooks *g_proxy_hooks = NULL;

const KlProxyHooks *kl_proxy_hooks(void) { return g_proxy_hooks; }
void kl_proxy_hooks_set(const KlProxyHooks *hooks) {
    g_proxy_hooks = (const KlProxyHooks *)hooks_set_once(g_proxy_hooks, hooks);
}
