/*
 * thread.h: INTERNAL. No ABI commitment, not installed.
 *
 * The library's only threading primitives, so nothing above this line names pthreads. It exists for
 * one reason: KlThreadPool is the single library TU that needs threads, and pthreads do not exist
 * under MSVC, which blocked a native MSVC build of Keel. A consumer (OTTO) uses KlThreadPool, so
 * dropping the module on that toolchain was not an option, and linking MinGW's winpthreads into an
 * MSVC build would drag a foreign runtime into consumers for three types.
 *
 * DELIBERATELY MINIMAL, and meant to stay that way. It is exactly what thread_pool.c uses and not one
 * operation more: no thread-local storage, no semaphores, no affinity, no try-lock, no timed wait. Add
 * to it only when a real consumer needs something, the same rule the socket and event seams follow.
 *
 * Two implementations, selected by the build exactly as platform_posix.c / platform_win.c are:
 *
 *   thread_posix.c   pthreads
 *   thread_win.c     CreateThread + SRWLOCK + CONDITION_VARIABLE
 *
 * SRW locks and condition variables rather than Win32 mutex/event kernel objects: they are
 * process-local and much cheaper, and their semantics line up with pthread mutex/cond, including the
 * part that matters most here. Both allow SPURIOUS WAKEUPS, so every caller must re-check its
 * predicate in a loop around kl_cond_wait. thread_pool.c already does
 * (`while (work_count == 0 && !shutdown) kl_cond_wait(...)`), which is what makes this a drop-in
 * substitution rather than a semantic change.
 *
 * Mutexes are NOT recursive on either side, matching the pthread default the pool already relied on.
 */
#ifndef KEEL_THREAD_H
#define KEEL_THREAD_H

#if defined(_WIN32)
#include <windows.h>
typedef struct { HANDLE h; } KlThread;
typedef struct { SRWLOCK l; } KlMutex;
typedef struct { CONDITION_VARIABLE c; } KlCond;
#else
#include <pthread.h>
typedef struct { pthread_t t; } KlThread;
typedef struct { pthread_mutex_t m; } KlMutex;
typedef struct { pthread_cond_t c; } KlCond;
#endif

/* The thread entry point. void-returning rather than pthread's void*: nothing in Keel uses a thread's
 * return value, and Win32's DWORD return would otherwise have to be invented from nothing. */
typedef void (*KlThreadFn)(void *arg);

/* Start a thread running fn(arg). Returns 0, or -1 with *out untouched. */
int  kl_thread_create(KlThread *out, KlThreadFn fn, void *arg);
/* Wait for the thread to finish and release its handle. Must be called exactly once per successful
 * create; the pool joins every worker it started before freeing anything they touch. */
void kl_thread_join(KlThread *t);

/* Returns 0, or -1 when the primitive could not be created. The Win32 initialisers cannot fail, so
 * that path always returns 0; the POSIX one reports pthread_*_init. */
int  kl_mutex_init(KlMutex *m);
void kl_mutex_lock(KlMutex *m);
void kl_mutex_unlock(KlMutex *m);
void kl_mutex_destroy(KlMutex *m);

int  kl_cond_init(KlCond *c);
/* Atomically releases the mutex, waits, and reacquires it. May wake spuriously: callers loop. */
void kl_cond_wait(KlCond *c, KlMutex *m);
void kl_cond_signal(KlCond *c);
void kl_cond_broadcast(KlCond *c);
void kl_cond_destroy(KlCond *c);

#endif /* KEEL_THREAD_H */
