/*
 * completion_http_server.c — the server + TLS leg of the completion driver.
 *
 * The KlHttpConn/HTTP-1 connection state machine over *completion* events, PLUS the
 * server-side memory-BIO TLS path (comp_tls_*). The generic tick (kl_comp_run) lives in
 * completion_core.c and the h2/ws drives in completion_http2.c / completion_ws.c.
 *
 * TLS is folded into this TU rather than a separate completion_tls.c: comp_tls_drive ↔
 * comp_after_state ↔ comp_h2_drive mutually recurse, so a separate TLS TU would need a
 * bidirectional forward-declaration web with no real decoupling — the load-bearing split
 * is core (generic, client-linkable) vs server (server-only), which this achieves.
 *
 * This TU installs comp_server_conn_dispatch on the ctx's comp_conn_dispatch hook (in
 * kl_http_comp_run, the single always-hit server-completion-init point,
 * before any accept is primed), so completion_core.c's kl_comp_run routes
 * ACCEPT/READ/WRITE here without a static reference — a client-only build links neither.
 *
 * Contains NO platform (Win32/IOCP) type or symbol — a future io_uring-completion /
 * POSIX-AIO backend reuses it verbatim. See docs/archive/phases/phase8b_iocp_breadth_design.md.
 */
#include <keel/http_server.h>
#include <keel/http_connection.h>
#include <keel/tls.h>            /* KlTls vtable ops — TLS-over-completion */
#include <keel/event_ctx.h>      /* KlEventCtx (comp_conn_dispatch hook) */
#include "http_internal.h"            /* kl_http_server_conn_release */
#include "http_conn_internal.h"       /* kl_http_conn_dispatch_request / kl_http_conn_send_complete */
#include "http_response_internal.h"   /* kl_http_response_build_iovec */
#include "completion.h"          /* the neutral completion axis (kl_comp_*_raw + kl_comp_drain) */
#include "completion_http.h"     /* the HTTP completion seam this TU DEFINES (wrappers + kl_http_comp_*) */
#include "completion_internal.h" /* the cross-TU h2/ws drives + exported server helpers */
#include "completion_io.h"           /* kl_comp_run (the neutral generic tick) */
#include "socket.h"              /* kl_sock_* (close / tcp_nodelay via the seam) */
#include <keel/sockaddr.h>       /* kl_sockaddr_family — neutral accept addrs from the event */
#include "platform.h"            /* kl_plat_file_pread — TLS file body chunks */
#include <keel/proxy_protocol.h> /* kl_cidr_match — PROXY-over-completion accept gate */
#include "http_proto_hooks.h"         /* ws/h2 upgrade + completion-drive seams */
#include <string.h>
#include <stddef.h>              /* offsetof (server_of_ctx containerof) */
#include <stdint.h>              /* SIZE_MAX */

/* Max events the generic tick drains per call (matches completion_core.c's clamp). */
#define KL_COMP_MAX_EVENTS 64

/* Blocking per-drain timeout (ms) while teardown quiescence waits for the forced accept completions
 * during teardown quiescence. Blocking so the pending completions are actually reaped; they arrive at once
 * (already forced), so this rarely elapses. */
#define KL_ACCEPT_QUIESCE_TIMEOUT_MS 1000

/* Stack chunk for draining the TLS engine's outgoing ciphertext to the socket. */
#define KL_TLS_FLUSH_CHUNK 16384

/* Plaintext file bytes read + encrypted per overlapped send for a TLS file body. */
#define KL_COMP_TLS_FILE_CHUNK 16384

/* Exported (completion_internal.h): release the connection. Called by the h2/ws drives. */
void kl_comp_close(struct KlHttpServer *s, KlHttpConn *c) {
    kl_http_server_conn_release(s, c);   /* closes the socket + returns the pool slot */
}

/* Recover the containing KlHttpConn from a completion event's KlStream target. KlStream is the
 * leading member of KlHttpConn, so this is address-preserving; offsetof keeps it robust
 * to any future reordering. Only the HTTP adapter does this — a backend never holds a KlHttpConn. */
static inline KlHttpConn *conn_of_stream(KlStream *st) {
    return (KlHttpConn *)((char *)st - offsetof(KlHttpConn, stream));
}

/* ── HTTP-adapter completion helpers (KlHttpConn form) ────────────────────────
 * These own ALL the TLS/PROXY/connection-state knowledge for posting transport I/O; the
 * raw backend (kl_comp_post_*_raw → the vtable) sees only (stream, buffer). */

/* Choose the receive buffer for the connection's current phase, then post the raw recv:
 *   - PROXY header phase → plaintext read_buf (even on a TLS conn — the header is pre-TLS);
 *   - TLS phase          → the per-conn ciphertext scratch (preallocated at server init, stable until
 *                          the recv completes; comp_on_read feeds it to the engine);
 *   - ordinary plaintext → the read_buf sliding window. */
int kl_comp_post_recv(KlHttpConn *c) {
    if (c->tls && c->state != KL_HTTP_CONN_PROXY_HEADER) {
        /* comp_cipher is preallocated at server init for TLS+completion slots (kl_http_server_init);
         * a NULL here is a misconfiguration, not a runtime allocation — fail the recv. */
        if (!c->comp_cipher) return -1;
        return kl_comp_post_recv_raw(&c->stream, c->comp_cipher, c->comp_cipher_cap);
    }
    size_t space = c->stream.read_cap - c->stream.read_len;
    if (space == 0) return -1;   /* headers overflowed */
    return kl_comp_post_recv_raw(&c->stream, c->stream.read_buf + c->stream.read_len, space);
}

int kl_comp_post_send(KlHttpConn *c, const KlIoVec *iov, int iovcnt, size_t total) {
    return kl_comp_post_send_raw(&c->stream, iov, iovcnt, total);
}

/* Accept/sendfile HTTP wrappers: each forwards the neutral arg (&s->ev, s->listen_fd, &c->stream)
 * to the neutral kl_comp_*_raw entry point. The backend never sees an HTTP type. */
int kl_comp_prime_accepts(struct KlHttpServer *s) {
    return kl_comp_prime_accepts_raw(&s->ev, s->listen_fd);
}
int kl_comp_post_accept(struct KlHttpServer *s) {
    return kl_comp_post_accept_raw(&s->ev);
}
int kl_comp_shutdown_accepts(struct KlHttpServer *s) {
    return kl_comp_shutdown_accepts_raw(&s->ev);
}
int kl_comp_post_sendfile(KlHttpConn *c, const KlIoVec *head_iov, int head_n,
                          size_t head_total, int file_fd, uint64_t count) {
    return kl_comp_post_sendfile_raw(&c->stream, head_iov, head_n, head_total, file_fd, count);
}

