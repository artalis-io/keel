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
#include "internal.h"            /* kl_server_conn_release */
#include "conn_internal.h"       /* kl_conn_dispatch_request / kl_conn_send_complete */
#include "response_internal.h"   /* kl_response_build_iovec */
#include "completion.h"          /* the abstract completion axis */
#include "io_engine.h"           /* kl_io_engine_run_completion (the seam) */
#include "socket.h"              /* kl_sock_* (close / tcp_nodelay via the seam) */
#include <keel/udp.h>            /* KlUdp (KL_COMP_UDP_RECV target) */
#include "udp_internal.h"        /* kl_udp_comp_on_recv */
#include <string.h>
#include <stddef.h>              /* offsetof (server_of_ctx containerof) */

#define KL_COMP_MAX_EVENTS 64

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

/* Act on the state a completed request produced. */
static void comp_after_state(struct KlServer *s, KlConn *c, KlConnState st) {
    if (st == KL_CONN_SENDING) {
        if (c->res.body_mode == KL_BODY_STREAM)
            comp_send_stream(s, c);
        else if (comp_send_response(c) < 0)
            comp_close(s, c);
    } else if (st == KL_CONN_READING_BODY) {
        comp_start_body_read(s, c);
    } else {
        /* CLOSED, or SUSPENDED (async handlers are not driven over IOCP in this
         * subset) — close rather than mis-serve. */
        comp_close(s, c);
    }
}

/* Run the model-blind headers core on bytes already in read_buf, then act. */
static void comp_drive_reading(struct KlServer *s, KlConn *c) {
    size_t consumed = 0;
    KlParseResult pr = c->parser->parse(c->parser, &c->req,
                                        c->read_buf, c->read_len, &consumed);
    if (pr == KL_PARSE_INCOMPLETE) {
        if (kl_comp_post_recv(c) < 0) comp_close(s, c);
        return;
    }
    if (pr == KL_PARSE_ERROR) { comp_close(s, c); return; }

    KlConnState st = (pr == KL_PARSE_HEADERS_OK)
        ? kl_conn_dispatch_request(c, &s->router,
                                   c->read_buf + consumed, c->read_len - consumed)
        : kl_conn_dispatch_request(c, &s->router, NULL, 0);
    comp_after_state(s, c, st);
}

/* Feed a completed body read through the model-blind body core, then act. */
static void comp_drive_body(struct KlServer *s, KlConn *c) {
    comp_after_state(s, c, kl_conn_ingest_body(c, c->read_len));
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
