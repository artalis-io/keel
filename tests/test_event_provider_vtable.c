/*
 * test_event_provider_vtable.c: a runtime event backend (KlEventProvider.ops) whose table omits a
 * REQUIRED op must be rejected at the install boundary BEFORE any op is called, WITHOUT crashing.
 * The kl_event_* dispatch calls init/add/mod/del/wait/close/caps unconditionally when a provider is
 * installed (no builtin fallback); `native_provider` and `completion` are optional (NULL-safe).
 *
 * Failure semantics (reviewer ruling): a missing required op is malformed input (KL_ERR_INVALID_ARG);
 * a COMPLETE provider whose init() fails is a runtime init failure (KL_ERR_EVENT_INIT). A NULL ops
 * table means "use the compiled-in default", not malformed.
 *
 * Public boundary: kl_event_ctx_init_ex, which reports via KlEventCtx.last_error.
 */
#include "utest.h"
#include <keel/event_ctx.h>
#include <keel/event.h>
#include <string.h>

/* ── a configurable stub event backend (one required op omitted per test) ──────── */
static int  g_init_ret;
static int  ev_init(KlEventLoop *l) { (void)l; return g_init_ret; }
static int  ev_add(KlEventLoop *l, KlSocketHandle fd, KlEventMask m, void *u) { (void)l;(void)fd;(void)m;(void)u; return 0; }
static int  ev_mod(KlEventLoop *l, KlSocketHandle fd, KlEventMask m, void *u) { (void)l;(void)fd;(void)m;(void)u; return 0; }
static int  ev_del(KlEventLoop *l, KlSocketHandle fd) { (void)l;(void)fd; return 0; }
static int  ev_wait(KlEventLoop *l, KlEvent *o, int mx, int to) { (void)l;(void)o;(void)mx;(void)to; return 0; }
static void ev_close(KlEventLoop *l) { (void)l; }
static unsigned ev_caps(const KlEventLoop *l) { (void)l; return KL_EVENT_CAP_READINESS | KL_EVENT_CAP_NATIVE_FD; }
static const struct KlSocketProvider *ev_native(const KlEventLoop *l) { (void)l; return NULL; }

enum { OMIT_NONE = -1, OMIT_INIT, OMIT_ADD, OMIT_MOD, OMIT_DEL, OMIT_WAIT,
       OMIT_CLOSE, OMIT_CAPS, OMIT_NATIVE /* optional: accepted */ };

static void fill_ops(KlEventOps *ops, int omit) {
    memset(ops, 0, sizeof(*ops));
    ops->init = ev_init; ops->add = ev_add; ops->mod = ev_mod; ops->del = ev_del;
    ops->wait = ev_wait; ops->close = ev_close; ops->caps = ev_caps;
    ops->native_provider = ev_native;
    /* completion left NULL: optional (readiness backend). */
    switch (omit) {
        case OMIT_INIT:   ops->init = NULL; break;
        case OMIT_ADD:    ops->add = NULL; break;
        case OMIT_MOD:    ops->mod = NULL; break;
        case OMIT_DEL:    ops->del = NULL; break;
        case OMIT_WAIT:   ops->wait = NULL; break;
        case OMIT_CLOSE:  ops->close = NULL; break;
        case OMIT_CAPS:   ops->caps = NULL; break;
        case OMIT_NATIVE: ops->native_provider = NULL; break;
        default: break;
    }
}

/* A complete provider (completion optional/NULL) installs cleanly and frees. */
UTEST(event_provider_vtable, complete_provider_installs) {
    KlAllocator alloc = kl_allocator_default();
    KlEventOps ops; fill_ops(&ops, OMIT_NONE);
    KlEventProvider prov = { &ops, "stub" };
    g_init_ret = 0;
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init_ex(&ev, &alloc, &prov), 0);
    ASSERT_EQ((int)ev.last_error, (int)KL_ERR_NONE);
    kl_event_ctx_free(&ev);
}

/* Each missing REQUIRED op => rejected as malformed input (KL_ERR_INVALID_ARG), no crash. */
UTEST(event_provider_vtable, missing_required_op_is_invalid_arg) {
    int cases[] = { OMIT_INIT, OMIT_ADD, OMIT_MOD, OMIT_DEL, OMIT_WAIT,
                    OMIT_CLOSE, OMIT_CAPS };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        KlAllocator alloc = kl_allocator_default();
        KlEventOps ops; fill_ops(&ops, cases[i]);
        KlEventProvider prov = { &ops, "stub" };
        g_init_ret = 0;
        KlEventCtx ev;
        ASSERT_EQ(kl_event_ctx_init_ex(&ev, &alloc, &prov), -1);
        ASSERT_EQ((int)ev.last_error, (int)KL_ERR_INVALID_ARG);
    }
}

/* The optional native_provider slot may be NULL: the provider installs cleanly. */
UTEST(event_provider_vtable, null_native_provider_accepted) {
    KlAllocator alloc = kl_allocator_default();
    KlEventOps ops; fill_ops(&ops, OMIT_NATIVE);
    KlEventProvider prov = { &ops, "stub" };
    g_init_ret = 0;
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init_ex(&ev, &alloc, &prov), 0);
    ASSERT_EQ((int)ev.last_error, (int)KL_ERR_NONE);
    kl_event_ctx_free(&ev);
}

/* A COMPLETE provider whose init() fails is a runtime init failure, not malformed input. */
UTEST(event_provider_vtable, complete_but_init_fails_is_event_init) {
    KlAllocator alloc = kl_allocator_default();
    KlEventOps ops; fill_ops(&ops, OMIT_NONE);
    KlEventProvider prov = { &ops, "stub" };
    g_init_ret = -1;   /* init reports failure */
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init_ex(&ev, &alloc, &prov), -1);
    ASSERT_EQ((int)ev.last_error, (int)KL_ERR_EVENT_INIT);
}

/* A provider with a NULL ops table means "use the compiled-in default": not malformed. */
UTEST(event_provider_vtable, null_ops_falls_back_to_builtin) {
    KlAllocator alloc = kl_allocator_default();
    KlEventProvider prov = { NULL, "stub" };
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init_ex(&ev, &alloc, &prov), 0);
    kl_event_ctx_free(&ev);
}

UTEST_MAIN();
