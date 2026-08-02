/*
 * platform_wakeup_posix.c — the POSIX run-loop wakeup channel (self-pipe).
 *
 * Split out of platform_posix.c into its own TU so a foreign stack whose event
 * backend cannot watch a host pipe (lwIP's lwip_poll, a UEFI SNP loop, …) can
 * OVERRIDE just the wakeup by linking its own kl_plat_wakeup_* object ahead of
 * libkeel.a — without touching the rest of the platform layer. Generic seam; the
 * lwIP override lives in integrations/lwip/platform_wakeup_lwip.c.
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

    /* Read end non-blocking: the drain must never stall the event loop. */
    int flags = fcntl(fds[0], F_GETFL, 0);
    if (flags >= 0)
        (void)fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);

    w->rd = fds[0];
    w->wr = fds[1];
    return 0;
}

void kl_plat_wakeup_signal(const KlPlatWakeup *w)
{
    char c = 1;
    ssize_t wr = write((int)w->wr, &c, 1);
    (void)wr;
}

void kl_plat_wakeup_drain(KlSocketHandle rd)
{
    /* Single non-blocking read is enough: the done queue is drained under the
     * mutex regardless of how many bytes we consume here, and any residue
     * re-fires the level-triggered watcher harmlessly. */
    char buf[64];
    ssize_t rc = read((int)rd, buf, sizeof(buf));
    (void)rc;
}

void kl_plat_wakeup_close(KlPlatWakeup *w)
{
    if (kl_handle_valid(w->rd)) close((int)w->rd);
    if (kl_handle_valid(w->wr)) close((int)w->wr);
    w->rd = w->wr = KL_INVALID_SOCKET;
}
