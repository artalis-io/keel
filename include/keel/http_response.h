#ifndef KEEL_HTTP_RESPONSE_H
#define KEEL_HTTP_RESPONSE_H

#include <keel/allocator.h>
#include <keel/handle.h>
#include <keel/drain.h>
#include <keel/socket.h>   /* KlIoVec */
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif


typedef struct KlTls KlTls;
/* Back-pointer to the owning event ctx (for the internal socket provider,
 * ctx->sockets). Opaque forward decl keeps the provider seam out of this header. */
struct KlEventCtx;

/** @brief Pluggable write callback: same signature as sh_json's ShJsonWriteFn. */
typedef int (*KlHttpResponseWriteFn)(void *ctx, const char *data, size_t len);

typedef enum {
    KL_HTTP_BODY_NONE,       /**< No body */
    KL_HTTP_BODY_BUFFER,     /**< Buffered body */
    KL_HTTP_BODY_FILE,       /**< File body (sendfile) */
    KL_HTTP_BODY_STREAM      /**< Chunked streaming body */
} KlHttpBodyMode;

typedef struct KlHttpResponse {
    KlAllocator *alloc;     /**< Allocator for header buffer */
    KlSocketHandle conn_fd;            /**< Connection file descriptor */

    char *hdr_buf;          /**< Header buffer (allocated, grows via allocator) */
    size_t hdr_len;         /**< Header buffer used length */
    size_t hdr_cap;         /**< Header buffer capacity */

    int status;             /**< HTTP status code */
    int headers_sent;       /**< 1 if headers already flushed */
    KlHttpBodyMode body_mode;   /**< Body mode (none/buffer/file/stream) */

    const char *body;       /**< Buffered body pointer (borrowed) */
    size_t body_len;        /**< Buffered body length */

    char *body_owned;       /**< Owned body copy (for kl_http_response_body_copy) */
    size_t body_owned_size; /**< Owned body copy size */

    int file_fd;            /**< File descriptor for sendfile */
    uint64_t file_size;     /**< File size in bytes */
    uint64_t file_offset;   /**< File offset for sendfile resume */

    int stream_error;       /**< Streaming error flag */
    int stream_ended;       /**< 1 = end_stream called (drain flush will close), or the response
                                 was released or reset between a connection's requests: no stream
                                 can be begun or written until it is initialized again */
    int stream_inflight;    /**< Internal (historical name): 1 while the response is the one embedded in
                                 a pooled server connection, whose output queue a completion loop
                                 sends a stream through; 0 for any other response (an HTTP/2 stream's,
                                 a standalone one). Set by the server; do not touch. */

    KlDrain drain;          /**< Streaming backpressure buffer */
    int drain_enabled;      /**< 0 = off (default), 1 = on */

    size_t send_offset;     /**< Buffer send progress (partial writev resume) */

    int keep_alive;         /**< Keep-alive flag (set by connection layer) */
    int head_request;       /**< 1 if HEAD request (suppress body) */

    KlTls *tls;             /**< TLS session (NULL for plaintext) */
    struct KlEventCtx *ctx; /**< Back-pointer to event ctx (for ctx->sockets;
                                 bound with conn_fd, preserved across reset) */
    /* Appended after 3.5.0. */
    int close_notice;       /**< Internal: the connection closes after this response for a server
                                 reason (a graceful drain); the head says Connection: close unless
                                 the handler set a Connection header itself. Set by the server. */

    /* Appended after 3.5.0: bounded streaming (kl_http_response_stream_try_writev). Set by the
     * server or by these functions; do not touch. */
    size_t stream_capacity;           /**< bound on pending stream output; 0 = the default (1 MiB) */
    int    stream_writable_armed;     /**< 1 = a WOULD_BLOCK armed the writable edge */
    void (*stream_writable_fn)(void *ctx); /**< the writable edge (a KlHttpStreamWritableFn) */
    void  *stream_writable_ctx;       /**< its argument */

    /* Appended after 3.6.0: a stream carried by something other than HTTP/1.1 chunked framing (an
     * HTTP/2 stream's response). Set by the server or by these functions; do not touch. */
    const struct KlHttpStreamSink *stream_sink; /**< where the stream's body goes (NULL: chunked) */
    void  *stream_sink_ctx;           /**< its argument */
    void (*stream_close_fn)(void *ctx, int completed); /**< the close notification (a KlHttpStreamCloseFn) */
    void  *stream_close_ctx;          /**< its argument */
} KlHttpResponse;

