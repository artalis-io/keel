/*
 * completion.h — INTERNAL. The platform-independent completion event axis (PAL 8b).
 *
 * Completion is an event *concept*, a peer to readiness: just as <keel/event.h> is
 * the abstract readiness axis with epoll/kqueue/poll/WSAPoll as interchangeable
 * implementations, this is the abstract completion axis with IOCP as ONE
 * implementation (io_uring-completion / POSIX AIO are future siblings). It names
 * WHAT a completion transport does — post an async op, drain finished ops — never
 * HOW; no platform (Win32/OVERLAPPED/WSA*) type appears here or in its generic
 * consumer (completion_driver.c). A backend TU (event_iocp.c) implements it.
 *
 * The generic driver (completion_driver.c) consumes KlCompletionEvent's and drives
 * connections via the model-blind protocol core (conn_internal.h /
 * response_internal.h) + these post primitives — platform-independently. See
 * docs/phase8b_iocp_breadth_design.md.
 */
#ifndef KEEL_SRC_COMPLETION_H
#define KEEL_SRC_COMPLETION_H

#include <keel/connection.h>   /* KlConn */
#include <keel/socket.h>       /* KlIoVec, KlSocketHandle */
#include <keel/net.h>          /* struct sockaddr_storage, socklen_t */
#include <stddef.h>

struct KlServer;

/* Platform-independent completion op kinds. */
typedef enum { KL_COMP_ACCEPT, KL_COMP_READ, KL_COMP_WRITE } KlCompKind;

/* One finished async op, handed from a completion backend to the generic driver.
 * The backend has already done any platform post-processing (address extraction,
 * partial-write re-posting) — the driver sees only high-level, completed events. */
typedef struct {
    KlConn        *conn;       /* READ / WRITE */
    KlCompKind     kind;
    size_t         bytes;      /* transferred (READ / WRITE) */
    int            ok;         /* 0 = failed / peer-closed */
    KlSocketHandle accepted_fd;              /* ACCEPT: the new socket */
    struct sockaddr_storage peer;            /* ACCEPT: filled by the backend */
    socklen_t      peer_len;                 /* ACCEPT: 0 if unavailable */
} KlCompletionEvent;

/* ── Backend contract (implemented once per completion platform) ──────── */

/* Drain up to `max` finished ops into `out`, waiting at most `timeout_ms`. On the
 * first call the backend primes its accept backlog (server-scoped). Returns the
 * number of events (>= 0), or -1 on a fatal loop error. */
int kl_comp_drain(struct KlServer *s, KlCompletionEvent *out, int max, int timeout_ms);

/* Post one async receive into c->read_buf (headers/body phase). */
int kl_comp_post_recv(KlConn *c);

/* Post one async send of the (already-serialized) response iovec. The backend owns
 * the bytes for the op's lifetime (it copies) and handles partial completion
 * internally, so the driver sees only a fully-completed WRITE event. */
int kl_comp_post_send(KlConn *c, const KlIoVec *iov, int iovcnt, size_t total);

/* Post one async accept on the server's listen socket (refill the backlog). */
int kl_comp_post_accept(struct KlServer *s);

/* Post one async file send: the serialized response head (`head_iov`) followed by
 * `count` bytes from `file_fd` at offset 0. On IOCP this is a single TransmitFile
 * (head as the transmit head-buffer, offset via OVERLAPPED); a future backend uses
 * its own zero-copy file send. Reports a KL_COMP_WRITE on completion — the driver
 * treats it like any completed response send. */
int kl_comp_post_sendfile(KlConn *c, const KlIoVec *head_iov, int head_n,
                          size_t head_total, int file_fd, uint64_t count);

#endif /* KEEL_SRC_COMPLETION_H */
