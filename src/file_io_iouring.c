#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <keel/file_io.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>
#include "iouring_internal.h"

#ifndef SPLICE_F_NONBLOCK
#define SPLICE_F_NONBLOCK 0x02
#endif

/* Per-socket slot tracking splice state and pipe lifecycle */
typedef struct {
    void *udata;
    int pipe_rd;        /* -1 if no pipe */
    int pipe_wr;
    int splice_phase;   /* 0=none, 1=splicing-in, 2=splicing-out */
    ssize_t splice_len; /* bytes remaining in pipe (decremented on partial out) */
    ssize_t splice_total; /* total bytes from phase 1 (reported to caller) */
} KlFileIOSlot;

typedef struct {
    KlFileIO base;           /* vtable */
    KlIoUringState *st;      /* shared io_uring state */
    KlAllocator *alloc;
    KlFileIOSlot *slots;     /* sock_fd -> slot mapping */
    int slots_cap;
    int has_splice;          /* probed at create time */
} KlFileIOIoUring;

static int ensure_slot_cap(KlFileIOIoUring *self, int fd) {
    if (fd < 0)
        return -1;
    if (fd < self->slots_cap)
        return 0;

    int new_cap = self->slots_cap;
    if (new_cap == 0) new_cap = 16;
    while (new_cap <= fd) {
        if (new_cap > INT_MAX / 2)
            return -1;
        new_cap *= 2;
    }

    size_t old_size = (size_t)self->slots_cap * sizeof(KlFileIOSlot);
    size_t new_size = (size_t)new_cap * sizeof(KlFileIOSlot);
    KlFileIOSlot *new_arr = kl_realloc(self->alloc, self->slots,
                                        old_size, new_size);
    if (!new_arr)
        return -1;

    for (int i = self->slots_cap; i < new_cap; i++) {
        new_arr[i].udata = NULL;
        new_arr[i].pipe_rd = -1;
        new_arr[i].pipe_wr = -1;
        new_arr[i].splice_phase = 0;
        new_arr[i].splice_len = 0;
        new_arr[i].splice_total = 0;
    }
    self->slots = new_arr;
    self->slots_cap = new_cap;
    return 0;
}

static void slot_close_pipe(KlFileIOSlot *slot) {
    if (slot->pipe_rd >= 0) {
        close(slot->pipe_rd);
        slot->pipe_rd = -1;
    }
    if (slot->pipe_wr >= 0) {
        close(slot->pipe_wr);
        slot->pipe_wr = -1;
    }
    slot->splice_phase = 0;
    slot->splice_len = 0;
    slot->splice_total = 0;
}

/* Submit splice phase 2: pipe_rd → sock_fd */
static int submit_splice_out(KlFileIOIoUring *self, KlFileIOSlot *slot,
                             KlSocketHandle sock_fd, ssize_t len) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&self->st->ring);
    if (!sqe)
        return -1;

    io_uring_prep_splice(sqe, slot->pipe_rd, -1, sock_fd, -1,
                          (unsigned)len, SPLICE_F_NONBLOCK);
    io_uring_sqe_set_data64(sqe,
        URING_OP_FILE | URING_OP_SPLICE_OUT | (uint64_t)sock_fd);
    self->st->pending++;
    slot->splice_phase = 2;
    return 0;
}

static int iouring_fio_submit(KlFileIO *fio, int file_fd, void *buf,
                               size_t len, off_t offset,
                              KlSocketHandle sock_fd, void *udata) {
    KlFileIOIoUring *self = (KlFileIOIoUring *)fio;

    /* io_uring_prep_splice/prep_read take a 32-bit unsigned length; reject an
     * oversized chunk rather than silently truncating it mod 2^32 (L3).
     * Callers cap file chunks well below this, so this is purely defensive.
     * The phase-2 splice length is derived from already-bounded bytes.
     * Guarded so the comparison isn't tautological where size_t == unsigned. */
#if SIZE_MAX > UINT_MAX
    if (len > UINT_MAX)
        return -1;
#endif

    if (ensure_slot_cap(self, sock_fd) < 0)
        return -1;

    KlFileIOSlot *slot = &self->slots[sock_fd];

    /* Splice path: buf == NULL, non-TLS (caller guarantees) */
    if (buf == NULL) {
        if (!self->has_splice)
            return -1;

        /* Lazy pipe creation (reused across chunks for same sock_fd) */
        if (slot->pipe_rd < 0) {
            int pfd[2];
            if (pipe2(pfd, O_NONBLOCK) < 0)
                return -1;
            slot->pipe_rd = pfd[0];
            slot->pipe_wr = pfd[1];
        }

        struct io_uring_sqe *sqe = io_uring_get_sqe(&self->st->ring);
        if (!sqe)
            return -1;

        /* Phase 1: file_fd → pipe_wr (splice in) */
        io_uring_prep_splice(sqe, file_fd, offset, slot->pipe_wr, -1,
                              (unsigned)len, 0);
        io_uring_sqe_set_data64(sqe, URING_OP_FILE | (uint64_t)sock_fd);
        self->st->pending++;
        slot->udata = udata;
        slot->splice_phase = 1;
        slot->splice_len = 0;
        return 0;
    }

    /* Normal read path: buf != NULL */
    struct io_uring_sqe *sqe = io_uring_get_sqe(&self->st->ring);
    if (!sqe)
        return -1;

    io_uring_prep_read(sqe, file_fd, buf, (unsigned)len, offset);
    io_uring_sqe_set_data64(sqe, URING_OP_FILE | (uint64_t)sock_fd);
    self->st->pending++;
    slot->udata = udata;
    slot->splice_phase = 0;
    return 0;
}

