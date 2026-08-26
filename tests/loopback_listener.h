#ifndef KEEL_TESTS_LOOPBACK_LISTENER_H
#define KEEL_TESTS_LOOPBACK_LISTENER_H

/*
 * loopback_listener.h: a minimal accept-only loopback TCP listener for client tests.
 *
 * Shared test support (like mock_tls.h) for client-family tests. A
 * background thread accepts and holds connections; with reply_http set it writes a canned
 * HTTP/1.1 200 to each accepted connection so a client that gets PAST the (identity) mock
 * TLS handshake would parse a success; the discriminator for fail-closed client behavior.
 */
#include "net_compat.h"
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <pthread.h>
#include <stdatomic.h>

typedef struct {
    int      listen_fd;
    int      port;
    pthread_t tid;
    _Atomic int stop;      /* written by the test thread, read by the listener */
    int      reply_http;   /* 1 = write a canned HTTP/1.1 200 to each conn */
    /* accepted[]/n_accepted are written only by the listener and read only AFTER
     * pthread_join (a synchronization point), so they need no atomics. */
    int      accepted[16];
    int      n_accepted;
} Listener;

/* A complete HTTP/1.1 200 response. With the passthrough mock TLS, a client that
 * gets PAST the (identity) handshake would parse this as a successful 200; so a
 * client that instead aborts at set_hostname will NOT see it. This is what makes
 * the sync/async assertions discriminate the fail-closed behavior. */
static const char kHttp200[] =
    "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK";

static void *listener_thread(void *arg)
{
    Listener *l = arg;
    for (;;) {
        int fd = accept(l->listen_fd, NULL, NULL);
        if (fd < 0) {
            if (l->stop) break;
            continue;
        }
        if (l->n_accepted < (int)(sizeof(l->accepted) / sizeof(l->accepted[0])))
            l->accepted[l->n_accepted++] = fd;
        if (l->reply_http) {
            /* Drain a bit of the request (bounded, the client may have aborted
             * and sent nothing), then reply 200 (best-effort). */
            kl_test_set_rcvtimeo(fd, 300);
            char tmp[512];
            (void)kl_test_sockread(fd, tmp, sizeof(tmp));
            (void)kl_test_sockwrite(fd, kHttp200, sizeof(kHttp200) - 1);
        }
        /* Hold the connection open until teardown. */
        if (l->stop) { kl_test_closesock(fd); break; }
    }
    return NULL;
}

static int listener_start(Listener *l)
{
    memset(l, 0, sizeof(*l));
    l->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (l->listen_fd < 0) return -1;
    struct sockaddr_in addr = { .sin_family = AF_INET };
    socklen_t sl = sizeof(addr);   /* declared before any goto (no jump over an initializer) */
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    addr.sin_port = 0;
    /* Fail-closed: any error after the socket exists closes it (no fd leak), the thread is
     * only created once setup fully succeeds, and pthread_create is checked. On failure the
     * caller sees -1 with listen_fd reset so listener_stop() is never called on a dead half. */
    if (bind(l->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) goto fail;
    if (listen(l->listen_fd, 8) < 0) goto fail;
    if (getsockname(l->listen_fd, (struct sockaddr *)&addr, &sl) < 0) goto fail;
    l->port = ntohs(addr.sin_port);
    if (pthread_create(&l->tid, NULL, listener_thread, l) != 0) goto fail;
    return 0;
fail:
    kl_test_closesock(l->listen_fd);
    l->listen_fd = -1;
    return -1;
}

static void listener_stop(Listener *l)
{
    l->stop = 1;
    /* Kick accept() by connecting once. */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
        struct sockaddr_in addr = { .sin_family = AF_INET };
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        addr.sin_port = htons((uint16_t)l->port);
        connect(fd, (struct sockaddr *)&addr, sizeof(addr));
        kl_test_closesock(fd);
    }
    pthread_join(l->tid, NULL);
    for (int i = 0; i < l->n_accepted; i++) kl_test_closesock(l->accepted[i]);
    kl_test_closesock(l->listen_fd);
}

static void make_url(char *buf, size_t n, const char *scheme, int port, const char *path)
{
    snprintf(buf, n, "%s://127.0.0.1:%d%s", scheme, port, path);
}

#endif /* KEEL_TESTS_LOOPBACK_LISTENER_H */