/* Serialize the response head and post it: buffered body inline via WSASend, or a
 * file body via the backend's zero-copy file send (TransmitFile) after the head.
 * The backend copies + owns the head bytes for the op's lifetime. */
static int comp_send_response(KlHttpConn *c) {
    char cl_buf[48];
    KlIoVec iov[7];
    size_t total = 0;
    int n = kl_http_response_build_iovec(&c->res, iov, 7, cl_buf, sizeof(cl_buf), &total);
    if (n < 0) return -1;
    /* FILE mode: the iovec is head-only; append the file bytes with sendfile. A HEAD
     * request has no body, so it falls through to the plain head send. */
    if (c->res.body_mode == KL_HTTP_BODY_FILE && !c->res.head_request)
        return kl_comp_post_sendfile(c, iov, n, total,
                                     c->res.file_fd, (uint64_t)c->res.file_size);
    return kl_comp_post_send(c, iov, n, total);
}

/* Start (or continue) a request-body read: reset the sliding-window buffer and post
 * the next receive. The body core (kl_http_conn_ingest_body) feeds read_buf[0..nread].
 * Read-side flow control (kl_http_request_pause_body): while paused, do NOT post the next recv —
 * the conn parks with no outstanding op until kl_http_request_resume_body re-posts it. */
static void comp_start_body_read(struct KlHttpServer *s, KlHttpConn *c) {
    c->stream.read_len = 0;
    if (c->stream.read_paused) return;   /* paused — resume re-posts via kl_http_comp_post_read */
    if (kl_comp_post_recv(c) < 0) kl_comp_close(s, c);
}

/* Drive a plaintext streaming connection over the completion loop. Post the
 * outbound buffer's pending bytes as ONE overlapped send — bounded, at most one in flight
 * — instead of busy-spinning a blocking flush on a slow client (the head-of-line defect:
 * a full socket send buffer made the old kl_drain_flush spin stall the whole loop thread).
 * kl_comp_post_send copies the bytes (so the buffer is released immediately) and the
 * backend surfaces one KL_COMP_WRITE when the whole chunk is out, at which point
 * comp_on_write pumps again — posting freshly-produced bytes (async producer) or completing
 * the response once the stream ended and the buffer drained. */
static void comp_stream_pump(struct KlHttpServer *s, KlHttpConn *c) {
    if (c->res.stream_inflight)
        return;   /* a send is already in flight — its completion pumps the next */

    KlDrain *d = &c->res.drain;
    size_t pending = kl_drain_buffered(d);
    if (pending > 0) {
        KlIoVec iov = { .base = (void *)kl_drain_data(d), .len = pending };
        int rc = kl_comp_post_send(c, &iov, 1, pending);
        kl_drain_consume(d, pending);   /* post_send copied — release the buffer */
        if (rc < 0) { kl_comp_close(s, c); return; }
        c->res.stream_inflight = 1;
        return;
    }
    if (!c->res.stream_ended)
        return;   /* async producer will write more and resume (kl_async_complete) */

    /* Fully sent + ended: keep-alive read or close. */
    KlHttpConnState st = kl_http_conn_send_complete(c);
    if (st == KL_HTTP_CONN_READING) {
        if (kl_comp_post_recv(c) < 0) kl_comp_close(s, c);
    } else {
        kl_comp_close(s, c);
    }
}

static void comp_send_stream(struct KlHttpServer *s, KlHttpConn *c) {
    if (c->res.drain_enabled) {   /* overlapped, non-blocking flush (no HOL) */
        comp_stream_pump(s, c);
        return;
    }
    /* No outbound buffer (no allocator at begin_stream): the legacy synchronous spin-write.
     * Only reachable when kl_http_response_enable_drain was skipped, so a fully-produced stream. */
    int r;
    do { r = kl_http_response_send(&c->res); } while (r == 1 && c->res.stream_ended);
    if (r < 0) { kl_comp_close(s, c); return; }
    KlHttpConnState st = kl_http_conn_send_complete(c);
    if (st == KL_HTTP_CONN_READING) {
        if (kl_comp_post_recv(c) < 0) kl_comp_close(s, c);
    } else {
        kl_comp_close(s, c);
    }
}

/* TLS-over-completion output. In completion mode the mbedTLS BIO writes outgoing
 * ciphertext into an in-memory ring (drain_output) rather than the socket; push that
 * ciphertext out with a synchronous send on the (blocking) accepted socket. Used for
 * bounded, latency-insensitive handshake records — the client is actively handshaking,
 * not slow-reading — so no overlapped send is warranted. Response bodies go out
 * overlapped (comp_tls_send_response) to avoid any head-of-line stall.
 * Exported (completion_internal.h): called by the h2/ws drives. */
int kl_comp_tls_flush(KlHttpConn *c) {
    const KlSocketProvider *sp = c->stream.ctx ? c->stream.ctx->sockets : NULL;
    unsigned char buf[KL_TLS_FLUSH_CHUNK];
    ssize_t n;
    while ((n = c->tls->drain_output(c->tls, buf, sizeof(buf))) > 0) {
        size_t off = 0;
        while (off < (size_t)n) {
            ssize_t w = kl_sock_send(sp, c->stream.fd, buf + off, (size_t)n - off);
            if (w <= 0) return -1;           /* seam retries EINTR; <=0 is fatal here */
            off += (size_t)w;
        }
    }
    return (n < 0) ? -1 : 0;
}

/* Encrypt the whole plaintext response into one heap ciphertext buffer via the memory
 * BIO, draining the engine's out ring as it fills (so a response larger than the ring
 * cap is handled). On success returns 0 with out, outlen and outcap set — the caller
 * frees the buffer (outlen bytes used, outcap allocated); on error frees any partial
 * buffer and returns -1. */
