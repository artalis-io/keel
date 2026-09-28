/*
 * completion_pipe.h: INTERNAL. The neutral completion seam for named-pipe byte I/O. Substrate.
 *
 * A named pipe is not a socket, so it cannot use the KlStream post_recv/post_send seam in
 * completion.h (that one treats KlStream.fd as a SOCKET and routes through the HTTP server's hook).
 * This seam carries a KlPipeHandle instead and routes each completion by its owner's
 * KlDgramLife token, the transport-neutral liveness + refcount token the datagram path already uses
 * (datagram_life.h). There is no second completion-lifetime model:
 *
 *   - the caller retains one life ref per post and TRANSFERS it into the op only on success; on a
 *     failed post the backend took nothing and the caller releases it (datagram rule, §2.5.1);
 *   - every accepted post yields EXACTLY ONE completion (KL_COMP_PIPE_READ / _WRITE), including an
 *     op the OS failed at issue time and a cancelled op; the event carries the ref and the owner's
 *     dispatch releases it after routing;
 *   - a READ's buffer is LENT (owned by the token, so it outlives the op); a WRITE's bytes are COPIED
 *     before a successful return.
 *
 * Deliberately NOT slots on KlCompletionOps: pipes exist only on the native IOCP engine, and adding
 * vtable slots would change every backend and every runtime-installed provider for a feature none of
 * them can offer. Instead the IOCP TU (event_iocp.c) defines these functions and every other build
 * links completion_pipe_absent.c, where kl_comp_pipe_available() is 0; that is the whole
 * unsupported-engine story, decided before any OS call.
 *
 * Names no platform type: HANDLE / OVERLAPPED / DWORD stay in event_iocp.c.
 */
#ifndef KEEL_SRC_COMPLETION_PIPE_H
#define KEEL_SRC_COMPLETION_PIPE_H

#include <stddef.h>
#include "platform_pipe.h"   /* KlPipeHandle */
#include "datagram_life.h"   /* KlDgramLife: the neutral completion-lifetime token */

struct KlEventCtx;

typedef enum { KL_PIPE_OP_READ = 0, KL_PIPE_OP_WRITE } KlPipeOpKind;

/* One pipe op, by value. READ: `buf`/`len` is the receive buffer (lent, token-owned). WRITE:
 * `data`/`len` is copied before a successful return. `life` is transferred into the op on success. */
typedef struct {
    KlPipeHandle       *h;
    KlPipeOpKind        kind;
    void               *buf;
    const void         *data;
    size_t              len;
    struct KlDgramLife *life;
} KlPipeIoOp;

/* 1 iff `ctx` runs the compiled-in IOCP engine (no runtime-installed event provider), the only
 * engine with pipe ops. 0 everywhere else, which the transport maps to KL_PIPE_UNSUPPORTED before
 * it opens anything. */
int  kl_comp_pipe_available(const struct KlEventCtx *ctx);

/* Associate an opened pipe handle with the ctx's completion port. 0, or -1. */
int  kl_comp_pipe_attach(struct KlEventCtx *ctx, KlPipeHandle *h);

/* Post one READ or WRITE. 0 = accepted (exactly one completion will follow), -1 = nothing taken. */
int  kl_comp_pipe_post(struct KlEventCtx *ctx, const KlPipeIoOp *op);

/* Request cancellation of `life`'s outstanding op of `kind`. Advisory and idempotent: the op still
 * completes exactly once (aborted, or with its real result if it won the race), and that completion
 * releases the ref. Never releases a ref itself. */
void kl_comp_pipe_cancel(struct KlEventCtx *ctx, const struct KlDgramLife *life, KlPipeOpKind kind);

#endif /* KEEL_SRC_COMPLETION_PIPE_H */
