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
#include <string.h>

#define KL_COMP_MAX_EVENTS 64

static void comp_close(struct KlServer *s, KlConn *c) {
    kl_server_conn_release(s, c);   /* closes the socket + returns the pool slot */
}

/* Serialize the buffered response and post it (backend copies + owns the bytes). */
static int comp_send_response(KlConn *c) {
    char cl_buf[48];
    KlIoVec iov[7];
    size_t total = 0;
    int n = kl_response_build_iovec(&c->res, iov, 7, cl_buf, sizeof(cl_buf), &total);
    if (n < 0) return -1;
    return kl_comp_post_send(c, iov, n, total);
}

/* Run the model-blind READING core on bytes already in read_buf, then act. */
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

    if (st == KL_CONN_SENDING) {
        if (comp_send_response(c) < 0) comp_close(s, c);
    } else {
        /* READING_BODY (request bodies: 8b-1), CLOSED, SUSPENDED, etc. — close
         * rather than mis-serve in the 8a/8b-0 plaintext-GET/HEAD subset. */
        comp_close(s, c);
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

    /* Register the accepted socket with the loop (associate) + post the first read. */
    if (kl_event_add(&s->ev.loop, nc->fd, KL_EVENT_READ, nc) < 0 ||
        kl_comp_post_recv(nc) < 0) {
        comp_close(s, nc);
    }

refill:
    (void)kl_comp_post_accept(s);   /* keep the accept backlog topped up */
}

static void comp_on_read(struct KlServer *s, const KlCompletionEvent *ev) {
    KlConn *c = ev->conn;
    if (!ev->ok || ev->bytes == 0) { comp_close(s, c); return; }   /* peer closed */
    c->read_len += ev->bytes;
    comp_drive_reading(s, c);
}

static void comp_on_write(struct KlServer *s, const KlCompletionEvent *ev) {
    KlConn *c = ev->conn;
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

/* The io_engine seam's completion tick — platform-independent. Drains finished ops
 * from whatever completion backend is linked and drives their connections. */
int kl_io_engine_run_completion(struct KlServer *s, int timeout_ms) {
    KlCompletionEvent ev[KL_COMP_MAX_EVENTS];
    int n = kl_comp_drain(s, ev, KL_COMP_MAX_EVENTS, timeout_ms);
    if (n < 0) return -1;

    for (int i = 0; i < n; i++) {
        switch (ev[i].kind) {
        case KL_COMP_ACCEPT: comp_on_accept(s, &ev[i]); break;
        case KL_COMP_READ:   comp_on_read(s, &ev[i]);   break;
        case KL_COMP_WRITE:  comp_on_write(s, &ev[i]);  break;
        }
    }
    return 0;
}