static int comp_tls_encrypt_all(KlHttpConn *c, const KlIoVec *iov, int n,
                                unsigned char **out, size_t *outlen, size_t *outcap) {
    unsigned char *buf = NULL;
    size_t cap = 0, len = 0;

    for (int i = 0; i < n; i++) {
        size_t off = 0;
        while (off < iov[i].len) {
            ssize_t w = c->tls->write(c->tls, c->stream.fd,
                                      (const char *)iov[i].base + off, iov[i].len - off);
            if (w < 0) goto fail;
            if (w > 0) off += (size_t)w;
            /* Drain the ciphertext this write produced (also frees ring space, so a
             * WANT_WRITE — w==0, ring at cap — makes progress on the next iteration). */
            for (;;) {
                if (len == cap) {
                    size_t ncap = cap ? cap * 2 : 8192;
                    unsigned char *nb = kl_realloc(c->stream.alloc, buf, cap, ncap);
                    if (!nb) goto fail;
                    buf = nb;
                    cap = ncap;
                }
                ssize_t d = c->tls->drain_output(c->tls, buf + len, cap - len);
                if (d < 0) goto fail;
                if (d == 0) break;
                len += (size_t)d;
            }
        }
    }
    *out = buf;
    *outlen = len;
    *outcap = cap;
    return 0;
fail:
    kl_free(c->stream.alloc, buf, cap);
    return -1;
}

/* Drain the TLS engine's pending outgoing ciphertext into one heap buffer (grow as it
 * fills). On success returns 0 with out/outlen/outcap set (caller frees outcap bytes),
 * -1 on error. Used to post h2 output overlapped instead of a synchronous flush.
 * Exported (completion_internal.h): called by the h2 drive. */
int kl_comp_tls_drain_output(KlHttpConn *c, unsigned char **out, size_t *outlen, size_t *outcap) {
    unsigned char *buf = NULL;
    size_t cap = 0, len = 0;
    for (;;) {
        if (len == cap) {
            size_t ncap = cap ? cap * 2 : 8192;
            unsigned char *nb = kl_realloc(c->stream.alloc, buf, cap, ncap);
            if (!nb) { kl_free(c->stream.alloc, buf, cap); return -1; }
            buf = nb;
            cap = ncap;
        }
        ssize_t d = c->tls->drain_output(c->tls, buf + len, cap - len);
        if (d < 0) { kl_free(c->stream.alloc, buf, cap); return -1; }
        if (d == 0) break;
        len += (size_t)d;
    }
    *out = buf;
    *outlen = len;
    *outcap = cap;
    return 0;
}

/* Encrypt an already-materialised plaintext iovec into one heap ciphertext buffer and
 * post it as a single overlapped send. Completion arrives as KL_COMP_WRITE →
 * comp_on_write. Returns 0 on posted, -1 on error (caller closes). */
static int comp_tls_post_encrypted(KlHttpConn *c, const KlIoVec *iov, int n) {
    unsigned char *cipher = NULL;
    size_t clen = 0, ccap = 0;
    if (comp_tls_encrypt_all(c, iov, n, &cipher, &clen, &ccap) < 0) return -1;
    /* The backend copies the buffer for the op's lifetime, so free ours after posting. */
    KlIoVec civ = { cipher, clen };
    int rc = kl_comp_post_send(c, &civ, 1, clen);
    kl_free(c->stream.alloc, cipher, ccap);
    return rc;
}

/* Read the next file chunk (kl_plat_file_pread — the portable file seam, not IOCP),
 * encrypt it and post it overlapped, advancing res->file_offset. Returns 1 if a chunk
 * was posted, 0 if the file is fully sent, -1 on error. Sequenced by comp_on_write so
 * memory stays bounded to one chunk regardless of file size. */
static int comp_tls_send_file_chunk(KlHttpConn *c) {
    if (c->res.file_offset >= c->res.file_size) return 0;
    size_t remaining = (size_t)(c->res.file_size - c->res.file_offset);
    char fbuf[KL_COMP_TLS_FILE_CHUNK];
    size_t to_read = remaining < sizeof(fbuf) ? remaining : sizeof(fbuf);
    ssize_t nr = kl_plat_file_pread(c->res.file_fd, fbuf, to_read,
                                    (long long)c->res.file_offset);
    if (nr <= 0) return -1;
    KlIoVec seg = { fbuf, (size_t)nr };
    if (comp_tls_post_encrypted(c, &seg, 1) < 0) return -1;
    c->res.file_offset += (uint64_t)nr;
    return 1;
}

/* Encrypt a buffered/file response and post it overlapped, so a slow client never
 * blocks the loop. Buffered bodies go in one send. A file body sends the head here; its
 * bytes stream as overlapped chunks on subsequent WRITE completions (comp_on_write →
 * send_file_chunk). Streaming is handled by comp_tls_send_stream. */
static void comp_tls_send_response(struct KlHttpServer *s, KlHttpConn *c) {
    char cl_buf[48];
    KlIoVec iov[7];
    size_t total = 0;
    /* For FILE mode this is head-only; the file bytes follow (comp_on_write). */
    int n = kl_http_response_build_iovec(&c->res, iov, 7, cl_buf, sizeof(cl_buf), &total);
    if (n < 0) { kl_comp_close(s, c); return; }
    if (comp_tls_post_encrypted(c, iov, n) < 0) kl_comp_close(s, c);
}

/* Chunked/streaming response (KL_HTTP_BODY_STREAM) over TLS. The TLS-aware streaming write
 * path (kl_http_response_send → stream_writev_all / response_drain_writer) already encrypts
 * via tls->write, but in completion mode that only appends ciphertext to the engine's
 * out ring — so flush the ring to the socket after each send. Mirrors comp_send_stream
 * (same synchronous-stream subset + head-of-line caveat); a single synchronous batch is
 * bounded by the TLS out-ring cap. Reuses only kl_http_response_send + the vtable — no
 * platform symbol. */
static void comp_tls_send_stream(struct KlHttpServer *s, KlHttpConn *c) {
    int r;
    do {
        r = kl_http_response_send(&c->res);         /* encrypts chunks into the out ring */
        if (r < 0) { kl_comp_close(s, c); return; }
        if (kl_comp_tls_flush(c) < 0) { kl_comp_close(s, c); return; }   /* ring → socket */
    } while (r == 1 && c->res.stream_ended);
    KlHttpConnState st = kl_http_conn_send_complete(c);
    if (st == KL_HTTP_CONN_READING) {
        if (kl_comp_post_recv(c) < 0) kl_comp_close(s, c);
    } else {
        kl_comp_close(s, c);
    }
}

