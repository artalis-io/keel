#ifndef KEEL_INTERNAL_H
#define KEEL_INTERNAL_H

#include <keel/http_connection.h>
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
#define KL_HTTP_CONN_WRITE_SPIN_MAX 256

/* Completion-mode TLS ciphertext scratch size (one TLS record + slack). The HTTP completion
 * adapter hands a per-connection buffer of this size (KlHttpConn.comp_cipher, preallocated at
 * server init for TLS+completion slots) to the raw receive; the backend does raw I/O into it
 * with no TLS knowledge. Internal — not a public API (was briefly in connection.h). */
#define KL_COMP_CIPHER_SIZE (17u * 1024u)

/* ── Transport helpers — TLS-aware read/write ────────────────────── */

/* The socket provider for a connection (ctx->sockets; NULL = POSIX fast path). */
/* ── Raw-transport seam over the embedded KlStream (step 6B-2) ─────────────────────────────────
 * HTTP connection code performs raw socket I/O and identity recovery through the neutral KlStream,
 * never touching the socket provider or fd directly. TLS is an adapter ABOVE these (conn_read/
 * conn_write). The generic KlStream write queue / strict-read facets stay dormant for HTTP/1. */
static inline const KlSocketProvider *kl_stream_provider(const KlStream *s) {
    return s->ctx ? s->ctx->sockets : NULL;
}
static inline ssize_t kl_stream_recv(const KlStream *s, void *buf, size_t len) {
    return kl_sock_recv(kl_stream_provider(s), s->fd, buf, len);
}
static inline ssize_t kl_stream_send(const KlStream *s, const void *buf, size_t len) {
    return kl_sock_send(kl_stream_provider(s), s->fd, buf, len);
}
static inline ssize_t kl_stream_recv_peek(const KlStream *s, void *buf, size_t len) {
    return kl_sock_recv_peek(kl_stream_provider(s), s->fd, buf, len);
}
/* Classify the last raw stream op's status (would-block / interrupted / etc.) via the seam,
 * so HTTP code never inspects the socket provider directly. */
static inline KlIoStatus kl_stream_io_status(const KlStream *s) {
    return kl_sock_io_status(kl_stream_provider(s));
}
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

/* Release a connection and resume listening if paused (defined in server.c) */
void kl_server_conn_release(KlServer *s, KlHttpConn *c);

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
 * flush produced output. Returns the next KlHttpConnState (KL_HTTP_CONN_HTTP2 / KL_HTTP_CONN_CLOSED).
 * The transport-agnostic h2 core (defined in h2.c): the readiness drive
 * (kl_h2_server_on_readable) is conn_read + this; the completion driver reads via its
 * own loop then calls this. Internal — the public h2 API and the KlH2ServerSession
 * vtable are unchanged, so the event axis stays invisible to h2 users. */
KlHttpConnState kl_h2_server_feed(KlHttpConn *c, const void *data, size_t len);

/* HTTP/2 output boundary seam (8d-4). The h2 server writes produced frame bytes through
 * a per-connection writer; the default writes the socket (conn_write). A completion
 * driver installs its own buffering writer around a feed to collect the frames for one
 * ordered overlapped send, then restores the default (fn == NULL). Symmetric with the
 * WebSocket server's kl_drain boundary; keeps all completion buffering in the driver, not
 * h2.c. Defined in h2.c. */
typedef ssize_t (*KlH2WriteFn)(void *ctx, const void *data, size_t len);
void kl_h2_server_set_writer(KlHttpConn *c, KlH2WriteFn fn, void *ctx);

/* Server logging helpers (defined in server.c; used by the per-platform
 * server_plat_*.c TUs too). */
__attribute__((format(printf, 3, 4)))
void kl_log(KlServer *s, int level, const char *fmt, ...);
void kl_log_errno(KlServer *s, int level, const char *msg);

#endif
