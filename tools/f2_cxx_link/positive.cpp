// F2-A positive C++ consumer: includes representative Keel umbrellas/headers and CALLS real
// functions, then links against the C-built libkeel.a. Proves that the extern "C" guards give the
// public API C linkage so a C++ translation unit resolves the archive's C symbols. Compiled and
// linked only from the staged install (no repository-private include path). See
// tools/f2_cxx_link_test.sh.
#include <keel/keel.h>          // substrate + HTTP surface (umbrella)
#include <keel/datagram.h>      // datagram facade
#include <keel/dns_resolver.h>  // built-in resolver
#include <keel/thread_pool.h>
#include <keel/event_ctx.h>
#include <keel/url.h>
#include <cstdio>

// Defined in the freestanding translation unit (proves multi-TU C++ linkage + the freestanding
// umbrella's guards).
const char *fs_probe(void);

// Instantiate the kl_event_dispatch() inline out-of-line from C++. Its body calls the internal
// keel__event_dispatch_watcher() link-support symbol; if that were not inside extern "C" the C++
// instantiation would reference a mangled name and fail to link. This proves the internal symbol
// links from a C++ consumer. (Runtime: a clear tag returns 0 without invoking it.)
extern "C" int f2_dispatch_probe(KlEventCtx *ctx, const KlEvent *ev) {
    return kl_event_dispatch(ctx, ev);
}

// Force the linker to resolve representative complex symbols across the axes without any runtime
// setup: taking a function's address requires its symbol to be found at link time.
static void *const g_refs[] = {
    (void *)&kl_http_server_init,     // HTTP
    (void *)&kl_datagram_socket_init, // datagram
    (void *)&kl_datagram_send,        // datagram
    (void *)&kl_dns_resolver_create,  // DNS
    (void *)&kl_thread_pool_create,   // thread pool
    (void *)&kl_event_ctx_init,       // event substrate
    (void *)&f2_dispatch_probe,       // forces kl_event_dispatch -> keel__event_dispatch_watcher link
};

int main(void) {
    // substrate: real calls into the C archive
    const char *v = kl_version();
    int vn = kl_version_number();
    const char *e = kl_strerror(KL_ERR_NONE);

    KlUrl u;
    int pr = kl_url_parse("http://example.com:8080/path?q=1", &u);

    (void)g_refs;
    if (!v || vn <= 0 || !e || pr != 0) return 1;

    // freestanding umbrella, second translation unit
    const char *fv = fs_probe();
    if (!fv) return 1;

    (void)u;
    std::printf("cxx-link positive: version=%s number=%d (url parsed, %s)\n", v, vn, e);
    return 0;
}
