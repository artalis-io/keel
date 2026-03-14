#include <keel/file_io.h>
#include <limits.h>
#include <string.h>
#include "iouring_internal.h"

typedef struct {
    KlFileIO base;           /* vtable */
    KlIoUringState *st;      /* shared io_uring state */
    KlAllocator *alloc;
    void **pending_udata;    /* sock_fd -> udata mapping */
    int pending_cap;
} KlFileIOIoUring;

static int ensure_pending_cap(KlFileIOIoUring *self, int fd) {
    if (fd < 0)
        return -1;
    if (fd < self->pending_cap)
        return 0;

    int new_cap = self->pending_cap;
    if (new_cap == 0) new_cap = 16;
    while (new_cap <= fd) {
        if (new_cap > INT_MAX / 2)
            return -1;
        new_cap *= 2;
    }

    size_t old_size = (size_t)self->pending_cap * sizeof(void *);
    size_t new_size = (size_t)new_cap * sizeof(void *);
    void **new_arr = kl_realloc(self->alloc, self->pending_udata,
                                 old_size, new_size);
    if (!new_arr)
        return -1;

    for (int i = self->pending_cap; i < new_cap; i++)
        new_arr[i] = NULL;
    self->pending_udata = new_arr;
    self->pending_cap = new_cap;
    return 0;
}

static int iouring_fio_submit(KlFileIO *fio, int file_fd, void *buf,
                               size_t len, off_t offset,
                               int sock_fd, void *udata) {
    KlFileIOIoUring *self = (KlFileIOIoUring *)fio;

    if (ensure_pending_cap(self, sock_fd) < 0)
        return -1;

    struct io_uring_sqe *sqe = io_uring_get_sqe(&self->st->ring);
    if (!sqe)
        return -1;

    io_uring_prep_read(sqe, file_fd, buf, (unsigned)len, offset);
    io_uring_sqe_set_data64(sqe, URING_OP_FILE | (uint64_t)sock_fd);
    self->st->pending++;
    self->pending_udata[sock_fd] = udata;
    return 0;
}

static int iouring_fio_cancel(KlFileIO *fio, int sock_fd) {
    KlFileIOIoUring *self = (KlFileIOIoUring *)fio;

    struct io_uring_sqe *sqe = io_uring_get_sqe(&self->st->ring);
    if (!sqe)
        return -1;

    io_uring_prep_cancel64(sqe, URING_OP_FILE | (uint64_t)sock_fd, 0);
    io_uring_sqe_set_data64(sqe, (uint64_t)-1);  /* sentinel */
    self->st->pending++;
    return 0;
}

static int iouring_fio_tick(KlFileIO *fio, KlFileIOResult *out, int max) {
    KlFileIOIoUring *self = (KlFileIOIoUring *)fio;
    KlIoUringState *st = self->st;
    int count = 0;

    for (int i = 0; i < st->file_completion_count && count < max; i++) {
        int fd = st->file_completions[i].sock_fd;
        if (fd >= 0 && fd < self->pending_cap && self->pending_udata[fd]) {
            out[count].udata = self->pending_udata[fd];
            out[count].result = st->file_completions[i].result;
            self->pending_udata[fd] = NULL;
            count++;
        }
    }
    st->file_completion_count = 0;
    return count;
}

static void iouring_fio_destroy(KlFileIO *fio) {
    KlFileIOIoUring *self = (KlFileIOIoUring *)fio;
    KlAllocator *alloc = self->alloc;

    if (self->pending_udata) {
        kl_free(alloc, self->pending_udata,
                sizeof(void *) * (size_t)self->pending_cap);
    }
    kl_free(alloc, self, sizeof(*self));
}

KlFileIO *kl_file_io_create(KlEventLoop *loop, KlAllocator *alloc) {
    if (!loop->_backend)
        return NULL;
    KlIoUringState *st = loop->_backend;

    KlFileIOIoUring *fio = kl_malloc(alloc, sizeof(*fio));
    if (!fio)
        return NULL;
    memset(fio, 0, sizeof(*fio));

    fio->base.submit = iouring_fio_submit;
    fio->base.cancel = iouring_fio_cancel;
    fio->base.tick = iouring_fio_tick;
    fio->base.destroy = iouring_fio_destroy;
    fio->st = st;
    fio->alloc = alloc;

    fio->pending_cap = INITIAL_FD_CAP;
    fio->pending_udata = kl_malloc(alloc,
                                    sizeof(void *) * INITIAL_FD_CAP);
    if (!fio->pending_udata) {
        kl_free(alloc, fio, sizeof(*fio));
        return NULL;
    }
    memset(fio->pending_udata, 0, sizeof(void *) * INITIAL_FD_CAP);

    return &fio->base;
}
