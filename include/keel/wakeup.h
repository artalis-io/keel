#ifndef KEEL_WAKEUP_H
#define KEEL_WAKEUP_H

#include <keel/handle.h>
#ifdef __cplusplus
extern "C" {
#endif


/**
 * wakeup.h: cross-thread event-loop wakeup channel.
 *
 * A pair of connected handles whose read end can be registered as a KlWatcher:
 * any thread writes a byte to the write end, and the event loop wakes and runs
 * the watcher callback on the loop thread. This is the signal half of the async
 * pattern, the piece that lets a background thread reach kl_async_complete()
 * safely (that call is loop-thread-only).
 *
 * A raw pipe(2) does NOT work everywhere: WSAPoll and IOCP can only watch
 * sockets, never pipe HANDLEs, so a POSIX-only pipe leaves the watcher silently
 * dead on Windows. This channel is a pipe where a pipe is watchable and a
 * connected loopback socket pair where it is not, so one piece of caller code
 * runs on every supported platform and event backend.
 *
 * KlThreadPool builds on this internally; use it directly when the completion
 * signal comes from somewhere the pool does not own (a callback from a
 * third-party library's own thread, a hardware or IPC event, a condition
 * variable a foreign thread signals).
 */
typedef struct {
    KlSocketHandle rd;   /**< Read end: register this with kl_watcher_add. */
    KlSocketHandle wr;   /**< Write end: signal this from any thread. */
} KlWakeup;

/**
 * @brief Open a wakeup channel.
 *
 * The read end is set non-blocking, so draining it from a watcher callback
 * never stalls the event loop.
 *
 * @param w Channel to fill in (caller-owned storage).
 * @return 0 on success; -1 on failure, with both ends set to KL_INVALID_SOCKET.
 */
int kl_wakeup_open(KlWakeup *w);

/**
 * @brief Signal the channel: wake the event loop.
 *
 * Safe to call from any thread, and the only part of this API that is. Writes
 * are coalescing, so a signal lost to a full channel only costs a wakeup that a
 * concurrent signal already delivered; failures are deliberately not reported.
 *
 * Signalling is all a foreign thread may do. The work it is reporting must be
 * handed to the loop thread through memory the watcher callback reads, and any
 * KEEL call it implies (kl_async_complete in particular) belongs in that
 * callback, not in the signalling thread.
 *
 * @param w Open channel.
 */
void kl_wakeup_signal(const KlWakeup *w);

/**
 * @brief Consume pending signal bytes. Event-loop thread only.
 *
 * Call this first thing in the watcher callback: a readiness backend reports
 * the read end ready for as long as a byte sits unread, so an undrained channel
 * spins the loop.
 *
 * @param w Open channel.
 */
void kl_wakeup_drain(const KlWakeup *w);

/**
 * @brief Close both ends and reset them to KL_INVALID_SOCKET.
 *
 * Remove the watcher (kl_watcher_del) before closing: the loop must not hold a
 * registration for a closed handle.
 *
 * @param w Channel to close. Closing an already-closed channel is a no-op.
 */
void kl_wakeup_close(KlWakeup *w);

#ifdef __cplusplus
}
#endif

#endif /* KEEL_WAKEUP_H */
