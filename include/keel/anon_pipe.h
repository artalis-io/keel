/*
 * keel/anon_pipe.h: an anonymous pipe pair, one end a KlStream, the other a native end for a child.
 *
 * kl_anon_pipe_create makes a connected, one-directional byte pipe:
 *
 *   endpoint A (the parent's): async, Keel-owned, returned as a KlPipeStream (<keel/pipe.h>), the same
 *                              object a named-pipe connect returns, driven through its KlStream;
 *   endpoint B (the child's):  a native end in a KlAnonPipeEnd, for the embedder's spawner to hand to a
 *                              child process (<keel/anon_pipe_native.h> reads the native value).
 *
 * `dir` is endpoint A's role. A KL_ANON_PIPE_READS pair gives a READ-only stream (the child writes into
 * B); a KL_ANON_PIPE_WRITES pair gives a WRITE-only stream (the child reads from B). The stream carries
 * only that facet: a write on a READS stream returns KL_STREAM_ERROR, and kl_stream_read_start on a
 * WRITES stream returns -1. Which of the child's standard streams a pair becomes is the embedder's
 * decision; Keel does not spawn processes.
 *
 * LIFETIME. The stream follows the KlPipeStream rules (kl_pipe_free; memory outlives every posted op).
 * B belongs to the caller from the moment create returns, and is closed with kl_anon_pipe_end_close,
 * normally right after spawning (or on spawn failure). While the parent keeps B open, a READS stream
 * never sees EOF. EOF towards the child on a WRITES pair is the parent closing A: kl_stream_close_begin
 * drains queued output, then kl_pipe_free.
 *
 * INHERITANCE. Both ends are created close-on-exec / non-inheritable, and Keel never changes that. The
 * spawner makes B inheritable for its child only (Windows: ideally through
 * PROC_THREAD_ATTRIBUTE_HANDLE_LIST, so concurrent spawns do not cross-inherit). A is never inheritable.
 * The native accessors in <keel/anon_pipe_native.h> BORROW B's value: reading it transfers no
 * ownership, and B is still released only by kl_anon_pipe_end_close. The full spawn sequence is there.
 *
 * PLATFORMS. Windows, IOCP engine: A is the overlapped server end of a private, single-instance named
 * pipe (random name, current-user + SYSTEM DACL, remote clients rejected, client process verified), and
 * B its synchronous client end, as a child's C runtime expects. Every other engine and platform returns
 * KL_PIPE_UNSUPPORTED before touching the OS. There is no emulation.
 */
#ifndef KEEL_ANON_PIPE_H
#define KEEL_ANON_PIPE_H

#include <keel/pipe.h>   /* KlPipeStream, KlPipeConfig, KlPipeStatus */
#ifdef __cplusplus
extern "C" {
#endif

struct KlEventCtx;

/** Endpoint A's role: what the Keel-owned stream does. */
typedef enum {
    KL_ANON_PIPE_READS  = 1,   /**< A reads what the child writes into B (READ-only stream) */
    KL_ANON_PIPE_WRITES = 2    /**< A writes what the child reads from B (WRITE-only stream) */
} KlAnonPipeDir;

/** Endpoint B, the child's native end. Opaque: read it with <keel/anon_pipe_native.h>. A
 *  zero-initialized value is EMPTY, as is one kl_anon_pipe_create did not fill or that has been
 *  closed, so kl_anon_pipe_end_close is always safe on it. */
typedef struct {
    void *_handle;   /* Windows: the HANDLE; NULL = empty */
    int   _fd1;      /* POSIX: the descriptor + 1; 0 = empty */
} KlAnonPipeEnd;

/** Create a pair on `ctx`. On KL_PIPE_OK, *stream is endpoint A (read side not yet started for a READS
 *  pair: call kl_stream_read_start on kl_pipe_stream(*stream)) and *peer is endpoint B. On any other
 *  status *stream is NULL, *peer is empty, and nothing is left open. `cfg` is borrowed for the call:
 *  read_capacity applies to a READS pair and write_capacity to a WRITES pair; on_data is required for a
 *  READS pair and may be NULL for a WRITES pair; on_close is optional. */
KlPipeStatus kl_anon_pipe_create(struct KlEventCtx *ctx, KlAnonPipeDir dir, const KlPipeConfig *cfg,
                                 KlPipeStream **stream, KlAnonPipeEnd *peer);

/** Close endpoint B in this process and leave it empty. Idempotent; NULL-safe. */
void kl_anon_pipe_end_close(KlAnonPipeEnd *peer);

#ifdef __cplusplus
}
#endif

#endif /* KEEL_ANON_PIPE_H */
