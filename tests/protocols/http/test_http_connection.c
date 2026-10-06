#include "utest.h"
#include "../../../src/protocols/http/http_conn_internal.h"
#include <keel/clock.h>
#include <keel/http_connection.h>
#include <keel/event_ctx.h>
#include <keel/socket.h>
#include <errno.h>
#include <string.h>

static int transient_calls, transient_interrupt, transient_terminal, transient_errors;
static kl_ssize_t transient_recv(void *ctx, KlSocketHandle fd, void *buf, size_t cap) {
    (void)ctx; (void)fd; (void)buf; (void)cap;
    transient_calls++;
    if (transient_interrupt && transient_calls == 1) { errno = EINTR; return -1; }
    if (transient_terminal == 1) return 0;
    errno = transient_terminal == 2 ? ECONNRESET : EAGAIN;
    return -1;
}
static KlIoStatus transient_status(void *ctx) {
    (void)ctx;
    if (errno == EINTR) return KL_IO_INTERRUPTED;
    return errno == EAGAIN ? KL_IO_WOULD_BLOCK : KL_IO_RESET;
}
static void transient_body_error(KlHttpBodyReader *reader) { (void)reader; transient_errors++; }

UTEST(connection, readiness_transient_recv_does_not_end_headers_or_body) {
    const KlSocketOps ops = { .recv = transient_recv, .io_status = transient_status };
    const KlSocketProvider provider = { .ops = &ops, .capabilities = KL_SOCK_CAP_NATIVE_FD };
    KlEventCtx ev = { .sockets = &provider };
    KlHttpBodyReader reader = { .on_error = transient_body_error };
    char buf[256]; KlHttpRouter router = {0};
    for (int body = 0; body < 2; body++) for (int interrupt = 0; interrupt < 2; interrupt++) {
        KlHttpConn c; memset(&c, 0, sizeof(c));
        c.stream.ctx = &ev; c.stream.fd = 42;
        c.stream.read_buf = buf; c.stream.read_cap = sizeof(buf);
        c.state = body ? KL_HTTP_CONN_READING_BODY : KL_HTTP_CONN_READING;
        c.req.body_reader = body ? &reader : NULL;
        c.max_header_size = sizeof(buf);
        transient_calls = transient_errors = transient_terminal = 0;
        transient_interrupt = interrupt;
        KlHttpConnState state = c.state;
        ASSERT_EQ(state, kl_http_conn_on_readable(&c, &router));
        ASSERT_EQ(1 + interrupt, transient_calls); /* EINTR retried, would-block re-armed */
        ASSERT_EQ(0, transient_errors);
        ASSERT_EQ((size_t)0, c.stream.read_len);
    }
}

UTEST(connection, readiness_eof_and_reset_still_end_headers_or_body) {
    const KlSocketOps ops = { .recv = transient_recv, .io_status = transient_status };
    const KlSocketProvider provider = { .ops = &ops, .capabilities = KL_SOCK_CAP_NATIVE_FD };
    KlEventCtx ev = { .sockets = &provider };
    KlHttpBodyReader reader = { .on_error = transient_body_error };
    char buf[256]; KlHttpRouter router = {0};
    for (int body = 0; body < 2; body++) for (int terminal = 1; terminal < 3; terminal++) {
        KlHttpConn c; memset(&c, 0, sizeof(c));
        c.stream.ctx = &ev; c.stream.fd = 42;
        c.stream.read_buf = buf; c.stream.read_cap = sizeof(buf);
        c.state = body ? KL_HTTP_CONN_READING_BODY : KL_HTTP_CONN_READING;
        c.req.body_reader = body ? &reader : NULL;
        c.max_header_size = sizeof(buf);
        transient_calls = transient_errors = transient_interrupt = 0;
        transient_terminal = terminal;
        ASSERT_EQ(KL_HTTP_CONN_CLOSED, kl_http_conn_on_readable(&c, &router));
        ASSERT_EQ(1, transient_calls);
        ASSERT_EQ(body, transient_errors);
    }
}

