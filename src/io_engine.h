/*
 * io_engine.h — INTERNAL. The event-model dispatch seam (PAL Phase 8).
 *
 * The server run loop is readiness-shaped inline (kl_event_wait → dispatch →
 * conn_read/write). When the event loop is a *completion* loop (IOCP, advertising
 * KL_EVENT_CAP_COMPLETION), the server delegates one tick here instead — so the
 * completion model stays contained to the backend and never leaks into the shared
 * readiness path or Keel's public API.
 *
 * Defined per-backend, selected by the Makefile (no #ifdef in shared code):
 *   - readiness builds link the stub in io_engine.c (never called — the server only
 *     enters the completion branch when the loop advertises COMPLETION);
 *   - the IOCP build links the real tick in event_iocp.c.
 * KlHttpServer is used by-pointer only, so an opaque forward declaration keeps this
 * header free of any other dependency. See docs/phase8_iocp_design.md.
 */
#ifndef KEEL_SRC_IO_ENGINE_H
#define KEEL_SRC_IO_ENGINE_H

#include <stddef.h>   /* size_t (kl_comp_post_dgram_send) */
#include <keel/handle.h>   /* KlSocketHandle (kl_comp_cancel) */
#include <keel/sockaddr.h> /* KlSockAddr (kl_comp_post_dgram_send) */
#include "datagram_life.h" /* KlDgramLife + KlDgramOpKind/KlDgramRetireResult (cancel_dgram/retire_dgram) */

struct KlHttpServer;
struct KlEventCtx;
struct sockaddr;

/* ── Neutral datagram completion-op descriptors (7B-2b) ───────────────────────────────────────
 * post_dgram_send/recv are CORE-NATIVE: they carry everything the op needs by value, so the public
 * KlDatagram/KlDgramCore (7B-3) builds them from its own state — the backend never dereferences a
 * transport object. OWNERSHIP (frozen §2.5.1): the caller
 * retains one `life` ref and TRANSFERS it into the op ONLY on a SUCCESSFUL post; on failure the post
 * takes nothing and the CALLER releases its ref (the backend must not retain/release it). A send op's
 * payload AND dest/src/tos are COPIED before a successful return; a recv op's `buf` is LENT (the
 * life-owned inbound slot — the backend writes it, never frees it). */
typedef struct {
    KlSocketHandle      fd;
    const void         *data;
    size_t              len;
    const KlSockAddr   *dest;   /* NULL/UNSPEC = connected send */
    const KlSockAddr   *src;    /* NULL/UNSPEC = no source pin */
    int                 tos;    /* -1 = no mark */
    struct KlDgramLife *life;   /* token ref — transferred into the op on success */
} KlDgramSendOp;

typedef struct {
    KlSocketHandle      fd;
    void               *buf;    /* the inbound slot — LENT (life-owned); the backend writes it */
    size_t              cap;
    unsigned            capture; /* KL_DGRAM_RX_* metadata-capture flags */
    struct KlDgramLife *life;
} KlDgramRecvOp;

/* Is the completion axis compiled into this build? 1 when the driver + dispatch are
 * linked (any completion or readiness build), 0 under KEEL_NO_COMPLETION (the axis is
 * stubbed out). Defined by the axis TU selected by the Makefile: completion_dispatch.c
 * (present) → 1; completion_absent.c → 0. The capability negotiation (async.c) consults
 * it to reject a KL_EVENT_CAP_COMPLETION provider fail-loud when the axis is absent,
 * keeping that #ifdef out of the shared negotiation code (F3 / no-#ifdef-in-shared). */
int kl_completion_axis_available(void);

/* Run one completion-loop tick for the server: prime accepts, then drive one
 * generic tick over its event ctx. Returns 0 to continue the run loop, <0 to stop. */
int kl_io_engine_run_completion(struct KlHttpServer *s, int timeout_ms);

/* Teardown accept quiescence (6B-3 2b review): force every posted accept to completion, then reap to
 * confirmed KlListener detachment with a TEARDOWN-SPECIFIC dispatcher that routes ONLY ACCEPT and
 * drops all other completions WITHOUT dispatch — so no HTTP step, application/consumer callback, or
 * timer runs against logically destroyed state (async ops / file_io already torn down). Called once
 * from kl_http_server_free AFTER kl_listener_close() and after the listen socket is closed. Returns 0, or
 * -1 if the force could not be guaranteed (caller leaves the backend close as the physical backstop).
 * The impl (completion_server.c) owns the completion dispatch hook + the listener; server_core.c
 * stays decoupled from the internal completion vtable. Aborting stub under KEEL_NO_COMPLETION. */
int kl_io_engine_quiesce_accepts(struct KlHttpServer *s);

/* The generic completion tick: drain the ctx's completion loop and route each op to
 * its consumer (connections; datagrams in 8b-4c). Shared by the server run loop and
 * the standalone kl_event_ctx_run, so standalone consumers (UDP/DNS) run on a
 * completion loop. Defined per-backend, selected by the Makefile (no #ifdef in shared
 * code): the completion driver (completion_driver.c) provides the real tick; the
 * io_engine.c stub is linked on readiness builds and never called (no readiness
 * backend advertises COMPLETION). Returns events processed (>= 0), or -1. */
