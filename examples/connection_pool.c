/*
 * connection_pool.c: Connection pool internals demo
 *
 * Concepts: kl_http_conn_pool_init, kl_http_conn_acquire, kl_http_conn_release,
 * pool exhaustion, active count tracking, kl_monotonic_ms.
 *
 * Build:  make examples
 * Run:    ./examples/connection_pool
 */

#include <keel/clock.h>
#include <keel/keel.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    printf("connection_pool example\n\n");

    KlAllocator alloc = kl_allocator_default();
    KlHttpConnPool pool;
    int capacity = 4;

    /* Initialize pool */
    if (kl_http_conn_pool_init(&pool, capacity, &alloc) < 0) {
        fprintf(stderr, "pool init failed\n");
        return 1;
    }
    printf("Pool initialized: capacity=%d, active=%d\n",
           pool.capacity, pool.active_count);

    /* Initialize parsers for each connection slot */
    for (int i = 0; i < capacity; i++) {
        pool.conns[i].parser = kl_http1_parser_llhttp(&alloc);
        if (!pool.conns[i].parser) {
            fprintf(stderr, "parser init failed for slot %d\n", i);
            kl_http_conn_pool_free(&pool);
            return 1;
        }
    }

    /* Acquire connections with fake fds */
    printf("\n--- Acquire ---\n");
    KlHttpConn *conns[4];
    for (int i = 0; i < capacity; i++) {
        conns[i] = kl_http_conn_acquire(&pool, 100 + i);
        printf("  acquired fd=%d, active=%d, state=%u\n",
               conns[i] ? (int)conns[i]->stream.fd : -1,
               pool.active_count,
               conns[i] ? (unsigned)conns[i]->state : 0);
    }

    /* Pool exhaustion */
    printf("\n--- Exhaustion ---\n");
    KlHttpConn *overflow = kl_http_conn_acquire(&pool, 200);
    printf("  acquire when full: %s\n",
           overflow == NULL ? "NULL (correct)" : "unexpected!");

    /* Release one and re-acquire */
    printf("\n--- Release + Re-acquire ---\n");
    conns[1]->stream.fd = KL_INVALID_SOCKET;  /* avoid closing real fds */
    kl_http_conn_release(&pool, conns[1]);
    printf("  released slot, active=%d\n", pool.active_count);

    KlHttpConn *reused = kl_http_conn_acquire(&pool, 201);
    printf("  re-acquired fd=%d, active=%d\n",
           reused ? (int)reused->stream.fd : -1, pool.active_count);

    /* Timing */
    printf("\n--- Monotonic clock ---\n");
    uint64_t t1 = kl_monotonic_ms();
    uint64_t t2 = kl_monotonic_ms();
    printf("  t1=%llu, t2=%llu, monotonic=%s\n",
           (unsigned long long)t1, (unsigned long long)t2,
           t2 >= t1 ? "YES" : "NO");

    /* Cleanup (set fake fds to -1 to avoid close() on bad fds) */
    conns[0]->stream.fd = KL_INVALID_SOCKET;
    conns[2]->stream.fd = KL_INVALID_SOCKET;
    conns[3]->stream.fd = KL_INVALID_SOCKET;
    if (reused) reused->stream.fd = KL_INVALID_SOCKET;
    kl_http_conn_pool_free(&pool);
    printf("\nPool freed.\n");

    return 0;
}
