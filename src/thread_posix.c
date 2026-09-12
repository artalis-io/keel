/*
 * thread_posix.c: the pthreads side of the thread.h seam. See that header for why the seam exists and
 * why it is this small. Behaviour here is exactly what thread_pool.c did inline before the seam, so a
 * POSIX build is unchanged.
 */
#include "thread.h"

#include <stdlib.h>

/* pthread entry points return void*; the seam's do not, because no caller uses the value. This
 * trampoline carries the real function and argument. Heap-allocated because it must outlive
 * kl_thread_create's frame, and freed by the thread itself. */
typedef struct { KlThreadFn fn; void *arg; } PosixStart;

static void *posix_trampoline(void *p) {
    PosixStart s = *(PosixStart *)p;
    free(p);
    s.fn(s.arg);
    return NULL;
}

int kl_thread_create(KlThread *out, KlThreadFn fn, void *arg) {
    if (!out || !fn) return -1;
    PosixStart *s = malloc(sizeof(*s));   /* not the KlAllocator: this is the thread's own trampoline
                                           * state, invisible to callers and freed by the thread */
    if (!s) return -1;
    s->fn = fn; s->arg = arg;
    if (pthread_create(&out->t, NULL, posix_trampoline, s) != 0) { free(s); return -1; }
    return 0;
}

void kl_thread_join(KlThread *t) { if (t) (void)pthread_join(t->t, NULL); }

int  kl_mutex_init(KlMutex *m)    { return (m && pthread_mutex_init(&m->m, NULL) == 0) ? 0 : -1; }
void kl_mutex_lock(KlMutex *m)    { if (m) (void)pthread_mutex_lock(&m->m); }
void kl_mutex_unlock(KlMutex *m)  { if (m) (void)pthread_mutex_unlock(&m->m); }
void kl_mutex_destroy(KlMutex *m) { if (m) (void)pthread_mutex_destroy(&m->m); }

int  kl_cond_init(KlCond *c)      { return (c && pthread_cond_init(&c->c, NULL) == 0) ? 0 : -1; }
void kl_cond_signal(KlCond *c)    { if (c) (void)pthread_cond_signal(&c->c); }
void kl_cond_broadcast(KlCond *c) { if (c) (void)pthread_cond_broadcast(&c->c); }
void kl_cond_destroy(KlCond *c)   { if (c) (void)pthread_cond_destroy(&c->c); }

void kl_cond_wait(KlCond *c, KlMutex *m) {
    if (c && m) (void)pthread_cond_wait(&c->c, &m->m);
}
