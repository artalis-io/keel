/*
 * platform_wakeup_posix.c: the POSIX run-loop wakeup channel (self-pipe).
 *
 * Kept in its own TU so a foreign stack whose event
 * backend cannot watch a host pipe (lwIP's lwip_poll, a UEFI SNP loop, …) can
 * OVERRIDE just the wakeup by linking its own kl_plat_wakeup_* object ahead of
 * libkeel.a, without touching the rest of the platform layer. Generic seam; the
 * lwIP override lives in integrations/platform/lwip/platform_wakeup_lwip.c.
 */
#include "platform.h"

#include <fcntl.h>
#include <unistd.h>

int kl_plat_wakeup_open(KlPlatWakeup *w)
{
    w->rd = w->wr = KL_INVALID_SOCKET;

    int fds[2];
    if (pipe(fds) < 0)
        return -1;

    /* Both ends close-on-exec: this pipe is Keel's own, and an embedder that spawns children must
     * not hand it to them. pipe2(O_CLOEXEC) is not portable (macOS lacks it), so set the flag right
     * after creation, as the socket provider does for sockets. */
    for (int i = 0; i < 2; i++) {
        int fdf = fcntl(fds[i], F_GETFD, 0);
        if (fdf >= 0) (void)fcntl(fds[i], F_SETFD, fdf | FD_CLOEXEC);
    }

    /* Both ends non-blocking. The read end so the drain never stalls the event loop; the write end so
     * a signal never blocks its caller on a full pipe (a worker, or the loop thread itself when it
     * signals its own channel). A full pipe already holds a pending wakeup, so the byte that would
     * not fit is not needed (wakeup.h: signals coalesce). */
    for (int i = 0; i < 2; i++) {
        int flags = fcntl(fds[i], F_GETFL, 0);
        if (flags >= 0)
            (void)fcntl(fds[i], F_SETFL, flags | O_NONBLOCK);
    }

    w->rd = fds[0];
    w->wr = fds[1];
    return 0;
}

void kl_plat_wakeup_signal(const KlPlatWakeup *w)
{
    char c = 1;
    kl_ssize_t wr = write((int)w->wr, &c, 1);   /* EAGAIN = full = a wakeup is already pending */
    (void)wr;
}

void kl_plat_wakeup_drain(KlSocketHandle rd)
{
    /* Empty the pipe: a burst of signals coalesces into one wakeup instead of re-firing the watcher
     * once per read. Stops at the first short read (or EAGAIN); the bound (4 MiB) only keeps a
     * producer signalling as fast as we read from holding the loop here. */
    char buf[4096];
    for (int i = 0; i < 1024; i++) {
        kl_ssize_t rc = read((int)rd, buf, sizeof(buf));
        if (rc < (kl_ssize_t)sizeof(buf)) break;
    }
}

void kl_plat_wakeup_close(KlPlatWakeup *w)
{
    if (kl_handle_valid(w->rd)) close((int)w->rd);
    if (kl_handle_valid(w->wr)) close((int)w->wr);
    w->rd = w->wr = KL_INVALID_SOCKET;
}
