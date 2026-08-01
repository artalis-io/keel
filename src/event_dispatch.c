/*
 * event_dispatch.c — the public kl_event_* API, dispatched to either the
 * compiled-in backend (default, no indirection) or a runtime-installed provider.
 *
 * When loop->ops is NULL (the common case) each call goes straight to the
 * backend's *_builtin function — a single, perfectly-predicted branch, no vtable
 * hop. When a bring-your-own backend is installed (KlEventCtx.event_provider →
 * loop->ops, e.g. lwIP), the same calls route through loop->ops instead, so the
 * core needs no recompile to run on a foreign event stack. See event.h /
 * event_builtin.h and docs/event_provider_design.md.
 */
#include <keel/event.h>
#include <stddef.h>
#include "event_caps.h"
#include "event_builtin.h"

int kl_event_init(KlEventLoop *loop) {
    /* Default entry: the compiled-in backend. Zero ops first so a stack-allocated
     * loop with indeterminate ops takes the builtin path (and all later kl_event_*
     * dispatch correctly). A runtime provider is installed instead via
     * kl_event_init_provider() (from kl_event_ctx_init when a provider is set). */
    loop->ops = NULL;
    return kl_event_init_builtin(loop);
}

int kl_event_init_provider(KlEventLoop *loop, const KlEventProvider *provider) {
    if (!provider || !provider->ops) return kl_event_init(loop);
    loop->ops = provider->ops;
    return loop->ops->init(loop);
}

int kl_event_add(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata) {
    return loop->ops ? loop->ops->add(loop, fd, mask, udata)
                     : kl_event_add_builtin(loop, fd, mask, udata);
}

int kl_event_mod(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata) {
    return loop->ops ? loop->ops->mod(loop, fd, mask, udata)
                     : kl_event_mod_builtin(loop, fd, mask, udata);
}

int kl_event_del(KlEventLoop *loop, KlSocketHandle fd) {
    return loop->ops ? loop->ops->del(loop, fd) : kl_event_del_builtin(loop, fd);
}

int kl_event_wait(KlEventLoop *loop, KlEvent *out, int max, int timeout_ms) {
    return loop->ops ? loop->ops->wait(loop, out, max, timeout_ms)
                     : kl_event_wait_builtin(loop, out, max, timeout_ms);
}

void kl_event_close(KlEventLoop *loop) {
    if (loop->ops) loop->ops->close(loop);
    else kl_event_close_builtin(loop);
}

unsigned kl_event_caps(const KlEventLoop *loop) {
    return loop->ops ? loop->ops->caps(loop) : kl_event_caps_builtin(loop);
}

const struct KlSocketProvider *kl_event_native_provider(const KlEventLoop *loop) {
    return loop->ops ? loop->ops->native_provider(loop)
                     : kl_event_native_provider_builtin(loop);
}
