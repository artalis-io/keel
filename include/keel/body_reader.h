#ifndef KEEL_BODY_READER_H
#define KEEL_BODY_READER_H

#include <keel/allocator.h>
#include <stddef.h>

/** @brief Forward declaration — full definition in request.h. */
typedef struct KlRequest KlRequest;

/**
 * @brief Pluggable body reader interface.
 *
 * The factory receives a fully-parsed KlRequest with valid header pointers.
 * Inspect method, path, Content-Type, Content-Length, etc. in the factory —
 * header pointers into read_buf may be invalidated once the body spans
 * multiple socket reads.  Handlers should access body data exclusively
 * through the body reader, not through KlRequest header fields.
 */
typedef struct KlBodyReader KlBodyReader;

struct KlBodyReader {
    int  (*on_data)(KlBodyReader *self, const char *data, size_t len); /**< Feed body chunk; return -1 to abort */
    void (*on_complete)(KlBodyReader *self);  /**< End of body signal */
    void (*on_error)(KlBodyReader *self);     /**< Connection error cleanup */
    void (*destroy)(KlBodyReader *self);      /**< Free all reader resources */
};

/**
 * @brief Factory creates a body reader for a given request.
 *
 * user_data is the value passed to kl_server_route / kl_router_add.
 * Return NULL to reject the request (KEEL sends 415 and closes).
 */
typedef KlBodyReader *(*KlBodyReaderFactory)(KlAllocator *alloc,
                                              const KlRequest *req,
                                              void *user_data);

/**
 * @brief Built-in buffer reader — accumulates body into a growable buffer.
 *
 * Pass max_size as user_data via cast: (void *)(size_t)max_size.
 * Pass NULL (0) for unlimited.  Exceeding max_size returns -1 from
 * on_data, which aborts the parse and sends 413.
 */
typedef struct {
    KlBodyReader base;  /**< Base body reader vtable */
    KlAllocator *alloc; /**< Allocator for buffer growth */
    char *data;         /**< Accumulated body data */
    size_t len;         /**< Current data length */
    size_t cap;         /**< Buffer capacity */
    size_t max_size;    /**< 0 = unlimited */
} KlBufReader;

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
KlBodyReader *kl_body_reader_buffer(KlAllocator *alloc, const KlRequest *req,
                                     void *user_data);

#endif
