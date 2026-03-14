#ifndef KEEL_IOURING_INTERNAL_H
#define KEEL_IOURING_INTERNAL_H

#include <keel/allocator.h>
#include <liburing.h>
#include <stdint.h>

#define RING_SIZE 256
#define INITIAL_FD_CAP 256

/* High bit tags a CQE as a file I/O completion (not a poll event) */
#define URING_OP_FILE ((uint64_t)1 << 63)

/*
 * Sentinel value for "fd slot is unused."  Distinct from NULL, which is a
 * valid udata value (the listen socket uses NULL to identify itself).
 * Pointer value 1 can never be returned by malloc on any architecture.
 */
#define UDATA_UNUSED ((void *)(uintptr_t)1)

typedef struct {
    ssize_t result;
    int sock_fd;
} KlIoUringFileCompletion;

typedef struct {
    struct io_uring ring;
    KlAllocator *alloc;
    void **fd_udata;    /* fd -> udata mapping (flat array, indexed by fd) */
    int fd_udata_cap;
    int pending;        /* SQEs queued but not yet submitted */

    /* File I/O completions buffered by kl_event_wait for tick() */
    KlIoUringFileCompletion *file_completions;
    int file_completion_count;
    int file_completion_cap;
} KlIoUringState;

#endif