UTEST(connection, pool_init_and_free) {
    KlAllocator a = kl_allocator_default();
    KlHttpConnPool pool;
    ASSERT_EQ(kl_http_conn_pool_init(&pool, 8, &a), 0);
    ASSERT_EQ(pool.capacity, 8);
    kl_http_conn_pool_free(&pool);
}

UTEST(connection, acquire_and_release) {
    KlAllocator a = kl_allocator_default();
    KlHttpConnPool pool;
    kl_http_conn_pool_init(&pool, 4, &a);

    /* Create parsers for the pool */
    for (int i = 0; i < 4; i++) {
        pool.conns[i].parser = kl_http1_parser_llhttp(&a);
    }

    /* Acquire connections with fake fds */
    int fakefd = 100;
    KlHttpConn *c1 = kl_http_conn_acquire(&pool, fakefd);
    ASSERT_TRUE(c1 != NULL);
    ASSERT_EQ(c1->stream.fd, fakefd);
    ASSERT_EQ(c1->state, KL_HTTP_CONN_READING);

    KlHttpConn *c2 = kl_http_conn_acquire(&pool, fakefd + 1);
    ASSERT_TRUE(c2 != NULL);
    ASSERT_TRUE(c1 != c2);

    /* Release: set fd to -1 to avoid closing real fds */
    c1->stream.fd = -1;
    kl_http_conn_release(&pool, c1);

    /* Should be able to acquire again */
    KlHttpConn *c3 = kl_http_conn_acquire(&pool, fakefd + 2);
    ASSERT_TRUE(c3 != NULL);

    /* Clean up */
    c2->stream.fd = -1;
    c3->stream.fd = -1;
    kl_http_conn_pool_free(&pool);
}

UTEST(connection, pool_exhaustion) {
    KlAllocator a = kl_allocator_default();
    KlHttpConnPool pool;
    kl_http_conn_pool_init(&pool, 2, &a);

    for (int i = 0; i < 2; i++) {
        pool.conns[i].parser = kl_http1_parser_llhttp(&a);
    }

    KlHttpConn *c1 = kl_http_conn_acquire(&pool, 100);
    KlHttpConn *c2 = kl_http_conn_acquire(&pool, 101);
    ASSERT_TRUE(c1 != NULL);
    ASSERT_TRUE(c2 != NULL);

    /* Pool exhausted */
    KlHttpConn *c3 = kl_http_conn_acquire(&pool, 102);
    ASSERT_TRUE(c3 == NULL);

    c1->stream.fd = -1;
    c2->stream.fd = -1;
    kl_http_conn_pool_free(&pool);
}

UTEST(connection, active_count_tracking) {
    KlAllocator a = kl_allocator_default();
    KlHttpConnPool pool;
    kl_http_conn_pool_init(&pool, 4, &a);
    ASSERT_EQ(pool.active_count, 0);

    for (int i = 0; i < 4; i++) {
        pool.conns[i].parser = kl_http1_parser_llhttp(&a);
    }

    KlHttpConn *c1 = kl_http_conn_acquire(&pool, 100);
    ASSERT_EQ(pool.active_count, 1);

    KlHttpConn *c2 = kl_http_conn_acquire(&pool, 101);
    ASSERT_EQ(pool.active_count, 2);

    KlHttpConn *c3 = kl_http_conn_acquire(&pool, 102);
    ASSERT_EQ(pool.active_count, 3);

    /* Release one, count decrements, free_list non-NULL */
    c2->stream.fd = -1;
    kl_http_conn_release(&pool, c2);
    ASSERT_EQ(pool.active_count, 2);
    ASSERT_TRUE(pool.free_list != NULL);

    /* Acquire again, count back to 3 */
    KlHttpConn *c4 = kl_http_conn_acquire(&pool, 103);
    ASSERT_TRUE(c4 != NULL);
    ASSERT_EQ(pool.active_count, 3);

    c1->stream.fd = -1;
    c3->stream.fd = -1;
    c4->stream.fd = -1;
    kl_http_conn_pool_free(&pool);
}

/* ═══════════════════════════════════════════════════════════════════
 * State machine tests
 * ═══════════════════════════════════════════════════════════════════ */

