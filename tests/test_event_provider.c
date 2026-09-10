/*
 * test_event_provider.c: proves a runtime-injected event backend
 * (KlEventProvider) actually drives a real KlHttpServer end to end.
 *
 * A self-contained poll()-based readiness backend is installed via
 * KlHttpServerConfig.event_provider (not the compiled-in epoll/kqueue). A raw-socket client
 * makes an HTTP/1.1 request; the handler must run and return 200; driven only by
 * the custom backend. A call counter on the backend's wait() proves the loop ran
 * on the provider, not the builtin. This is the second consumer of the seam
 * (alongside the compiled default) and the exact mechanism lwIP uses.
 */
#include "utest.h"
#include <keel/keel.h>
#include "net_compat.h"
#include <pthread.h>
#include <string.h>
#include <time.h>

/* The in-test backend polls readiness directly, so it needs the one primitive net_compat does
 * not wrap: a multi-fd poll. WSAPoll is the Winsock spelling, and there POLLIN/POLLOUT are
 * OUTPUT-only aliases; POLLRDNORM/POLLWRNORM are the settable input flags (same distinction
 * src/event_wsapoll.c documents). net_compat.h already resolved winsock2.h vs poll.h above. */
#if defined(_WIN32)
typedef WSAPOLLFD ep_pollfd_t;
typedef SOCKET    ep_fd_t;
#define EP_POLL(f, n, t)  WSAPoll((f), (ULONG)(n), (t))
#define EP_IN             POLLRDNORM
#define EP_OUT            POLLWRNORM
#else
typedef struct pollfd ep_pollfd_t;
typedef int           ep_fd_t;
#define EP_POLL(f, n, t)  poll((f), (nfds_t)(n), (t))
#define EP_IN             POLLIN
#define EP_OUT            POLLOUT
#endif

/* ── a minimal poll()-based runtime event backend ───────────────────── */

#define EP_MAX 64
typedef struct { ep_pollfd_t fds[EP_MAX]; void *ud[EP_MAX]; int n; KlAllocator *a; } EpState;

static int          ep_wait_calls;   /* proves this backend drove the loop */

static int ep_find(EpState *s, KlSocketHandle fd) {
    for (int i = 0; i < s->n; i++) if (s->fds[i].fd == (ep_fd_t)fd) return i;
    return -1;
}
static short ep_ev(KlEventMask m) {
    short e = 0; if (m & KL_EVENT_READ) e |= EP_IN; if (m & KL_EVENT_WRITE) e |= EP_OUT; return e;
}
static int ep_init(KlEventLoop *l) {
    EpState *s = kl_malloc(l->alloc, sizeof(*s));
    if (!s) return -1;
    memset(s, 0, sizeof(*s));
    s->a = l->alloc; l->_backend = s;
    return 0;
}
static int ep_add(KlEventLoop *l, KlSocketHandle fd, KlEventMask mask, void *ud) {
    EpState *s = l->_backend;
    int i = ep_find(s, fd);
    if (i < 0) { if (s->n >= EP_MAX) return -1; i = s->n++; s->fds[i].fd = (ep_fd_t)fd; }
    s->fds[i].events = ep_ev(mask); s->fds[i].revents = 0; s->ud[i] = ud;
    return 0;
}
static int ep_mod(KlEventLoop *l, KlSocketHandle fd, KlEventMask mask, void *ud) {
    EpState *s = l->_backend; int i = ep_find(s, fd); if (i < 0) return -1;
    s->fds[i].events = ep_ev(mask); s->ud[i] = ud; return 0;
}
static int ep_del(KlEventLoop *l, KlSocketHandle fd) {
    EpState *s = l->_backend; int i = ep_find(s, fd); if (i < 0) return -1;
    int last = s->n - 1;
    if (i != last) { s->fds[i] = s->fds[last]; s->ud[i] = s->ud[last]; }
    s->n--; return 0;
}
static int ep_wait(KlEventLoop *l, KlEvent *out, int max, int timeout_ms) {
    EpState *s = l->_backend;
    ep_wait_calls++;
    int n = EP_POLL(s->fds, s->n, timeout_ms);
    if (n < 0) return -1;
    int c = 0;
    for (int i = 0; i < s->n && c < max; i++) {
        short r = s->fds[i].revents; if (!r) continue;
        KlEventMask rd = 0;
        if (r & (EP_IN | POLLHUP | POLLERR)) rd |= KL_EVENT_READ;
        if (r & EP_OUT) rd |= KL_EVENT_WRITE;
        if (rd) { out[c].udata = s->ud[i]; out[c].ready = rd; c++; }
    }
    return c;
}
static void ep_close(KlEventLoop *l) {
    EpState *s = l->_backend; if (s) kl_free(s->a, s, sizeof(*s)); l->_backend = NULL;
}
static unsigned ep_caps(const KlEventLoop *l) { (void)l; return KL_EVENT_CAP_READINESS | KL_EVENT_CAP_NATIVE_FD; }
static const struct KlSocketProvider *ep_native(const KlEventLoop *l) { (void)l; return NULL; }