/**
 * @brief Initialize a response, allocating the header buffer.
 * @param res   Response to initialize.
 * @param alloc Allocator for header buffer growth.
 * @return 0 on success, -1 on allocation failure.
 */
int  kl_http_response_init(KlHttpResponse *res, KlAllocator *alloc);

/** @brief Fast reinit for keep-alive: reuses header buffer, no alloc. */
void kl_http_response_reset(KlHttpResponse *res);

/**
 * @brief Set the HTTP status code (default 200).
 *        Only before the response head is committed (see kl_http_response_send()).
 */
void kl_http_response_status(KlHttpResponse *res, int code);

/**
 * @brief Append a header. Both name and value must be null-terminated.
 *        Strings containing CR or LF are rejected (header injection guard).
 *        Only before the response head is committed (see kl_http_response_send()).
 * @return 0 on success, -1 on failure (CRLF injection, NULL args, alloc failure).
 */
int kl_http_response_header(KlHttpResponse *res, const char *name, const char *value);

/**
 * @brief Set a buffered body (pointer is borrowed, not copied).
 *
 * The body setters below (this one, kl_http_response_body_copy(), kl_http_response_file(),
 * kl_http_response_json(), kl_http_response_error()) refuse a response whose stream has begun
 * (kl_http_response_begin_stream() succeeded): they change nothing, and those that return a value
 * return -1. The stream is that response's body.
 *
 * @param res  Response.
 * @param data Body bytes (must remain valid until send completes).
 * @param len  Length in bytes.
 */
void kl_http_response_body_borrow(KlHttpResponse *res, const char *data, size_t len);

/**
 * @brief Set a buffered body by copying data into an owned buffer.
 *
 * Unlike kl_http_response_body_borrow(), the data is copied into a response-owned
 * allocation that is freed by kl_http_response_reset() / kl_http_response_free().
 * Safe when the source may be freed before the response is sent.
 *
 * @param res  Response.
 * @param data Body bytes (copied).
 * @param len  Length in bytes.
 * @return 0 on success, -1 on allocation failure.
 */
int kl_http_response_body_copy(KlHttpResponse *res, const char *data, size_t len);

/**
 * @brief Set a file body for zero-copy sendfile transfer.
 *
 * @p fd is a FILE descriptor, not a socket handle, and so is an `int` like every other file descriptor
 * on this surface: `KlSocketOps.sendfile` takes `int in_fd` beside `KlSocketHandle out_fd`,
 * `KlFileIO.submit` takes `int file_fd` beside `KlSocketHandle sock_fd`, and `KlHttpResponse.file_fd`
 * stores it as an `int`. It reaches `sendfile(2)` on POSIX and `_get_osfhandle()` on Windows, both of
 * which take an `int`.
 *
 * It was declared `KlSocketHandle` (pointer-width) through 3.0.1, which made it the one place on the
 * public API where a file descriptor was typed as a socket handle. On 64-bit Windows that is a real
 * narrowing, which MSVC correctly diagnoses; the value itself has always been a small `int`, so no
 * call that ever worked stops working.
 *
 * @param res  Response.
 * On Windows open it in binary mode (`_O_BINARY`): a text-mode descriptor translates CRLF and stops
 * at 0x1A, so it reads fewer bytes than `size`, and the response fails (HTTP/2 answers 500).
 *
 * @param fd   Open file descriptor (ownership transferred to response; closed by kl_http_response_free).
 * @param size File size in bytes.
 */
void kl_http_response_file(KlHttpResponse *res, int fd, uint64_t size);

