/*
 * completion_driver.c — the platform-independent completion connection driver
 * (PAL Phase 8b). The completion-axis counterpart of the server's readiness loop:
 * the connection state machine that runs over *completion* events, reusing the
 * model-blind protocol core (conn_internal.h / response_internal.h) and the
 * abstract completion axis (completion.h). Contains NO platform (Win32/IOCP) type
 * or symbol — a future io_uring-completion / POSIX-AIO backend reuses it verbatim.
 * This TU is linked only on completion builds (Makefile), where a backend supplies
 * completion.h; it defines the io_engine seam's kl_io_engine_run_completion().
 * See docs/phase8b_iocp_breadth_design.md.
 */
#include <keel/server.h>
#include <keel/connection.h>
#include <keel/tls.h>            /* KlTls vtable ops — TLS-over-completion (8b-5b) */
#include "internal.h"            /* kl_server_conn_release */
#include "conn_internal.h"       /* kl_conn_dispatch_request / kl_conn_send_complete */
#include "response_internal.h"   /* kl_response_build_iovec */
#include "completion.h"          /* the abstract completion axis */
#include "io_engine.h"           /* kl_io_engine_run_completion (the seam) */
#include "socket.h"              /* kl_sock_* (close / tcp_nodelay via the seam) */
#include "platform.h"            /* kl_plat_file_pread — TLS file body chunks (8c-2) */
#include <keel/udp.h>            /* KlUdp (KL_COMP_UDP_RECV target) */
#include "udp_internal.h"        /* kl_udp_comp_on_recv */
#include <string.h>
#include <stddef.h>              /* offsetof (server_of_ctx containerof) */
#include <stdint.h>              /* SIZE_MAX (h2 output capture growth guard) */

#define KL_COMP_MAX_EVENTS 64

/* Stack chunk for draining the TLS engine's outgoing ciphertext to the socket. */
#define KL_TLS_FLUSH_CHUNK 16384

/* Plaintext file bytes read + encrypted per overlapped send for a TLS file body. */
#define KL_COMP_TLS_FILE_CHUNK 16384

static void comp_close(struct KlServer *s, KlConn *c) {
    kl_server_conn_release(s, c);   /* closes the socket + returns the pool slot */
}

/* Serialize the response head and post it: buffered body inline via WSASend, or a
 * file body via the backend's zero-copy file send (TransmitFile) after the head.
 * The backend copies + owns the head bytes for the op's lifetime. */
static int comp_send_response(KlConn *c) {
    char cl_buf[48];
    KlIoVec iov[7];
    size_t total = 0;
    int n = kl_response_build_iovec(&c->res, iov, 7, cl_buf, sizeof(cl_buf), &total);
    if (n < 0) return -1;
    /* FILE mode: the iovec is head-only; append the file bytes with sendfile. A HEAD
     * request has no body, so it falls through to the plain head send. */
    if (c->res.body_mode == KL_BODY_FILE && !c->res.head_request)
        return kl_comp_post_sendfile(c, iov, n, total,
                                     c->res.file_fd, (uint64_t)c->res.file_size);
    return kl_comp_post_send(c, iov, n, total);
}

/* Start (or continue) a request-body read: reset the sliding-window buffer and post
 * the next receive. The body core (kl_conn_ingest_body) feeds read_buf[0..nread]. */
static void comp_start_body_read(struct KlServer *s, KlConn *c) {
    c->read_len = 0;
    if (kl_comp_post_recv(c) < 0) comp_close(s, c);
}

/* Chunked/streaming response (KL_BODY_STREAM) over the completion loop. The
 * streaming write path (kl_response_send → drain / kl_stream_write) routes through
 * the socket seam, which on a completion backend is a synchronous send on the
 * (blocking) accepted socket — so a fully-produced stream is already sent, and any
 * drained remainder is flushed here. Then complete like any response.
 *
 * Scope: synchronous streams (the handler produces the whole body during dispatch).
 * Async / long-lived streams (data over later events) are not driven over the
 * completion loop in this subset. NOTE: the sends are synchronous — a slow client
 * can head-of-line block; true overlapped chunk sends are a later refinement. This
 * reuses only the existing internal kl_response_send (no platform symbol). */
