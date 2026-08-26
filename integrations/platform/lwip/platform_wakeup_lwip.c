/*
 * platform_wakeup_lwip.c: the lwIP run-loop wakeup channel.
 *
 * The host self-pipe (platform_wakeup_posix.c) is invisible to lwip_poll, so
 * kl_http_server_stop would only be noticed on the next poll tick. This provides a
 * wakeup that IS an lwIP socket, a self-connected UDP socket on loopback: a
 * send-to-self delivers a datagram that wakes lwip_poll(POLLIN) immediately.
 *
 * Link this object AHEAD of a stock libkeel.a so it overrides the platform_wakeup
 * seam's kl_plat_wakeup_* before the archive's host version is pulled; the same
 * link-time override as socket_lwip.c / resolve_sync_lwip.c. Generic seam lives in
 * src/platform_wakeup_posix.c; only this lwIP-specific impl lives under integrations/.
 */
#define KEEL_PLATFORM_LWIP 1
#include "platform.h"        /* KlPlatWakeup, KlSocketHandle, kl_handle_valid */

#include "lwip/sockets.h"
#include "lwip/inet.h"

#include <string.h>

int kl_plat_wakeup_open(KlPlatWakeup *w)
{
    w->rd = w->wr = KL_INVALID_SOCKET;

    int fd = lwip_socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = lwip_htonl(INADDR_LOOPBACK);
    a.sin_port        = 0;   /* ephemeral */

    socklen_t al = sizeof(a);
    if (lwip_bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0 ||
        lwip_getsockname(fd, (struct sockaddr *)&a, &al) != 0 ||
        lwip_connect(fd, (struct sockaddr *)&a, al) != 0) {   /* connect to self */
        lwip_close(fd);
        return -1;
    }
    (void)lwip_fcntl(fd, F_SETFL, O_NONBLOCK);   /* read end non-blocking */

    w->rd = w->wr = (KlSocketHandle)fd;   /* one self-connected socket */
    return 0;
}

void kl_plat_wakeup_signal(const KlPlatWakeup *w)
{
    char c = 1;
    (void)lwip_send((int)w->wr, &c, 1, 0);   /* datagram to self → POLLIN */
}

void kl_plat_wakeup_drain(KlSocketHandle rd)
{
    char buf[64];
    (void)lwip_recv((int)rd, buf, sizeof(buf), 0);
}

void kl_plat_wakeup_close(KlPlatWakeup *w)
{
    if (kl_handle_valid(w->rd))
        lwip_close((int)w->rd);   /* rd == wr: single socket */
    w->rd = w->wr = KL_INVALID_SOCKET;
}
