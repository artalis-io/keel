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
    int stream_ended;       /**< 1 = end_stream called, drain flush will close */
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

/** @brief Set the HTTP status code (default 200). */
void kl_http_response_status(KlHttpResponse *res, int code);

/**
 * @brief Append a header. Both name and value must be null-terminated.
 *        Strings containing CR or LF are rejected (header injection guard).
 * @return 0 on success, -1 on failure (CRLF injection, NULL args, alloc failure).
 */
int kl_http_response_header(KlHttpResponse *res, const char *name, const char *value);

/**
 * @brief Set a buffered body (pointer is borrowed, not copied).
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

/** @brief Free response resources (header buffer, close file fd). */
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
 * refused before any byte is produced (and @p out_write / @p out_ctx are left untouched): the
 * response of an HTTP/2 stream, whose connection is framed by the HTTP/2 session rather than by
 * HTTP/1.1, and any response never bound to a socket. Check the return value before writing.
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
 * @return 0 on success (including "already ended"), -1 on error (including no HTTP/1.1 write target).
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
 * - CLOSED: the stream was not begun, was ended, or the response was reset or released.
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
 * A refusal before @p fn was installed is remembered: once it is installed, it fires as above.
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

/**
 * @brief Flush headers + body to the connection fd.
 *
 * Only for a response bound to an HTTP/1.1 write target (its own connection fd or TLS session).
 * A response with none is refused rather than written to a handle that is not its own: an HTTP/2
 * stream's response, for instance, is sent by the HTTP/2 session, never flushed here.
 *
 * @return 0 when done, positive if more to send, -1 on error (including no write target).
 */
int kl_http_response_send(KlHttpResponse *res);

#ifdef __cplusplus
}
#endif

#endif
