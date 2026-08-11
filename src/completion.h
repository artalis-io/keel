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
#include <keel/sockaddr.h>     /* KlSockAddr (event addrs + KlCompletionOps.post_dgram_send) */
#include <stdint.h>            /* uint64_t (KlCompletionOps.post_sendfile) */
#include <stddef.h>

struct KlServer;   /* the accept ops (prime_accepts/post_accept) take it directly (6B-3) */

/* KL_COMP_CIPHER_SIZE (the interim completion-mode TLS ciphertext scratch size) is an internal
 * detail defined in "internal.h", used at the allocation site (kl_server_init). The buffer's
 * size travels with the connection as KlConn.comp_cipher_cap, so this header does not need it. */

/* Platform-independent completion op kinds. */
typedef enum {
    KL_COMP_ACCEPT,                                /* TCP accept (server recovered from ctx) */
    KL_COMP_READ, KL_COMP_WRITE,                   /* TCP conn (target = KlStream*) */
    KL_COMP_DGRAM_RECV, KL_COMP_DGRAM_SEND,            /* datagram (target = KlDatagram*) */
    KL_COMP_CONNECT,  /* an outbound connect finished (LC-0). The completion mirror of
                       * KL_COMP_ACCEPT for the OUTBOUND direction: pollcomp does a real
                       * connect()+POLLOUT+SO_ERROR, io_uring an IORING_OP_CONNECT, IOCP a
                       * ConnectEx, lwip-raw a tcp_connect+connected_cb (LC-1). The consumer is
                       * the async KlClient, NOT the server driver — so, like KL_COMP_WATCHER,
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
 * KlStream* for READ/WRITE (the HTTP adapter recovers the KlConn); KlDatagram* for UDP kinds.
 * ACCEPT carries no target (the generic tick recovers the server from its KlEventCtx). */
typedef struct {
    void          *target;     /* KlStream* (READ/WRITE) — a KlDatagram* for UDP kinds. The HTTP
                                * adapter recovers the containing KlConn (kl_conn_of_stream) and
                                * the UDP adapter recovers the KlUdp (kl_udp_of_dg); a completion
                                * backend never holds/derefs a KlConn or KlUdp. */
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
    /* UDP_RECV/_SEND stable-liveness token (transport-neutral; src/datagram_life.h). A datagram
     * completion outlives the KlUdp wrapper, so instead of dereferencing `target` (a KlDatagram
     * embedded in a possibly-freed KlUdp) the UDP adapter recovers the owner via this token and
     * touches it only while live. The posting op transferred its reference to this event; the
     * adapter releases it after dispatch. NULL on a backend not yet on the token path (the adapter
     * falls back to the legacy `target` recovery). */
    struct KlDgramLife *life;
} KlCompletionEvent;

struct KlEventCtx;
struct KlDatagram;   /* UDP completion target — the backend holds a KlDatagram*, never a KlUdp */

/* The generic completion tick kl_comp_run() is declared in io_engine.h (the event-
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
    /* Accept ops take the KlServer directly (6B-3: KlAcceptTarget removed). prime_accepts is a
     * one-time SETUP call that RETURNS the backend's accept window (6B-3 2b-ii) and does NOT post:
     *   >=1  post-driven — the completion KlListener drives accepts with this window, reserving one
     *        pool credit per posted accept and PAUSING (not accept-and-dropping) when the pool is
     *        full. window 1 for io_uring/pollcomp; KL_IOCP_ACCEPT_BACKLOG for IOCP.
     *    0   autonomous — the backend generates accepts itself under its own capacity gate (EFI's
     *        drain gates on pool.active_count; lwip's tcp_accept fills a slot table == pool cap).
     *        The listener is NOT installed; prime_accepts is re-called each tick to top the gate up.
     *   <0   fatal setup error.
     * post_accept posts exactly ONE accept op (post-driven backends). The server reaches its
     * pool/listen fd/event ctx as before. See docs/generic_transport_audit.md (6B-3). */
    int  (*prime_accepts)(struct KlServer *s);
    /* Raw transport I/O — the backend gets a KlStream and a caller-chosen buffer; it does
     * NOT inspect TLS/HTTP/connection state (that lives in the HTTP adapter). READ/WRITE
     * completions target the KlStream. */
    int  (*post_recv)(KlStream *stream, void *buf, size_t cap);
    int  (*post_send)(KlStream *stream, const KlIoVec *iov, int iovcnt, size_t total);
    int  (*post_accept)(struct KlServer *s);
    /* post_sendfile stays KlConn-typed and HTTP/file-transfer-specific — NOT part of the
     * generic stream seam in Phase A (file transfer is an Optional capability; the generic
     * KlStream promises byte reads/writes only). See docs/generic_transport_audit.md §8. */
    int  (*post_sendfile)(KlConn *c, const KlIoVec *head_iov, int head_n,
                          size_t head_total, int file_fd, uint64_t count);
    void (*cancel)(struct KlEventCtx *ctx, KlSocketHandle fd);
    int  (*post_dgram_recv)(struct KlDatagram *dg);
    int  (*post_dgram_send)(struct KlDatagram *dg, const void *data, size_t len,
                          const KlSockAddr *dest);
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
     * treated as success (0) by kl_comp_shutdown_accepts. */
    int (*shutdown_accepts)(struct KlServer *s);
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

/* Prime the server's accept backlog on its completion loop (idempotent — the backend
 * latches after the first call). Server-scoped; keeps kl_comp_drain server-agnostic. */
int kl_comp_prime_accepts(struct KlServer *s);

/* Teardown accept-side force-completion (6B-3 2b review): guarantee every posted accept will
 * complete so the caller's kl_comp_run drain reaps + retires each. See
 * KlCompletionOps.shutdown_accepts. Returns 0 (incl. a NULL/autonomous/readiness no-op), -1 if the
 * force could not be guaranteed. Call only after kl_listener_close() + closing the listen socket. */
int kl_comp_shutdown_accepts(struct KlServer *s);

/* HTTP-adapter helper (defined in completion_server.c): choose the receive buffer for the
 * connection's current phase — plaintext/PROXY → c->stream.read_buf; TLS → the per-conn
 * ciphertext scratch (c->comp_cipher) — then post the raw receive. This is where all
 * TLS/PROXY/state knowledge lives; the backend below sees only (stream, buf, cap). */
int kl_comp_post_recv(KlConn *c);

/* Raw receive (dispatch router, completion_dispatch.c): post one async recv of up to `cap`
 * bytes into `buf` on `stream`. buf != NULL and cap > 0 are validated by the backend. On
 * completion a KL_COMP_READ targeting `stream` reports the byte count. */
int kl_comp_post_recv_raw(KlStream *stream, void *buf, size_t cap);

/* Post one async send of the (already-serialized) response iovec. The backend handles
 * partial completion internally, so the driver sees only a fully-completed WRITE event.
 *
 * RESPONSE-SEGMENT LIFETIME CONTRACT. A backend MAY defensively copy the iov bytes for
 * the op's lifetime (IOCP/io_uring/pollcomp do), OR it MAY reference the iov segments in
 * place until the KL_COMP_WRITE is delivered (lwip-raw does, to keep transmit memory
 * bounded). The latter is safe because the generic driver keeps the response's serialized
 * segments valid until the completion: comp_send_response() builds the iovec from the LIVE
 * c->res (res->hdr_buf, res->body) + static literals + a small stack Content-Length scratch,
 * and the driver does NOT reset or reuse c->res between kl_comp_post_send() and comp_on_write()
 * (it only resets in kl_conn_send_complete, which runs FROM comp_on_write). The one transient
 * segment (the stack Content-Length scratch) is tiny; a backend that references in place must
 * snapshot small segments itself. Copying backends are unaffected by this note. */
int kl_comp_post_send(KlConn *c, const KlIoVec *iov, int iovcnt, size_t total);

/* Raw send (dispatch router): the KlStream form of kl_comp_post_send. The WRITE completion
 * targets `stream`. */
int kl_comp_post_send_raw(KlStream *stream, const KlIoVec *iov, int iovcnt, size_t total);

/* Post one async accept on the server's listen socket (refill the backlog). */
int kl_comp_post_accept(struct KlServer *s);

/* Post one async file send: the serialized response head (`head_iov`) followed by
 * `count` bytes from `file_fd` at offset 0. On IOCP this is a single TransmitFile
 * (head as the transmit head-buffer, offset via OVERLAPPED); a future backend uses
 * its own zero-copy file send. Reports a KL_COMP_WRITE on completion — the driver
 * treats it like any completed response send. */
int kl_comp_post_sendfile(KlConn *c, const KlIoVec *head_iov, int head_n,
                          size_t head_total, int file_fd, uint64_t count);

/* Post one outbound connect (LC-0). Declared in io_engine.h (the shared event-model seam)
 * so the async client reaches it without pulling the whole backend contract, mirroring
 * kl_comp_run / kl_comp_post_udp_*. See io_engine.h + KL_COMP_CONNECT above. */

#endif /* KEEL_SRC_COMPLETION_H */
