/*
 * event_builtin.h: the compiled-in event backend's implementation entry points.
 *
 * Exactly one backend TU (event_epoll.c / event_kqueue.c / event_poll.c /
 * event_wsapoll.c / event_iouring.c / event_iocp.c / event_pollcomp.c) is built
 * in via the Makefile's BACKEND= selection and defines these `*_builtin`
 * functions. The public kl_event_* API (event.h) is implemented once in
 * event_dispatch.c, which calls these directly on the default path (loop->ops ==
 * NULL) or dispatches through loop->ops for a runtime-injected provider (lwIP).
 *
 * Internal: not part of the public API.
 */
#ifndef KEEL_EVENT_BUILTIN_H
#define KEEL_EVENT_BUILTIN_H

#include <keel/event.h>

int  kl_event_init_builtin(KlEventLoop *loop);
int  kl_event_add_builtin(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata);
int  kl_event_mod_builtin(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata);
int  kl_event_del_builtin(KlEventLoop *loop, KlSocketHandle fd);
int  kl_event_wait_builtin(KlEventLoop *loop, KlEvent *out, int max, int timeout_ms);
void kl_event_close_builtin(KlEventLoop *loop);
unsigned kl_event_caps_builtin(const KlEventLoop *loop);
const struct KlSocketProvider *kl_event_native_provider_builtin(const KlEventLoop *loop);

#endif /* KEEL_EVENT_BUILTIN_H */
