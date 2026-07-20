/*
 * connection_pool.c — Connection pool internals demo
 *
 * Concepts: kl_conn_pool_init, kl_conn_acquire, kl_conn_release,
 * pool exhaustion, active count tracking, kl_monotonic_ms.
 *
 * Build:  make examples
 * Run:    ./examples/connection_pool
 */

#include <keel/keel.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    printf("connection_pool example\n\n");

    KlAllocator alloc = kl_allocator_default();
    KlConnPool pool;
    int capacity = 4;

    /* Initialize pool */
    if (kl_conn_pool_init(&pool, capacity, &alloc) < 0) {
        fprintf(stderr, "pool init failed\n");
        return 1;
    }
    printf("Pool initialized: capacity=%d, active=%d\n",
           pool.capacity, pool.active_count);

    /* Initialize parsers for each connection slot */
    for (int i = 0; i < capacity; i++) {
        pool.conns[i].parser = kl_parser_llhttp(&alloc);
        if (!pool.conns[i].parser) {
            fprintf(stderr, "parser init failed for slot %d\n", i);
            kl_conn_pool_free(&pool);
            return 1;
        }
    }

    /* Acquire connections with fake fds */
    printf("\n--- Acquire ---\n");
    KlConn *conns[4];
    for (int i = 0; i < capacity; i++) {
        conns[i] = kl_conn_acquire(&pool, 100 + i);
        printf("  acquired fd=%d, active=%d, state=%u\n",
               conns[i] ? (int)conns[i]->fd : -1,
               pool.active_count,
               conns[i] ? (unsigned)conns[i]->state : 0);
    }

    /* Pool exhaustion */
    printf("\n--- Exhaustion ---\n");
    KlConn *overflow = kl_conn_acquire(&pool, 200);
    printf("  acquire when full: %s\n",
           overflow == NULL ? "NULL (correct)" : "unexpected!");

    /* Release one and re-acquire */
    printf("\n--- Release + Re-acquire ---\n");
    conns[1]->fd = -1;  /* avoid closing real fds */
    kl_conn_release(&pool, conns[1]);
    printf("  released slot, active=%d\n", pool.active_count);

    KlConn *reused = kl_conn_acquire(&pool, 201);
    printf("  re-acquired fd=%d, active=%d\n",
           reused ? (int)reused->fd : -1, pool.active_count);

    /* Timing */
    printf("\n--- Monotonic clock ---\n");
    uint64_t t1 = kl_monotonic_ms();
    uint64_t t2 = kl_monotonic_ms();
    printf("  t1=%llu, t2=%llu, monotonic=%s\n",
           (unsigned long long)t1, (unsigned long long)t2,
           t2 >= t1 ? "YES" : "NO");

    /* Cleanup (set fake fds to -1 to avoid close() on bad fds) */
    conns[0]->fd = -1;
    conns[2]->fd = -1;
    conns[3]->fd = -1;
    if (reused) reused->fd = -1;
    kl_conn_pool_free(&pool);
    printf("\nPool freed.\n");

    return 0;
}
