/*
 * test_event_caps.c — event/socket capability negotiation.
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
#include "../src/socket.h"   /* internal KL_SOCK_CAP_OVERLAPPED */
#include "../src/completion_io.h"   /* kl_completion_axis_available() — 0 under KEEL_NO_COMPLETION */

#include <keel/event_ctx.h>
#include <keel/allocator.h>
#include <keel/socket.h>

/* A provider whose ops are never dereferenced by the negotiation (it reads only
 * capabilities), so a zeroed ops table is fine. */
static const KlSocketOps stub_ops = { .name = "stub" };

static const KlSocketProvider native_provider = {
    &stub_ops, NULL, KL_SOCK_CAP_NATIVE_FD | KL_SOCK_CAP_WRITEV,
    NULL,
};

/* No NATIVE_FD — models a completion/raw provider (e.g. lwIP raw tcp_pcb*) that a
 * readiness loop cannot watch. */
static const KlSocketProvider non_native_provider = {
    &stub_ops, NULL, KL_SOCK_CAP_WRITEV,
    NULL,
};

/* Overlapped provider (IOCP): native SOCKET handles whose I/O is driven
 * by the completion loop's submit path. */
static const KlSocketProvider overlapped_provider = {
    &stub_ops, NULL, KL_SOCK_CAP_NATIVE_FD | KL_SOCK_CAP_OVERLAPPED,
    NULL,
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
    /* This readiness backend does not advertise the completion model. */
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

/* The full negotiation matrix over both axes, via the pure helper — unit-testable
 * without a real IOCP backend (the completion arm). */
UTEST(event_caps, negotiation_matrix_readiness) {
    unsigned readiness = KL_EVENT_CAP_READINESS | KL_EVENT_CAP_NATIVE_FD;
    ASSERT_TRUE(kl_caps_compatible(readiness, NULL));                    /* POSIX default */
    ASSERT_TRUE(kl_caps_compatible(readiness, &native_provider));        /* native fd */
    ASSERT_TRUE(kl_caps_compatible(readiness, &overlapped_provider));    /* also native */
    ASSERT_FALSE(kl_caps_compatible(readiness, &non_native_provider));   /* not watchable */
}

UTEST(event_caps, negotiation_matrix_completion) {
    unsigned completion = KL_EVENT_CAP_COMPLETION | KL_EVENT_CAP_NATIVE_FD;
    if (!kl_completion_axis_available()) {
        /* KEEL_NO_COMPLETION: the completion axis is compiled out (completion_absent.c),
         * so a completion loop is fail-loud INCOMPATIBLE with every provider — there is
         * no driver to run it. */
        ASSERT_FALSE(kl_caps_compatible(completion, &overlapped_provider));
        ASSERT_FALSE(kl_caps_compatible(completion, &native_provider));
        ASSERT_FALSE(kl_caps_compatible(completion, NULL));
        return;
    }
    /* A completion loop needs a provider that routes I/O through its submit path. */
    ASSERT_TRUE(kl_caps_compatible(completion, &overlapped_provider));   /* OVERLAPPED */
    ASSERT_FALSE(kl_caps_compatible(completion, &native_provider));      /* sync send/recv only */
    ASSERT_FALSE(kl_caps_compatible(completion, NULL));                  /* POSIX default */
    ASSERT_FALSE(kl_caps_compatible(completion, &non_native_provider));
}

UTEST_MAIN();