/* Act on the state a completed request produced. */
static void comp_after_state(struct KlHttpServer *s, KlHttpConn *c, KlHttpConnState st) {
    if (st == KL_HTTP_CONN_SENDING) {
        if (c->tls) {
            if (c->res.body_mode == KL_HTTP_BODY_STREAM)
                comp_tls_send_stream(s, c);    /* flush encrypted chunks synchronously */
            else
                comp_tls_send_response(s, c);  /* buffered/file: encrypt + overlapped */
        } else if (c->res.body_mode == KL_HTTP_BODY_STREAM) {
            comp_send_stream(s, c);
        } else if (comp_send_response(c) < 0) {
            kl_comp_close(s, c);
        }
    } else if (st == KL_HTTP_CONN_READING_BODY) {
        comp_start_body_read(s, c);
    } else if (st == KL_HTTP_CONN_HTTP2) {
        /* Entered h2 via an h2c Upgrade (plaintext): upgrade_from_h1 already sent the
         * 101 + initial SETTINGS through conn_write and consumed the leftover. Reset the
         * read window so the next recv starts a fresh h2 frame buffer (the HTTP/1.1
         * upgrade request must not be re-fed), then read h2 frames. */
        c->stream.read_len = 0;
        if (c->tls && kl_comp_tls_flush(c) < 0) { kl_comp_close(s, c); return; }
        if (kl_comp_post_recv(c) < 0) kl_comp_close(s, c);
    } else if (st == KL_HTTP_CONN_WEBSOCKET) {
        /* WS upgrade done during dispatch — kl_ws_server_upgrade wrote the 101 handshake
         * (and processed any leftover frames) through conn_write; for TLS that ciphertext
         * is in the out ring. Flush it, reset the read window, then read WS frames. */
        c->stream.read_len = 0;
        if (c->tls && kl_comp_tls_flush(c) < 0) { kl_comp_close(s, c); return; }
        if (kl_comp_post_recv(c) < 0) kl_comp_close(s, c);
    } else if (st == KL_HTTP_CONN_SUSPENDED) {
        /* Handler suspended for async I/O: leave the connection parked — it holds
         * no pending op and is exempt from the idle sweep. kl_async_complete resumes it
         * (via kl_http_comp_resume) once the async op finishes. Do nothing. */
    } else {
        /* CLOSED. A TLS streaming response that finished during dispatch reports CLOSED
         * (the handler "already sent" it), but on a completion loop its chunks were
         * written into the memory-BIO out ring rather than the socket — flush them before
         * closing so the response is actually delivered. */
        if (c->tls && c->res.body_mode == KL_HTTP_BODY_STREAM)
            (void)kl_comp_tls_flush(c);
        kl_comp_close(s, c);
    }
}

/* Run the model-blind headers core on bytes already in read_buf and act. Returns 1
 * if more header bytes are needed (caller posts the next read, per its transport), 0
 * if the request was dispatched/acted-on (or the connection was closed). The TLS read loop reuses it across coalesced records
 * via pending(). */
static int comp_try_reading(struct KlHttpServer *s, KlHttpConn *c) {
    /* HTTP/2 prior-knowledge connection preface (before the HTTP/1.1 parser) —
     * mirrors the readiness read loop. A client with prior knowledge opens straight
     * into h2 by sending the 24-byte preface; upgrade and feed the leftover. */
    if (c->h2_config != NULL) {
        static const char h2_preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
        const KlHttp2ServerHooks *h2h = kl_http2_server_hooks();
        if (c->stream.read_len >= 24 && h2h && h2h->upgrade) {
            if (memcmp(c->stream.read_buf, h2_preface, 24) == 0) {
                KlHttpConnState st = (KlHttpConnState)h2h->upgrade(
                    c, &s->router, c->h2_config, c->stream.read_buf + 24, c->stream.read_len - 24);
                comp_after_state(s, c, st);
                return 0;
            }
            /* not a preface — fall through to the HTTP/1.1 parser */
        } else if (memcmp(c->stream.read_buf, h2_preface, c->stream.read_len) == 0) {
            return 1;   /* partial preface — need more bytes */
        }
    }

    size_t consumed = 0;
    KlHttp1ParseResult pr = c->parser->parse(c->parser, &c->req,
                                        c->stream.read_buf, c->stream.read_len, &consumed);
    if (pr == KL_HTTP1_PARSE_INCOMPLETE) return 1;
    if (pr == KL_HTTP1_PARSE_ERROR) { kl_comp_close(s, c); return 0; }

    KlHttpConnState st = (pr == KL_HTTP1_PARSE_HEADERS_OK)
        ? kl_http_conn_dispatch_request(c, &s->router,
                                   c->stream.read_buf + consumed, c->stream.read_len - consumed)
        : kl_http_conn_dispatch_request(c, &s->router, NULL, 0);
    comp_after_state(s, c, st);
    return 0;
}

/* Plaintext headers path: parse what's in read_buf; on need-more, post a read. */
static void comp_drive_reading(struct KlHttpServer *s, KlHttpConn *c) {
    if (comp_try_reading(s, c) == 1 && kl_comp_post_recv(c) < 0)
        kl_comp_close(s, c);
}

/* Feed a completed body read through the model-blind body core, then act. */
static void comp_drive_body(struct KlHttpServer *s, KlHttpConn *c) {
    comp_after_state(s, c, kl_http_conn_ingest_body(c, c->stream.read_len));
}

/* Drive a TLS connection after the backend has fed newly-received ciphertext to the
 * engine (kl_comp_drain → tls->feed_input). Handshake, then decrypt into read_buf and
 * run the model-blind core — reusing kl_http_conn_on_handshake / comp_try_reading /
 * kl_http_conn_ingest_body verbatim. Because TLS records can arrive coalesced in one recv,
 * each decrypt phase loops on pending() so buffered plaintext isn't stranded waiting
 * for the next network read. Ciphertext output is flushed synchronously. */