static const KlEventOps EP_OPS = {
    .init = ep_init, .add = ep_add, .mod = ep_mod, .del = ep_del, .wait = ep_wait,
    .close = ep_close, .caps = ep_caps, .native_provider = ep_native,
    .completion = NULL,   /* readiness test backend, no completion axis */
};
static const KlEventProvider EP_PROVIDER = { &EP_OPS, "test-poll" };

/* ── server on the custom backend ───────────────────────────────────── */

#define EPP_PORT 18499
static int g_handler_called;
static void epp_handler(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)req; (void)ud; g_handler_called++;
    kl_http_response_json(res, 200, "{\"backend\":\"custom\"}", 20);
}
static KlHttpServer g_srv;
static void *epp_thread(void *a) { (void)a; kl_http_server_run(&g_srv); return NULL; }
static void epp_nap(int ms) { struct timespec t = {ms/1000,(long)(ms%1000)*1000000L}; nanosleep(&t,NULL); }

UTEST(event_provider, server_runs_on_injected_backend) {
    ep_wait_calls = 0; g_handler_called = 0;

    KlHttpServerConfig cfg = { .port = EPP_PORT, .bind_addr = "127.0.0.1",
                     .event_provider = &EP_PROVIDER };   /* <-- runtime backend */
    ASSERT_EQ(0, kl_http_server_init(&g_srv, &cfg));
    kl_http_server_route(&g_srv, "GET", "/", epp_handler, NULL, NULL);

    pthread_t tid;
    ASSERT_EQ(0, pthread_create(&tid, NULL, epp_thread, NULL));
    for (int i = 0; i < 200 && g_srv.bound_port == 0; i++) epp_nap(5);
    ASSERT_TRUE(g_srv.bound_port > 0);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_TRUE(fd >= 0);
    struct sockaddr_in a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t)g_srv.bound_port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    ASSERT_EQ(0, connect(fd, (struct sockaddr *)&a, sizeof(a)));
    const char *req = "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    ASSERT_EQ((long)strlen(req), kl_test_sockwrite(fd, req, strlen(req)));

    char buf[512]; size_t got = 0;
    while (got < sizeof(buf) - 1) {
        if (kl_test_poll1(fd, 0, 2000) <= 0) break;
        long n = kl_test_sockread(fd, buf + got, sizeof(buf) - 1 - got);
        if (n <= 0) break;
        got += (size_t)n; buf[got] = 0;
        if (strstr(buf, "\r\n\r\n")) break;
    }
    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);
    ASSERT_TRUE(strstr(buf, "\"backend\":\"custom\"") != NULL);
    ASSERT_EQ(1, g_handler_called);
    ASSERT_TRUE(ep_wait_calls > 0);   /* the injected backend actually drove the loop */

    kl_test_closesock(fd);
    kl_http_server_stop(&g_srv);
    pthread_join(tid, NULL);
    kl_http_server_free(&g_srv);
}

UTEST_MAIN();