UTEST(connection, state_initial) {
    KlAllocator a = kl_allocator_default();
    KlHttpConnPool pool;
    kl_http_conn_pool_init(&pool, 4, &a);
    for (int i = 0; i < 4; i++)
        pool.conns[i].parser = kl_http1_parser_llhttp(&a);

    KlHttpConn *c = kl_http_conn_acquire(&pool, 100);
    ASSERT_TRUE(c != NULL);
    ASSERT_EQ(c->state, KL_HTTP_CONN_READING);

    c->stream.fd = -1;
    kl_http_conn_pool_free(&pool);
}

UTEST(connection, release_resets_state) {
    KlAllocator a = kl_allocator_default();
    KlHttpConnPool pool;
    kl_http_conn_pool_init(&pool, 4, &a);
    for (int i = 0; i < 4; i++)
        pool.conns[i].parser = kl_http1_parser_llhttp(&a);

    KlHttpConn *c = kl_http_conn_acquire(&pool, 100);
    ASSERT_TRUE(c != NULL);

    /* Simulate some activity */
    c->state = KL_HTTP_CONN_PROCESSING;
    c->stream.read_len = 42;

    /* Release and re-acquire */
    c->stream.fd = -1;
    kl_http_conn_release(&pool, c);

    KlHttpConn *c2 = kl_http_conn_acquire(&pool, 101);
    ASSERT_TRUE(c2 != NULL);
    ASSERT_EQ(c2->state, KL_HTTP_CONN_READING);
    ASSERT_EQ(c2->stream.read_len, (size_t)0);

    c2->stream.fd = -1;
    kl_http_conn_pool_free(&pool);
}

UTEST(connection, acquire_after_release) {
    KlAllocator a = kl_allocator_default();
    KlHttpConnPool pool;
    kl_http_conn_pool_init(&pool, 2, &a);
    for (int i = 0; i < 2; i++)
        pool.conns[i].parser = kl_http1_parser_llhttp(&a);

    KlHttpConn *c1 = kl_http_conn_acquire(&pool, 100);
    KlHttpConn *c2 = kl_http_conn_acquire(&pool, 101);
    ASSERT_TRUE(c1 != NULL);
    ASSERT_TRUE(c2 != NULL);

    /* Pool full */
    ASSERT_TRUE(kl_http_conn_acquire(&pool, 102) == NULL);

    /* Release c1 */
    c1->stream.fd = -1;
    kl_http_conn_release(&pool, c1);
    ASSERT_EQ(pool.active_count, 1);

    /* Acquire again; should succeed */
    KlHttpConn *c3 = kl_http_conn_acquire(&pool, 103);
    ASSERT_TRUE(c3 != NULL);
    ASSERT_EQ(c3->stream.fd, 103);
    ASSERT_EQ(pool.active_count, 2);

    c2->stream.fd = -1;
    c3->stream.fd = -1;
    kl_http_conn_pool_free(&pool);
}

UTEST(connection, monotonic_ms) {
    uint64_t t1 = kl_monotonic_ms();
    uint64_t t2 = kl_monotonic_ms();
    /* Monotonic: t2 >= t1 */
    ASSERT_TRUE(t2 >= t1);
    /* Should be reasonably close (within 1 second) */
    ASSERT_TRUE(t2 - t1 < 1000);
}