/**
 * @brief Free response resources (header buffer, close file fd).
 *
 * The response is left with no stream and no write target (only its allocator kept): every stream
 * call on it answers KL_HTTP_STREAM_CLOSED (or -1) and writes nothing, and a begin_stream is refused
 * before it changes anything, until kl_http_response_init() initializes it again. A second free is
 * harmless. This is a lifecycle state, not stale-pointer protection: once the storage is initialized
 * for another response, a pointer kept from before acts on that response. A server connection's
 * response is freed when the connection is released, and initialized again for the slot's next
 * connection: a producer must not use the pointer after its op's on_cancel, or once the op
 * completed and the response ended
 * (docs/contracts/streaming.md, "Lifetime").
 */
void kl_http_response_free(KlHttpResponse *res);

/**
 * @brief Convenience: set status, Content-Type: application/json, and body.
 */
int kl_http_response_json(KlHttpResponse *res, int code, const char *json, size_t len);

/**
 * @brief Convenience: set status, Content-Type: text/plain, and error message.
 * @return 0 on success, -1 on header append failure.
 */
int kl_http_response_error(KlHttpResponse *res, int code, const char *message);

/**
 * @brief Enable drain-based backpressure for chunked streaming.
 *
 * Must be called before kl_http_response_begin_stream(). When enabled,
 * kl_stream_write() buffers data on would-block instead of spin-looping.
 *
 * The drain's writer flushes HTTP/1.1 bytes to the response's write target, so a response with no
 * HTTP/1.1 target refuses it: the buffered remainder could never be flushed.
 *
 * @param res      Response.
 * @param alloc    Allocator for drain buffer.
 * @param max_size Hard cap on buffered bytes (0 = unlimited).
 * @return 0 on success, -1 on error (including a response with no HTTP/1.1 write target).
 */
int kl_http_response_enable_drain(KlHttpResponse *res, KlAllocator *alloc,
                              size_t max_size);

/**
 * @brief Begin chunked streaming response.
 *
 * Emits eagerly: the status line, the headers and the terminating framing go out through the
 * response's write target as this is called. A response with no HTTP/1.1 write target is therefore
 * refused before any byte is produced (and @p out_write / @p out_ctx are left untouched): any
 * response never bound to a socket. Check the return value before writing.
 *
 * An HTTP/2 stream's response is the exception: its body is not chunked but sent as the stream's
 * DATA by the HTTP/2 session. The begin submits the stream's HEADERS (the status and the headers
 * set so far), and every stream call below works on it as on HTTP/1.1, with the same bound
 * (counting payload bytes; there is no chunk framing). The stream stays open after the handler
 * returns: a producer writes it later and learns of its end from kl_http_response_stream_on_close().
 * This needs a session with the streaming ops (KlHttp2ServerSession::submit_stream_response; the
 * bundled nghttp2 adapter has them); with one that lacks them the begin is refused and the stream
 * is answered 500.
 *
 * The writer returns 0 when the chunk was accepted and -1 otherwise. It refuses, writing nothing,
 * once the stream has ended (kl_http_response_end_stream succeeded): termination is final, and a
 * chunk after the terminating chunk would be read as the start of the next response on the
 * connection. That refusal leaves the ended body intact. It refuses too when the response was reset
 * or released, and fails (stickily) on a transport or allocation error.
 *
 * One stream per response: refused (-1, nothing written or changed) once the response's head was
 * sent or a stream was begun or ended on it, and on a server connection's response between requests.
 *
 * @param res       Response.
 * @param status    HTTP status code.
 * @param out_write Receives the write callback function.
 * @param out_ctx   Receives the write callback context.
 * @return 0 on success, -1 on write error or when the response has no HTTP/1.1 write target.
 */
int kl_http_response_begin_stream(KlHttpResponse *res, int status,
                             KlHttpResponseWriteFn *out_write, void **out_ctx);