static void comp_tls_drive(struct KlHttpServer *s, KlHttpConn *c) {
    /* Upgraded connections drive through the completion-drive seam (NULL in a
     * freestanding HTTP/1.1 build, which never reaches these states). */
    if (c->state == KL_HTTP_CONN_HTTP2) {
        const KlHttp2CompHooks *h2c = kl_http2_comp_hooks();
        if (h2c && h2c->drive) h2c->drive(s, c);       /* ALPN h2 */
        return;
    }
    if (c->state == KL_HTTP_CONN_WEBSOCKET) {
        const KlWsCompHooks *wsc = kl_ws_comp_hooks();
        if (wsc && wsc->drive) wsc->drive(s, c);       /* WS/TLS */
        return;
    }

    if (c->state == KL_HTTP_CONN_TLS_HANDSHAKE) {
        KlHttpConnState st = kl_http_conn_on_handshake(c);      /* reads fed ciphertext */
        if (kl_comp_tls_flush(c) < 0) { kl_comp_close(s, c); return; }   /* push HS records */
        if (st == KL_HTTP_CONN_TLS_HANDSHAKE) {             /* still handshaking — read more */
            if (kl_comp_post_recv(c) < 0) kl_comp_close(s, c);
            return;
        }
        if (st == KL_HTTP_CONN_HTTP2) {                     /* ALPN h2 — SETTINGS flushed above */
            c->stream.read_len = 0;
            const KlHttp2CompHooks *h2c = kl_http2_comp_hooks();
            if (h2c && h2c->drive) h2c->drive(s, c);   /* process any buffered h2 frames */
            return;
        }
        if (st != KL_HTTP_CONN_READING) { kl_comp_close(s, c); return; }   /* CLOSED */
        /* Handshake complete. Fall through to the read logic below to process any
         * application data already buffered with the handshake (TLS 1.3 0-RTT / a
         * client's Finished coalesced with its first request) rather than blindly
         * waiting for another network read; a plain WANT_READ still posts a recv. */
        c->stream.read_len = 0;
    }

    if (c->state == KL_HTTP_CONN_READING_BODY) {
        for (;;) {
            ssize_t p = c->tls->read(c->tls, c->stream.fd, c->stream.read_buf, c->stream.read_cap);
            if (p < 0) { kl_comp_close(s, c); return; }
            if (p == 0) {                              /* WANT_READ — need the network */
                if (kl_comp_post_recv(c) < 0) kl_comp_close(s, c);
                return;
            }
            c->stream.read_len = (size_t)p;
            KlHttpConnState st = kl_http_conn_ingest_body(c, c->stream.read_len);
            if (st != KL_HTTP_CONN_READING_BODY) { comp_after_state(s, c, st); return; }
            if (!c->tls->pending || c->tls->pending(c->tls) == 0) {
                if (kl_comp_post_recv(c) < 0) kl_comp_close(s, c);
                return;                                /* more body needs the network */
            }
            /* else: another record is already buffered — decrypt the next chunk */
        }
    }

    /* Reading request headers: accumulate decrypted plaintext, parse, dispatch. */
    for (;;) {
        size_t off = c->stream.read_len;
        if (off >= c->stream.read_cap) { kl_comp_close(s, c); return; }   /* headers overflowed */
        ssize_t p = c->tls->read(c->tls, c->stream.fd, c->stream.read_buf + off, c->stream.read_cap - off);
        if (p < 0) { kl_comp_close(s, c); return; }
        if (p == 0) {                                  /* WANT_READ — need the network */
            if (kl_comp_post_recv(c) < 0) kl_comp_close(s, c);
            return;
        }
        c->stream.read_len += (size_t)p;
        if (comp_try_reading(s, c) == 0) return;       /* dispatched / acted / closed */
        if (!c->tls->pending || c->tls->pending(c->tls) == 0) {
            if (kl_comp_post_recv(c) < 0) kl_comp_close(s, c);
            return;                                    /* more headers need the network */
        }
        /* else: more buffered — loop and decrypt the next record */
    }
}

/* Set up an accepted connection on the completion loop: acquire a pooled KlHttpConn for `fd`, wire the
 * peer address + response allocator, choose the initial state (PROXY header / TLS handshake / HTTP
 * read), register the socket with the loop (identity = the raw KlStream), and post the first recv.
 * `lease` is the pool-owned release capability committed to this conn (consumed exactly once on
 * close via kl_http_server_conn_release); on the legacy autonomous path it is a zero lease (no credit
 * accounting). On ANY failure the fd is closed and the lease consumed — the caller must not touch
 * `fd` afterward. Shared by the post-driven listener hook and the autonomous accept path. */
static void comp_setup_accepted(struct KlHttpServer *s, KlSocketHandle fd,
                                const KlSockAddr *peer, KlSlotLease lease) {
    KlHttpConn *nc = kl_http_conn_acquire(&s->pool, fd);
    if (!nc) {
        /* Post-driven path: a credit was reserved, so a free slot is guaranteed — reaching here is
         * an invariant failure. Autonomous path: the pool is full (accept-and-drop). Either way,
         * never leak the fd or the credit: close the descriptor and consume the lease. */
        kl_sock_close(s->ev.sockets, fd);
        kl_slot_lease_release(&lease);             /* zero lease → no-op */
        return;
    }
    nc->slot_lease = lease;                        /* consumed once on close (zero lease → no-op) */
    nc->peer_source = KL_HTTP_PEER_SOCKET;
    /* The backend already converted the native accept peer to the neutral KlSockAddr
     * once at its seam; copy it straight in. KL_AF_UNSPEC = unavailable. */
    nc->stream.peer_addr = *peer;
    nc->res.alloc = &s->alloc_storage;
    (void)kl_sock_set_tcp_nodelay(nc->stream.ctx ? nc->stream.ctx->sockets : NULL, nc->stream.fd, 1);

    /* HTTP/2 over the completion loop is supported: h2_config is left intact, so
     * kl_http_conn_on_handshake may run the ALPN-h2 upgrade and comp_tls_drive / comp_after_
     * state route KL_HTTP_CONN_HTTP2 to kl_comp_http2_drive. */

    /* TLS needs the backend's memory-BIO ops (feed_input/drain_output) on a completion loop —
     * reject a backend that lacks them before doing anything else. */
    if (nc->tls && (!nc->tls->feed_input || !nc->tls->drain_output)) {
        kl_comp_close(s, nc);   /* backend can't do completion-mode TLS (releases conn + lease) */
        return;
    }

    /* PROXY protocol: a trusted-source connection sends a plaintext PROXY header
     * before any TLS/HTTP. Reached only through the PROXY seam — NULL in a
     * freestanding firmware server (proxy_protocol.c not linked; proxy_trusted_cidrs
     * never set), so this is skipped and the archive omits the hosted PROXY parser.
     * Mirrors the readiness accept gate (http_server.c). */
    const KlProxyHooks *ph = kl_proxy_hooks();
    if (s->proxy_cidr_count > 0 && ph && ph->cidr_match &&
        kl_sockaddr_family(&nc->stream.peer_addr) != KL_AF_UNSPEC &&
        ph->cidr_match(s->proxy_cidrs, s->proxy_cidr_count, &nc->stream.peer_addr)) {
        nc->state = KL_HTTP_CONN_PROXY_HEADER;   /* TLS memory-BIO enabled later, after the header */
    } else if (nc->tls) {
        /* TLS: enter the handshake state and switch the backend into completion (memory BIO)
         * mode so handshake()/read()/write() operate on fed/drained buffers, not the socket. */
        nc->state = KL_HTTP_CONN_TLS_HANDSHAKE;
        nc->tls->feed_input(nc->tls, NULL, 0);   /* enable memory-BIO mode */
    }

    /* Register the accepted socket with the loop (associate) + post the first read. Event identity
     * is the raw KlStream, matching the READ/WRITE completion target and the readiness
     * path; the owning KlHttpConn is recovered at the HTTP adapter boundary. */
    if (kl_event_add(&s->ev.loop, nc->stream.fd, KL_EVENT_READ, &nc->stream) < 0 ||
        kl_comp_post_recv(nc) < 0) {
        kl_comp_close(s, nc);
    }
}

