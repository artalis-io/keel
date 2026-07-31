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
typedef enum {
    KL_COMP_ACCEPT, KL_COMP_READ, KL_COMP_WRITE,   /* TCP conn (target = KlConn*) */
    KL_COMP_UDP_RECV, KL_COMP_UDP_SEND,            /* datagram (target = KlUdp*) */
    KL_COMP_WATCHER   /* a readiness FD watch fired (target = the tagged KlWatcher udata,
                       * `bytes` carries the ready KlEventMask) — 8e-2. Lets a completion
                       * loop relay generic FD watchers (thread-pool wakeup, timers) through
                       * the same abstract axis; the driver routes it to kl_event_dispatch.
                       * How a backend watches a readiness fd alongside its completions is
                       * the backend's business, not part of this abstract contract. */
} KlCompKind;

/* One finished async op, handed from a completion backend to the generic driver.
 * The backend has already done any platform post-processing (address extraction,
 * partial-write re-posting) — the driver sees only high-level, completed events.
 * `target` is the consumer the event belongs to, disambiguated by `kind`:
 * KlConn* for READ/WRITE; UDP kinds (8b-4c/d) will use KlUdp*. ACCEPT carries no
 * target (the generic tick recovers the server from its KlEventCtx). */
typedef struct {
    void          *target;     /* KlConn* (READ/WRITE) — a KlUdp* for UDP kinds */
    KlCompKind     kind;
    size_t         bytes;      /* transferred (READ / WRITE) */
    int            ok;         /* 0 = failed / peer-closed */
    KlSocketHandle accepted_fd;              /* ACCEPT: the new socket */
    void          *buf;                      /* UDP_RECV: the datagram buffer */
    struct sockaddr_storage peer;            /* ACCEPT peer / UDP_RECV source addr */
    socklen_t      peer_len;                 /* 0 if unavailable */
    struct sockaddr_storage local;           /* UDP_RECV: local (dest) addr via pktinfo */
    socklen_t      local_len;                /* 0 if unavailable (pktinfo off / backend lacks it) */
} KlCompletionEvent;

struct KlEventCtx;

/* The generic completion tick kl_comp_run() is declared in io_engine.h (the event-
 * model seam), so async.c's kl_event_ctx_run can reach it without pulling the whole
 * completion backend contract. */

/* ── Backend contract (implemented once per completion platform) ──────── */

/* Drain up to `max` finished ops into `out`, waiting at most `timeout_ms`. Server-
 * agnostic (keyed on the ctx's loop); accept priming is separate (kl_comp_prime).
 * Returns the number of events (>= 0), or -1 on a fatal loop error. */
int kl_comp_drain(struct KlEventCtx *ctx, KlCompletionEvent *out, int max, int timeout_ms);

/* Prime the server's accept backlog on its completion loop (idempotent — the backend
 * latches after the first call). Server-scoped; keeps kl_comp_drain server-agnostic. */
int kl_comp_prime_accepts(struct KlServer *s);

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