static void comp_send_stream(struct KlServer *s, KlConn *c) {
    int r;
    do { r = kl_response_send(&c->res); } while (r == 1 && c->res.stream_ended);
    if (r < 0) { comp_close(s, c); return; }
    KlConnState st = kl_conn_send_complete(c);
    if (st == KL_CONN_READING) {
        if (kl_comp_post_recv(c) < 0) comp_close(s, c);
    } else {
        comp_close(s, c);
    }
}

/* TLS-over-completion output. In completion mode the mbedTLS BIO writes outgoing
 * ciphertext into an in-memory ring (drain_output) rather than the socket; push that
 * ciphertext out with a synchronous send on the (blocking) accepted socket. Used for
 * bounded, latency-insensitive handshake records — the client is actively handshaking,
 * not slow-reading — so no overlapped send is warranted. Response bodies go out
 * overlapped (comp_tls_send_response) to avoid any head-of-line stall. */
static int comp_tls_flush(KlConn *c) {
    const KlSocketProvider *sp = c->ctx ? c->ctx->sockets : NULL;
    unsigned char buf[KL_TLS_FLUSH_CHUNK];
    ssize_t n;
    while ((n = c->tls->drain_output(c->tls, buf, sizeof(buf))) > 0) {
        size_t off = 0;
        while (off < (size_t)n) {
            ssize_t w = kl_sock_send(sp, c->fd, buf + off, (size_t)n - off);
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
static int comp_tls_encrypt_all(KlConn *c, const KlIoVec *iov, int n,
                                unsigned char **out, size_t *outlen, size_t *outcap) {
    unsigned char *buf = NULL;
    size_t cap = 0, len = 0;

    for (int i = 0; i < n; i++) {
        size_t off = 0;
        while (off < iov[i].len) {
            ssize_t w = c->tls->write(c->tls, c->fd,
                                      (const char *)iov[i].base + off, iov[i].len - off);
            if (w < 0) goto fail;
            if (w > 0) off += (size_t)w;
            /* Drain the ciphertext this write produced (also frees ring space, so a
             * WANT_WRITE — w==0, ring at cap — makes progress on the next iteration). */
            for (;;) {
                if (len == cap) {
                    size_t ncap = cap ? cap * 2 : 8192;
                    unsigned char *nb = kl_realloc(c->alloc, buf, cap, ncap);
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
    kl_free(c->alloc, buf, cap);
    return -1;
}

/* Drain the TLS engine's pending outgoing ciphertext into one heap buffer (grow as it
 * fills). On success returns 0 with out/outlen/outcap set (caller frees outcap bytes),
 * -1 on error. Used to post h2 output overlapped (8d-3) instead of a synchronous flush. */
static int comp_tls_drain_output(KlConn *c, unsigned char **out, size_t *outlen, size_t *outcap) {
    unsigned char *buf = NULL;
    size_t cap = 0, len = 0;
    for (;;) {
        if (len == cap) {
            size_t ncap = cap ? cap * 2 : 8192;
            unsigned char *nb = kl_realloc(c->alloc, buf, cap, ncap);
            if (!nb) { kl_free(c->alloc, buf, cap); return -1; }
            buf = nb;
            cap = ncap;
        }
        ssize_t d = c->tls->drain_output(c->tls, buf + len, cap - len);
        if (d < 0) { kl_free(c->alloc, buf, cap); return -1; }
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
static int comp_tls_post_encrypted(KlConn *c, const KlIoVec *iov, int n) {
    unsigned char *cipher = NULL;
    size_t clen = 0, ccap = 0;
    if (comp_tls_encrypt_all(c, iov, n, &cipher, &clen, &ccap) < 0) return -1;
    /* The backend copies the buffer for the op's lifetime, so free ours after posting. */
    KlIoVec civ = { cipher, clen };
    int rc = kl_comp_post_send(c, &civ, 1, clen);
    kl_free(c->alloc, cipher, ccap);
    return rc;
}

/* Read the next file chunk (kl_plat_file_pread — the portable file seam, not IOCP),
 * encrypt it and post it overlapped, advancing res->file_offset. Returns 1 if a chunk
 * was posted, 0 if the file is fully sent, -1 on error. Sequenced by comp_on_write so
 * memory stays bounded to one chunk regardless of file size. */
static int comp_tls_send_file_chunk(KlConn *c) {
    if (c->res.file_offset >= c->res.file_size) return 0;
    size_t remaining = (size_t)(c->res.file_size - c->res.file_offset);
    char fbuf[KL_COMP_TLS_FILE_CHUNK];
    size_t to_read = remaining < sizeof(fbuf) ? remaining : sizeof(fbuf);
    ssize_t nr = kl_plat_file_pread(c->res.file_fd, fbuf, to_read,
                                    (long long)c->res.file_offset);
    if (nr <= 0) return -1;
    KlIoVec seg = { fbuf, (size_t)nr };
    if (comp_tls_post_encrypted(c, &seg, 1) < 0) return -1;
    c->res.file_offset += (off_t)nr;
    return 1;
}

/* Encrypt a buffered/file response and post it overlapped, so a slow client never
 * blocks the loop. Buffered bodies go in one send. A file body sends the head here; its
 * bytes stream as overlapped chunks on subsequent WRITE completions (comp_on_write →
 * send_file_chunk). Streaming is handled by comp_tls_send_stream. */
static void comp_tls_send_response(struct KlServer *s, KlConn *c) {
    char cl_buf[48];
    KlIoVec iov[7];
    size_t total = 0;
    /* For FILE mode this is head-only; the file bytes follow (comp_on_write). */
    int n = kl_response_build_iovec(&c->res, iov, 7, cl_buf, sizeof(cl_buf), &total);
    if (n < 0) { comp_close(s, c); return; }
    if (comp_tls_post_encrypted(c, iov, n) < 0) comp_close(s, c);
}

/* Chunked/streaming response (KL_BODY_STREAM) over TLS. The TLS-aware streaming write
 * path (kl_response_send → stream_writev_all / response_drain_writer) already encrypts
 * via tls->write, but in completion mode that only appends ciphertext to the engine's
 * out ring — so flush the ring to the socket after each send. Mirrors comp_send_stream
 * (same synchronous-stream subset + head-of-line caveat); a single synchronous batch is
 * bounded by the TLS out-ring cap. Reuses only kl_response_send + the vtable — no
 * platform symbol. */
static void comp_tls_send_stream(struct KlServer *s, KlConn *c) {
    int r;
    do {
        r = kl_response_send(&c->res);         /* encrypts chunks into the out ring */
        if (r < 0) { comp_close(s, c); return; }
        if (comp_tls_flush(c) < 0) { comp_close(s, c); return; }   /* ring → socket */
    } while (r == 1 && c->res.stream_ended);
    KlConnState st = kl_conn_send_complete(c);
    if (st == KL_CONN_READING) {
        if (kl_comp_post_recv(c) < 0) comp_close(s, c);
    } else {
        comp_close(s, c);
    }
}

/* Driver-owned h2 output capture (8d-4): the completion driver installs this writer via
 * the h2 output seam (kl_h2_server_set_writer) around a feed, so the session's produced
 * frames land in one buffer the driver posts as a single overlapped send. The buffer +
 * grow logic live here, not h2.c — h2.c only exposes the generic writer seam. */
typedef struct { KlAllocator *alloc; char *buf; size_t len, cap; int err; } CompH2Cap;
static ssize_t comp_h2_capture_write(void *ctx, const void *data, size_t len) {
    CompH2Cap *cp = ctx;
    if (cp->err) return -1;
    if (len > SIZE_MAX - cp->len) { cp->err = 1; return -1; }
    if (cp->len + len > cp->cap) {
        size_t ncap = cp->cap ? cp->cap : 4096;
        while (ncap < cp->len + len) {
            if (ncap > SIZE_MAX / 2) { cp->err = 1; return -1; }
            ncap *= 2;
        }
        char *nb = kl_realloc(cp->alloc, cp->buf, cp->cap, ncap);
        if (!nb) { cp->err = 1; return -1; }
        cp->buf = nb;
        cp->cap = ncap;
    }
    memcpy(cp->buf + cp->len, data, len);
    cp->len += len;
    return (ssize_t)len;
}

/* Drive an established HTTP/2 connection over the completion loop (8d-1). Feed received
 * plaintext to the h2 session via kl_h2_server_feed (which parses frames and flushes
 * produced output through conn_write — a synchronous blocking send for plaintext, the
 * memory-BIO ring for TLS), then read more. The h2 session vtable and kl_h2_server_feed
 * are reused verbatim — this only inverts the transport, exactly as the HTTP/1.1 path
 * does. For TLS the received ciphertext was already fed to the engine (kl_comp_drain);
 * loop on pending() so coalesced records aren't stranded. */
static void comp_h2_drive(struct KlServer *s, KlConn *c) {
    if (c->tls) {
        /* Decrypt + feed every currently-available record (the h2 session writes its
         * output ciphertext into the memory-BIO out ring via conn_write→tls->write),
         * then drain that ring and post it as ONE ordered overlapped send (8d-3) —
         * deferring the next recv to comp_on_write, so at most one h2 send is in flight
         * and frames cannot reorder. */
        for (;;) {
            ssize_t p = c->tls->read(c->tls, c->fd, c->read_buf, c->read_cap);
            if (p < 0) { comp_close(s, c); return; }
            if (p == 0) break;                         /* WANT_READ — batch done */
            KlConnState st = kl_h2_server_feed(c, c->read_buf, (size_t)p);
            if (st != KL_CONN_HTTP2) { comp_close(s, c); return; }
            if (!c->tls->pending || c->tls->pending(c->tls) == 0) break;
        }
        unsigned char *cipher = NULL;
        size_t clen = 0, ccap = 0;
        if (comp_tls_drain_output(c, &cipher, &clen, &ccap) < 0) { comp_close(s, c); return; }
        if (clen > 0) {
            KlIoVec iov = { cipher, clen };
            int rc = kl_comp_post_send(c, &iov, 1, clen);
            kl_free(c->alloc, cipher, ccap);
            if (rc < 0) comp_close(s, c);
        } else {
            kl_free(c->alloc, cipher, ccap);
            if (kl_comp_post_recv(c) < 0) comp_close(s, c);
        }
        return;
    }
    /* Plaintext: the received frame bytes are already in read_buf (comp_on_read added
     * this recv's bytes). Capture the session's output (8d-3) so all frames it produces
     * this feed go out as ONE ordered overlapped send; defer the next recv until that
     * send completes (comp_on_write) so at most one h2 send is ever in flight — frames
     * must not reorder. */
    CompH2Cap cap = { c->alloc, NULL, 0, 0, 0 };
    kl_h2_server_set_writer(c, comp_h2_capture_write, &cap);
    KlConnState st = kl_h2_server_feed(c, c->read_buf, c->read_len);
    kl_h2_server_set_writer(c, NULL, NULL);       /* restore the default socket writer */
    c->read_len = 0;
    if (st != KL_CONN_HTTP2 || cap.err) {
        kl_free(c->alloc, cap.buf, cap.cap);
        comp_close(s, c);
        return;
    }
    if (cap.len > 0) {
        KlIoVec iov = { cap.buf, cap.len };
        int rc = kl_comp_post_send(c, &iov, 1, cap.len);
        kl_free(c->alloc, cap.buf, cap.cap);
        if (rc < 0) comp_close(s, c);
    } else {
        kl_free(c->alloc, cap.buf, cap.cap);
        if (kl_comp_post_recv(c) < 0) comp_close(s, c);   /* no output — read more */
    }
}

/* Act on the state a completed request produced. */
static void comp_after_state(struct KlServer *s, KlConn *c, KlConnState st) {
    if (st == KL_CONN_SENDING) {
        if (c->tls) {
            if (c->res.body_mode == KL_BODY_STREAM)
                comp_tls_send_stream(s, c);    /* flush encrypted chunks synchronously */
            else
                comp_tls_send_response(s, c);  /* buffered/file: encrypt + overlapped */
        } else if (c->res.body_mode == KL_BODY_STREAM) {
            comp_send_stream(s, c);
        } else if (comp_send_response(c) < 0) {
            comp_close(s, c);
        }
    } else if (st == KL_CONN_READING_BODY) {
        comp_start_body_read(s, c);
    } else if (st == KL_CONN_HTTP2) {
        /* Entered h2 via an h2c Upgrade (plaintext): upgrade_from_h1 already sent the
         * 101 + initial SETTINGS through conn_write and consumed the leftover. Reset the
         * read window so the next recv starts a fresh h2 frame buffer (the HTTP/1.1
         * upgrade request must not be re-fed), then read h2 frames. */
        c->read_len = 0;
        if (c->tls && comp_tls_flush(c) < 0) { comp_close(s, c); return; }
        if (kl_comp_post_recv(c) < 0) comp_close(s, c);
    } else {
        /* CLOSED, or SUSPENDED (async handlers are not driven over the completion loop
         * in this subset) — close rather than mis-serve. */
        comp_close(s, c);
    }
}

/* Run the model-blind headers core on bytes already in read_buf and act. Returns 1
 * if more header bytes are needed (caller posts the next read, per its transport), 0
 * if the request was dispatched/acted-on (or the connection was closed). Split out so
 * the TLS read loop can reuse it across coalesced records via pending(). */
static int comp_try_reading(struct KlServer *s, KlConn *c) {
    /* HTTP/2 prior-knowledge connection preface (before the HTTP/1.1 parser, 8d-2) —
     * mirrors the readiness read loop. A client with prior knowledge opens straight
     * into h2 by sending the 24-byte preface; upgrade and feed the leftover. */
    if (c->h2_config != NULL) {
        static const char h2_preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
        if (c->read_len >= 24) {
            if (memcmp(c->read_buf, h2_preface, 24) == 0) {
                KlConnState st = (KlConnState)kl_h2_server_upgrade(
                    c, &s->router, c->h2_config, c->read_buf + 24, c->read_len - 24);
                comp_after_state(s, c, st);
                return 0;
            }
            /* not a preface — fall through to the HTTP/1.1 parser */
        } else if (memcmp(c->read_buf, h2_preface, c->read_len) == 0) {
            return 1;   /* partial preface — need more bytes */
        }
    }

    size_t consumed = 0;
    KlParseResult pr = c->parser->parse(c->parser, &c->req,
                                        c->read_buf, c->read_len, &consumed);
    if (pr == KL_PARSE_INCOMPLETE) return 1;
    if (pr == KL_PARSE_ERROR) { comp_close(s, c); return 0; }

    KlConnState st = (pr == KL_PARSE_HEADERS_OK)
        ? kl_conn_dispatch_request(c, &s->router,
                                   c->read_buf + consumed, c->read_len - consumed)
        : kl_conn_dispatch_request(c, &s->router, NULL, 0);
    comp_after_state(s, c, st);
    return 0;
}

/* Plaintext headers path: parse what's in read_buf; on need-more, post a read. */
static void comp_drive_reading(struct KlServer *s, KlConn *c) {
    if (comp_try_reading(s, c) == 1 && kl_comp_post_recv(c) < 0)
        comp_close(s, c);
}

/* Feed a completed body read through the model-blind body core, then act. */
static void comp_drive_body(struct KlServer *s, KlConn *c) {
    comp_after_state(s, c, kl_conn_ingest_body(c, c->read_len));
}

/* Drive a TLS connection after the backend has fed newly-received ciphertext to the
 * engine (kl_comp_drain → tls->feed_input). Handshake, then decrypt into read_buf and
 * run the model-blind core — reusing kl_conn_on_handshake / comp_try_reading /
 * kl_conn_ingest_body verbatim. Because TLS records can arrive coalesced in one recv,
 * each decrypt phase loops on pending() so buffered plaintext isn't stranded waiting
 * for the next network read. Ciphertext output is flushed synchronously. */
static void comp_tls_drive(struct KlServer *s, KlConn *c) {
    if (c->state == KL_CONN_HTTP2) { comp_h2_drive(s, c); return; }   /* ALPN h2 (8d-1) */

    if (c->state == KL_CONN_TLS_HANDSHAKE) {
        KlConnState st = kl_conn_on_handshake(c);      /* reads fed ciphertext */
        if (comp_tls_flush(c) < 0) { comp_close(s, c); return; }   /* push HS records */
        if (st == KL_CONN_TLS_HANDSHAKE || st == KL_CONN_READING || st == KL_CONN_HTTP2) {
            /* HTTP2: ALPN negotiated h2; kl_conn_on_handshake ran the upgrade and its
             * initial SETTINGS were flushed just above. Read h2 frames next. */
            if (st == KL_CONN_HTTP2) c->read_len = 0;   /* fresh h2 frame window */
            if (kl_comp_post_recv(c) < 0) comp_close(s, c);
        } else {
            comp_close(s, c);   /* CLOSED (handshake failure) */
        }
        return;
    }

    if (c->state == KL_CONN_READING_BODY) {
        for (;;) {
            ssize_t p = c->tls->read(c->tls, c->fd, c->read_buf, c->read_cap);
            if (p < 0) { comp_close(s, c); return; }
            if (p == 0) {                              /* WANT_READ — need the network */
                if (kl_comp_post_recv(c) < 0) comp_close(s, c);
                return;
            }
            c->read_len = (size_t)p;
            KlConnState st = kl_conn_ingest_body(c, c->read_len);
            if (st != KL_CONN_READING_BODY) { comp_after_state(s, c, st); return; }
            if (!c->tls->pending || c->tls->pending(c->tls) == 0) {
                if (kl_comp_post_recv(c) < 0) comp_close(s, c);
                return;                                /* more body needs the network */
            }
            /* else: another record is already buffered — decrypt the next chunk */
        }
    }

    /* Reading request headers: accumulate decrypted plaintext, parse, dispatch. */
    for (;;) {
        size_t off = c->read_len;
        if (off >= c->read_cap) { comp_close(s, c); return; }   /* headers overflowed */
        ssize_t p = c->tls->read(c->tls, c->fd, c->read_buf + off, c->read_cap - off);
        if (p < 0) { comp_close(s, c); return; }
        if (p == 0) {                                  /* WANT_READ — need the network */
            if (kl_comp_post_recv(c) < 0) comp_close(s, c);
            return;
        }
        c->read_len += (size_t)p;
        if (comp_try_reading(s, c) == 0) return;       /* dispatched / acted / closed */
        if (!c->tls->pending || c->tls->pending(c->tls) == 0) {
            if (kl_comp_post_recv(c) < 0) comp_close(s, c);
            return;                                    /* more headers need the network */
        }
        /* else: more buffered — loop and decrypt the next record */
    }
}

static void comp_on_accept(struct KlServer *s, const KlCompletionEvent *ev) {
    if (!ev->ok || !kl_handle_valid(ev->accepted_fd)) goto refill;

    KlConn *nc = kl_conn_acquire(&s->pool, ev->accepted_fd);
    if (!nc) {
        kl_sock_close(s->ev.sockets, ev->accepted_fd);   /* pool full — drop */
        goto refill;
    }
    nc->peer_source = KL_PEER_SOCKET;
    if (ev->peer_len > 0 && (size_t)ev->peer_len <= sizeof(nc->peer_addr)) {
        memcpy(&nc->peer_addr, &ev->peer, (size_t)ev->peer_len);
        nc->peer_addr_len = ev->peer_len;
    } else {
        nc->peer_addr_len = 0;
    }
    nc->res.alloc = &s->alloc_storage;
    (void)kl_sock_set_tcp_nodelay(nc->ctx ? nc->ctx->sockets : NULL, nc->fd, 1);

    /* HTTP/2 over the completion loop is supported (8d-1): h2_config is left intact, so
     * kl_conn_on_handshake may run the ALPN-h2 upgrade and comp_tls_drive / comp_after_
     * state route KL_CONN_HTTP2 to comp_h2_drive. (8c-4 cleared h2_config here as a
     * well-defined refusal before the driver existed; that clear is now removed.) */

    /* TLS: enter the handshake state and switch the backend into completion (memory
     * BIO) mode so handshake()/read()/write() operate on fed/drained buffers instead
     * of the socket. Requires the backend's optional feed_input/drain_output ops. */
    if (nc->tls) {
        if (!nc->tls->feed_input || !nc->tls->drain_output) {
            comp_close(s, nc);   /* backend can't do completion-mode TLS */
            goto refill;
        }
        nc->state = KL_CONN_TLS_HANDSHAKE;
        nc->tls->feed_input(nc->tls, NULL, 0);   /* enable memory-BIO mode */
    }

    /* Register the accepted socket with the loop (associate) + post the first read. */
    if (kl_event_add(&s->ev.loop, nc->fd, KL_EVENT_READ, nc) < 0 ||
        kl_comp_post_recv(nc) < 0) {
        comp_close(s, nc);
    }

refill:
    (void)kl_comp_post_accept(s);   /* keep the accept backlog topped up */
}

static void comp_on_read(struct KlServer *s, const KlCompletionEvent *ev) {
    KlConn *c = ev->target;
    if (!ev->ok || ev->bytes == 0) { comp_close(s, c); return; }   /* peer closed */
    /* TLS: the backend already fed this recv's ciphertext to the engine (bytes counts
     * ciphertext, not plaintext); the driver handshakes / decrypts. read_buf holds
     * plaintext and is written by comp_tls_drive, not here. */
    if (c->tls) { comp_tls_drive(s, c); return; }
    c->read_len += ev->bytes;
    /* Body reads use a fresh sliding window (read_len was reset to 0 before the
     * post), so read_len == the bytes just received. Headers accumulate. */
    if (c->state == KL_CONN_HTTP2)
        comp_h2_drive(s, c);           /* plaintext h2 (h2c) frames (8d-1) */
    else if (c->state == KL_CONN_READING_BODY)
        comp_drive_body(s, c);
    else
        comp_drive_reading(s, c);
}

static void comp_on_write(struct KlServer *s, const KlCompletionEvent *ev) {
    KlConn *c = ev->target;
    if (!ev->ok || ev->bytes == 0) { comp_close(s, c); return; }
    /* h2 output send completed (8d-3): the frames produced by the last feed are out —
     * read the next frames. Deferring the recv until here means at most one h2 send is
     * in flight, so overlapped output cannot reorder frames. */
    if (c->state == KL_CONN_HTTP2) {
        if (kl_comp_post_recv(c) < 0) comp_close(s, c);
        return;
    }
    /* TLS file body still streaming: the head (or the previous chunk) just went out —
     * post the next encrypted file chunk. HEAD requests carry no body. */
    if (c->tls && c->res.body_mode == KL_BODY_FILE && !c->res.head_request) {
        int r = comp_tls_send_file_chunk(c);
        if (r < 0) { comp_close(s, c); return; }
        if (r == 1) return;                    /* another chunk now in flight */
        /* r == 0: file fully sent — fall through to complete the response */
    }
    /* The backend only reports a WRITE once the whole response is out (it handles
     * partial sends internally). Response fully sent: keep-alive reset or close. */
    KlConnState st = kl_conn_send_complete(c);
    if (st == KL_CONN_READING) {
        if (kl_comp_post_recv(c) < 0) comp_close(s, c);
    } else {
        comp_close(s, c);
    }
}

/* Recover the server from its embedded event ctx. Every pooled connection's ctx is
 * &server->ev, and conn/accept completions only occur on a server's loop — so this
 * containerof is valid wherever it is used, without a public KlConn/KlServer field. */
static inline struct KlServer *server_of_ctx(struct KlEventCtx *ctx) {
    return (struct KlServer *)((char *)ctx - offsetof(struct KlServer, ev));
}

/* Generic completion tick — platform-independent. Drains the ctx's completion loop
 * and routes each finished op to its consumer. Shared by the server run loop and the
 * standalone kl_event_ctx_run (datagram routing lands in 8b-4c). */
int kl_comp_run(struct KlEventCtx *ctx, int max, int timeout_ms) {
    if (max > KL_COMP_MAX_EVENTS) max = KL_COMP_MAX_EVENTS;
    KlCompletionEvent ev[KL_COMP_MAX_EVENTS];
    int n = kl_comp_drain(ctx, ev, max, timeout_ms);
    if (n < 0) return -1;

    for (int i = 0; i < n; i++) {
        switch (ev[i].kind) {
        case KL_COMP_ACCEPT: comp_on_accept(server_of_ctx(ctx), &ev[i]); break;
        case KL_COMP_READ:   comp_on_read(server_of_ctx(ctx), &ev[i]);   break;
        case KL_COMP_WRITE:  comp_on_write(server_of_ctx(ctx), &ev[i]);  break;
        case KL_COMP_UDP_RECV:   /* datagram — the target is a KlUdp*, no server */
            kl_udp_comp_on_recv((KlUdp *)ev[i].target, ev[i].buf, ev[i].bytes,
                                (struct sockaddr *)&ev[i].peer, ev[i].peer_len);
            break;
        case KL_COMP_UDP_SEND:
            kl_udp_comp_on_send((KlUdp *)ev[i].target, ev[i].bytes);
            break;
        }
    }
    return n;
}

/* The server's io_engine seam entry: prime the accept backlog (idempotent), then run
 * one generic tick over its shared event ctx. */
int kl_io_engine_run_completion(struct KlServer *s, int timeout_ms) {
    if (kl_comp_prime_accepts(s) < 0) return -1;
    return kl_comp_run(&s->ev, KL_COMP_MAX_EVENTS, timeout_ms) < 0 ? -1 : 0;
}
