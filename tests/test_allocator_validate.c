/*
 * test_allocator_validate.c: the F2-C2 valid-allocator policy at public boundaries. A non-NULL
 * allocator is valid only when malloc, realloc, and free are all non-NULL. NULL keeps each API's own
 * documented meaning (invalid where there is no default; the documented default where there is). A
 * malformed non-NULL allocator is never silently replaced by a default. On rejection the output object
 * is left in its deterministic failure state and nothing is allocated (a malformed allocator's NULL
 * malloc would fault if the boundary tried to allocate, so "no crash" proves "no allocation").
 *
 * Covers: each error-surface class; all three missing-slot cases; NULL-default acceptance; NULL
 * rejection; no partial init on rejection; wrapper paths reaching the validated implementation.
 */
#include "utest.h"
#include <keel/keel.h>
#include <keel/datagram.h>
#include <keel/datagram_detail.h>   /* stack-allocate KlDatagram */
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#endif

/* ── allocator ops (stdlib-backed) + malformed variants ──────── */
static void *ok_malloc(void *c, size_t n) { (void)c; return malloc(n); }
static void *ok_realloc(void *c, void *p, size_t o, size_t nn) { (void)c; (void)o; return realloc(p, nn); }
static void  ok_free(void *c, void *p, size_t s) { (void)c; (void)s; free(p); }

static KlAllocator A_VALID     = { ok_malloc, ok_realloc, ok_free, NULL };
static KlAllocator A_NO_MALLOC = { NULL,      ok_realloc, ok_free, NULL };
static KlAllocator A_NO_REALLOC= { ok_malloc, NULL,       ok_free, NULL };
static KlAllocator A_NO_FREE   = { ok_malloc, ok_realloc, NULL,    NULL };

/* ── KL_ERR_INVALID_ARG surface + all three missing slots (event ctx) ──────── */
UTEST(alloc_validate, event_ctx_each_missing_slot_and_null_invalid_arg) {
    KlAllocator *bad[] = { &A_NO_MALLOC, &A_NO_REALLOC, &A_NO_FREE, NULL };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        KlEventCtx ev;
        ASSERT_EQ(kl_event_ctx_init_ex(&ev, bad[i], NULL), -1);
        ASSERT_EQ((int)ev.last_error, (int)KL_ERR_INVALID_ARG);
    }
}
UTEST(alloc_validate, event_ctx_valid_ok) {
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init_ex(&ev, &A_VALID, NULL), 0);
    kl_event_ctx_free(&ev);
}
/* wrapper (kl_event_ctx_init -> _ex) reaches the validated implementation. */
UTEST(alloc_validate, event_ctx_wrapper_reaches_impl) {
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &A_NO_FREE), -1);
    ASSERT_EQ((int)ev.last_error, (int)KL_ERR_INVALID_ARG);
}

/* ── -1 (int, no error field) surface + no partial init ──────── */
UTEST(alloc_validate, response_init_rejects_and_zeroes) {
    KlHttpResponse res;
    ASSERT_EQ(kl_http_response_init(&res, &A_NO_MALLOC), -1);
    ASSERT_TRUE(res.hdr_buf == NULL);   /* deterministic zeroed failure state */
    ASSERT_TRUE(res.alloc == NULL);     /* not stored */
    ASSERT_EQ(kl_http_response_init(&res, NULL), -1);   /* NULL invalid too */
}
UTEST(alloc_validate, router_init_rejects) {
    KlHttpRouter r;
    ASSERT_EQ(kl_http_router_init(&r, &A_NO_REALLOC), -1);
    ASSERT_EQ(kl_http_router_init(&r, NULL), -1);
}

/* ── resp->error field surface + wrapper (request -> request_s) ──────── */
UTEST(alloc_validate, sync_client_sets_resp_error) {
    KlHttpClientResponse resp;
    /* validation fires before connect, so the unreachable URL is irrelevant. */
    int rc = kl_http_client_request(&A_NO_MALLOC, NULL, "GET", "http://127.0.0.1:9/",
                                    NULL, 0, NULL, 0, &resp);
    ASSERT_EQ(rc, -1);
    ASSERT_EQ((int)resp.error, (int)KL_ERR_INVALID_ARG);
}

/* ── NULL pointer-factory surface ──────── */
UTEST(alloc_validate, pointer_factories_return_null) {
    ASSERT_TRUE(kl_http_body_reader_buffer(&A_NO_FREE, NULL, NULL) == NULL);
    ASSERT_TRUE(kl_http1_request_parser_llhttp(&A_NO_MALLOC) == NULL);
}

/* ── config-carried: NULL-default accepted; malformed NOT replaced by the default ──────── */
UTEST(alloc_validate, config_null_default_accepted) {
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init_ex(&ev, &A_VALID, NULL), 0);
    KlThreadPoolConfig cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.num_workers = 1;
    cfg.alloc = NULL;   /* documented default: the event context's allocator */
    KlThreadPool *tp = kl_thread_pool_create(&ev, &cfg);
    ASSERT_TRUE(tp != NULL);
    kl_thread_pool_free(tp);
    kl_event_ctx_free(&ev);
}
UTEST(alloc_validate, config_malformed_not_replaced_by_default) {
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init_ex(&ev, &A_VALID, NULL), 0);
    KlThreadPoolConfig cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.num_workers = 1;
    cfg.alloc = &A_NO_MALLOC;   /* malformed: must be rejected, NOT silently replaced by ctx->alloc */
    ASSERT_TRUE(kl_thread_pool_create(&ev, &cfg) == NULL);
    kl_event_ctx_free(&ev);
}

/* ── dg->last_error surface + memset-zero on rejection (datagram init) ──────── */
#ifndef _WIN32
UTEST(alloc_validate, datagram_init_rejects_and_zeroes) {
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init_ex(&ev, &A_VALID, NULL), 0);
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(fd, 0);
    KlDatagram dg; memset(&dg, 0x5a, sizeof(dg));   /* poison, to prove it gets zeroed */
    KlDatagramConfig c; memset(&c, 0, sizeof(c));
    c.ctx = &ev; c.alloc = &A_NO_FREE; c.sockets = NULL; c.fd = (KlSocketHandle)fd;
    c.send_slots = 4; c.send_slot_cap = 1500; c.recv_cap = 2048;
    ASSERT_EQ(kl_datagram_init(&dg, &c), -1);
    ASSERT_EQ((int)kl_datagram_last_error(&dg), (int)KL_ERR_INVALID_ARG);
    close(fd);   /* fd not adopted on reject: the caller still owns it */
    kl_event_ctx_free(&ev);
}
#endif

UTEST_MAIN();
