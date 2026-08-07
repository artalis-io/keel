/*
 * proto_hooks.c — storage for the per-protocol server upgrade seam (proto_hooks.h).
 *
 * Freestanding-safe: two file-scope pointers + getters/setters, nothing else.
 * Linked into BOTH the hosted core and the freestanding server archive. In a
 * freestanding build nothing calls the *_set() installers, so the pointers stay
 * NULL and the shared core runs pure HTTP/1.1.
 */

#include "proto_hooks.h"

static const KlWsServerHooks *g_ws_hooks = NULL;
static const KlH2ServerHooks *g_h2_hooks = NULL;

const KlWsServerHooks *kl_ws_server_hooks(void) { return g_ws_hooks; }
void kl_ws_server_hooks_set(const KlWsServerHooks *hooks) { g_ws_hooks = hooks; }

const KlH2ServerHooks *kl_h2_server_hooks(void) { return g_h2_hooks; }
void kl_h2_server_hooks_set(const KlH2ServerHooks *hooks) { g_h2_hooks = hooks; }
