/*
 * smoke_dns.c — DNS resolver link + init/resolve smoke test.
 *
 * NOT a utest suite — a standalone program built by the `smoke-dns` Makefile
 * target. It links the built-in DNS resolver and proves it both links and runs
 * on the target platform (the Windows CI gate for dns_sys_win.c: the iphlpapi
 * config discovery + the resolver pipeline). Runs on POSIX too.
 *
 * Deterministic (no network): creating the resolver with an all-default config
 * runs the platform config discovery — on Windows that's GetAdaptersAddresses
 * (nameservers + DNS suffix) + the System32\drivers\etc\hosts path. Resolving
 * "localhost" then completes via the loopback shortcut, driven by the event
 * loop, without any DNS query going out.
 */
#include <keel/dns_resolver.h>
#include <keel/resolver.h>
#include <keel/event_ctx.h>
#include <keel/allocator.h>
#include <keel/net.h>
#ifndef _WIN32
#include <netinet/in.h>
#endif
#include <string.h>
#include <stdio.h>

static int              g_done;
static int              g_err;
static KlResolveResult  g_res;

static void on_done(KlResolveReq *req, const KlResolveResult *result,
                    int error, void *ud) {
    (void)req; (void)ud;
    g_done++;
    g_err = error;
    if (result) g_res = *result;
}

int main(void) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    if (kl_event_ctx_init(&ctx, &alloc) != 0) {
        fprintf(stderr, "smoke-dns: event ctx init failed\n");
        return 1;
    }

    /* All-default config → the platform discovery path runs (iphlpapi on
     * Windows, /etc/resolv.conf + /etc/hosts on POSIX). */
    KlDnsResolverConfig dc;
    memset(&dc, 0, sizeof(dc));
    KlResolver *r = kl_dns_resolver_create(&ctx, &dc);
    if (!r) {
        fprintf(stderr, "smoke-dns: resolver create failed\n");
        kl_event_ctx_free(&ctx);
        return 1;
    }

    if (r->resolve(r, &ctx, "localhost", 8080, on_done, NULL) == NULL) {
        fprintf(stderr, "smoke-dns: resolve() returned NULL\n");
        r->destroy(r);
        kl_event_ctx_free(&ctx);
        return 1;
    }

    for (int i = 0; i < 200 && g_done == 0; i++)
        kl_event_ctx_run(&ctx, 16, 10);

    int ok = (g_done == 1 && g_err == 0 && g_res.naddrs >= 1);
    /* "localhost" → loopback; confirm the primary address is a loopback IP. */
    int loopback = ok && kl_sockaddr_is_loopback(&g_res.addrs[0]);

    r->destroy(r);
    kl_event_ctx_free(&ctx);

    if (!ok || !loopback) {
        fprintf(stderr, "smoke-dns: FAILED (done=%d err=%d naddrs=%d loopback=%d)\n",
                g_done, g_err, g_res.naddrs, loopback);
        return 1;
    }
    printf("smoke-dns: resolver init + localhost resolve OK\n");
    return 0;
}