/* ── Completion accept adapter: drive the shared KlListener over the completion accept
 * ops + the split-credit pool, for POST-DRIVEN backends (io_uring/IOCP/pollcomp). The listener
 * reserves one pool credit per posted accept and PAUSES (stops posting; the kernel TCP backlog
 * queues the rest) instead of accept-and-dropping when the pool is full. Autonomous backends
 * (EFI/lwip, prime_accepts → 0) keep their own capacity gate and never install this. ───────────── */
static int comp_accept_reserve(void *ctx) {
    KlHttpServer *s = ctx;
    return kl_http_conn_pool_reserve(&s->pool);
}
static void comp_accept_release(void *ctx) {
    KlHttpServer *s = ctx;
    kl_http_conn_pool_return_credit(&s->pool);               /* free_credits++ ... */
    kl_listener_notify_slot_free(&s->accept_listener);  /* ...then resume the accept if paused */
}
static int comp_accept_arm(void *ctx) {
    KlHttpServer *s = ctx;
    return kl_comp_post_accept(s);                       /* post ONE accept op */
}
static void comp_accept_cancel(void *ctx) {
    KlHttpServer *s = ctx;
    /* Batch-cancel every posted accept on the listen fd; each completes as failed
     * (kl_listener_on_accept_failed → returns its credit + decrements inflight), so the
     * listener detaches only once every posted accept has retired (confirmed detachment). */
    kl_comp_cancel(&s->ev, s->listen_fd);
}
static void comp_accept_dispose(void *ctx, KlSocketHandle fd) {
    kl_sock_close(((KlHttpServer *)ctx)->ev.sockets, fd);   /* no local var: read-only, keep ctx void* */
}
static void comp_accept_on_accept(void *ctx, KlSocketHandle fd, KlSlotLease lease) {
    KlHttpServer *s = ctx;
    /* Commit the reserved credit to a KlHttpConn. The peer was stashed on the server just before
     * kl_listener_on_accepted (single-threaded, consumed immediately) — mirrors http_server.c. */
    comp_setup_accepted(s, fd, &s->accept_pending_peer, lease);
}

/* Init + start the completion-mode KlListener with the backend's accept window. The reserve/release
 * hooks are the split-credit pool ops (same accounting as readiness); arm posts one accept, cancel
 * batch-cancels the whole window. Returns 0 on success, -1 on a listener setup failure. */
static int comp_accept_listener_start(KlHttpServer *s, int window) {
    s->accept_alive = 1;                                /* liveness token live before any lease issues */
    KlListenerHooks h = {
        .reserve = comp_accept_reserve, .release = comp_accept_release, .credit_ctx = s,
        .liveness = &s->accept_alive,
        .arm_accept = comp_accept_arm, .cancel_accept = comp_accept_cancel,
        .on_accept = comp_accept_on_accept, .dispose_fd = comp_accept_dispose,
    };
    if (kl_listener_init(&s->accept_listener, /*completion_mode=*/1, &h, s) < 0) return -1;
    if (window > 1 && kl_listener_set_accept_window(&s->accept_listener, window) < 0) return -1;
    return kl_listener_start(&s->accept_listener);
}

static void comp_on_accept(struct KlHttpServer *s, const KlCompletionEvent *ev) {
#ifndef KEEL_FREESTANDING
    /* Register the completion-mode ws/h2 drive tables once, and (the reason this is
     * an explicit installer call, not a constructor) pull completion_ws.o /
     * completion_h2.o out of the static archive — the seam's function-pointer
     * dispatch above references no completion-TU symbol, so nothing else would.
     * Guarded because a freestanding HTTP/1.1 archive links neither completion TU
     * (nor their installers); it never upgrades, so the dispatch sees NULL hooks.
     * This is the one irreducible build-axis line the pure seam can't absorb. */
    static int comp_hooks_done = 0;
    if (!comp_hooks_done) {
        kl_ws_comp_hooks_install();
        kl_http2_comp_hooks_install();
        comp_hooks_done = 1;
    }
#endif

    if (s->accept_via_listener) {
        /* Post-driven backend: route the accept through the counted KlListener. A failed/cancelled
         * accept retires one posted accept and returns its reserved credit; a good one commits that
         * credit to a KlHttpConn (comp_accept_on_accept) and the listener tops the window back up — or
         * PAUSEs if the pool is now full (the kernel backlog queues; no accept-and-drop). */
        if (!ev->ok || !kl_handle_valid(ev->accepted_fd)) {
            kl_listener_on_accept_failed(&s->accept_listener, -1);
            return;
        }
        s->accept_pending_peer = ev->peer;   /* stashed for the on_accept hook (single-threaded) */
        kl_listener_on_accepted(&s->accept_listener, ev->accepted_fd);
        return;
    }

    /* Autonomous backend (EFI/lwip): the backend gates capacity itself (it does not accept into a
     * full pool), so commit directly with a zero lease and top the backlog up with one post. */
    if (ev->ok && kl_handle_valid(ev->accepted_fd)) {
        KlSlotLease none = {0};
        comp_setup_accepted(s, ev->accepted_fd, &ev->peer, none);
    }
    (void)kl_comp_post_accept(s);   /* keep the accept backlog topped up */
}

/* PROXY protocol header phase over the completion loop: the plaintext header arrived in
 * read_buf via an overlapped recv. Parse it (kl_http_conn_ingest_proxy), then enter the real
 * initial state — TLS handshake (enable the memory-BIO, feed the buffered ClientHello) or HTTP
 * read — processing any bytes that followed the header this tick. The header recv is plaintext
 * even for a TLS conn (post_recv skips the TLS branch while state == KL_HTTP_CONN_PROXY_HEADER). */
