/*
 * event_pollcomp_builtin.c — the compiled-in-backend glue for pollcomp.
 *
 * Linked ONLY when BACKEND=pollcomp (the Makefile appends it to EVENT_SRC). It adapts
 * the pure-runtime provider in event_pollcomp.c to the compiled-in-default contract:
 *   - the kl_event_*_builtin free functions event_dispatch.c calls on the default path
 *     (loop->ops == NULL), and
 *   - kl_comp_ops_builtin() the completion dispatch (completion_dispatch.c) falls back to.
 * Both just forward to the provider's named ops (kl_pollcomp_ev_* / kl_pollcomp_completion_ops).
 *
 * Keeping these here — NOT in event_pollcomp.c — is what lets event_pollcomp.o be injected
 * into a DEFAULT (epoll/kqueue) libkeel at runtime: that lib already defines its own
 * kl_event_*_builtin (its readiness backend) + kl_comp_ops_builtin (the readiness stub), so
 * a second copy in event_pollcomp.o would be a duplicate-symbol link error. In this separate TU,
 * event_pollcomp.o carries ONLY the runtime provider and links cleanly alongside.
 * COMPLETION_BACKEND=1 (set for BACKEND=pollcomp) suppresses the readiness stub so this
 * kl_comp_ops_builtin is the one that wins. See event_pollcomp_internal.h / event_builtin.h.
 */
#include "event_builtin.h"
#include "event_pollcomp_internal.h"

/* ── kl_event_*_builtin: the default-path event ops (forward to the provider ops) ── */

int kl_event_init_builtin(KlEventLoop *loop) { return kl_pollcomp_ev_init(loop); }

int kl_event_add_builtin(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata) {
    return kl_pollcomp_ev_add(loop, fd, mask, udata);
}

int kl_event_mod_builtin(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata) {
    return kl_pollcomp_ev_mod(loop, fd, mask, udata);
}

int kl_event_del_builtin(KlEventLoop *loop, KlSocketHandle fd) {
    return kl_pollcomp_ev_del(loop, fd);
}

int kl_event_wait_builtin(KlEventLoop *loop, KlEvent *out, int max, int timeout_ms) {
    return kl_pollcomp_ev_wait(loop, out, max, timeout_ms);
}

void kl_event_close_builtin(KlEventLoop *loop) { kl_pollcomp_ev_close(loop); }

unsigned kl_event_caps_builtin(const KlEventLoop *loop) { return kl_pollcomp_ev_caps(loop); }

const struct KlSocketProvider *kl_event_native_provider_builtin(const KlEventLoop *loop) {
    return kl_pollcomp_ev_native_provider(loop);
}

/* ── kl_comp_ops_builtin: the compiled-in completion vtable (the same provider table) ── */

const KlCompletionOps *kl_comp_ops_builtin(void) { return &kl_pollcomp_completion_ops; }
