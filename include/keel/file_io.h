#ifndef KEEL_FILE_IO_H
#define KEEL_FILE_IO_H

#include <keel/allocator.h>
#include <keel/event.h>
#include <sys/types.h>

typedef struct KlFileIO KlFileIO;

typedef struct {
    void *udata;       /* connection pointer (opaque to file I/O backend) */
    ssize_t result;    /* bytes read/transferred, or negative error */
    int zero_copy;     /* 1 = data already sent to socket (splice path) */
} KlFileIOResult;

struct KlFileIO {
    /** Submit an async file read into buf.
     *  sock_fd is an opaque key for routing/cancellation.
     *  Returns 0 on success, -1 on error. */
    int (*submit)(KlFileIO *fio, int file_fd, void *buf,
                  size_t len, off_t offset, int sock_fd, void *udata);

    /** Cancel pending read keyed by sock_fd. Best-effort. */
    int (*cancel)(KlFileIO *fio, int sock_fd);

    /** Collect completions since last tick.
     *  Called once per event loop iteration. Returns count. */
    int (*tick)(KlFileIO *fio, KlFileIOResult *out, int max);

    /** Free backend resources. */
    void (*destroy)(KlFileIO *fio);
};

/** Create async file I/O backend from the event loop.
 *  Returns NULL if the backend doesn't support async file I/O.
 *  Caller owns the returned object; free with fio->destroy(fio). */
KlFileIO *kl_file_io_create(KlEventLoop *loop, KlAllocator *alloc);

#endif
