/*
 * platform_wakeup_win.c: the Windows run-loop wakeup channel.
 *
 * A separate TU (mirrors platform_wakeup_posix.c)
 * so the wakeup is an independently-overridable seam. Windows has no pipe(2) that
 * WSAPoll can watch, so the channel is a connected loopback TCP pair.
 */
#include "platform.h"
#include "platform_socket.h"   /* kl_plat_socket_runtime_init: the PAL socket-runtime invariant */

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
    /* Winsock sockets are inheritable by default; these two are Keel's own and must not leak into a
     * child spawned with handle inheritance (the analog of FD_CLOEXEC; kl_sockdef_set_cloexec). */
    (void)SetHandleInformation((HANDLE)server, HANDLE_FLAG_INHERIT, 0);
    (void)SetHandleInformation((HANDLE)client, HANDLE_FLAG_INHERIT, 0);
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
    if (kl_plat_socket_runtime_init() != 0) return -1;   /* PAL invariant: builds a loopback socket pair */
    w->rd = w->wr = KL_INVALID_SOCKET;

    SOCKET sv[2];
    if (win_wakeup_pair(sv) != 0)
        return -1;

    /* Both ends non-blocking: the read end so the drain never stalls the loop, the write end so a
     * signal never blocks its caller once the pair's buffers are full (a full pair already holds a
     * pending wakeup; wakeup.h: signals coalesce). */
    u_long nonblocking = 1;
    (void)ioctlsocket(sv[0], FIONBIO, &nonblocking);
    (void)ioctlsocket(sv[1], FIONBIO, &nonblocking);

    w->rd = (KlSocketHandle)sv[0];
    w->wr = (KlSocketHandle)sv[1];
    return 0;
}

/* PAL-gate: dominated-by kl_plat_wakeup_open
 * signal/drain/close, and the win_wakeup_pair helper, all act on a KlPlatWakeup that only
 * kl_plat_wakeup_open can have filled in, so the PAL gate there has already run. Local to this TU. */
void kl_plat_wakeup_signal(const KlPlatWakeup *w)
{
    char c = 1;
    (void)send((SOCKET)w->wr, &c, 1, 0);   /* WSAEWOULDBLOCK = full = a wakeup is already pending */
}

void kl_plat_wakeup_drain(KlSocketHandle rd)
{
    /* Empty the channel (bounded), as the POSIX drain does: a burst coalesces into one wakeup. */
    char buf[4096];
    for (int i = 0; i < 1024; i++) {
        int rc = recv((SOCKET)rd, buf, (int)sizeof(buf), 0);
        if (rc < (int)sizeof(buf)) break;
    }
}

void kl_plat_wakeup_close(KlPlatWakeup *w)
{
    if (kl_handle_valid(w->rd)) closesocket((SOCKET)w->rd);
    if (kl_handle_valid(w->wr)) closesocket((SOCKET)w->wr);
    w->rd = w->wr = KL_INVALID_SOCKET;
}
