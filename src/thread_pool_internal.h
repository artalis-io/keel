/*
 * thread_pool_internal.h: INTERNAL. A white-box probe of KlThreadPool for tests.
 *
 * Not part of the public surface and not used by the library itself. A test that must order an event
 * against kl_thread_pool_free (for example, let a running job finish only once shutdown has begun, so
 * the jobs still queued are certain to reach cancel_fn) polls this from another thread.
 */
#ifndef KEEL_SRC_THREAD_POOL_INTERNAL_H
#define KEEL_SRC_THREAD_POOL_INTERNAL_H

#include <keel/thread_pool.h>

/* 1 once kl_thread_pool_free has signalled shutdown (no queued item will start after that), else 0.
 * Safe from any thread while the pool is alive: it reads the flag under the pool's mutex. */
int keel__thread_pool_shutting_down(KlThreadPool *pool);

#endif /* KEEL_SRC_THREAD_POOL_INTERNAL_H */
