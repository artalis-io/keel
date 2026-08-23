/*
 * watcher_internal.h — INTERNAL. Completion-connect watcher helpers.
 *
 * The async KlHttpClient drives connect over the completion axis (kl_comp_post_connect) when the
 * loop advertises KL_EVENT_CAP_COMPLETION. Unlike the readiness path — which arms a
 * KL_EVENT_WRITE KlWatcher on the connecting fd — the completion path must NOT register a
 * backend readiness watch on the connecting fd (a completion backend would either poll it in
 * parallel with the connect op → a double fire, or, on IOCP, turn a WRITE watch into a
 * readable-only WSARecv that never fires). Instead it creates a *detached* KlWatcher: a
 * ctx-owned node (so the tagged pointer resolves in kl_event_dispatch when the connect
 * completion is relayed) with NO kl_event_add. kl_comp_post_connect then carries the tagged
 * pointer and the backend fires the connect completion against it.
 *
 * Once connected, the client switches to the send/recv phase via the ordinary kl_watcher_mod
 * (which registers the real readiness relay on the completion backend), so the detached node
 * seamlessly becomes a normal watcher. Defined in async.c alongside the rest of the watcher
 * API. See docs/archive/phases/phase10_lwip_raw_client_design.md §3/§4 and the async client
 * (src/protocols/http/http_client_async.c).
 */
#ifndef KEEL_SRC_WATCHER_INTERNAL_H
#define KEEL_SRC_WATCHER_INTERNAL_H

#include <keel/event_ctx.h>

/* Create a ctx-owned KlWatcher for `fd` WITHOUT registering it with the event loop, and
 * return its tagged pointer (LSB=1) — the value the completion driver hands back so
 * kl_event_dispatch resolves and calls on_ready. Idempotent per fd (updates an existing
 * node). Returns NULL on allocation failure. Used only for the completion connect phase. */
void *kl_watcher_add_detached(KlEventCtx *ctx, KlSocketHandle fd,
                              KlWatcherFn on_ready, void *user_data);

#endif /* KEEL_SRC_WATCHER_INTERNAL_H */
