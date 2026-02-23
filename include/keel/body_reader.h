#ifndef KEEL_BODY_READER_H
#define KEEL_BODY_READER_H

#include <keel/allocator.h>
#include <stddef.h>

/* Forward declaration — full definition in request.h */
typedef struct KlRequest KlRequest;

/*
 * Pluggable body reader interface.
 *
 * The factory receives a fully-parsed KlRequest with valid header pointers.
 * Inspect method, path, Content-Type, Content-Length, etc. in the factory —
 * header pointers into read_buf may be invalidated once the body spans
 * multiple socket reads.  Handlers should access body data exclusively
 * through the body reader, not through KlRequest header fields.
 */
typedef struct KlBodyReader KlBodyReader;

struct KlBodyReader {
    int  (*on_data)(KlBodyReader *self, const char *data, size_t len);
    void (*on_complete)(KlBodyReader *self);
    void (*on_error)(KlBodyReader *self);
    void (*destroy)(KlBodyReader *self);
};

/*
 * Factory creates a body reader for a given request.
 * user_data is the value passed to kl_server_route / kl_router_add.
 * Return NULL to reject the request (KEEL sends 415 and closes).
 */
typedef KlBodyReader *(*KlBodyReaderFactory)(KlAllocator *alloc,
                                              KlRequest *req,
                                              void *user_data);

/*
 * Built-in buffer reader — accumulates body into a growable buffer.
 * Pass max_size as user_data via cast: (void *)(size_t)max_size.
 * Pass NULL (0) for unlimited.  Exceeding max_size returns -1 from
 * on_data, which aborts the parse and sends 413.
 */
typedef struct {
    KlBodyReader base;
    KlAllocator *alloc;
    char *data;
    size_t len;
    size_t cap;
    size_t max_size;    /* 0 = unlimited */
} KlBufReader;

KlBodyReader *kl_body_reader_buffer(KlAllocator *alloc, KlRequest *req,
                                     void *user_data);

#endif
