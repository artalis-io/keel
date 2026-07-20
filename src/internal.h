#ifndef KEEL_INTERNAL_H
#define KEEL_INTERNAL_H

#include <keel/connection.h>
#include <keel/server.h>
#include <keel/tls.h>
#include <unistd.h>
#include <errno.h>

#include "socket.h"

/* Max retries on zero-byte write before giving up (conn_write_all, writev_all) */
#define KL_CONN_WRITE_SPIN_MAX 256

/* ── Transport helpers — TLS-aware read/write ────────────────────── */

/* The socket provider for a connection (ctx->sockets; NULL = POSIX fast path). */
static inline const KlSocketProvider *conn_provider(const KlConn *c) {
    return c->ctx ? c->ctx->sockets : NULL;
}

static inline ssize_t conn_read(KlConn *c, void *buf, size_t len) {
    if (c->tls) return c->tls->read(c->tls, c->fd, buf, len);
    return kl_sock_recv(conn_provider(c), c->fd, buf, len);
}

static inline ssize_t conn_write(KlConn *c, const void *buf, size_t len) {
    if (c->tls) return c->tls->write(c->tls, c->fd, buf, len);
    return kl_sock_send(conn_provider(c), c->fd, buf, len);
}

/* Write all bytes, retrying on short writes (TLS WANT_WRITE, etc.) */
static inline int conn_write_all(KlConn *c, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    size_t remaining = len;
    int spins = 0;
    while (remaining > 0) {
        ssize_t nw = conn_write(c, p, remaining);
        if (nw < 0) return -1;
        if (nw == 0) {
            if (++spins > KL_CONN_WRITE_SPIN_MAX) return -1;
            continue;
        }
        spins = 0;
        p += nw;
        remaining -= (size_t)nw;
    }
    return 0;
}

/* Suppress warn_unused_result on best-effort error writes */
static inline void best_effort_conn_write(KlConn *c, const void *buf, size_t len) {
    ssize_t r = conn_write(c, buf, len);
    (void)r;
}

/* Release a connection and resume listening if paused (defined in server.c) */
void kl_server_conn_release(KlServer *s, KlConn *c);

#endif
