#ifndef KEEL_HTTP_RESPONSE_H
#define KEEL_HTTP_RESPONSE_H

#include <keel/allocator.h>
#include <keel/handle.h>
#include <keel/drain.h>
#include <stddef.h>
#include <stdint.h>

typedef struct KlTls KlTls;
/* Back-pointer to the owning event ctx (for the internal socket provider,
 * ctx->sockets). Opaque forward decl keeps the provider seam out of this header. */
struct KlEventCtx;

/** @brief Pluggable write callback — same signature as sh_json's ShJsonWriteFn. */
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
    int stream_inflight;    /**< completion loop: an overlapped stream send is posted */

    KlDrain drain;          /**< Streaming backpressure buffer */
    int drain_enabled;      /**< 0 = off (default), 1 = on */

    size_t send_offset;     /**< Buffer send progress (partial writev resume) */

    int keep_alive;         /**< Keep-alive flag (set by connection layer) */
    int head_request;       /**< 1 if HEAD request (suppress body) */

    KlTls *tls;             /**< TLS session (NULL for plaintext) */
    struct KlEventCtx *ctx; /**< Back-pointer to event ctx (for ctx->sockets;
                                 bound with conn_fd, preserved across reset) */
} KlHttpResponse;

/**
 * @brief Initialize a response, allocating the header buffer.
 * @param res   Response to initialize.
 * @param alloc Allocator for header buffer growth.
 * @return 0 on success, -1 on allocation failure.
 */
int  kl_http_response_init(KlHttpResponse *res, KlAllocator *alloc);

/** @brief Fast reinit for keep-alive — reuses header buffer, no alloc. */
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
 * @param res  Response.
 * @param fd   Open file descriptor (ownership transferred to response).
 * @param size File size in bytes.
 */
void kl_http_response_file(KlHttpResponse *res, KlSocketHandle fd, uint64_t size);

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
 * @param res      Response.
 * @param alloc    Allocator for drain buffer.
 * @param max_size Hard cap on buffered bytes (0 = unlimited).
 * @return 0 on success, -1 on error.
 */
int kl_http_response_enable_drain(KlHttpResponse *res, KlAllocator *alloc,
                              size_t max_size);

/**
 * @brief Begin chunked streaming response.
 * @param res       Response.
 * @param status    HTTP status code.
 * @param out_write Receives the write callback function.
 * @param out_ctx   Receives the write callback context.
 * @return 0 on success, -1 on write error.
 */
int kl_http_response_begin_stream(KlHttpResponse *res, int status,
                             KlHttpResponseWriteFn *out_write, void **out_ctx);

/**
 * @brief End chunked stream (sends final zero-length chunk).
 * @return 0 on success, -1 on error.
 */
int kl_http_response_end_stream(KlHttpResponse *res);

/**
 * @brief Flush headers + body to the connection fd.
 * @return 0 when done, positive if more to send, -1 on error.
 */
int kl_http_response_send(KlHttpResponse *res);

#endif
