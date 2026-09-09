/*
 * wakeup.c: the public KlWakeup channel.
 *
 * A thin adapter over the platform wakeup seam (platform_wakeup_posix.c /
 * platform_wakeup_win.c), which KlThreadPool already uses. The adapter exists so the
 * seam stays a private, overridable platform TU while callers get a supported way to
 * signal the event loop from another thread: an embedder that replaces the seam
 * replaces it for the thread pool and for this API at once.
 */
#include <keel/wakeup.h>
#include "platform.h"

/* KlWakeup and KlPlatWakeup are the same two handles in the same order; the casts keep
 * the public type free of any dependency on the private platform header. */
int kl_wakeup_open(KlWakeup *w) {
    if (!w) return -1;
    KlPlatWakeup p;
    if (kl_plat_wakeup_open(&p) < 0) {
        w->rd = w->wr = KL_INVALID_SOCKET;
        return -1;
    }
    w->rd = p.rd;
    w->wr = p.wr;
    return 0;
}

void kl_wakeup_signal(const KlWakeup *w) {
    if (!w) return;
    KlPlatWakeup p = { w->rd, w->wr };
    kl_plat_wakeup_signal(&p);
}

void kl_wakeup_drain(const KlWakeup *w) {
    if (!w) return;
    kl_plat_wakeup_drain(w->rd);
}

void kl_wakeup_close(KlWakeup *w) {
    if (!w) return;
    KlPlatWakeup p = { w->rd, w->wr };
    kl_plat_wakeup_close(&p);
    w->rd = p.rd;
    w->wr = p.wr;
}
