#ifndef KEEL_INTERNAL_H
#define KEEL_INTERNAL_H

#include <keel/connection.h>
#include <keel/server.h>
#include <keel/tls.h>
#include <errno.h>            /* freestanding: supplied by the UEFI/cross shim */
#ifdef KEEL_FREESTANDING
#include <sys/types.h>        /* ssize_t (no <unistd.h> in a freestanding build) */
#else
#include <unistd.h>
#endif

#include "socket.h"

/* Max retries on zero-byte write before giving up (conn_write_all, writev_all) */
#define KL_CONN_WRITE_SPIN_MAX 256

/* ── Transport helpers — TLS-aware read/write ────────────────────── */

/* The socket provider for a connection (ctx->sockets; NULL = POSIX fast path). */
static inline const KlSocketProvider *conn_provider(const KlConn *c) {
    return c->stream.ctx ? c->stream.ctx->sockets : NULL;
}

static inline ssize_t conn_read(KlConn *c, void *buf, size_t len) {
    if (c->tls) return c->tls->read(c->tls, c->stream.fd, buf, len);
    return kl_sock_recv(conn_provider(c), c->stream.fd, buf, len);
}

static inline ssize_t conn_write(KlConn *c, const void *buf, size_t len) {
    if (c->tls) return c->tls->write(c->tls, c->stream.fd, buf, len);
    return kl_sock_send(conn_provider(c), c->stream.fd, buf, len);
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

/* Server bisection (S-1): the completion run-loop tick lives in the freestanding-safe
 * server core (server_core.c); the idle/drain sweeps stay in server.c (they own
 * kl_408_response). One completion iteration; returns 0 to continue, -1 to break. */
int  kl_server_run_completion_loop(KlServer *s);
/* Close the listen socket (+ unlink an owned AF_UNIX path, hosted). Non-static so the
 * freestanding kl_server_free (server_core.c) reaches it on the hosted path (S-7). */
void kl_server_close_listener(KlServer *s);
void kl_server_sweep_conn_timeouts(KlServer *s, uint64_t now, int completion_loop);
void kl_server_drain_progress(KlServer *s, uint64_t now);

/* Drive the HTTP/2 server session with already-received plaintext: parse frames +
 * flush produced output. Returns the next KlConnState (KL_CONN_HTTP2 / KL_CONN_CLOSED).
 * The transport-agnostic h2 core (defined in h2.c): the readiness drive
 * (kl_h2_server_on_readable) is conn_read + this; the completion driver reads via its
 * own loop then calls this. Internal — the public h2 API and the KlH2ServerSession
 * vtable are unchanged, so the event axis stays invisible to h2 users. */
KlConnState kl_h2_server_feed(KlConn *c, const void *data, size_t len);

/* HTTP/2 output boundary seam (8d-4). The h2 server writes produced frame bytes through
 * a per-connection writer; the default writes the socket (conn_write). A completion
 * driver installs its own buffering writer around a feed to collect the frames for one
 * ordered overlapped send, then restores the default (fn == NULL). Symmetric with the
 * WebSocket server's kl_drain boundary; keeps all completion buffering in the driver, not
 * h2.c. Defined in h2.c. */
typedef ssize_t (*KlH2WriteFn)(void *ctx, const void *data, size_t len);
void kl_h2_server_set_writer(KlConn *c, KlH2WriteFn fn, void *ctx);

/* Server logging helpers (defined in server.c; used by the per-platform
 * server_plat_*.c TUs too). */
__attribute__((format(printf, 3, 4)))
void kl_log(KlServer *s, int level, const char *fmt, ...);
void kl_log_errno(KlServer *s, int level, const char *msg);

#endif
