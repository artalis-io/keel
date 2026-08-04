/*
 * event_pollcomp_internal.h — the pollcomp completion backend's dual-role seam (RC-2).
 *
 * event_pollcomp.c is a PURE runtime provider: static KlEventOps + KlCompletionOps +
 * a KlEventProvider factory (kl_event_provider_pollcomp), with NO kl_event_*_builtin /
 * kl_comp_ops_builtin. That lets it be compiled as an extra object and injected into a
 * DEFAULT (epoll/kqueue) libkeel at runtime WITHOUT clashing with the default backend's
 * own _builtin symbols or the readiness build's kl_comp_ops_builtin stub.
 *
 * To ALSO serve as the compiled-in BACKEND=pollcomp default it must still expose the
 * kl_event_*_builtin free functions + kl_comp_ops_builtin. Those live in a SEPARATE glue
 * TU (event_pollcomp_builtin.c), linked ONLY when BACKEND=pollcomp. The glue reaches the
 * provider's ops through the prefixed, non-static symbols declared here — so there is one
 * copy of the mechanics and the two roles never both define _builtin.
 *
 * Internal — not part of the public API. See event_lwip.c (the pure-provider template)
 * and completion.h / event_dispatch.c (the RC-1 dispatch mechanism this rides on).
 */
#ifndef KEEL_SRC_EVENT_POLLCOMP_INTERNAL_H
#define KEEL_SRC_EVENT_POLLCOMP_INTERNAL_H

#include <keel/event.h>
#include "completion.h"   /* KlCompletionOps */

/* The provider's KlEventLoop lifecycle ops (bodies in event_pollcomp.c). Named, not
 * static, so the compiled-in glue (event_pollcomp_builtin.c) can wrap them as the
 * kl_event_*_builtin the default-path dispatch expects. */
int      kl_pollcomp_ev_init(KlEventLoop *loop);
int      kl_pollcomp_ev_add(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata);
int      kl_pollcomp_ev_mod(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata);
int      kl_pollcomp_ev_del(KlEventLoop *loop, KlSocketHandle fd);
int      kl_pollcomp_ev_wait(KlEventLoop *loop, KlEvent *out, int max, int timeout_ms);
void     kl_pollcomp_ev_close(KlEventLoop *loop);
unsigned kl_pollcomp_ev_caps(const KlEventLoop *loop);
const struct KlSocketProvider *kl_pollcomp_ev_native_provider(const KlEventLoop *loop);

/* The pollcomp completion sub-vtable (poll-driven post/drain mechanics). Hung off the
 * provider's KlEventOps.completion (runtime path) and returned by the glue's
 * kl_comp_ops_builtin (compiled-in path). */
extern const KlCompletionOps kl_pollcomp_completion_ops;

/* The runtime provider factory — hand to KlConfig.event_provider / KlEventCtx to inject
 * pollcomp's completion axis into an otherwise-default libkeel. Internal/test-facing
 * (pollcomp is the completion test double), not the public keel/event.h surface. */
const KlEventProvider *kl_event_provider_pollcomp(void);

#endif /* KEEL_SRC_EVENT_POLLCOMP_INTERNAL_H */