/**
 * @brief End chunked stream (sends final zero-length chunk).
 *
 * Needs the same write target as kl_http_response_begin_stream(): with none there is no stream to
 * terminate, and it is refused rather than framing written somewhere else.
 *
 * Idempotent once it has succeeded: once the terminating chunk has been accepted for sending, a
 * further call emits nothing and returns 0. A second terminating chunk would not be a repeat of a
 * harmless step -- it is framing the client reads as the start of whatever comes next on the
 * connection.
 *
 * Success is acceptance, not delivery: it means the terminating chunk was handed to this response's
 * output path (written inline, or admitted to the drain when one is enabled by
 * kl_http_response_enable_drain()), and bytes may still be queued when the call returns. Neither
 * physical transmission nor any peer acknowledgment follows from a 0 return.
 *
 * A call that *failed* is not a completed one: it returns -1, and a retry still reports the failure
 * rather than being swallowed as "already ended".
 *
 * Once it succeeded no body byte can follow: every stream writer (the one from
 * kl_http_response_begin_stream, kl_http_response_stream_try_writev, the compressed writers) refuses.
 * The connection is reused for another request only after everything, the terminating chunk
 * included, was sent.
 *
 * With no stream to end (never begun, or the response was reset or released) it returns -1 and
 * writes nothing.
 *
 * @return 0 on success (including "already ended"), -1 on error (including no HTTP/1.1 write target
 *         and no stream).
 */
int kl_http_response_end_stream(KlHttpResponse *res);

/* ── Bounded streaming: non-fatal, atomic admission and a writable edge ──────────────────────────
 *
 * The same contract as KlStream's write facet (<keel/stream.h>: kl_stream_write,
 * kl_stream_on_writable), for a chunked response begun with kl_http_response_begin_stream(). Every
 * byte Keel holds for the response and has not yet handed to the kernel counts toward one bound,
 * the response's stream capacity (KlHttpServerConfig.stream_capacity; 1 MiB by default): its
 * outbound buffer and, on a completion loop, the connection's output queue (ciphertext with TLS).
 * The bound is guaranteed for a response written ONLY through these functions. The writer returned
 * by kl_http_response_begin_stream() keeps its own semantics: its bytes count toward the pending
 * total but it is not refused by it (it fails only at its outbound buffer's cap, which is the
 * stream capacity, and on a completion loop the output queue does not count against it). Mixing the
 * two on one response keeps framing and order intact (both append whole chunks to one ordered
 * pipeline), but forfeits the bound: pending can exceed the capacity, and the functions below then
 * return WOULD_BLOCK until it falls back under it.
 *
 * The capacity bounds pending bytes, not memory: a buffer grows by doubling, and a completion
 * queue keeps the previous buffer while a send from it is in flight. For capacity C, retained
 * memory is at most about C on readiness and about 5C + 64 KiB + min(256 KiB, C) on a completion
 * loop, plus fixed per-connection buffers (exact terms: docs/contracts/streaming.md, "Bounded
 * streaming").
 */

/** Result of a bounded stream write. */
typedef enum {
    KL_HTTP_STREAM_ACCEPTED = 0,  /**< the whole write was taken (copied); Keel delivers it */
    KL_HTTP_STREAM_WOULD_BLOCK,   /**< nothing taken: retry after the writable edge */
    KL_HTTP_STREAM_TOO_LARGE,     /**< its framed size exceeds the capacity: permanent, split it */
    KL_HTTP_STREAM_CLOSED,        /**< no open stream: not begun, ended, or the response is gone */
    KL_HTTP_STREAM_ERROR          /**< the stream failed (allocation or transport), sticky */
} KlHttpStreamStatus;

/**
 * @brief Write one chunk, all or nothing, without blocking or failing on a full stream.
 *
 * The vector is framed as ONE chunk (its total size, the bytes, CRLF) and admitted whole or not at
 * all: on any status other than ACCEPTED no byte of it, framing included, entered the response's
 * output. Accepted bytes are copied before return; refused ones stay the caller's.
 *
 * - ACCEPTED: taken. A zero-length write is ACCEPTED and emits nothing (a zero-size chunk would
 *   end the body). A HEAD response takes everything and emits nothing.
 * - WOULD_BLOCK: the framed chunk fits the capacity but not the room left now. It arms the writable
 *   edge (kl_http_response_stream_on_writable).
 * - TOO_LARGE: the framed chunk exceeds the capacity (or its size overflows): it can never fit.
 * - CLOSED: the stream was not begun, was ended, or the response was reset or released. A
 *   released or reset response answers CLOSED to every stream call until a new stream is begun on
 *   it; on a server connection that is the slot's next request, so a pointer kept past the op's
 *   on_cancel or completion is not safe to use (docs/contracts/streaming.md, "Lifetime").
 * - ERROR: the stream failed earlier or now; every later write and kl_http_response_end_stream()
 *   fail too, and the connection closes. Invalid arguments (a NULL vector with a count, more than
 *   32 entries) are ERROR as well.
 *
 * Loop thread only, like every response call.
 */
