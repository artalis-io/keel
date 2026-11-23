/*
 * event_hangup.h: INTERNAL. Shared helpers for KL_EVENT_HANGUP / KL_EVENT_ERROR in the event
 * backends. No ABI commitment.
 *
 * A hang-up registration is one whose interest is KL_EVENT_HANGUP alone (no READ, no WRITE; combined
 * with either it is ignored). Its reports differ from a READ/WRITE registration's (HANGUP/ERROR, never
 * readable data), so a backend must know, at wait, which kind each reported registration is. It
 * decides from the interest it registered, never from the udata: udata is the caller's (kl_event_add
 * is public and takes any cookie). epoll: EPOLLRDHUP is reported only to a registration that asked for
 * it. kqueue: a per-descriptor flag (kevent returns the descriptor). poll: the entry's events.
 * WSAPoll: a per-entry flag (events = 0 is also a paused READ). io_uring / pollcomp: the watch's mask.
 *
 * The report mapping follows the measured behavior recorded in
 * docs/architecture/suspended_hangup_detection.md: an orderly FIN shows as RDHUP (Linux) / HUP
 * (WSAPoll) / EV_EOF without an errno (kqueue); a reset additionally shows ERR, or HUP on Linux
 * (both directions shut, which a suspended connection that has not shut its own side down sees only
 * on a reset), or an errno in kqueue's fflags.
 */
#ifndef KEEL_SRC_EVENT_HANGUP_H
#define KEEL_SRC_EVENT_HANGUP_H

#include <keel/event.h>

/* 1 when this interest asks for a hang-up registration. */
static inline int kl_hup_wanted(KlEventMask mask) {
    return (mask & KL_EVENT_HANGUP) && !(mask & (KL_EVENT_READ | KL_EVENT_WRITE));
}

/* Map a poll-family result for a hang-up registration. `rdhup`: the peer's write-side shutdown was
 * reported (POLLRDHUP / EPOLLRDHUP, or WSAPoll's POLLHUP). `hup_is_failure`: on Linux a HUP means
 * both directions are shut, which for a connection that has not shut its own side down is a reset.
 * `err`: an error was reported. Returns 0 when there is nothing to report (readable data only). */
static inline KlEventMask kl_hup_ready(int rdhup, int hup, int err, int hup_is_failure) {
    KlEventMask r = 0;
    if (rdhup || hup || err) r |= KL_EVENT_HANGUP;
    if (err || (hup && hup_is_failure)) r |= KL_EVENT_ERROR;
    return r;
}

#endif /* KEEL_SRC_EVENT_HANGUP_H */
