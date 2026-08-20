#ifndef KEEL_SSE_H
#define KEEL_SSE_H

#include <keel/http_response.h>
#include <stddef.h>

/**
 * @brief SSE (Server-Sent Events) stream handle.
 *
 * Wraps a chunked streaming response with SSE line framing.
 * All writes go through write_fn — zero allocation.
 */
typedef struct {
    KlHttpResponseWriteFn  write_fn;   /**< Chunked stream write callback */
    void      *write_ctx;  /**< Chunked stream write context */
    KlHttpResponse *res;       /**< Response (for end_stream) */
} KlSse;

/**
 * @brief Begin an SSE stream.
 *
 * Sets Content-Type: text/event-stream, Cache-Control: no-cache,
 * and starts a chunked streaming response at status 200.
 *
 * @param res  Response to stream on.
 * @param sse  SSE handle to initialize (caller-owned).
 * @return 0 on success, -1 on failure.
 */
int kl_sse_begin(KlHttpResponse *res, KlSse *sse);

/**
 * @brief Send an SSE event.
 *
 * Writes event:/id:/data: fields followed by a blank line.
 * NULL event or id omits that field. Multiline data is auto-split
 * on '\n' — each line gets its own "data: " prefix.
 *
 * @param sse      SSE handle from kl_sse_begin.
 * @param event    Event type (NULL to omit).
 * @param data     Event data (may contain '\n' for multiline).
 * @param data_len Length of data in bytes.
 * @param id       Event ID (NULL to omit).
 * @return 0 on success, -1 on write error.
 */
int kl_sse_event(KlSse *sse, const char *event,
                 const char *data, size_t data_len, const char *id);

/**
 * @brief Send an SSE comment (keep-alive ping pattern).
 *
 * Writes ": text\n". Comments are ignored by SSE clients but
 * keep the connection alive through proxies.
 *
 * @param sse  SSE handle.
 * @param text Comment text.
 * @param len  Length of text in bytes.
 * @return 0 on success, -1 on write error.
 */
int kl_sse_comment(KlSse *sse, const char *text, size_t len);

/**
 * @brief End the SSE stream.
 *
 * Sends the final zero-length chunk via kl_http_response_end_stream.
 *
 * @param sse  SSE handle.
 * @return 0 on success, -1 on error.
 */
int kl_sse_end(KlSse *sse);

#endif
