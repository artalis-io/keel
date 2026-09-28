/*
 * keel/pipe.h: Windows Named Pipe client as a KlStream.
 *
 * Opens the client end of a LOCAL named pipe (\\.\pipe\<name>) and hands back an ordinary KlStream
 * over it. Everything a consumer does after that is the KlStream contract in <keel/stream.h>:
 * kl_stream_read_start / kl_stream_write / kl_stream_pause / kl_stream_resume /
 * kl_stream_close_begin / kl_stream_cancel. Nothing above the stream is pipe-specific; this header
 * only supplies the endpoint (a path) and the object that owns the pipe handle.
 *
 * ENGINE. Named pipes are completion-native, so the transport runs ONLY on the IOCP engine
 * (BACKEND=iocp). On every other engine (WSAPoll, epoll, kqueue, poll, io_uring, pollcomp) and on
 * every non-Windows platform kl_pipe_connect returns KL_PIPE_UNSUPPORTED before touching the OS.
 * There is no readiness emulation. POSIX local IPC is AF_UNIX, through the socket provider.
 *
 * NOT A SOCKET. The pipe handle never becomes a KlSocketHandle and never reaches a KlSocketProvider.
 *
 * CONNECT NEVER WAITS. A local pipe open succeeds, or fails at once with KL_PIPE_ABSENT (no server)
 * or KL_PIPE_BUSY (every server instance is taken). Retrying is the caller's policy.
 *
 * CLOSE. Both KlStream close modes apply unchanged. A GRACEFUL close (kl_stream_close_begin) drains
 * queued output and then, as for any completion-mode KlStream, waits for the outstanding read to
 * retire, which happens when the peer writes or disconnects; escalate with kl_stream_cancel to stop
 * waiting. on_close fires exactly once, after every operation on the pipe has physically retired.
 *
 * SECURITY. The client connects with identification-level impersonation only (the server may learn
 * who connected, never act as them) and refuses a remote \\host\pipe\ name.
 *
 * LISTENER. kl_pipe_listen serves a local pipe name: each connecting client arrives as a
 * KlPipeStream, the same object a client connect returns, so the server side is KlStream too. The
 * accept machinery is KlListener (its object handoff family). Instances are created on demand, and
 * each is closed only after its operation has physically retired, never recycled. Server instances
 * reject remote clients and carry a DACL for the current user and LocalSystem only. The first
 * instance claims the name, so a name another process already created fails with KL_PIPE_IN_USE.
 */
#ifndef KEEL_PIPE_H
#define KEEL_PIPE_H

