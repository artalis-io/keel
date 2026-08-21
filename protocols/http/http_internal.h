#ifndef KEEL_PROTOCOLS_HTTP_INTERNAL_H
#define KEEL_PROTOCOLS_HTTP_INTERNAL_H

/*
 * http_internal.h — HTTP-family internal seam: TLS-aware connection I/O over the
 * embedded KlStream + HTTP-server cross-TU forward decls. Owned by protocols/http/;
 * the http2/websocket completion adapters include it for conn_read/conn_write
 * (permitted HTTP-family coordination seam). Substrate stream I/O lives in
 * src/stream_io.h (reached via -Isrc).
 */

#include <keel/http_connection.h>
#include <keel/http_server.h>
#include <keel/tls.h>
#include <errno.h>            /* freestanding: supplied by the UEFI/cross shim */
#ifdef KEEL_FREESTANDING
#include <sys/types.h>        /* ssize_t (no <unistd.h> in a freestanding build) */
#else
#include <unistd.h>
#endif

#include "stream_io.h"        /* kl_stream_* raw I/O (substrate, -Isrc) */

/* Max retries on zero-byte write before giving up (conn_write_all, writev_all) */
#define KL_HTTP_CONN_WRITE_SPIN_MAX 256

/* Completion-mode TLS ciphertext scratch size (one TLS record + slack). The HTTP completion
 * adapter hands a per-connection buffer of this size (KlHttpConn.comp_cipher, preallocated at
 * server init for TLS+completion slots) to the raw receive; the backend does raw I/O into it
 * with no TLS knowledge. Internal — not a public API (was briefly in http_connection.h). */
#define KL_COMP_CIPHER_SIZE (17u * 1024u)

/* ── Transport helpers — TLS-aware read/write over the embedded KlStream ──────── */

/* Recover the owning KlHttpConn from its embedded stream at the HTTP adapter boundary. The stream is
 * the leading member (offset 0), but containerof keeps that an implementation detail. */
static inline KlHttpConn *kl_http_conn_from_stream(KlStream *s) {
    return (KlHttpConn *)((char *)s - offsetof(KlHttpConn, stream));
}

static inline const KlSocketProvider *conn_provider(const KlHttpConn *c) {
    return kl_stream_provider(&c->stream);
}

static inline ssize_t conn_read(KlHttpConn *c, void *buf, size_t len) {
    if (c->tls) return c->tls->read(c->tls, c->stream.fd, buf, len);
    return kl_stream_recv(&c->stream, buf, len);
}

static inline ssize_t conn_write(KlHttpConn *c, const void *buf, size_t len) {
    if (c->tls) return c->tls->write(c->tls, c->stream.fd, buf, len);
    return kl_stream_send(&c->stream, buf, len);
}

/* Write all bytes, retrying on short writes (TLS WANT_WRITE, etc.) */
static inline int conn_write_all(KlHttpConn *c, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    size_t remaining = len;
    int spins = 0;
    while (remaining > 0) {
        ssize_t nw = conn_write(c, p, remaining);
        if (nw < 0) return -1;
        if (nw == 0) {
            if (++spins > KL_HTTP_CONN_WRITE_SPIN_MAX) return -1;
            continue;
        }
        spins = 0;
        p += nw;
        remaining -= (size_t)nw;
    }
    return 0;
}

/* Suppress warn_unused_result on best-effort error writes */
static inline void best_effort_conn_write(KlHttpConn *c, const void *buf, size_t len) {
    ssize_t r = conn_write(c, buf, len);
    (void)r;
}

/* Release a connection and resume listening if paused (defined in http_server.c) */
void kl_http_server_conn_release(KlHttpServer *s, KlHttpConn *c);

/* Server bisection (S-1): the completion run-loop tick lives in the freestanding-safe
 * server core (http_server_core.c); the idle/drain sweeps stay in http_server.c (they own
 * kl_408_response). One completion iteration; returns 0 to continue, -1 to break. */
int  kl_http_server_run_completion_loop(KlHttpServer *s);
/* Close the listen socket (+ unlink an owned AF_UNIX path, hosted). Non-static so the
 * freestanding kl_http_server_free (http_server_core.c) reaches it on the hosted path (S-7). */
void kl_http_server_close_listener(KlHttpServer *s);
void kl_http_server_sweep_conn_timeouts(KlHttpServer *s, uint64_t now, int completion_loop);
void kl_http_server_drain_progress(KlHttpServer *s, uint64_t now);

/* Server logging helpers (defined in http_server.c; used by the per-platform
 * http_server_plat_*.c TUs too). */
__attribute__((format(printf, 3, 4)))
void kl_http_server_log(KlHttpServer *s, int level, const char *fmt, ...);
void kl_http_server_log_errno(KlHttpServer *s, int level, const char *msg);

#endif /* KEEL_PROTOCOLS_HTTP_INTERNAL_H */