static void comp_drive_proxy(struct KlHttpServer *s, KlHttpConn *c) {
    int consumed = kl_http_conn_ingest_proxy(c, c->stream.read_len);
    if (consumed == -1) { kl_comp_close(s, c); return; }       /* malformed / oversized */
    if (consumed == -2) {                                    /* need more header bytes */
        if (kl_comp_post_recv(c) < 0) kl_comp_close(s, c);
        return;
    }
    if (consumed > 0) {                                      /* drop the header, keep remainder */
        c->stream.read_len -= (size_t)consumed;
        if (c->stream.read_len > 0)
            memmove(c->stream.read_buf, c->stream.read_buf + consumed, c->stream.read_len);
    }

    if (c->tls) {
        c->state = KL_HTTP_CONN_TLS_HANDSHAKE;
        c->tls->feed_input(c->tls, NULL, 0);                /* enable memory-BIO mode */
        if (c->stream.read_len > 0) {                              /* buffered ClientHello ciphertext */
            if (c->tls->feed_input(c->tls, c->stream.read_buf, c->stream.read_len) < 0) {
                kl_comp_close(s, c); return;
            }
            c->stream.read_len = 0;
        }
        comp_tls_drive(s, c);
    } else {
        c->state = KL_HTTP_CONN_READING;
        if (c->stream.read_len > 0)
            comp_drive_reading(s, c);                       /* parse buffered HTTP bytes */
        else if (kl_comp_post_recv(c) < 0)
            kl_comp_close(s, c);
    }
}

static void comp_on_read(struct KlHttpServer *s, const KlCompletionEvent *ev) {
    KlHttpConn *c = conn_of_stream(ev->target);
    if (!ev->ok || ev->bytes == 0) { kl_comp_close(s, c); return; }   /* peer closed */
    /* PROXY header phase (plaintext, before TLS/HTTP) — must run before the TLS branch since a
     * PROXY+TLS conn has c->tls set but hasn't started the handshake yet. The recv used the
     * plaintext read_buf (kl_comp_post_recv picks it while state == PROXY_HEADER). */
    if (c->state == KL_HTTP_CONN_PROXY_HEADER) {
        c->stream.read_len += ev->bytes;
        comp_drive_proxy(s, c);
        return;
    }
    /* TLS: this recv's ciphertext landed in c->comp_cipher (ev->bytes counts ciphertext). Feed
     * it to the engine HERE — the backend does raw I/O only and no longer knows about TLS — then
     * handshake / decrypt. read_buf holds plaintext and is written by comp_tls_drive, not here. */
    if (c->tls) {
        if (c->tls->feed_input(c->tls, c->comp_cipher, ev->bytes) < 0) { kl_comp_close(s, c); return; }
        comp_tls_drive(s, c);
        return;
    }
    c->stream.read_len += ev->bytes;
    /* Body reads use a fresh sliding window (read_len was reset to 0 before the
     * post), so read_len == the bytes just received. Headers accumulate. */
    if (c->state == KL_HTTP_CONN_HTTP2) {
        const KlHttp2CompHooks *h2c = kl_http2_comp_hooks();
        if (h2c && h2c->drive) h2c->drive(s, c);   /* plaintext h2 (h2c) frames */
    }
    else if (c->state == KL_HTTP_CONN_WEBSOCKET) {
        const KlWsCompHooks *wsc = kl_ws_comp_hooks();
        if (wsc && wsc->drive) wsc->drive(s, c);   /* plaintext WebSocket frames */
    }
    else if (c->state == KL_HTTP_CONN_READING_BODY)
        comp_drive_body(s, c);
    else
        comp_drive_reading(s, c);
}

static void comp_on_write(struct KlHttpServer *s, const KlCompletionEvent *ev) {
    KlHttpConn *c = conn_of_stream(ev->target);
    if (!ev->ok || ev->bytes == 0) { kl_comp_close(s, c); return; }
    /* h2 output send completed: the frames produced by the last feed are out —
     * read the next frames. Deferring the recv until here means at most one h2 send is
     * in flight, so overlapped output cannot reorder frames. */
    if (c->state == KL_HTTP_CONN_HTTP2) {
        if (kl_comp_post_recv(c) < 0) kl_comp_close(s, c);
        return;
    }
    /* TLS file body still streaming: the head (or the previous chunk) just went out —
     * post the next encrypted file chunk. HEAD requests carry no body. */
    if (c->tls && c->res.body_mode == KL_HTTP_BODY_FILE && !c->res.head_request) {
        int r = comp_tls_send_file_chunk(c);
        if (r < 0) { kl_comp_close(s, c); return; }
        if (r == 1) return;                    /* another chunk now in flight */
        /* r == 0: file fully sent — fall through to complete the response */
    }
    /* Plaintext streaming: the posted stream chunk is out. Pump the next buffered
     * chunk (a slow client's remainder, or bytes an async producer wrote meanwhile), else
     * complete once the stream ended and the buffer drained. */
    if (!c->tls && c->res.body_mode == KL_HTTP_BODY_STREAM && c->res.drain_enabled) {
        c->res.stream_inflight = 0;
        comp_stream_pump(s, c);
        return;
    }
    /* The backend only reports a WRITE once the whole response is out (it handles
     * partial sends internally). Response fully sent: keep-alive reset or close. */
    KlHttpConnState st = kl_http_conn_send_complete(c);
    if (st == KL_HTTP_CONN_READING) {
        if (kl_comp_post_recv(c) < 0) kl_comp_close(s, c);
    } else {
        kl_comp_close(s, c);
    }
}

/* Recover the server from its embedded event ctx. Every pooled connection's ctx is
 * &server->ev, and conn/accept completions only occur on a server's loop — so this
 * containerof is valid wherever it is used, without a public KlHttpConn/KlHttpServer field. */
static inline struct KlHttpServer *server_of_ctx(struct KlEventCtx *ctx) {
    return (struct KlHttpServer *)((char *)ctx - offsetof(struct KlHttpServer, ev));
}

/* The comp_conn_dispatch hook (completion_core.c → here): route a TCP conn completion
 * (ACCEPT/READ/WRITE) to its server handler. Registered on the ctx in
 * kl_http_comp_run, so completion_core.c's kl_comp_run reaches the server
 * without a static reference — a client-only build links neither this TU nor these
 * handlers. The event is a const KlCompletionEvent* (opaque const void* at the hook seam,
 * exactly as KlEventOps.completion). */