int kl_comp_run(struct KlEventCtx *ctx, int max, int timeout_ms);

/* Cancel any pending completion ops on `fd` (PAL hardening — completion-path idle
 * timeout). On a completion loop a conn waiting on an overlapped read holds that op
 * indefinitely; the server's idle-timeout sweep calls this to abort it. The op then
 * completes with an error, and the driver releases the connection through its normal
 * completion path (so there is never a dangling op or a double release). Backend-defined
 * (event_pollcomp.c removes the ops; event_iocp.c CancelIoEx's them); stubbed in
 * io_engine.c on readiness builds, where it is never called. */
void kl_comp_cancel(struct KlEventCtx *ctx, KlSocketHandle fd);

/* Resume a suspended connection on a completion loop after an async op completed (8e-2).
 * kl_async_complete's readiness path re-arms the fd + drives kl_http_conn_on_writable; on a
 * completion loop the resume instead drives the completion send path. Defined in
 * completion_driver.c (runs comp_after_state on the conn's post-resume state); stubbed in
 * io_engine.c on readiness builds, where kl_async_complete never calls it (it branches on
 * KL_EVENT_CAP_COMPLETION). Keeps the async API free of any event-axis awareness. */
struct KlHttpConn;
void kl_io_engine_resume_completion(struct KlHttpServer *s, struct KlHttpConn *conn);

/* Re-arm a body read on a completion loop after read-side flow control resumes
 * (kl_http_request_resume_body): post a fresh recv for the conn. The readiness path re-arms READ
 * interest directly (kl_event_mod) and never calls this. Defined in completion_driver.c
 * (kl_comp_post_recv); stubbed in io_engine.c on readiness builds, where it is never reached
 * (resume branches on KL_EVENT_CAP_COMPLETION). Keeps the request API event-axis-agnostic. */
void kl_io_engine_post_read(struct KlHttpConn *conn);

/* Post one overlapped datagram receive on a completion loop from a neutral descriptor (7B-2b). The
 * completion surfaces a KL_COMP_DGRAM_RECV. Stubbed in io_engine.c on non-completion builds. */
int kl_comp_post_dgram_recv(struct KlEventCtx *ctx, const KlDgramRecvOp *op);

/* Post one overlapped datagram send on a completion loop from a neutral descriptor (7B-2b). The
 * backend COPIES the payload + dest/src/tos before a successful return; the completion surfaces a
 * KL_COMP_DGRAM_SEND. Ownership per KlDgramSendOp. Stubbed in io_engine.c on non-completion builds. */
int kl_comp_post_dgram_send(struct KlEventCtx *ctx, const KlDgramSendOp *op);

/* Request cancellation of the outstanding datagram op(s) of `kind` belonging to `life` (7B-2). The
 * completion adapter (7B-3) binds a KlDgramClose cancel hook to this. Idempotent; does NOT release the
 * token ref — the op's terminal completion (even when cancelled) releases it. Stubbed on non-completion
 * builds. */
int kl_comp_cancel_dgram(struct KlEventCtx *ctx, struct KlDgramLife *life, KlDgramOpKind kind);

/* Classify the retirement of `life`'s datagram op(s) of `kind` for the close coordinator (§4.3, 7B-2).
 * Pure query (no ownership effect): PENDING while a cancelled completion op has not yet drained; RETIRED
 * once physically done; QUARANTINED when a backend cannot confirm retirement (EFI unconfirmed op).
 * `*transport_err` is set to 1 iff a terminal transport error occurred. Stubbed on non-completion
 * builds. */
KlDgramRetireResult kl_comp_retire_dgram(struct KlEventCtx *ctx, struct KlDgramLife *life,
                                         KlDgramOpKind kind, int *transport_err);

/* Post one outbound connect on a completion loop (LC-0). `fd` is a nonblocking socket the
 * async KlHttpClient created and owns; `addr` is the destination; `watcher_udata` is the tagged
 * KlWatcher the client already registered on `fd` (kl_watcher_add). The backend performs the
 * native connect (pollcomp: connect()+POLLOUT+SO_ERROR; io_uring: IORING_OP_CONNECT; IOCP:
 * ConnectEx) and, on completion, surfaces a KL_COMP_CONNECT event targeting `watcher_udata`
 * (bytes = KL_EVENT_WRITE, ok = success), which the driver routes to the client's connect
 * watcher via kl_event_dispatch — leaving the socket so getsockopt(SO_ERROR) reports the
 * truth, so the client's Happy-Eyeballs win/fail logic is unchanged. The client branches to
 * this only when the loop advertises KL_EVENT_CAP_COMPLETION; the readiness path uses a
 * KL_EVENT_WRITE watcher directly. Declared here (the shared seam) and stubbed in
 * completion_absent.c under KEEL_NO_COMPLETION, where it is never reached. Returns 0 on
 * posted, -1 on a hard local failure (caller closes the fd + tries the next address). */
int kl_comp_post_connect(struct KlEventCtx *ctx, KlSocketHandle fd,
                         const KlSockAddr *addr, void *watcher_udata);

#endif /* KEEL_SRC_IO_ENGINE_H */
