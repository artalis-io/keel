/*
 * test_event_caps.c — PAL Phase 7: event/socket capability negotiation.
 *
 * Proves the contract is real, not a constant `true`:
 *   1. the built (compile-time-selected) event backend advertises
 *      READINESS | NATIVE_FD;
 *   2. kl_event_ctx_sockets_compatible() ACCEPTS the built-in POSIX default
 *      (NULL provider) and an explicit native-fd provider;
 *   3. it REJECTS a non-native provider against the readiness loop — the case the
 *      old provider-only guard could not distinguish from "no provider".
 * Internal seam test: it reaches into ../src/event_caps.h directly.
 */
#include "utest.h"
#include "../src/event_caps.h"

#include <keel/event_ctx.h>
#include <keel/allocator.h>
#include <keel/socket.h>

/* A provider whose ops are never dereferenced by the negotiation (it reads only
 * capabilities), so a zeroed ops table is fine. */
static const KlSocketOps stub_ops = { .name = "stub" };

static const KlSocketProvider native_provider = {
    &stub_ops, NULL, KL_SOCK_CAP_NATIVE_FD | KL_SOCK_CAP_WRITEV,
};

/* No NATIVE_FD — models a completion/raw provider (e.g. lwIP raw tcp_pcb*) that a
 * readiness loop cannot watch. */
static const KlSocketProvider non_native_provider = {
    &stub_ops, NULL, KL_SOCK_CAP_WRITEV,
};

UTEST(event_caps, backend_advertises_readiness_native_fd) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(kl_event_ctx_init(&ctx, &alloc), 0);

    unsigned caps = kl_event_caps(&ctx.loop);
    /* Every backend Keel ships today (epoll/kqueue/poll/wsapoll/io_uring) is a
     * readiness poller of native OS descriptors. */
    ASSERT_TRUE(caps & KL_EVENT_CAP_READINESS);
    ASSERT_TRUE(caps & KL_EVENT_CAP_NATIVE_FD);
    /* No backend advertises the reserved completion model yet (Phase 8). */
    ASSERT_FALSE(caps & KL_EVENT_CAP_COMPLETION);

    kl_event_ctx_free(&ctx);
}

UTEST(event_caps, accepts_builtin_and_native_provider) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(kl_event_ctx_init(&ctx, &alloc), 0);

    /* Built-in default: NULL provider == POSIX native-fd. */
    ctx.sockets = NULL;
    ASSERT_TRUE(kl_event_ctx_sockets_compatible(&ctx));

    /* Explicit native-fd provider. */
    ctx.sockets = &native_provider;
    ASSERT_TRUE(kl_event_ctx_sockets_compatible(&ctx));

    kl_event_ctx_free(&ctx);
}

UTEST(event_caps, rejects_non_native_provider) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(kl_event_ctx_init(&ctx, &alloc), 0);

    /* A non-native provider cannot be watched by a readiness loop — the guard must
     * reject it (proving it is a real two-sided negotiation, not `true`). */
    ctx.sockets = &non_native_provider;
    ASSERT_FALSE(kl_event_ctx_sockets_compatible(&ctx));

    kl_event_ctx_free(&ctx);
}

UTEST(event_caps, null_ctx_is_incompatible) {
    /* Defensive: a NULL ctx negotiates as incompatible, never a crash. */
    ASSERT_FALSE(kl_event_ctx_sockets_compatible(NULL));
}

UTEST_MAIN();
