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
    } else {
        /* CLOSED, or SUSPENDED (async handlers are not driven over IOCP in this
         * subset) — close rather than mis-serve. */
        comp_close(s, c);
    }
}

/* Run the model-blind headers core on bytes already in read_buf and act. Returns 1
 * if more header bytes are needed (caller posts the next read, per its transport), 0
 * if the request was dispatched/acted-on (or the connection was closed). Split out so
 * the TLS read loop can reuse it across coalesced records via pending(). */
static int comp_try_reading(struct KlServer *s, KlConn *c) {
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
    if (c->state == KL_CONN_TLS_HANDSHAKE) {
        KlConnState st = kl_conn_on_handshake(c);      /* reads fed ciphertext */
        if (comp_tls_flush(c) < 0) { comp_close(s, c); return; }   /* push HS records */
        if (st == KL_CONN_TLS_HANDSHAKE || st == KL_CONN_READING) {
            if (kl_comp_post_recv(c) < 0) comp_close(s, c);
        } else {
            /* CLOSED (handshake failure). KL_CONN_HTTP2 cannot occur here: h2_config is
             * cleared on the completion accept path, so kl_conn_on_handshake never runs
             * the ALPN-h2 upgrade — the completion loop serves HTTP/1.1 over TLS. */
            comp_close(s, c);
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

    /* HTTP/2 is a full multiplexed readiness-driven session (h2.c, via conn_read/
     * conn_write); it is not driven over the completion loop in this subset. Clear
     * h2_config on the completion path so kl_conn_on_handshake never attempts the
     * (doomed) ALPN-h2 upgrade — no h2 session is allocated and no stray SETTINGS/
     * preface is written before a close. The completion loop serves HTTP/1.1 over TLS;
     * a TLS ctx used with IOCP should not advertise the "h2" ALPN protocol (if it does
     * and a client selects h2, the h2 preface fails HTTP/1.1 parsing and the connection
     * closes cleanly — never mis-served as HTTP/1.1). Full h2-over-completion is a
     * future phase. Completion-path only — the readiness h2 path is untouched. */
    nc->h2_config = NULL;

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
    if (c->state == KL_CONN_READING_BODY)
        comp_drive_body(s, c);
    else
        comp_drive_reading(s, c);
}

static void comp_on_write(struct KlServer *s, const KlCompletionEvent *ev) {
    KlConn *c = ev->target;
    if (!ev->ok || ev->bytes == 0) { comp_close(s, c); return; }
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
