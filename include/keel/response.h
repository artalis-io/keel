#ifndef KEEL_RESPONSE_H
#define KEEL_RESPONSE_H

#include <keel/allocator.h>
#include <stddef.h>
#include <sys/types.h>

typedef struct KlTls KlTls;

/* Pluggable write callback — same signature as sh_json's ShJsonWriteFn */
typedef int (*KlWriteFn)(void *ctx, const char *data, size_t len);

typedef enum {
    KL_BODY_NONE,
    KL_BODY_BUFFER,
    KL_BODY_FILE,
    KL_BODY_STREAM
} KlBodyMode;

typedef struct KlResponse {
    KlAllocator *alloc;
    int conn_fd;

    /* Header buffer (allocated, grows via allocator) */
    char *hdr_buf;
    size_t hdr_len;
    size_t hdr_cap;

    int status;
    int headers_sent;
    KlBodyMode body_mode;

    /* KL_BODY_BUFFER */
    const char *body;
    size_t body_len;

    /* KL_BODY_FILE */
    int file_fd;
    off_t file_size;
    off_t file_offset;

    /* KL_BODY_STREAM */
    int stream_error;

    /* Protocol flags (set by connection layer) */
    int keep_alive;
    int head_request;

    /* TLS session (NULL for plaintext — set by connection layer) */
    KlTls *tls;
} KlResponse;

/**
 * @brief Initialize a response, allocating the header buffer.
 * @param res   Response to initialize.
 * @param alloc Allocator for header buffer growth.
 * @return 0 on success, -1 on allocation failure.
 */
int  kl_response_init(KlResponse *res, KlAllocator *alloc);

/** @brief Fast reinit for keep-alive — reuses header buffer, no alloc. */
void kl_response_reset(KlResponse *res);

/** @brief Set the HTTP status code (default 200). */
void kl_response_status(KlResponse *res, int code);

/**
 * @brief Append a header. Both name and value must be null-terminated.
 *        Strings containing CR or LF are silently rejected (header injection guard).
 */
void kl_response_header(KlResponse *res, const char *name, const char *value);

/**
 * @brief Set a buffered body (pointer is borrowed, not copied).
 * @param res  Response.
 * @param data Body bytes (must remain valid until send completes).
 * @param len  Length in bytes.
 */
void kl_response_body(KlResponse *res, const char *data, size_t len);

/**
 * @brief Set a file body for zero-copy sendfile transfer.
 * @param res  Response.
 * @param fd   Open file descriptor (ownership transferred to response).
 * @param size File size in bytes.
 */
void kl_response_file(KlResponse *res, int fd, off_t size);

/** @brief Free response resources (header buffer, close file fd). */
void kl_response_free(KlResponse *res);

/**
 * @brief Convenience: set status, Content-Type: application/json, and body.
 */
void kl_response_json(KlResponse *res, int code, const char *json, size_t len);

/**
 * @brief Convenience: set status, Content-Type: text/plain, and error message.
 */
void kl_response_error(KlResponse *res, int code, const char *message);

/**
 * @brief Begin chunked streaming response.
 * @param res       Response.
 * @param status    HTTP status code.
 * @param out_write Receives the write callback function.
 * @param out_ctx   Receives the write callback context.
 * @return 0 on success, -1 on write error.
 */
int kl_response_begin_stream(KlResponse *res, int status,
                             KlWriteFn *out_write, void **out_ctx);

/**
 * @brief End chunked stream (sends final zero-length chunk).
 * @return 0 on success, -1 on error.
 */
int kl_response_end_stream(KlResponse *res);

/**
 * @brief Flush headers + body to the connection fd.
 * @return 0 when done, positive if more to send, -1 on error.
 */
int kl_response_send(KlResponse *res);

#endif
