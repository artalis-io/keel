/*
 * event_dispatch.c: the public kl_event_* API, dispatched to either the
 * compiled-in backend (default, no indirection) or a runtime-installed provider.
 *
 * When loop->ops is NULL (the common case) each call goes straight to the
 * backend's *_builtin function: a single, perfectly-predicted branch, no vtable
 * hop. When a bring-your-own backend is installed (KlEventCtx.event_provider →
 * loop->ops, e.g. lwIP), the same calls route through loop->ops instead, so the
 * core needs no recompile to run on a foreign event stack. See event.h /
 * event_builtin.h and docs/archive/designs/event_provider_design.md.
 */
#include <keel/event.h>
#include <stddef.h>
#include "event_caps.h"
#include "event_builtin.h"
#include "completion_io.h"   /* kl_completion_axis_available (KEEL_NO_COMPLETION guard) */

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
    /* Reject a malformed provider before touching loop->ops or calling any slot: a
     * table missing a required op would fault at the first dispatch. Direct callers
     * see a <0 return; the ctx wire-up pre-validates to attach KL_ERR_INVALID_ARG. */
    if (!kl_event_ops_required_valid(provider->ops))
        return -1;
    loop->ops = provider->ops;
    int r = loop->ops->init(loop);
    if (r < 0) return r;
    /* Reject a completion provider fail-loud when the completion axis was compiled out
     * (KEEL_NO_COMPLETION): the driver/dispatch that would drive it are absent, so an
     * installed completion loop could only abort() later. Fail at install instead. The
     * axis TU reports its presence, keeping this build knob out of the shared code. */
    if ((loop->ops->caps(loop) & KL_EVENT_CAP_COMPLETION) &&
        !kl_completion_axis_available()) {
        loop->ops->close(loop);
        loop->ops = NULL;
        return -1;
    }
    return r;
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
    /* native_provider is an optional slot: a provider that leaves it NULL offers no
     * native socket provider, so report none (the caller keeps its configured
     * sockets). The builtin path is used only when no provider is installed. */
    if (!loop->ops) return kl_event_native_provider_builtin(loop);
    return loop->ops->native_provider ? loop->ops->native_provider(loop) : NULL;
}