static int iouring_fio_cancel(KlFileIO *fio, KlSocketHandle sock_fd) {
    KlFileIOIoUring *self = (KlFileIOIoUring *)fio;

    struct io_uring_sqe *sqe = io_uring_get_sqe(&self->st->ring);
    if (!sqe)
        return -1;

    io_uring_prep_cancel64(sqe, URING_OP_FILE | (uint64_t)sock_fd, 0);
    io_uring_sqe_set_data64(sqe, (uint64_t)-1);  /* sentinel */
    self->st->pending++;

    /* Also cancel any splice-out in flight */
    if (sock_fd >= 0 && sock_fd < self->slots_cap &&
        self->slots[sock_fd].splice_phase == 2) {
        sqe = io_uring_get_sqe(&self->st->ring);
        if (sqe) {
            io_uring_prep_cancel64(sqe,
                URING_OP_FILE | URING_OP_SPLICE_OUT | (uint64_t)sock_fd, 0);
            io_uring_sqe_set_data64(sqe, (uint64_t)-1);
            self->st->pending++;
        }
    }

    /* Close pipes on cancel */
    if (sock_fd >= 0 && sock_fd < self->slots_cap)
        slot_close_pipe(&self->slots[sock_fd]);

    return 0;
}

static int iouring_fio_tick(KlFileIO *fio, KlFileIOResult *out, int max) {
    KlFileIOIoUring *self = (KlFileIOIoUring *)fio;
    KlIoUringState *st = self->st;
    int count = 0;

    for (int i = 0; i < st->file_completion_count && count < max; i++) {
        int fd = st->file_completions[i].sock_fd;
        if (fd < 0 || fd >= self->slots_cap)
            continue;

        KlFileIOSlot *slot = &self->slots[fd];
        if (!slot->udata)
            continue;

        ssize_t res = st->file_completions[i].result;

        /* Splice-out completion (phase 2: pipe→socket) */
        if (st->file_completions[i].is_splice_out) {
            if (slot->splice_phase != 2)
                continue;

            if (res <= 0) {
                /* Splice-out error — report to caller */
                out[count].udata = slot->udata;
                out[count].result = res;
                out[count].zero_copy = 0;
                slot->udata = NULL;
                slot_close_pipe(slot);
                count++;
                continue;
            }

            /* Short splice: not all bytes went to socket */
            slot->splice_len -= res;
            if (slot->splice_len > 0) {
                if (submit_splice_out(self, slot, fd, slot->splice_len) < 0) {
                    out[count].udata = slot->udata;
                    out[count].result = -1;
                    out[count].zero_copy = 0;
                    slot->udata = NULL;
                    slot_close_pipe(slot);
                    count++;
                }
                continue;
            }

            /* Fully sent — report total bytes transferred */
            out[count].udata = slot->udata;
            out[count].result = slot->splice_total;
            out[count].zero_copy = 1;
            slot->udata = NULL;
            slot->splice_phase = 0;
            count++;
            continue;
        }

        /* Splice-in completion (phase 1: file→pipe) */
        if (slot->splice_phase == 1) {
            if (res <= 0) {
                /* File read error in splice path */
                out[count].udata = slot->udata;
                out[count].result = res;
                out[count].zero_copy = 0;
                slot->udata = NULL;
                slot_close_pipe(slot);
                count++;
                continue;
            }

            /* Phase 1 done — submit phase 2 (pipe→socket) */
            slot->splice_len = res;
            slot->splice_total = res;
            if (submit_splice_out(self, slot, fd, res) < 0) {
                out[count].udata = slot->udata;
                out[count].result = -1;
                out[count].zero_copy = 0;
                slot->udata = NULL;
                slot_close_pipe(slot);
                count++;
            }
            /* Don't report to caller yet — wait for phase 2 */
            continue;
        }

        /* Normal IORING_OP_READ completion (non-splice) */
        out[count].udata = slot->udata;
        out[count].result = res;
        out[count].zero_copy = 0;
        slot->udata = NULL;
        count++;
    }
    st->file_completion_count = 0;
    return count;
}

static void iouring_fio_destroy(KlFileIO *fio) {
    KlFileIOIoUring *self = (KlFileIOIoUring *)fio;
    KlAllocator *alloc = self->alloc;

    if (self->slots) {
        /* Close any open pipes */
        for (int i = 0; i < self->slots_cap; i++)
            slot_close_pipe(&self->slots[i]);
        kl_free(alloc, self->slots,
                sizeof(KlFileIOSlot) * (size_t)self->slots_cap);
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

    fio->slots_cap = INITIAL_FD_CAP;
    fio->slots = kl_malloc(alloc,
                            sizeof(KlFileIOSlot) * INITIAL_FD_CAP);
    if (!fio->slots) {
        kl_free(alloc, fio, sizeof(*fio));
        return NULL;
    }
    for (int i = 0; i < INITIAL_FD_CAP; i++) {
        fio->slots[i].udata = NULL;
        fio->slots[i].pipe_rd = -1;
        fio->slots[i].pipe_wr = -1;
        fio->slots[i].splice_phase = 0;
        fio->slots[i].splice_len = 0;
        fio->slots[i].splice_total = 0;
    }

    /* Probe for IORING_OP_SPLICE support */
    struct io_uring_probe *probe = io_uring_get_probe_ring(&st->ring);
    fio->has_splice = probe &&
        io_uring_opcode_supported(probe, IORING_OP_SPLICE);
    if (probe)
        io_uring_free_probe(probe);

    return &fio->base;
}
