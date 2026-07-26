/*
 * net_compat_win.c — Winsock implementation of the test network helpers.
 *
 * Sibling of net_compat_posix.c, selected by the Makefile (never both). Windows
 * has no socketpair() and no pollable pipes, so the pair is a self-connected
 * loopback TCP pair; close/read/write map to closesocket/recv/send. See
 * tests/net_compat.h.
 */
#include "net_compat.h"

#include <string.h>   /* memset */

int kl_test_closesock(int fd) {
    return closesocket((SOCKET)fd);
}

int kl_test_set_nonblock(int fd) {
    u_long m = 1;
    return ioctlsocket((SOCKET)fd, FIONBIO, &m) == 0 ? 0 : -1;
}

long kl_test_sockwrite(int fd, const void *buf, size_t len) {
    return send((SOCKET)fd, (const char *)buf, (int)len, 0);
}

long kl_test_sockread(int fd, void *buf, size_t len) {
    return recv((SOCKET)fd, (char *)buf, (int)len, 0);
}

int kl_test_poll1(int fd, int for_write, int timeout_ms) {
    WSAPOLLFD p;
    p.fd = (SOCKET)fd;
    p.events = (SHORT)(for_write ? POLLWRNORM : POLLRDNORM);
    p.revents = 0;
    return WSAPoll(&p, 1, timeout_ms);
}

int kl_test_set_rcvtimeo(int fd, int ms) {
    DWORD tv = (DWORD)ms;
    return setsockopt((SOCKET)fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
}

int kl_test_socketpair(int sv[2]) {
    SOCKET listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == INVALID_SOCKET) return -1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    int addrlen = (int)sizeof(addr);
    SOCKET client = INVALID_SOCKET, server = INVALID_SOCKET;
    if (bind(listener, (struct sockaddr *)&addr, addrlen) != 0) goto fail;
    if (listen(listener, 1) != 0) goto fail;
    if (getsockname(listener, (struct sockaddr *)&addr, &addrlen) != 0) goto fail;
    client = socket(AF_INET, SOCK_STREAM, 0);
    if (client == INVALID_SOCKET) goto fail;
    if (connect(client, (struct sockaddr *)&addr, addrlen) != 0) goto fail;
    server = accept(listener, NULL, NULL);
    if (server == INVALID_SOCKET) goto fail;
    closesocket(listener);
    sv[0] = (int)client;
    sv[1] = (int)server;
    return 0;
fail:
    if (listener != INVALID_SOCKET) closesocket(listener);
    if (client != INVALID_SOCKET) closesocket(client);
    if (server != INVALID_SOCKET) closesocket(server);
    return -1;
}
