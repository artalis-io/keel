/*
 * platform_thread_posix.c: the pthreads side of the platform_thread.h seam. See that header for why the seam exists and
 * why it is this small. Behaviour here is exactly what thread_pool.c did inline before the seam, so a
 * POSIX build is unchanged.
 */
#include "platform_thread.h"

#include <stdlib.h>

/* pthread entry points return void*; the seam's do not, because no caller uses the value. This
 * trampoline carries the real function and argument. Heap-allocated because it must outlive
 * kl_plat_thread_create's frame, and freed by the thread itself. */
typedef struct { KlPlatThreadFn fn; void *arg; } PosixStart;

static void *posix_trampoline(void *p) {
    PosixStart s = *(PosixStart *)p;
    free(p);
    s.fn(s.arg);
    return NULL;
}

int kl_plat_thread_create(KlPlatThread *out, KlPlatThreadFn fn, void *arg) {
    if (!out || !fn) return -1;
    PosixStart *s = malloc(sizeof(*s));   /* not the KlAllocator: this is the thread's own trampoline
                                           * state, invisible to callers and freed by the thread */
    if (!s) return -1;
    s->fn = fn; s->arg = arg;
    if (pthread_create(&out->t, NULL, posix_trampoline, s) != 0) { free(s); return -1; }
    return 0;
}

void kl_plat_thread_join(KlPlatThread *t) { if (t) (void)pthread_join(t->t, NULL); }

int  kl_plat_mutex_init(KlPlatMutex *m)    { return (m && pthread_mutex_init(&m->m, NULL) == 0) ? 0 : -1; }
void kl_plat_mutex_lock(KlPlatMutex *m)    { if (m) (void)pthread_mutex_lock(&m->m); }
void kl_plat_mutex_unlock(KlPlatMutex *m)  { if (m) (void)pthread_mutex_unlock(&m->m); }
void kl_plat_mutex_destroy(KlPlatMutex *m) { if (m) (void)pthread_mutex_destroy(&m->m); }

int  kl_plat_cond_init(KlPlatCond *c)      { return (c && pthread_cond_init(&c->c, NULL) == 0) ? 0 : -1; }
void kl_plat_cond_signal(KlPlatCond *c)    { if (c) (void)pthread_cond_signal(&c->c); }
void kl_plat_cond_broadcast(KlPlatCond *c) { if (c) (void)pthread_cond_broadcast(&c->c); }
void kl_plat_cond_destroy(KlPlatCond *c)   { if (c) (void)pthread_cond_destroy(&c->c); }

void kl_plat_cond_wait(KlPlatCond *c, KlPlatMutex *m) {
    if (c && m) (void)pthread_cond_wait(&c->c, &m->m);
}
