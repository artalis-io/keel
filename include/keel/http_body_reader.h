#ifndef KEEL_HTTP_BODY_READER_H
#define KEEL_HTTP_BODY_READER_H

#include <keel/allocator.h>
#include <stddef.h>

/** @brief Forward declaration — full definition in http_request.h. */
typedef struct KlHttpRequest KlHttpRequest;

/**
 * @brief Pluggable body reader interface.
 *
 * The factory receives a fully-parsed KlHttpRequest with valid header pointers.
 * Inspect method, path, Content-Type, Content-Length, etc. in the factory —
 * header pointers into read_buf may be invalidated once the body spans
 * multiple socket reads.  Handlers should access body data exclusively
 * through the body reader, not through KlHttpRequest header fields.
 */
typedef struct KlHttpBodyReader KlHttpBodyReader;

struct KlHttpBodyReader {
    int  (*on_data)(KlHttpBodyReader *self, const char *data, size_t len); /**< Feed body chunk; return -1 to abort */
    void (*on_complete)(KlHttpBodyReader *self);  /**< End of body signal */
    void (*on_error)(KlHttpBodyReader *self);     /**< Connection error cleanup */
    void (*destroy)(KlHttpBodyReader *self);      /**< Free all reader resources */
};

/**
 * @brief Factory creates a body reader for a given request.
 *
 * user_data is the value passed to kl_http_server_route / kl_http_router_add.
 * Return NULL to reject the request (KEEL sends 415 and closes).
 */
typedef KlHttpBodyReader *(*KlHttpBodyReaderFactory)(KlAllocator *alloc,
                                              const KlHttpRequest *req,
                                              void *user_data);

/**
 * @brief Built-in buffer reader — accumulates body into a growable buffer.
 *
 * Pass max_size as user_data via cast: (void *)(size_t)max_size.
 * Pass NULL (0) for unlimited.  Exceeding max_size returns -1 from
 * on_data, which aborts the parse and sends 413.
 */
typedef struct {
    KlHttpBodyReader base;  /**< Base body reader vtable */
    KlAllocator *alloc; /**< Allocator for buffer growth */
    char *data;         /**< Accumulated body data */
    size_t len;         /**< Current data length */
    size_t cap;         /**< Buffer capacity */
    size_t max_size;    /**< 0 = unlimited */
} KlHttpBufReader;

/**
 * @brief Factory: create a buffer body reader.
 *
 * Pass max_size as user_data via cast: (void *)(size_t)max_size.
 * Pass NULL (0) for unlimited.
 *
 * @param alloc     Allocator for buffer growth.
 * @param req       Parsed request (headers inspectable).
 * @param user_data Cast to size_t max_size limit.
 * @return Body reader, or NULL on allocation failure.
 */
KlHttpBodyReader *kl_http_body_reader_buffer(KlAllocator *alloc, const KlHttpRequest *req,
                                     void *user_data);

#endif
