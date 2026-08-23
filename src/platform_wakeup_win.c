/*
 * platform_wakeup_win.c — the Windows run-loop wakeup channel.
 *
 * A separate TU (mirrors platform_wakeup_posix.c)
 * so the wakeup is an independently-overridable seam. Windows has no pipe(2) that
 * WSAPoll can watch, so the channel is a connected loopback TCP pair.
 */
#include "platform.h"

#include "sockcompat.h"   /* winsock2.h before windows.h */
#include <windows.h>
#include <string.h>

/* Winsock must already be started (the socket provider's WSAStartup, ref-counted). */
static int win_wakeup_pair(SOCKET sv[2])
{
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    SOCKET client   = INVALID_SOCKET;
    SOCKET server   = INVALID_SOCKET;
    if (listener == INVALID_SOCKET)
        return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;   /* ephemeral */

    int addrlen = (int)sizeof(addr);
    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0) goto fail;
    if (listen(listener, 1) != 0) goto fail;
    if (getsockname(listener, (struct sockaddr *)&addr, &addrlen) != 0) goto fail;

    client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client == INVALID_SOCKET) goto fail;
    if (connect(client, (struct sockaddr *)&addr, addrlen) != 0) goto fail;

    server = accept(listener, NULL, NULL);
    if (server == INVALID_SOCKET) goto fail;

    closesocket(listener);
    sv[0] = server;   /* read end  (event loop watches this) */
    sv[1] = client;   /* write end (workers signal this)     */
    return 0;

fail:
    if (listener != INVALID_SOCKET) closesocket(listener);
    if (client   != INVALID_SOCKET) closesocket(client);
    if (server   != INVALID_SOCKET) closesocket(server);
    return -1;
}

int kl_plat_wakeup_open(KlPlatWakeup *w)
{
    w->rd = w->wr = KL_INVALID_SOCKET;

    SOCKET sv[2];
    if (win_wakeup_pair(sv) != 0)
        return -1;

    u_long nonblocking = 1;
    (void)ioctlsocket(sv[0], FIONBIO, &nonblocking);   /* read end non-blocking */

    w->rd = (KlSocketHandle)sv[0];
    w->wr = (KlSocketHandle)sv[1];
    return 0;
}

void kl_plat_wakeup_signal(const KlPlatWakeup *w)
{
    char c = 1;
    (void)send((SOCKET)w->wr, &c, 1, 0);
}

void kl_plat_wakeup_drain(KlSocketHandle rd)
{
    char buf[64];
    (void)recv((SOCKET)rd, buf, (int)sizeof(buf), 0);
}

void kl_plat_wakeup_close(KlPlatWakeup *w)
{
    if (kl_handle_valid(w->rd)) closesocket((SOCKET)w->rd);
    if (kl_handle_valid(w->wr)) closesocket((SOCKET)w->wr);
    w->rd = w->wr = KL_INVALID_SOCKET;
}
