#ifndef KEEL_CONNECTION_H
#define KEEL_CONNECTION_H

#include <keel/allocator.h>
#include <keel/request.h>
#include <keel/response.h>
#include <keel/parser.h>
#include <keel/router.h>
#include <stddef.h>
#include <stdint.h>

#define KL_READ_BUF_SIZE 8192

typedef enum {
    KL_CONN_READING,
    KL_CONN_READING_BODY,
    KL_CONN_PROCESSING,
    KL_CONN_SENDING,
    KL_CONN_CLOSED
} KlConnState;

typedef struct KlConn {
    int fd;
    KlConnState state;
    KlAllocator *alloc;     /* set once on pool init, never NULL */

    char read_buf[KL_READ_BUF_SIZE];
    size_t read_len;

    KlRequest req;
    KlResponse res;
    KlParser *parser;

    size_t hdr_sent;

    /* Routing (set after HEADERS_OK, used in PROCESSING) */
    KlRoute *route;
    KlParam params[KL_MAX_PARAMS];
    int num_params;
    int route_result;

    uint64_t last_active_ms;   /* monotonic clock, updated on every I/O */

    /* Pool linkage */
    struct KlConn *next_free;
} KlConn;

typedef struct {
    KlConn *conns;
    int capacity;
    KlConn *free_list;
    KlAllocator *alloc;
} KlConnPool;

int     kl_conn_pool_init(KlConnPool *pool, int capacity, KlAllocator *alloc);
KlConn *kl_conn_acquire(KlConnPool *pool, int fd);
void    kl_conn_release(KlConnPool *pool, KlConn *c);
void    kl_conn_pool_free(KlConnPool *pool);

/* State transitions */
KlConnState kl_conn_on_readable(KlConn *c, KlRouter *router);
KlConnState kl_conn_on_writable(KlConn *c);

/* Monotonic clock in milliseconds */
uint64_t kl_monotonic_ms(void);

#endif
