#ifndef KEEL_FILE_IO_H
#define KEEL_FILE_IO_H

#include <keel/allocator.h>
#include <keel/handle.h>   /* KlSocketHandle, kl_ssize_t */
#include <keel/event.h>
#include <stdint.h>        /* uint64_t (submit offset): file offsets are non-negative,
                              same neutral type as the sendfile seam (src/socket.h) */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct KlFileIO KlFileIO;

typedef struct {
    void *udata;       /**< connection pointer (opaque to file I/O backend) */
    kl_ssize_t result; /**< bytes read/transferred, or negative error */
    int zero_copy;     /**< 1 = data already sent to socket (splice path) */
} KlFileIOResult;

struct KlFileIO {
    /** Submit an async file read into buf.
     *  sock_fd is an opaque key for routing/cancellation.
     *  Returns 0 on success, -1 on error. */
    int (*submit)(KlFileIO *fio, int file_fd, void *buf,
                  size_t len, uint64_t offset, KlSocketHandle sock_fd, void *udata);

    /** Cancel pending read keyed by sock_fd. Best-effort. */
    int (*cancel)(KlFileIO *fio, KlSocketHandle sock_fd);

    /** Collect completions since last tick.
     *  Called once per event loop iteration. Returns count. */
    int (*tick)(KlFileIO *fio, KlFileIOResult *out, int max);

    /** Free backend resources. */
    void (*destroy)(KlFileIO *fio);
};

/** @brief Create async file I/O backend from the event loop.
 *  Returns NULL if the backend doesn't support async file I/O.
 *  Caller owns the returned object; free with fio->destroy(fio). */
KlFileIO *kl_file_io_create(KlEventLoop *loop, KlAllocator *alloc);

#ifdef __cplusplus
}
#endif

#endif