KlHttpStreamStatus kl_http_response_stream_try_writev(KlHttpResponse *res, const KlIoVec *iov,
                                                      int iovcnt);

/** @brief kl_http_response_stream_try_writev() of one buffer. */
KlHttpStreamStatus kl_http_response_stream_try_write(KlHttpResponse *res, const void *data,
                                                     size_t len);

/**
 * @brief Bytes accepted for this response and not yet handed to the kernel: its outbound buffer,
 *        plus, on a completion loop, the connection's output queue (posted sends included).
 */
size_t kl_http_response_stream_pending(const KlHttpResponse *res);

/** @brief The bound kl_http_response_stream_pending() is admitted against. */
size_t kl_http_response_stream_capacity(const KlHttpResponse *res);

/** Writable-edge callback: "retry the write you were refused". Carries no status; the retry's own
 *  KlHttpStreamStatus is the answer. */
typedef void (*KlHttpStreamWritableFn)(void *ctx);

/**
 * @brief Install (or, with @p fn NULL, remove) the writable edge: "retry the write you were refused".
 *
 * After kl_http_response_stream_try_writev() returns WOULD_BLOCK, @p fn fires at most once, when the
 * pending bytes fell below the capacity; the retry's own status is the answer (WOULD_BLOCK again if
 * it needs more room than has freed, which re-arms). Only a WOULD_BLOCK arms it and an ACCEPTED
 * write disarms it; draining alone never fires it.
 *
 * A refusal before @p fn was installed is remembered: once it is installed, it fires as above, from
 * a progress point, without the producer polling, even if the output had already gone by then. On a
 * readiness loop the next wait watches the connection for writability and the edge fires from that
 * report (at once if the socket has room, otherwise once the client reads). On a completion loop,
 * where no send may be left to complete, the loop fires it on its next iteration, before it waits,
 * with no transport event needed. Never from inside this call.
 *
 * It fires only while the connection is suspended (kl_async_suspend: a producer owns the stream),
 * from the point where output progressed (a writable socket's flush, a completed send), never from
 * inside a write. It never fires once the stream ended, failed or was reset, or once the op
 * completed or was cancelled: on_cancel is the terminal signal. The callback may write, end the
 * stream, complete or cancel the op; it must not free the response. Firing disarms it.
 *
 * @return 0, or -1 for a NULL response.
 */
int kl_http_response_stream_on_writable(KlHttpResponse *res, KlHttpStreamWritableFn fn, void *ctx);

/** Close notification: the stream's response is gone. @p completed is 1 when its body was ended and
 *  all of it went out, 0 when it was cut short. */
typedef void (*KlHttpStreamCloseFn)(void *ctx, int completed);

/**
 * @brief Install (or, with @p fn NULL, remove) the close notification of an HTTP/2 stream's response.
 *
 * An HTTP/2 handler returns while its streamed response stays open, and a producer outside the
 * handler (a timer, a watcher, a thread pool's done_fn) writes the body later, with the same calls
 * as on HTTP/1.1: kl_http_response_stream_try_writev() and its writable edge, the writer from
 * kl_http_response_begin_stream(), kl_http_sse_*, the compressed writers, and
 * kl_http_response_end_stream(). There is no KlAsyncOp: the connection carries other streams, so it
 * is not suspended. This notification is the producer's terminal signal instead. It fires exactly
 * once, when the stream is released: with @p completed 1 once the ended body went out (END_STREAM
 * sent), with 0 when the client reset the stream, the connection closed, or the server was freed
 * first. From then on the response pointer is not this producer's to use: stop writing and drop it.
 * It fires from the loop, never from inside a response call, and no writable edge fires after it.
 *
 * Each stream on a connection is bounded and flow-controlled on its own: a stream whose client
 * stops opening its window fills only its own buffer (the response's stream capacity), its writes
 * answer WOULD_BLOCK, and the connection's other streams keep sending. Resetting that stream fires
 * only its own notification.
 *
 * Install it after kl_http_response_begin_stream() succeeded (for an HTTP/2 response a begin
 * submits the stream's HEADERS). Loop thread only.
 *
 * @return 0, or -1 for a NULL response or one that is not an HTTP/2 stream's begun stream (an
 *         HTTP/1.1 producer has its KlAsyncOp's on_cancel instead).
 */
