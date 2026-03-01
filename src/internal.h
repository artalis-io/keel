#ifndef KEEL_INTERNAL_H
#define KEEL_INTERNAL_H

#include <keel/connection.h>
#include <keel/tls.h>
#include <unistd.h>
#include <errno.h>

/* ── Transport helpers — TLS-aware read/write ────────────────────── */

static inline ssize_t conn_read(KlConn *c, void *buf, size_t len) {
    if (c->tls) return c->tls->read(c->tls, c->fd, buf, len);
    ssize_t r;
    do { r = read(c->fd, buf, len); } while (r < 0 && errno == EINTR);
    return r;
}

static inline ssize_t conn_write(KlConn *c, const void *buf, size_t len) {
    if (c->tls) return c->tls->write(c->tls, c->fd, buf, len);
    ssize_t r;
    do { r = write(c->fd, buf, len); } while (r < 0 && errno == EINTR);
    return r;
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
            if (++spins > 256) return -1;
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

#endif
