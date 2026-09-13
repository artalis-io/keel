/*
 * msvc_consumer.c - a native-MSVC consumer of Keel, using ONLY the public API.
 *
 * The suites in test-msvc link against the archive too, but they are built with -Isrc and reach
 * internal headers, so they cannot answer the question a consumer actually has: is the INSTALLED
 * surface enough, compiled by cl, linked against a lib.exe archive, with no MinGW runtime? This TU is
 * deliberately built with no -Isrc and includes nothing but <keel/...>.
 *
 * It exercises the places where the compiler choice could plausibly have changed BEHAVIOUR rather
 * than merely compilation:
 *
 *   - kl_http_server_init, which is where the C11 atomics lock-free policy is enforced
 *   - the PAL threading seam, through KlThreadPool (no winpthreads anywhere in the link)
 *   - the PAL socket runtime, through the cross-thread wakeup channel
 *   - the event loop, driven far enough to deliver a worker completion back on the loop thread
 *
 * Built and run by `make CC=cl check-msvc-consumer`, and by the same target under GCC/MinGW, so the
 * consumer contract is checked on both toolchains rather than only the new one.
 */
#include <keel/keel.h>
#include <keel/http_server.h>
#include <keel/thread_pool.h>
#include <keel/wakeup.h>
#include <keel/event_ctx.h>
#include <keel/error.h>
#include <stdio.h>
#include <string.h>

static int g_work_ran;   /* set on a worker thread */
static int g_done_ran;   /* set on the event-loop thread */

static void consumer_work(void *ud) { (void)ud; g_work_ran = 1; }
static void consumer_done(void *ud) { (void)ud; g_done_ran = 1; }

static void handler(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)req; (void)ud;
    static const char body[] = "{\"ok\":true}";
    kl_http_response_json(res, 200, body, sizeof(body) - 1);
}

/* One line on purpose: no continuation to get wrong. */
static int fail(const char *what) { printf("FAIL: %s\n", what); return 1; }
#define CHECK(cond, what) do { if (!(cond)) return fail(what); } while (0)

int main(void) {
    /* Named, because this target runs under BOTH toolchains and a report that always said "MSVC"
     * would be wrong half the time. */
#if defined(_MSC_VER) && !defined(__clang__)
    const char *toolchain = "MSVC cl";
#else
    const char *toolchain = "GCC/Clang";
#endif
    printf("keel %s, consumed through the public API by %s\n", kl_version(), toolchain);

    /* Server init is the call that verifies the lock-free contract on its own stop flags and refuses
     * the platform with KL_ERR_UNSUPPORTED if the atomics policy is not satisfied. */
    KlHttpServer s;
    KlHttpServerConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.port = 0;
    cfg.bind_addr = "127.0.0.1";
    CHECK(kl_http_server_init(&s, &cfg) == 0, "kl_http_server_init");
    CHECK(kl_http_server_route(&s, "GET", "/", handler, NULL, NULL) == 0, "kl_http_server_route");

    KlEventCtx *ctx = kl_http_server_event_ctx(&s);
    CHECK(ctx != NULL, "kl_http_server_event_ctx");

    /* PAL threads, via the public pool. If winpthreads were needed, the link would have failed. */
    KlThreadPoolConfig pcfg;
    memset(&pcfg, 0, sizeof(pcfg));
    pcfg.num_workers = 2;
    pcfg.queue_capacity = 4;
    KlThreadPool *pool = kl_thread_pool_create(ctx, &pcfg);
    CHECK(pool != NULL, "kl_thread_pool_create");

    KlWorkItem item;
    memset(&item, 0, sizeof(item));
    item.work_fn = consumer_work;
    item.done_fn = consumer_done;
    CHECK(kl_thread_pool_submit(pool, &item) == 0, "kl_thread_pool_submit");

    /* Drive the loop until the completion comes back on this thread. BOUNDED, so a failure is a
     * reported failure rather than a hang. */
    for (int i = 0; i < 500 && !g_done_ran; i++)
        kl_event_ctx_run(ctx, 8, 10);
    CHECK(g_work_ran, "work_fn never ran on a worker thread");
    CHECK(g_done_ran, "done_fn never ran on the event-loop thread");

    kl_thread_pool_free(pool);

    /* The PAL socket runtime, reached with no server socket in play. */
    KlWakeup w;
    CHECK(kl_wakeup_open(&w) == 0, "kl_wakeup_open");
    kl_wakeup_signal(&w);
    kl_wakeup_close(&w);

    kl_http_server_free(&s);

    printf("public-API consumer: server init, PAL thread pool, wakeup, event loop all OK\n");
    return 0;
}
