/*
 * net_compat_win.c: Winsock implementation of the test network helpers.
 *
 * Sibling of net_compat_posix.c, selected by the Makefile (never both). Windows
 * has no socketpair() and no pollable pipes, so the pair is a self-connected
 * loopback TCP pair; close/read/write map to closesocket/recv/send. See
 * tests/net_compat.h.
 *
 * Precondition: Winsock must already be initialized (WSAStartup) before these
 * helpers are called; the library does this at load (socket_winsock.c's
 * constructor), which every test links, so tests need no explicit WSAStartup.
 */
#include "net_compat.h"
#include "../src/socket.h"   /* kl_socket_provider_* */
#include "../src/platform_socket.h"   /* kl_plat_socket_runtime_init: tests reach ws2_32 directly too */

#include <string.h>   /* memset */

/* PAL-gate: the helpers below call ws2_32 directly, on descriptors the harness may not have
 * created, so each states the PAL invariant itself (src/platform_socket.h). This used to be
 * satisfied for free by socket_winsock.c's load-time constructor; nothing initialises Winsock
 * implicitly any more, and a test binary whose first socket call comes from the harness rather
 * than from Keel would otherwise see WSANOTINITIALISED. */
int kl_test_closesock(KlSocketHandle fd) {
    if (kl_plat_socket_runtime_init() != 0) return -1;   /* PAL invariant */
    return closesocket((SOCKET)fd);
}

int kl_test_set_nonblock(KlSocketHandle fd) {
    if (kl_plat_socket_runtime_init() != 0) return -1;   /* PAL invariant */
    u_long m = 1;
    return ioctlsocket((SOCKET)fd, FIONBIO, &m) == 0 ? 0 : -1;
}

long kl_test_sockwrite(KlSocketHandle fd, const void *buf, size_t len) {
    if (kl_plat_socket_runtime_init() != 0) return -1;   /* PAL invariant */
    return send((SOCKET)fd, (const char *)buf, (int)len, 0);
}

long kl_test_sockread(KlSocketHandle fd, void *buf, size_t len) {
    if (kl_plat_socket_runtime_init() != 0) return -1;   /* PAL invariant */
    return recv((SOCKET)fd, (char *)buf, (int)len, 0);
}

int kl_test_poll1(KlSocketHandle fd, int for_write, int timeout_ms) {
    if (kl_plat_socket_runtime_init() != 0) return -1;   /* PAL invariant */
    WSAPOLLFD p;
    p.fd = (SOCKET)fd;
    p.events = (SHORT)(for_write ? POLLWRNORM : POLLRDNORM);
    p.revents = 0;
    return WSAPoll(&p, 1, timeout_ms);
}

int kl_test_set_rcvtimeo(KlSocketHandle fd, int ms) {
    if (kl_plat_socket_runtime_init() != 0) return -1;   /* PAL invariant */
    DWORD tv = (DWORD)ms;
    return setsockopt((SOCKET)fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
}

int kl_test_socketpair(int sv[2]) {
    if (kl_plat_socket_runtime_init() != 0) return -1;   /* PAL invariant */
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

/* See net_compat.h: the platform's built-in socket provider, named per platform. */
const void *kl_test_builtin_provider(void) {
    return (const void *)kl_socket_provider_winsock();
}

/* See net_compat.h: millisecond sleep. */
void kl_test_sleep_ms(unsigned ms) {
    Sleep((DWORD)ms);
}

/* See net_compat.h: calling thread identity. */
KlTestThreadId kl_test_thread_id(void) {
    return (KlTestThreadId)GetCurrentThreadId();
}

#if defined(_MSC_VER)
/* See tests/win_prelude.h. Returning from this handler is the entire point: it makes the UCRT
 * report the error through errno like POSIX instead of fast-failing the process. Test-only;
 * the library never installs it and its behaviour is unchanged. */
#include <stdlib.h>
#include <stdint.h>
static void keel_test_invalid_parameter(const wchar_t *expr, const wchar_t *fn,
                                        const wchar_t *file, unsigned int line, uintptr_t res) {
    (void)expr; (void)fn; (void)file; (void)line; (void)res;
}

/* Defined by the test TU: UTEST_MAIN()'s main(), renamed by the prelude. */
int keel_utest_main(int argc, const char *const argv[]);

int main(int argc, const char *const argv[]) {
    _set_invalid_parameter_handler(keel_test_invalid_parameter);
    return keel_utest_main(argc, argv);
}
#endif
