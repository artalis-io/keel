#ifndef KEEL_EVENT_H
#define KEEL_EVENT_H

#include <keel/allocator.h>

typedef enum { KL_EVENT_READ = 1, KL_EVENT_WRITE = 2 } KlEventMask;

typedef struct {
    void *udata;
    KlEventMask ready;
} KlEvent;

typedef struct {
    int fd;             /* epoll_fd or kqueue_fd, -1 for io_uring */
    void *_backend;     /* reserved for backend-specific state */
    KlAllocator *alloc; /* set before kl_event_init; used by io_uring backend */
} KlEventLoop;

int  kl_event_init(KlEventLoop *loop);
int  kl_event_add(KlEventLoop *loop, int fd, KlEventMask mask, void *udata);
int  kl_event_mod(KlEventLoop *loop, int fd, KlEventMask mask, void *udata);
int  kl_event_del(KlEventLoop *loop, int fd);
int  kl_event_wait(KlEventLoop *loop, KlEvent *out, int max, int timeout_ms);
void kl_event_close(KlEventLoop *loop);

#endif
