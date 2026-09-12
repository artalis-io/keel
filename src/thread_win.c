/*
 * thread_win.c: the Win32 side of the thread.h seam. See that header for why the seam exists and why
 * it is this small.
 *
 * SRWLOCK and CONDITION_VARIABLE rather than CreateMutex/CreateEvent: those are kernel objects,
 * nameable and shareable across processes, which this needs none of and pays for in every lock. SRW
 * locks and condition variables are process-local, need no handle, cannot fail to initialise, and
 * their wait semantics match pthreads closely enough that thread_pool.c needs no change beyond the
 * renamed calls. Used in EXCLUSIVE mode only: the pool has one lock protecting one queue, with no
 * reader/writer distinction to exploit.
 *
 * CreateThread rather than _beginthreadex: the worker runs no CRT function requiring per-thread CRT
 * state that CreateThread fails to set up on a modern UCRT, and avoiding the CRT wrapper keeps this
 * TU free of a <process.h> dependency. If a worker ever needs errno or strtok-style state, that trade
 * has to be revisited, which is why it is written down here.
 */
#include "thread.h"

#include <stdlib.h>

/* CreateThread wants DWORD WINAPI f(LPVOID); the seam's entry point returns void. This trampoline
 * carries the real function and argument, heap-allocated so it outlives kl_thread_create's frame and
 * freed by the thread itself. */
typedef struct { KlThreadFn fn; void *arg; } WinStart;

static DWORD WINAPI win_trampoline(LPVOID p) {
    WinStart s = *(WinStart *)p;
    free(p);
    s.fn(s.arg);
    return 0;
}

int kl_thread_create(KlThread *out, KlThreadFn fn, void *arg) {
    if (!out || !fn) return -1;
    WinStart *s = malloc(sizeof(*s));
    if (!s) return -1;
    s->fn = fn; s->arg = arg;
    HANDLE h = CreateThread(NULL, 0, win_trampoline, s, 0, NULL);
    if (h == NULL) { free(s); return -1; }
    out->h = h;
    return 0;
}

void kl_thread_join(KlThread *t) {
    if (!t || t->h == NULL) return;
    (void)WaitForSingleObject(t->h, INFINITE);
    (void)CloseHandle(t->h);      /* join is once-per-create, so the handle is released here */
    t->h = NULL;
}

/* InitializeSRWLock and InitializeConditionVariable return void and cannot fail, so these always
 * succeed; the int return exists to match the POSIX side, which can report pthread_*_init. */
int  kl_mutex_init(KlMutex *m)    { if (!m) return -1; InitializeSRWLock(&m->l); return 0; }
void kl_mutex_lock(KlMutex *m)    { if (m) AcquireSRWLockExclusive(&m->l); }
void kl_mutex_unlock(KlMutex *m)  { if (m) ReleaseSRWLockExclusive(&m->l); }
void kl_mutex_destroy(KlMutex *m) { (void)m; }   /* SRW locks own no resource to release */

int  kl_cond_init(KlCond *c)      { if (!c) return -1; InitializeConditionVariable(&c->c); return 0; }
void kl_cond_signal(KlCond *c)    { if (c) WakeConditionVariable(&c->c); }
void kl_cond_broadcast(KlCond *c) { if (c) WakeAllConditionVariable(&c->c); }
void kl_cond_destroy(KlCond *c)   { (void)c; }   /* likewise */

void kl_cond_wait(KlCond *c, KlMutex *m) {
    if (!c || !m) return;
    /* INFINITE matches pthread_cond_wait. A FALSE return means the wait failed rather than that the
     * predicate is unmet, and the caller re-checks its predicate in a loop either way, so there is
     * nothing useful to do with it here: returning lets that loop re-evaluate. The lock is held again
     * on every return path, failure included, which is what keeps the caller's loop correct. */
    (void)SleepConditionVariableSRW(&c->c, &m->l, INFINITE, 0);
}