int kl_http_response_stream_on_close(KlHttpResponse *res, KlHttpStreamCloseFn fn, void *ctx);

/**
 * @brief Abort an HTTP/2 stream's begun streamed response: reset the stream (RST_STREAM CANCEL).
 *
 * The producer's way to give up on a stream, typically one its client stopped reading (a stream
 * whose flow-control window stays closed never fires its writable edge again, and holds its buffer
 * until it closes). It needs no writable edge or other progress on the stream: what the stream
 * buffered is dropped at once, and the reset is sent from the next loop iteration. The connection
 * and its other streams are unaffected.
 *
 * After a successful abort the stream is terminated: writes answer KL_HTTP_STREAM_ERROR (the
 * writer from kl_http_response_begin_stream() returns -1), kl_http_response_end_stream() returns
 * -1, no writable edge fires, and a second abort returns -1. The close notification
 * (kl_http_response_stream_on_close()) still fires exactly once, from the loop, never from inside
 * this call, with completed 0. The response pointer stays the producer's until then.
 *
 * Loop thread only.
 *
 * @return 0, or -1 for a NULL response, one that is not an HTTP/2 stream's begun stream (an
 *         HTTP/1.1 producer cancels through its KlAsyncOp instead), or a stream that is no longer
 *         open (already aborted, failed, or closed).
 */
int kl_http_response_stream_abort(KlHttpResponse *res);

/**
 * @brief Flush headers + body to the connection fd.
 *
 * Only for a response bound to an HTTP/1.1 write target (its own connection fd or TLS session).
 * A response with none is refused rather than written to a handle that is not its own: an HTTP/2
 * stream's response, for instance, is sent by the HTTP/2 session, never flushed here.
 *
 * A response must have exactly one output driver. An application must not initiate direct
 * transmission while the completion-driven server machinery owns transmission of that response.
 * On a completion loop (io_uring, IOCP, pollcomp, and the completion integrations) the server owns
 * the transmission of every response it hands a handler: it sends the response itself once the
 * handler returns or the async op completes. There this call is refused for a buffered, empty or
 * file response, and for a streamed one without an outbound buffer: it returns -1 and writes
 * nothing, leaving the response and its send progress exactly as they were, so the server's send
 * is the only one. (A streamed response with an outbound buffer, the default from
 * kl_http_response_begin_stream(), is not refused: flushing it only moves its buffered bytes onto
 * the connection's output queue, which the server sends.) A handler needs no call to send its
 * response on any loop: building it is enough. On a readiness loop the call stays a direct flush,
 * and the server resumes from what it wrote.
 *
 * Once any byte of the response head has been sent (by this call or by the server), the response
 * is committed and its serialized layout is immutable. Do not call kl_http_response_status,
 * kl_http_response_header, the body setters, kl_http_response_file, kl_http_response_json,
 * kl_http_response_error or kl_http_response_body_compress on it afterwards. They are not
 * refused, but a change to a committed response no longer matches what is already on the wire,
 * and the resumed send corrupts it. Keel itself never alters a committed response: when it must
 * end the connection it clears only the request's keep-alive and closes after the response. A
 * response is valid for its producer only while its request is being handled (see
 * docs/contracts/streaming.md, "Lifetime").
 *
 * @return 0 when done, positive if more to send, -1 on error (including no write target, and a
 *         response whose transmission the completion machinery owns).
 */
int kl_http_response_send(KlHttpResponse *res);

#ifdef __cplusplus
}
#endif

#endif