UTEST(connection, pool_capacity_zero) {
    KlAllocator a = kl_allocator_default();
    KlHttpConnPool pool;
    /* Capacity 0, should handle gracefully (return error or empty pool) */
    int rc = kl_http_conn_pool_init(&pool, 0, &a);
    if (rc == 0) {
        /* If it succeeds with 0 capacity, acquire should return NULL */
        KlHttpConn *c = kl_http_conn_acquire(&pool, 100);
        ASSERT_TRUE(c == NULL);
        kl_http_conn_pool_free(&pool);
    } else {
        /* Init returning -1 for 0 capacity is also acceptable */
        ASSERT_EQ(rc, -1);
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * Growable read buffer tests
 * ═══════════════════════════════════════════════════════════════════ */

UTEST(connection, read_buf_allocated) {
    KlAllocator a = kl_allocator_default();
    KlHttpConnPool pool;
    ASSERT_EQ(kl_http_conn_pool_init(&pool, 4, &a), 0);

    for (int i = 0; i < 4; i++) {
        ASSERT_TRUE(pool.conns[i].stream.read_buf != NULL);
        ASSERT_EQ(pool.conns[i].stream.read_cap, (size_t)KL_HTTP_CONN_READ_BUF_SIZE);
    }

    kl_http_conn_pool_free(&pool);
}

UTEST(connection, read_buf_survives_acquire_release) {
    KlAllocator a = kl_allocator_default();
    KlHttpConnPool pool;
    kl_http_conn_pool_init(&pool, 2, &a);
    for (int i = 0; i < 2; i++)
        pool.conns[i].parser = kl_http1_parser_llhttp(&a);

    KlHttpConn *c = kl_http_conn_acquire(&pool, 100);
    ASSERT_TRUE(c != NULL);
    ASSERT_TRUE(c->stream.read_buf != NULL);
    ASSERT_EQ(c->stream.read_cap, (size_t)KL_HTTP_CONN_READ_BUF_SIZE);

    char *buf_ptr = c->stream.read_buf;
    c->stream.fd = -1;
    kl_http_conn_release(&pool, c);

    /* Re-acquire; buffer should still be valid */
    KlHttpConn *c2 = kl_http_conn_acquire(&pool, 101);
    ASSERT_TRUE(c2 != NULL);
    ASSERT_TRUE(c2->stream.read_buf != NULL);
    ASSERT_EQ(c2->stream.read_cap, (size_t)KL_HTTP_CONN_READ_BUF_SIZE);
    /* Same slot, same buffer pointer */
    ASSERT_EQ(c2->stream.read_buf, buf_ptr);

    c2->stream.fd = -1;
    kl_http_conn_pool_free(&pool);
}

UTEST(connection, max_header_size_default) {
    /* max_header_size defaults to KL_HTTP_CONN_READ_BUF_SIZE when 0 */
    KlAllocator a = kl_allocator_default();
    KlHttpConnPool pool;
    kl_http_conn_pool_init(&pool, 2, &a);

    /* Before server init sets it, max_header_size is 0 */
    ASSERT_EQ(pool.conns[0].max_header_size, (size_t)0);

    /* Simulate what http_server.c does */
    for (int i = 0; i < 2; i++)
        pool.conns[i].max_header_size = KL_HTTP_CONN_READ_BUF_SIZE;

    ASSERT_EQ(pool.conns[0].max_header_size, (size_t)KL_HTTP_CONN_READ_BUF_SIZE);
    ASSERT_EQ(pool.conns[1].max_header_size, (size_t)KL_HTTP_CONN_READ_BUF_SIZE);

    kl_http_conn_pool_free(&pool);
}

/* F2-B accessor: kl_http_conn_response[_const] return the connection's response, NULL on NULL. */
UTEST(connection, response_accessor) {
    KlAllocator a = kl_allocator_default();
    KlHttpConnPool pool;
    ASSERT_EQ(kl_http_conn_pool_init(&pool, 2, &a), 0);
    for (int i = 0; i < 2; i++)
        pool.conns[i].parser = kl_http1_parser_llhttp(&a);

    KlHttpConn *c = kl_http_conn_acquire(&pool, 100);
    ASSERT_TRUE(c != NULL);
    ASSERT_EQ((void *)kl_http_conn_response(c), (void *)&c->res);
    ASSERT_EQ((const void *)kl_http_conn_response_const(c), (const void *)&c->res);
    ASSERT_TRUE(kl_http_conn_response(NULL) == NULL);
    ASSERT_TRUE(kl_http_conn_response_const(NULL) == NULL);

    /* peer-address accessor: returns the connection's stored peer address and reads it back. */
    uint8_t ip[4] = { 203, 0, 113, 7 };
    kl_sockaddr_from_ipv4(&c->stream.peer_addr, ip, 4321);
    const KlSockAddr *pa = kl_http_conn_peer_addr(c);
    ASSERT_EQ((const void *)pa, (const void *)&c->stream.peer_addr);
    ASSERT_EQ((int)kl_sockaddr_family(pa), (int)KL_AF_INET);
    ASSERT_EQ(4321, (int)kl_sockaddr_port(pa));

    c->stream.fd = -1;
    kl_http_conn_pool_free(&pool);
}

/* ── A posted send stays where it is until it completes ─────────────────────────────────────────
 * A completion backend may read a posted send in place until it completes (completion.h permits it;
 * the lwIP raw integration does). The output queue compacted and reallocated its buffer while such a
 * send was in flight, so the backend then sent moved or freed memory. Growing the queue must leave
 * the posted bytes exactly where they were. The allocator never really frees: it poisons a block
 * when it is released (or moved by realloc), so a posted region that was moved or freed shows. */
#ifndef KEEL_NO_COMPLETION
kl_ssize_t kl_comp_queue_write(KlHttpConn *c, const void *buf, size_t len);

#define PQ_MAX_BLOCKS 16
static struct { void *p; size_t n; } pq_blocks[PQ_MAX_BLOCKS];
static int pq_nblocks;

static void *pq_malloc(void *ctx, size_t n) {
    (void)ctx;
    if (pq_nblocks == PQ_MAX_BLOCKS) return NULL;
    void *p = malloc(n);
    if (p) { pq_blocks[pq_nblocks].p = p; pq_blocks[pq_nblocks].n = n; pq_nblocks++; }
    return p;
}
static void pq_free(void *ctx, void *p, size_t n) {
    (void)ctx;
    if (p) memset(p, 0xDD, n);                             /* released: poisoned, kept until the end */
}
static void *pq_realloc(void *ctx, void *p, size_t old_n, size_t n) {
    void *q = pq_malloc(ctx, n);                           /* always moves, like a realloc may */
    if (!q) return NULL;
    if (p) { memcpy(q, p, old_n < n ? old_n : n); pq_free(ctx, p, old_n); }
    return q;
}

UTEST(conn, a_posted_send_is_not_moved_while_the_queue_grows) {
    KlAllocator pa = { pq_malloc, pq_realloc, pq_free, NULL };
    pq_nblocks = 0;
    KlHttpConn conn;
    memset(&conn, 0, sizeof conn);
    conn.stream.fd = KL_INVALID_SOCKET;
    conn.stream.alloc = &pa;
    conn.comp_driven = 1;
    conn.state = KL_HTTP_CONN_PROCESSING;
    const size_t posted = 16 * 1024;
    unsigned char *region = kl_malloc(&pa, posted);
    ASSERT_TRUE(region != NULL);
    memset(region, 'A', posted);
    conn.comp_tlsq = region;                               /* one send posted from the whole buffer */
    conn.comp_tlsq_cap = conn.comp_tlsq_len = posted;
    conn.comp_tlsq_head = 0;
    conn.comp_tlsq_inflight = 1;
    conn.comp_tlsq_inflight_len = posted;

    static unsigned char more[64 * 1024];
    memset(more, 'B', sizeof more);
    kl_ssize_t n = kl_comp_queue_write(&conn, more, sizeof more);   /* must grow the queue */

    size_t intact = 0;                                     /* the posted bytes, where they were */
    while (intact < posted && region[intact] == 'A') intact++;
    int tail_ok = conn.comp_tlsq != NULL && conn.comp_tlsq_len >= sizeof more &&
                  memcmp(conn.comp_tlsq + conn.comp_tlsq_len - sizeof more, more, sizeof more) == 0;

    for (int i = 0; i < pq_nblocks; i++) free(pq_blocks[i].p);
    pq_nblocks = 0;
    ASSERT_EQ(n, (kl_ssize_t)sizeof more);
    ASSERT_TRUE(tail_ok);
    ASSERT_EQ(intact, posted);                             /* was: moved, the old block released */
}
#endif /* !KEEL_NO_COMPLETION */

UTEST_MAIN();
