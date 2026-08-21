#ifndef KEEL_HTTP_BODY_READER_MULTIPART_H
#define KEEL_HTTP_BODY_READER_MULTIPART_H

#include <keel/http_body_reader.h>
#include <keel/http_request.h>
#include <stddef.h>

/**
 * @brief Streaming multipart/form-data parser.
 *
 * The reader is a pull iterator on top of the framework's push-shaped
 * body callback. Each call to kl_http_multipart_next() advances the parser
 * by one event:
 *
 *   PART_BEGIN  — headers parsed; meta is populated. The pointers in
 *                 meta are owned by the reader and remain valid until
 *                 the NEXT call to kl_http_multipart_next().
 *   PART_DATA   — a chunk of the current part body is available. *data
 *                 / *data_len are populated with a borrowed slice of
 *                 the internal input buffer. Valid until the next call.
 *   PART_END    — the current part finished (boundary hit). No data.
 *   DONE        — the closing boundary was seen. The reader is finished.
 *   NEED_DATA   — input buffer drained; the caller should yield and
 *                 wait for more bytes to arrive via on_data.
 *   ERROR       — malformed input or a cap was exceeded. Call
 *                 kl_http_multipart_last_error() for the specific code.
 *
 * Caps:
 *   max_part_size    — bytes of body per part. Exceeded → ERROR.
 *   max_total_size   — total bytes received from the socket.
 *   max_parts        — distinct parts; counted when PART_BEGIN is emitted.
 *   max_input_buffer — bytes the reader may hold internally. Exceeded
 *                      means the framework delivered data faster than
 *                      the handler consumed it. 0 = unlimited.
 *
 * 413 mapping:
 *   The factory returns NULL for non-multipart content types (→ 415).
 *   Any cap overflow during parsing causes kl_http_multipart_next() to
 *   return ERROR; the handler should respond 413.
 */

/** RFC 2046 maximum boundary length. */
#define KL_HTTP_MP_MAX_BOUNDARY 70

/** Events returned by kl_http_multipart_next(). */
typedef enum {
    KL_HTTP_MP_EVT_PART_BEGIN = 1,
    KL_HTTP_MP_EVT_PART_DATA,
    KL_HTTP_MP_EVT_PART_END,
    KL_HTTP_MP_EVT_DONE,
    KL_HTTP_MP_EVT_NEED_DATA,
    KL_HTTP_MP_EVT_ERROR
} KlHttpMultipartEvent;

/** Error codes reported by kl_http_multipart_last_error(). */
typedef enum {
    KL_HTTP_MP_ERR_NONE = 0,
    KL_HTTP_MP_ERR_MALFORMED,         /**< bad preamble / boundary / header line */
    KL_HTTP_MP_ERR_PART_TOO_LARGE,    /**< per-part body cap exceeded */
    KL_HTTP_MP_ERR_TOTAL_TOO_LARGE,   /**< total body cap exceeded */
    KL_HTTP_MP_ERR_TOO_MANY_PARTS,    /**< max-parts cap exceeded */
    KL_HTTP_MP_ERR_HEADERS_TOO_LARGE, /**< single part's headers exceeded buffer */
    KL_HTTP_MP_ERR_INPUT_OVERFLOW,    /**< unconsumed input buffer over cap */
    KL_HTTP_MP_ERR_PREMATURE_EOF,     /**< on_complete called mid-part */
    KL_HTTP_MP_ERR_NOMEM              /**< allocator failure */
} KlHttpMultipartErrorCode;

/** Per-part metadata returned at PART_BEGIN. Pointers are reader-owned. */
typedef struct {
    const char *name;
    size_t      name_len;
    const char *filename;        /**< NULL if no filename= attribute */
    size_t      filename_len;
    const char *content_type;    /**< NULL if no Content-Type header */
    size_t      content_type_len;
} KlHttpMultipartPartMeta;

/** Parser caps. All fields default to 0 = unlimited; the caller is
 * responsible for setting sensible limits for adversarial input. */
typedef struct {
    size_t max_part_size;     /**< per-part body bytes; 0 = unlimited */
    size_t max_total_size;    /**< total bytes received; 0 = unlimited */
    int    max_parts;         /**< distinct parts; 0 = unlimited */
    size_t max_headers_size;  /**< per-part header bytes; 0 = unlimited */
    size_t max_input_buffer;  /**< unconsumed bytes held by reader; 0 = unlimited */
} KlHttpMultipartConfig;

/** Opaque reader. Obtain via the factory; consume via kl_http_multipart_next. */
typedef struct KlHttpMultipartReader KlHttpMultipartReader;

/**
 * @brief Factory: produce a streaming multipart reader for a request.
 *
 * Extracts boundary from Content-Type. Returns NULL if the content type
 * is not multipart/form-data, the boundary is missing or too long, or
 * allocation fails. NULL triggers a 415 response from the framework.
 *
 * @param alloc     Allocator used for all internal allocations.
 * @param req       Parsed request; Content-Type header must be set.
 * @param user_data Optional KlHttpMultipartConfig*; NULL → defaults.
 * @return Body reader, or NULL.
 */
KlHttpBodyReader *kl_http_body_reader_multipart(KlAllocator *alloc, const KlHttpRequest *req,
                                        void *user_data);

/**
 * @brief Advance the parser by one event.
 *
 * @param br        The body reader returned by the factory.
 * @param meta      On PART_BEGIN: populated with part metadata. Otherwise untouched.
 * @param data      On PART_DATA: set to a borrowed slice of the input buffer.
 *                  Borrowed pointer is valid only until the NEXT mutation of
 *                  the reader — that is, the next kl_http_multipart_next() call
 *                  OR the next on_data callback. If your caller can yield
 *                  to the framework's read loop while holding the slice
 *                  (so on_data may fire), copy the bytes before yielding.
 * @param data_len  On PART_DATA: bytes in *data.
 * @return One of the KL_HTTP_MP_EVT_* event codes.
 *
 * meta and data may be NULL if the caller doesn't care about that variant.
 */
KlHttpMultipartEvent kl_http_multipart_next(KlHttpBodyReader *br,
                                    KlHttpMultipartPartMeta *meta,
                                    const char **data,
                                    size_t *data_len);

/**
 * @brief Return the error code for the latest ERROR event.
 *
 * @param br The body reader.
 * @return KL_HTTP_MP_ERR_NONE if no error, else the specific cause.
 */
KlHttpMultipartErrorCode kl_http_multipart_last_error(const KlHttpBodyReader *br);

#endif