static void comp_server_conn_dispatch(struct KlEventCtx *ctx, const void *evp) {
    const KlCompletionEvent *ev = evp;
    switch (ev->kind) {
    case KL_COMP_ACCEPT:
        /* ACCEPT recovers the server from the event ctx, exactly
         * like READ/WRITE below — server_of_ctx is a containerof over &server->ev and is always
         * valid on a server loop (see its contract), so it needs no NULL guard. */
        comp_on_accept(server_of_ctx(ctx), ev);
        break;
    /* READ/WRITE recover the server from the ctx (their target is the KlStream, which
     * comp_on_read/write resolve to a conn). */
    case KL_COMP_READ:   comp_on_read(server_of_ctx(ctx), ev);   break;
    case KL_COMP_WRITE:  comp_on_write(server_of_ctx(ctx), ev);  break;
    default: break;   /* core routes only ACCEPT/READ/WRITE here */
    }
}

/* Resume a suspended connection on a completion loop after an async op completed:
 * drive the completion send path for the state on_resume produced — the same path a
 * normal request's dispatch takes. The completion seam kl_async_complete calls (only on a
 * completion loop). Keeps the async runtime free of completion knowledge. */
void kl_http_comp_resume(struct KlHttpServer *s, struct KlHttpConn *conn) {
    if (conn->state == KL_HTTP_CONN_READING) {   /* handler yielded without a response — read on */
        conn->stream.read_len = 0;
        if (kl_comp_post_recv(conn) < 0) kl_comp_close(s, conn);
        return;
    }
    comp_after_state(s, conn, conn->state);
}

/* Re-arm a body read after read-side flow control resumes (kl_http_request_resume_body → the
 * completion seam on a completion loop): post a fresh recv; on failure release via the normal
 * completion path. Mirrors comp_start_body_read's post (read_len was reset when the body read
 * started; the next recv appends into read_buf as usual). */
void kl_http_comp_post_read(struct KlHttpConn *c) {
    if (kl_comp_post_recv(c) < 0)
        kl_comp_close(server_of_ctx(c->stream.ctx), c);
}

/* The server's completion seam entry: install the conn-dispatch hook (idempotent, the single
 * always-hit server-completion-init point), set up the accept path once from the backend's window,
 * then run one generic tick over its shared event ctx.
 *
 * One-time accept setup: prime_accepts returns the backend's accept window. window>=1
 * is a POST-DRIVEN backend — start the completion KlListener, which reserves one pool credit per
 * posted accept and PAUSEs (kernel backlog queues) when the pool is full instead of accept-and-
 * dropping. window==0 is an AUTONOMOUS backend (EFI/lwip) that gates capacity in its own drain; it
 * gets no listener and is re-primed each tick so a freed slot re-opens its gate (EFI top-up). */
int kl_http_comp_run(struct KlHttpServer *s, int timeout_ms) {
    s->ev.comp_conn_dispatch = comp_server_conn_dispatch;
    if (!s->accept_setup_done) {
        int window = kl_comp_prime_accepts(s);   /* one-time setup; returns the accept window */
        if (window < 0) return -1;               /* setup NOT latched — a retry re-attempts */
        if (window >= 1) {
            /* Post-driven: start the listener BEFORE latching setup_done, so a start failure
             * leaves setup unlatched (honest retry / error cleanup). prime_accepts is idempotent
             * (returns the same window), so the retry re-primes cheaply then re-starts. */
            if (comp_accept_listener_start(s, window) < 0) return -1;
            s->accept_via_listener = 1;
        }
        /* window == 0: autonomous backend — prime already armed its own gate. */
        s->accept_setup_done = 1;                /* latch only after setup fully succeeded */
    } else if (!s->accept_via_listener) {
        if (kl_comp_prime_accepts(s) < 0) return -1;   /* autonomous: re-arm the gate each tick */
    }
    return kl_comp_run(&s->ev, KL_COMP_MAX_EVENTS, timeout_ms) < 0 ? -1 : 0;
}

/* Teardown accept quiescence: force every posted accept to completion, then reap
 * to CONFIRMED DETACHMENT with a TEARDOWN-SPECIFIC dispatcher — NOT the live kl_comp_run tick.
 *
 * Called from kl_http_server_free() AFTER higher-level consumers (async ops, file_io) are torn down, so
 * the live tick is unsafe here: kl_comp_run would advance the HTTP state machine on READ/WRITE, run
 * watcher/connect/UDP callbacks, and fire timers — invoking application/consumer code against
 * logically destroyed state. Instead this drains raw completions (kl_comp_drain — no dispatch) and:
 *   - routes ONLY KL_COMP_ACCEPT to the server dispatcher, so the KlListener retires each posted
 *     accept (a shutdown-race ok=1 fd is disposed by the CLOSING listener) and reaches detachment;
 *   - DROPS every other completion (READ/WRITE/UDP/WATCHER/CONNECT) without dispatch — the backend
 *     drain already freed each op record, and the owning conn/udp/watcher is freed by its own
 *     teardown, so nothing leaks and no callback, HTTP step, or timer runs;
 *   - never calls kl_timer_fire.
 * Terminating: kl_comp_shutdown_accepts guaranteed every posted accept will complete, and each
 * blocking kl_comp_drain reaps the pending completions. Returns 0, or -1 if the force could not be
 * guaranteed (caller leaves the backend close as the physical backstop). The listen socket must be
 * closed by the caller first (forces IOCP AcceptEx completion). */
int kl_http_comp_quiesce_accepts(struct KlHttpServer *s) {
    if (kl_comp_shutdown_accepts(s) < 0) return -1;
    while (!kl_listener_is_detached(&s->accept_listener)) {
        KlCompletionEvent evs[KL_COMP_MAX_EVENTS];
        int n = kl_comp_drain(&s->ev, evs, KL_COMP_MAX_EVENTS, KL_ACCEPT_QUIESCE_TIMEOUT_MS);
        if (n < 0) return -1;
        for (int i = 0; i < n; i++)
            if (evs[i].kind == KL_COMP_ACCEPT)
                comp_server_conn_dispatch(&s->ev, &evs[i]);   /* accept → listener retire only */
        /* all other kinds: op already freed by the drain; intentionally not dispatched */
    }
    return 0;
}
