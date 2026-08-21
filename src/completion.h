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
 * connections via the model-blind protocol core (http_conn_internal.h /
 * http_response_internal.h) + these post primitives — platform-independently. See
 * docs/phase8b_iocp_breadth_design.md.
 */
#ifndef KEEL_SRC_COMPLETION_H
#define KEEL_SRC_COMPLETION_H

#include <keel/stream.h>       /* KlStream (READ/WRITE/sendfile target — neutral) */
#include <keel/socket.h>       /* KlIoVec, KlSocketHandle */
#include <keel/sockaddr.h>     /* KlSockAddr (event addrs + KlCompletionOps.post_dgram_send) */
#include "completion_io.h"         /* KlDgramSendOp / KlDgramRecvOp — the neutral datagram post descriptors */
#include <stdint.h>            /* uint64_t (KlCompletionOps.post_sendfile) */
#include <stddef.h>

/* This axis is protocol-NEUTRAL: it names KlEventCtx / KlStream / KlSocketHandle only — never an HTTP
 * type. The HTTP wrappers over the accept/sendfile/recv/send ops (KlHttpServer/KlHttpConn form) live in
 * protocols/http/completion_http.h; see docs/protocols_restructure_freeze.md §4.8. */

/* Platform-independent completion op kinds. */
typedef enum {
    KL_COMP_ACCEPT,                                /* TCP accept (server recovered from ctx) */
    KL_COMP_READ, KL_COMP_WRITE,                   /* TCP conn (target = KlStream*) */
    KL_COMP_DGRAM_RECV, KL_COMP_DGRAM_SEND,            /* datagram (target = KlDgramCore*) */
    KL_COMP_CONNECT,  /* an outbound connect finished (LC-0). The completion mirror of
                       * KL_COMP_ACCEPT for the OUTBOUND direction: pollcomp does a real
                       * connect()+POLLOUT+SO_ERROR, io_uring an IORING_OP_CONNECT, IOCP a
                       * ConnectEx, lwip-raw a tcp_connect+connected_cb (LC-1). The consumer is
                       * the async KlHttpClient, NOT the server driver — so, like KL_COMP_WATCHER,
                       * the result is routed to a tagged KlWatcher (target = the tagged
                       * KlWatcher udata the client registered; `bytes` carries KL_EVENT_WRITE,
                       * `ok` reflects connect success) and dispatched via kl_event_dispatch to
                       * the client's connect watcher (he_on_writable). The backend leaves the
                       * socket so getsockopt(SO_ERROR) reports the truth, so the client's
                       * existing win/fail logic is unchanged. See
                       * docs/phase10_lwip_raw_client_design.md §3 / §8 LC-0. */
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
 * KlStream* for READ/WRITE (the HTTP adapter recovers the KlHttpConn). ACCEPT carries no target (the
 * generic tick recovers the server from its KlEventCtx). UDP kinds use `life`, not `target`. */
typedef struct KlCompletionEvent {
    void          *target;     /* KlStream* (READ/WRITE); the HTTP adapter recovers the containing
                                * KlHttpConn (kl_http_conn_of_stream). A completion backend never holds/derefs
                                * a KlHttpConn. UNUSED for UDP kinds — they carry `life` (below). */
    KlCompKind     kind;
    size_t         bytes;      /* transferred (READ / WRITE) */
    int            ok;         /* 0 = failed / peer-closed */
    KlSocketHandle accepted_fd;              /* ACCEPT: the new socket */
    void          *buf;                      /* UDP_RECV: the datagram buffer */
    KlSockAddr     peer;                      /* ACCEPT peer / UDP_RECV source; family
                                              * KL_AF_UNSPEC = unavailable. The backend
                                              * converts its native address once at the seam. */
    KlSockAddr     local;                     /* UDP_RECV local (dest) addr via pktinfo;
                                              * family KL_AF_UNSPEC = unavailable. */
    int            gro_seg;                  /* UDP_RECV: GRO coalesced segment size, 0 = none */
    int            truncated;                /* UDP_RECV: 1 if the datagram was truncated (MSG_TRUNC) */
    int            tos;                      /* UDP_RECV: received TOS/Traffic-Class byte, or -1 = none
                                              * (M6.0a). A backend that captures it (RX_TOS requested)
                                              * MUST set -1 when the cmsg was absent; one that does not
                                              * capture TOS leaves it 0 — the dispatch gates on the
                                              * socket's accepted RX_TOS mask, so an uncaptured 0 is
                                              * never surfaced as a real TOS. */
    /* DGRAM_RECV/_SEND stable-liveness token (transport-neutral; src/datagram_life.h). A datagram
     * completion outlives the transport owner, so rather than dereferencing possibly-freed socket state,
     * the dispatch adapter recovers the owner via this token and touches it only while live. The posting
     * op transferred its reference to this event; the adapter releases it after dispatch UNLESS
     * `retain_life` (below). ALL completion backends set this for datagram kinds (Phase B.6:
     * pollcomp/io_uring/IOCP/lwIP-raw/EFI); a datagram completion with life == NULL yields a NULL owner
     * and is safely dropped — there is no `target`-deref fallback. */
    struct KlDgramLife *life;
    /* 7B-9: a BORROWED life ref — the event routes/retires but its ref is NOT released after dispatch.
     * Set (==1) only by the EFI drain for a QUARANTINED recv terminal: the backend op keeps the ref
     * forever (fail-closed — the abandoned firmware op may still write the inbound storage). Default 0
     * (transferred ref, released after dispatch — the invariant for every other event). This governs ref
     * ownership at EVERY release site BEFORE routing: kl_comp_run's no-handler fallback AND the owner
     * dispatch handler (kl_datagram_comp_dispatch) release iff !retain_life.
     * MUST default 0 — every backend zero-inits the event; only the EFI QUARANTINED branch sets it. */
    int            retain_life;
} KlCompletionEvent;

struct KlEventCtx;

/* The generic completion tick kl_comp_run() is declared in completion_io.h (the event-
 * model seam), so async.c's kl_event_ctx_run can reach it without pulling the whole
 * completion backend contract. */

/* ── Completion sub-vtable (RC-1: runtime-injectable completion axis) ──────
 *
 * The per-backend completion primitives, grouped so they can be reached through a
 * runtime-installed event provider (loop->ops->completion) exactly as the readiness
 * primitives are reached through loop->ops (event_dispatch.c). On the compiled-in path
 * (loop->ops == NULL) the dispatchers in completion_dispatch.c fall back to
 * kl_comp_ops_builtin() — the backend's own table — so that path stays a single,
 * perfectly-predicted branch with no behavioral change. RC-2 proves runtime injection;
 * RC-1 only relocates the primitives behind this vtable, leaving the default identical.
 *
 * The `kl_comp_*` free functions below remain the call surface used by callers + the
 * generic driver; completion_dispatch.c OWNS those names and routes each through this
 * vtable. Each completion backend renames its impls (static, e.g. iou_post_recv) and
 * exposes them via kl_comp_ops_builtin(). See docs/event_provider_design.md. */
typedef struct KlCompletionOps {
    int  (*drain)(struct KlEventCtx *ctx, KlCompletionEvent *out, int max, int timeout_ms);
    /* Accept ops are NEUTRAL (R2f): they take the event ctx + the listen fd, not any HTTP type. The
     * backend latches `listen_fd` on prime and reaches its state via ctx->loop._backend; the HTTP
     * accept policy (pool credit, listener) lives in the HTTP adapter. prime_accepts is a one-time
     * SETUP call that RETURNS the backend's accept window (6B-3 2b-ii) and does NOT post:
     *   >=1  post-driven — the completion KlListener drives accepts with this window, reserving one
     *        pool credit per posted accept and PAUSING (not accept-and-dropping) when the pool is
     *        full. window 1 for io_uring/pollcomp; KL_IOCP_ACCEPT_BACKLOG for IOCP.
     *    0   autonomous — the backend generates accepts itself under its own capacity gate (EFI's
     *        drain gates on pool.active_count; lwip's tcp_accept fills a slot table == pool cap).
     *        The listener is NOT installed; prime_accepts is re-called each tick to top the gate up.
     *   <0   fatal setup error.
     * post_accept posts exactly ONE accept op (post-driven backends). See docs/generic_transport_audit.md
     * (6B-3) + docs/protocols_restructure_freeze.md §4.8. */
    int  (*prime_accepts)(struct KlEventCtx *ctx, KlSocketHandle listen_fd);
    /* Raw transport I/O — the backend gets a KlStream and a caller-chosen buffer; it does
     * NOT inspect TLS/HTTP/connection state (that lives in the HTTP adapter). READ/WRITE
     * completions target the KlStream. */
    int  (*post_recv)(KlStream *stream, void *buf, size_t cap);
    int  (*post_send)(KlStream *stream, const KlIoVec *iov, int iovcnt, size_t total);
    int  (*post_accept)(struct KlEventCtx *ctx);
    /* post_sendfile is neutral over KlStream (R2f) but HTTP/file-transfer-specific in POLICY — NOT part
     * of the generic stream seam in Phase A (file transfer is an Optional capability; the generic
     * KlStream promises byte reads/writes only). See docs/generic_transport_audit.md §8. */
    int  (*post_sendfile)(KlStream *stream, const KlIoVec *head_iov, int head_n,
                          size_t head_total, int file_fd, uint64_t count);
    void (*cancel)(struct KlEventCtx *ctx, KlSocketHandle fd);
    /* Neutral datagram post seam (7B-2b): descriptors carry fd + payload/buffer + KlDgramLife, so the
     * backend never dereferences a transport. See completion_io.h KlDgramSendOp/KlDgramRecvOp + ownership. */
    int  (*post_dgram_recv)(struct KlEventCtx *ctx, const KlDgramRecvOp *op);
    int  (*post_dgram_send)(struct KlEventCtx *ctx, const KlDgramSendOp *op);
    /* Datagram cancel/retire seam (7B-2): key an op by its KlDgramLife token + kind (no transport
     * deref). cancel_dgram requests cancellation (idempotent, no ref release — the terminal completion
     * releases); retire_dgram is a pure §4.3 classifier query (PENDING/RETIRED/QUARANTINED). The 7B-3
     * facade binds the KlDgramClose cancel/retire hooks to these. See completion_io.h kl_comp_cancel_dgram. */
    int  (*cancel_dgram)(struct KlEventCtx *ctx, struct KlDgramLife *life, KlDgramOpKind kind);
    KlDgramRetireResult (*retire_dgram)(struct KlEventCtx *ctx, struct KlDgramLife *life,
                                        KlDgramOpKind kind, int *transport_err);
    /* Post one outbound connect on `fd` (a nonblocking socket the client created + owns)
     * to `addr`; its completion is surfaced as KL_COMP_CONNECT targeting `watcher_udata`
     * (the tagged KlWatcher the client registered on `fd`). LC-0 — the client-side
     * outbound counterpart of post_accept. See completion.h KL_COMP_CONNECT + client.c. */
    int  (*post_connect)(struct KlEventCtx *ctx, KlSocketHandle fd,
                         const KlSockAddr *addr, void *watcher_udata);
    /* Teardown accept-side FORCE-COMPLETION (6B-3 2b review). Called once at server teardown, AFTER
     * kl_listener_close() and AFTER the listen socket has been closed. GUARANTEE, by construction,
     * that every posted accept WILL complete promptly, so the caller's subsequent kl_comp_run drain
     * is certain to reap + retire each: io_uring UNCONDITIONALLY submits a cancel per posted accept
     * (not relying on iou_comp_cancel having found an SQE — a full SQ could have skipped it) and
     * flushes them; pollcomp marks each posted accept aborted (its next drain emits the abort);
     * IOCP relies on the already-closed listen socket (its AcceptEx are all completing). This does
     * NOT itself reap or dispatch — the SERVER drives the drain through the normal completion path
     * (kl_comp_run), so every dequeued entry (accept / read / write / udp / watcher) gets its
     * ordinary routing and NONE is lost. Returns 0 on success, -1 if the force could not be
     * guaranteed (the caller then skips the reap loop rather than risk an unbounded wait). Post-
     * driven backends only; an autonomous backend (EFI/lwip) never installs a listener → NULL slot,
     * treated as success (0) by kl_comp_shutdown_accepts_raw. */
    int (*shutdown_accepts)(struct KlEventCtx *ctx);
} KlCompletionOps;

/* The compiled-in completion backend's vtable (one per completion backend TU). A
 * readiness build links a stub returning NULL (never dereferenced — a readiness loop
 * lacks KL_EVENT_CAP_COMPLETION so the server never enters the completion branch). */
const KlCompletionOps *kl_comp_ops_builtin(void);

/* ── Backend contract (implemented once per completion platform) ──────── */

/* Drain up to `max` finished ops into `out`, waiting at most `timeout_ms`. Server-
 * agnostic (keyed on the ctx's loop); accept priming is separate (kl_comp_prime).
 * Returns the number of events (>= 0), or -1 on a fatal loop error. */
int kl_comp_drain(struct KlEventCtx *ctx, KlCompletionEvent *out, int max, int timeout_ms);

/* Prime a completion loop's accept backlog from `listen_fd` (idempotent — the backend latches after
 * the first call). Neutral (R2f): the HTTP wrapper kl_comp_prime_accepts(KlHttpServer*) in
 * completion_http.h passes &s->ev + s->listen_fd. Keeps kl_comp_drain loop-agnostic. */
int kl_comp_prime_accepts_raw(struct KlEventCtx *ctx, KlSocketHandle listen_fd);

/* Teardown accept-side force-completion (6B-3 2b review): guarantee every posted accept will
 * complete so the caller's kl_comp_run drain reaps + retires each. See
 * KlCompletionOps.shutdown_accepts. Returns 0 (incl. a NULL/autonomous/readiness no-op), -1 if the
 * force could not be guaranteed. Call only after kl_listener_close() + closing the listen socket. */
int kl_comp_shutdown_accepts_raw(struct KlEventCtx *ctx);

/* Raw receive (dispatch router, completion_dispatch.c): post one async recv of up to `cap`
 * bytes into `buf` on `stream`. buf != NULL and cap > 0 are validated by the backend. On
 * completion a KL_COMP_READ targeting `stream` reports the byte count. The HTTP-adapter helper
 * kl_comp_post_recv(KlHttpConn*) (completion_http.h) chooses the buffer, then calls this. */
int kl_comp_post_recv_raw(KlStream *stream, void *buf, size_t cap);

/* Raw send (dispatch router): post one async send of the (already-serialized) iovec on `stream`.
 * The backend handles partial completion internally, so the driver sees only a fully-completed WRITE
 * event targeting `stream`. The HTTP wrapper kl_comp_post_send(KlHttpConn*) (completion_http.h) forwards
 * &c->stream here.
 *
 * RESPONSE-SEGMENT LIFETIME CONTRACT. A backend MAY defensively copy the iov bytes for
 * the op's lifetime (IOCP/io_uring/pollcomp do), OR it MAY reference the iov segments in
 * place until the KL_COMP_WRITE is delivered (lwip-raw does, to keep transmit memory
 * bounded). The latter is safe because the generic driver keeps the response's serialized
 * segments valid until the completion: comp_send_response() builds the iovec from the LIVE
 * c->res (res->hdr_buf, res->body) + static literals + a small stack Content-Length scratch,
 * and the driver does NOT reset or reuse c->res between the send post and comp_on_write()
 * (it only resets in kl_http_conn_send_complete, which runs FROM comp_on_write). The one transient
 * segment (the stack Content-Length scratch) is tiny; a backend that references in place must
 * snapshot small segments itself. Copying backends are unaffected by this note. */
int kl_comp_post_send_raw(KlStream *stream, const KlIoVec *iov, int iovcnt, size_t total);

/* Post one async accept on the loop's latched listen socket (refill the backlog). Neutral (R2f):
 * the HTTP wrapper kl_comp_post_accept(KlHttpServer*) passes &s->ev. */
int kl_comp_post_accept_raw(struct KlEventCtx *ctx);

/* Post one async file send: the serialized response head (`head_iov`) followed by `count` bytes from
 * `file_fd` at offset 0, on `stream`. On IOCP this is a single TransmitFile (head as the transmit
 * head-buffer, offset via OVERLAPPED); a future backend uses its own zero-copy file send. Reports a
 * KL_COMP_WRITE on completion. Neutral (R2f): the HTTP wrapper kl_comp_post_sendfile(KlHttpConn*)
 * forwards &c->stream. */
int kl_comp_post_sendfile_raw(KlStream *stream, const KlIoVec *head_iov, int head_n,
                              size_t head_total, int file_fd, uint64_t count);

/* Post one outbound connect (LC-0). Declared in completion_io.h (the shared event-model seam)
 * so the async client reaches it without pulling the whole backend contract, mirroring
 * kl_comp_run / kl_comp_post_udp_*. See completion_io.h + KL_COMP_CONNECT above. */

#endif /* KEEL_SRC_COMPLETION_H */
