/*
 * http_proto_hooks.c: storage for the per-protocol server upgrade seam (http_proto_hooks.h).
 *
 * Freestanding-safe: atomic capability pointers + getters/setters.
 * Linked into BOTH the hosted core and the freestanding server archive. In a
 * freestanding build nothing calls the *_set() installers, so the pointers stay
 * NULL and the shared core runs pure HTTP/1.1.
 */

#include "http_proto_hooks.h"
#include <stdatomic.h>

/* Install-once guard (makes the "install-once global registration" invariant executable).
 * The hook tables are process-wide compiled-in capability registrations, not per-server
 * config: installed by a load-time constructor and/or kl_http_server_init, always with the
 * SAME canonical static table. Accept: the first install, an idempotent re-install of the
 * identical table, or a reset to NULL. Silently keep the first table if a DIFFERENT
 * non-NULL table is offered (a programming error) rather than allowing live replacement. */
static void hooks_set_once(_Atomic(const void *) *slot, const void *next) {
    const void *cur = atomic_load(slot);
    do {
        if (cur && next) return; /* same-table reinstall or rejected replacement */
    } while (!atomic_compare_exchange_weak(slot, &cur, next));
}

static _Atomic(const void *) g_ws_hooks = NULL;
static _Atomic(const void *) g_h2_hooks = NULL;

const KlWsServerHooks *kl_ws_server_hooks(void) { return atomic_load(&g_ws_hooks); }
void kl_ws_server_hooks_set(const KlWsServerHooks *hooks) {
    hooks_set_once(&g_ws_hooks, hooks);
}

const KlHttp2ServerHooks *kl_http2_server_hooks(void) { return atomic_load(&g_h2_hooks); }
void kl_http2_server_hooks_set(const KlHttp2ServerHooks *hooks) {
    hooks_set_once(&g_h2_hooks, hooks);
}

static _Atomic(const void *) g_ws_comp_hooks = NULL;
static _Atomic(const void *) g_h2_comp_hooks = NULL;

const KlWsCompHooks *kl_ws_comp_hooks(void) { return atomic_load(&g_ws_comp_hooks); }
void kl_ws_comp_hooks_set(const KlWsCompHooks *hooks) {
    hooks_set_once(&g_ws_comp_hooks, hooks);
}

const KlHttp2CompHooks *kl_http2_comp_hooks(void) { return atomic_load(&g_h2_comp_hooks); }
void kl_http2_comp_hooks_set(const KlHttp2CompHooks *hooks) {
    hooks_set_once(&g_h2_comp_hooks, hooks);
}

static _Atomic(const void *) g_proxy_hooks = NULL;

const KlProxyHooks *kl_proxy_hooks(void) { return atomic_load(&g_proxy_hooks); }
void kl_proxy_hooks_set(const KlProxyHooks *hooks) {
    hooks_set_once(&g_proxy_hooks, hooks);
}