#include <stddef.h>
#include <keel/stream.h>   /* KlStream: the object a pipe is used through */
#ifdef __cplusplus
extern "C" {
#endif

struct KlEventCtx;

/** @brief Opaque named-pipe transport. Owns the pipe handle and the KlStream over it. */
typedef struct KlPipeStream KlPipeStream;

/** Outcome of kl_pipe_connect. */
typedef enum {
    KL_PIPE_OK = 0,        /**< connected; *out is live */
    KL_PIPE_ABSENT,        /**< no server has created the pipe */
    KL_PIPE_BUSY,          /**< every server instance is connected; retry later */
    KL_PIPE_DENIED,        /**< the pipe's security descriptor refused access */
    KL_PIPE_INVALID,       /**< bad argument, or not a local \\.\pipe\ name */
    KL_PIPE_UNSUPPORTED,   /**< this engine/platform has no named pipes (not IOCP) */
    KL_PIPE_NOMEM,         /**< allocation failed */
    KL_PIPE_ERROR,         /**< any other failure */
    KL_PIPE_IN_USE         /**< listen: the name already exists (another server or a squatter) */
} KlPipeStatus;

/** Received bytes, same shape and rules as KlStreamReadDeliverFn: `ok`=1 is `len` bytes in `buf`
 *  (valid only during the call); `ok`=0 is the single terminal (EOF, broken pipe, or error; the
 *  stream contract does not distinguish them), after which no further data is delivered. */
typedef void (*KlPipeDataFn)(void *user_data, const char *buf, size_t len, int ok);
/** Confirmed detachment: every operation has physically retired. Fires at most once, and never after
 *  kl_pipe_free. */
typedef void (*KlPipeCloseFn)(void *user_data);

#define KL_PIPE_READ_CAP_DEFAULT  (16u * 1024u)
#define KL_PIPE_WRITE_CAP_DEFAULT (64u * 1024u)

typedef struct {
    size_t        read_capacity;   /**< receive buffer; 0 = KL_PIPE_READ_CAP_DEFAULT */
    size_t        write_capacity;  /**< bounded write queue (KL_STREAM_TOO_LARGE above it); 0 = default */
    KlPipeDataFn  on_data;         /**< required */
    KlPipeCloseFn on_close;        /**< optional */
    void         *user_data;
} KlPipeConfig;

/** Connect to the local named pipe `path` (UTF-8, "\\.\pipe\<name>") on `ctx`, which must run the
 *  IOCP engine. On KL_PIPE_OK *out is a connected stream whose read side is NOT yet started (call
 *  kl_stream_read_start on kl_pipe_stream(*out)); on any other status *out is NULL. Memory comes from
 *  the ctx allocator. `path` and `cfg` are borrowed for the call only. */
KlPipeStatus kl_pipe_connect(struct KlEventCtx *ctx, const char *path, const KlPipeConfig *cfg,
                             KlPipeStream **out);

/** The KlStream to drive. Valid until kl_pipe_free. */
KlStream *kl_pipe_stream(KlPipeStream *p);

/** Release the pipe. Legal at any time, including from inside on_data / on_close. Aborts anything
 *  still outstanding (as kl_stream_cancel) and suppresses every later callback; the memory and the
 *  handle are reclaimed once the last operation physically retires. Call it before
 *  kl_event_ctx_free (or kl_http_server_free, when the pipe shares the server's loop), and at most
 *  once. NULL-safe.
 *
 *  Loop teardown: destroying the event loop delivers no further completions to anything on it, so a
 *  pipe still live at that point gets no on_data terminal and no on_close. Its outstanding operations
 *  are still cancelled and reclaimed memory-safely by the loop's close, but the object itself must
 *  already have been released with kl_pipe_free. */
void kl_pipe_free(KlPipeStream *p);

/** Set (or replace) a stream's callbacks. This is how the owner of an ACCEPTED stream attaches its
 *  per-connection state: an accepted stream arrives with no callbacks and its read side not started,
 *  so call this, then kl_stream_read_start. Works on a connected client stream too. Returns 0, or -1
 *  if `p` or `on_data` is NULL. */
int kl_pipe_bind(KlPipeStream *p, KlPipeDataFn on_data, KlPipeCloseFn on_close, void *user_data);

/* ── Listener ──────────────────────────────────────────────────────────────────────────────── */

/** @brief Opaque named-pipe listener (server). */
typedef struct KlPipeListener KlPipeListener;

/** A client connected. `p` is a connected stream the callback now owns: bind it, start reading, and
 *  eventually kl_pipe_free it. It outlives the listener. */
typedef void (*KlPipeAcceptFn)(void *user_data, KlPipeStream *p);

#define KL_PIPE_LISTEN_INSTANCES_DEFAULT 4

typedef struct {
    int            instances;      /**< server instances kept waiting for a client (the accept
                                        window); 0 = KL_PIPE_LISTEN_INSTANCES_DEFAULT */
    size_t         read_capacity;  /**< per accepted stream; 0 = KL_PIPE_READ_CAP_DEFAULT */
    size_t         write_capacity; /**< per accepted stream; 0 = KL_PIPE_WRITE_CAP_DEFAULT */
    KlPipeAcceptFn on_accept;      /**< required */
    KlPipeCloseFn  on_close;       /**< optional: the listener has detached (free is now legal) */
    void          *user_data;
} KlPipeListenConfig;

/** Listen on the local pipe name `path` on `ctx` (IOCP engine only; otherwise KL_PIPE_UNSUPPORTED).
 *  The name is claimed before this returns (KL_PIPE_IN_USE if it already exists). On KL_PIPE_OK
 *  *out is listening; on any other status *out is NULL. */
KlPipeStatus kl_pipe_listen(struct KlEventCtx *ctx, const char *path, const KlPipeListenConfig *cfg,
                            KlPipeListener **out);

/** Stop accepting: every waiting instance's connect is cancelled, and on_close fires once all of them
 *  have physically retired. Accepted streams are unaffected. Idempotent. Returns 0, or -1 if NULL. */
int kl_pipe_listener_close(KlPipeListener *pl);

/** Free a DETACHED listener (after on_close, including from inside it). Returns 0, or -1 (freeing
 *  nothing) if it has not detached yet. Drive the loop until on_close before kl_event_ctx_free. */
int kl_pipe_listener_free(KlPipeListener *pl);

/** A short constant name for a status ("ok", "absent", ...). */
const char *kl_pipe_status_str(KlPipeStatus s);

#ifdef __cplusplus
}
#endif

#endif /* KEEL_PIPE_H */
