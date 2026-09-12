/*
 * platform_thread.h: INTERNAL platform-services interface, part of the PAL. No ABI commitment.
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
 * WHY A SEPARATE PAL HEADER rather than declarations in platform.h, where the clock, secure random
 * and thread-pool wakeup live: these types are <pthread.h> and <windows.h> types. platform.h is
 * included by protocol TUs, which must stay free of platform headers, so putting them there would
 * leak windows.h into every includer. The split follows the per-concern TU pattern the PAL already
 * uses for wakeup (platform_wakeup_posix.c / platform_wakeup_win.c).
 *
 * Two implementations, selected by the build exactly as platform_posix.c / platform_win.c are:
 *
 *   platform_thread_posix.c   pthreads
 *   platform_thread_win.c     CreateThread + SRWLOCK + CONDITION_VARIABLE
 *
 * SRW locks and condition variables rather than Win32 mutex/event kernel objects: they are
 * process-local and much cheaper, and their semantics line up with pthread mutex/cond, including the
 * part that matters most here. Both allow SPURIOUS WAKEUPS, so every caller must re-check its
 * predicate in a loop around kl_plat_cond_wait. thread_pool.c already does
 * (`while (work_count == 0 && !shutdown) kl_plat_cond_wait(...)`), which is what makes this a drop-in
 * substitution rather than a semantic change.
 *
 * Mutexes are NOT recursive on either side, matching the pthread default the pool already relied on.
 */
#ifndef KEEL_SRC_PLATFORM_THREAD_H
#define KEEL_SRC_PLATFORM_THREAD_H

#if defined(_WIN32)
#include <windows.h>
typedef struct { HANDLE h; } KlPlatThread;
typedef struct { SRWLOCK l; } KlPlatMutex;
typedef struct { CONDITION_VARIABLE c; } KlPlatCond;
#else
#include <pthread.h>
typedef struct { pthread_t t; } KlPlatThread;
typedef struct { pthread_mutex_t m; } KlPlatMutex;
typedef struct { pthread_cond_t c; } KlPlatCond;
#endif

/* The thread entry point. void-returning rather than pthread's void*: nothing in Keel uses a thread's
 * return value, and Win32's DWORD return would otherwise have to be invented from nothing. */
typedef void (*KlPlatThreadFn)(void *arg);

/* Start a thread running fn(arg). Returns 0, or -1 with *out untouched. */
int  kl_plat_thread_create(KlPlatThread *out, KlPlatThreadFn fn, void *arg);
/* Wait for the thread to finish and release its handle. Must be called exactly once per successful
 * create; the pool joins every worker it started before freeing anything they touch. */
void kl_plat_thread_join(KlPlatThread *t);

/* Returns 0, or -1 when the primitive could not be created. The Win32 initialisers cannot fail, so
 * that path always returns 0; the POSIX one reports pthread_*_init. */
int  kl_plat_mutex_init(KlPlatMutex *m);
void kl_plat_mutex_lock(KlPlatMutex *m);
void kl_plat_mutex_unlock(KlPlatMutex *m);
void kl_plat_mutex_destroy(KlPlatMutex *m);

int  kl_plat_cond_init(KlPlatCond *c);
/* Atomically releases the mutex, waits, and reacquires it. May wake spuriously: callers loop. */
void kl_plat_cond_wait(KlPlatCond *c, KlPlatMutex *m);
void kl_plat_cond_signal(KlPlatCond *c);
void kl_plat_cond_broadcast(KlPlatCond *c);
void kl_plat_cond_destroy(KlPlatCond *c);

#endif /* KEEL_SRC_PLATFORM_THREAD_H */
