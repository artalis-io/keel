/*
 * test_http_client_eof.c: how the HTTP/1.1 client ends a response, and the pooled request's timers.
 *
 * End of stream. A response is complete when the parser says so, or when the peer closes a
 * close-delimited body (no Content-Length, not chunked). Before the fix the client treated any EOF
 * after a status line as success: a close-delimited response came back with no headers and no body
 * (they are handed over only at message-complete), and a truncated one (EOF inside the headers or a
 * Content-Length body) was reported as a successful, partial response. Sync and async are covered.
 *
 * HEAD. A HEAD response has no body whatever its Content-Length says. The client tells the parser
 * (expect_no_body), so the response is complete at the end of its headers: over a kept-alive
 * connection it no longer waits for a body that never comes, and at end of stream it is not taken
 * for a truncation.
 *
 * Interim responses. A 1xx other than 101 (103 Early Hints, 100 Continue) is followed by the final
 * response on the same connection; the client must report the final one, not the interim one.
 *
 * Pooled requests. kl_http_client_start_pooled left the timer ids at the memset's 0 and never set a
 * timeout: every completion cancelled whichever timer held id 0 (someone else's), and a pooled
 * request to a silent server never timed out.
 *
 * The peer is a raw loopback socket driven by a thread, so each case controls exactly which bytes
 * arrive before the close.
 */
#include "utest.h"
#include <keel/keel.h>
#include <keel/http_client.h>
#include <keel/http_client_pool.h>
#include <keel/timer.h>
#include "net_compat.h"
#include "platform_thread.h"   /* Keel PAL threads: portable to MSVC */
#include "platform_socket.h"   /* kl_plat_socket_runtime_init */
#include <string.h>
#include <stdio.h>

/* ── A one-shot raw peer ─────────────────────────────────────────── */

typedef struct {
    KlSocketHandle listen_fd;
    int            port;
    const char    *reply;   /* sent after the request headers; NULL = stay silent */
    int            hold;    /* after the reply, keep the connection open until the client closes */
    KlPlatThread   tid;
} Peer;

static int peer_listen(Peer *p, const char *reply) {
    memset(p, 0, sizeof(*p));
    p->reply = reply;
    if (kl_plat_socket_runtime_init() != 0) return -1;
    KlSocketHandle fd = (KlSocketHandle)socket(AF_INET, SOCK_STREAM, 0);
    if (!kl_handle_valid(fd)) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t al = sizeof(a);
    if (bind((int)fd, (struct sockaddr *)&a, sizeof(a)) != 0 ||
        listen((int)fd, 4) != 0 ||
        getsockname((int)fd, (struct sockaddr *)&a, &al) != 0) {
        kl_test_closesock(fd);
        return -1;
    }
    p->listen_fd = fd;
    p->port = ntohs(a.sin_port);
    return 0;
}

static void peer_thread(void *arg) {
    Peer *p = arg;
    if (kl_test_poll1(p->listen_fd, 0, 5000) <= 0) return;
    KlSocketHandle c = (KlSocketHandle)accept((int)p->listen_fd, NULL, NULL);
    if (!kl_handle_valid(c)) return;
    char buf[2048];
    size_t got = 0;
    while (got < sizeof(buf) - 1 && kl_test_poll1(c, 0, 3000) > 0) {   /* the request headers */
        long n = kl_test_sockread(c, buf + got, sizeof(buf) - 1 - got);
        if (n <= 0) break;
        got += (size_t)n;
        buf[got] = '\0';
        if (strstr(buf, "\r\n\r\n")) break;
    }
    if (p->reply) {
        (void)kl_test_sockwrite(c, p->reply, strlen(p->reply));
        if (p->hold)
            while (kl_test_poll1(c, 0, 5000) > 0 && kl_test_sockread(c, buf, sizeof(buf)) > 0) {}
    } else {
        /* Silent: hold the connection until the client gives up and closes it. */
        while (kl_test_poll1(c, 0, 5000) > 0 && kl_test_sockread(c, buf, sizeof(buf)) > 0) {}
    }
    kl_test_closesock(c);   /* the end of stream the client sees */
}

static void peer_start(Peer *p) { kl_plat_thread_create(&p->tid, peer_thread, p); }

static void peer_finish(Peer *p) {
    kl_plat_thread_join(&p->tid);
    kl_test_closesock(p->listen_fd);
}

static const char *url_for(const Peer *p) {
    static char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/", p->port);
    return url;
}

static const char *find_header(const KlHttpClientResponse *r, const char *name) {
    for (int i = 0; i < r->num_headers; i++)
        if (strcmp(r->headers[i].name, name) == 0) return r->headers[i].value;
    return NULL;
}

#define CLOSE_DELIMITED  "HTTP/1.1 200 OK\r\nX-Probe: yes\r\n\r\nhello"
#define TRUNCATED_BODY   "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc"
#define TRUNCATED_HEADER "HTTP/1.1 200 OK\r\nContent-Le"

/* ── Sync ────────────────────────────────────────────────────────── */

static int sync_req(const char *method, const char *reply, int hold, KlHttpClientResponse *resp) {
    Peer p;
    if (peer_listen(&p, reply) != 0) return -2;
    p.hold = hold;
    peer_start(&p);
    KlAllocator a = kl_allocator_default();
    KlHttpClientConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.timeout_ms = 3000;
    memset(resp, 0, sizeof(*resp));
    int rc = kl_http_client_request(&a, &cfg, method, url_for(&p), NULL, 0, NULL, 0, resp);
    peer_finish(&p);
    return rc;
}

static int sync_get(const char *reply, KlHttpClientResponse *resp) {
    return sync_req("GET", reply, 0, resp);
}

UTEST(eof, sync_close_delimited_body_is_delivered) {
    KlHttpClientResponse r;
    ASSERT_EQ(sync_get(CLOSE_DELIMITED, &r), 0);
    ASSERT_EQ(r.status, 200);
    ASSERT_EQ(r.body_len, (size_t)5);
    ASSERT_TRUE(r.body && memcmp(r.body, "hello", 5) == 0);   /* was: no body */
    ASSERT_TRUE(find_header(&r, "X-Probe") != NULL);          /* was: no headers */
    kl_http_client_response_free(&r);
}

UTEST(eof, sync_truncated_body_fails) {
    KlHttpClientResponse r;
    ASSERT_EQ(sync_get(TRUNCATED_BODY, &r), -1);   /* was: 0, a "successful" 3-of-10-byte body */
    kl_http_client_response_free(&r);
}

UTEST(eof, sync_truncated_headers_fail) {
    KlHttpClientResponse r;
    ASSERT_EQ(sync_get(TRUNCATED_HEADER, &r), -1);
    kl_http_client_response_free(&r);
}

#define HEAD_REPLY "HTTP/1.1 200 OK\r\nContent-Length: 10\r\nX-Probe: yes\r\n\r\n"

/* The server keeps the connection open: the response must complete at the end of its headers. */
UTEST(head, sync_completes_at_end_of_headers) {
    KlHttpClientResponse r;
    ASSERT_EQ(sync_req("HEAD", HEAD_REPLY, 1, &r), 0);   /* was: waited for 10 bytes, timed out */
    ASSERT_EQ(r.status, 200);
    ASSERT_EQ(r.body_len, (size_t)0);
    ASSERT_TRUE(find_header(&r, "X-Probe") != NULL);
    kl_http_client_response_free(&r);
}

/* The server closes right after the headers: not a truncation. */
UTEST(head, sync_close_after_headers_is_not_a_truncation) {
    KlHttpClientResponse r;
    ASSERT_EQ(sync_req("HEAD", HEAD_REPLY, 0, &r), 0);
    ASSERT_EQ(r.status, 200);
    ASSERT_TRUE(find_header(&r, "X-Probe") != NULL);
    kl_http_client_response_free(&r);
}

/* The same bytes answering a GET are a truncation (the hint is per request, not sticky). */
UTEST(head, sync_same_reply_to_get_is_truncated) {
    KlHttpClientResponse r;
    ASSERT_EQ(sync_req("GET", HEAD_REPLY, 0, &r), -1);
    kl_http_client_response_free(&r);
}

/* ── Async ───────────────────────────────────────────────────────── */

typedef struct {
    int  done;
    int  err;
    int  status;
    char body[16];
    size_t body_len;
    int  has_probe;
} AsyncResult;

static void async_done(KlHttpClient *c, void *ud) {
    AsyncResult *r = ud;
    r->done = 1;
    r->err = (int)kl_http_client_last_error(c);
    const KlHttpClientResponse *resp = kl_http_client_response(c);
    if (resp) {
        r->status = resp->status;
        if (resp->body && resp->body_len < sizeof(r->body)) {
            memcpy(r->body, resp->body, resp->body_len);
            r->body_len = resp->body_len;
        }
        r->has_probe = find_header(resp, "X-Probe") != NULL;
    }
}

static void async_req(const char *method, const char *reply, int hold, AsyncResult *res) {
    memset(res, 0, sizeof(*res));
    Peer p;
    if (peer_listen(&p, reply) != 0) { res->err = -2; return; }
    p.hold = hold;
    peer_start(&p);
    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    if (kl_event_ctx_init(&ev, &a) != 0) { res->err = -3; peer_finish(&p); return; }
    KlHttpClientConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.timeout_ms = 3000;
    KlHttpClient *c = kl_http_client_start(&ev, &a, &cfg, method, url_for(&p), NULL, 0, NULL, 0,
                                           async_done, res);
    for (int i = 0; i < 400 && c && !res->done; i++) (void)kl_event_ctx_run(&ev, 16, 10);
    if (c) kl_http_client_free(c);
    kl_event_ctx_free(&ev);
    peer_finish(&p);
}

static void async_get(const char *reply, AsyncResult *res) { async_req("GET", reply, 0, res); }

UTEST(eof, async_close_delimited_body_is_delivered) {
    AsyncResult r;
    async_get(CLOSE_DELIMITED, &r);
    ASSERT_EQ(r.done, 1);
    ASSERT_EQ(r.err, 0);
    ASSERT_EQ(r.status, 200);
    ASSERT_EQ(r.body_len, (size_t)5);
    ASSERT_EQ(memcmp(r.body, "hello", 5), 0);
    ASSERT_EQ(r.has_probe, 1);
}

UTEST(eof, async_truncated_body_fails) {
    AsyncResult r;
    async_get(TRUNCATED_BODY, &r);
    ASSERT_EQ(r.done, 1);
    ASSERT_NE(r.err, 0);   /* was: success */
}

UTEST(eof, async_truncated_headers_fail) {
    AsyncResult r;
    async_get(TRUNCATED_HEADER, &r);
    ASSERT_EQ(r.done, 1);
    ASSERT_NE(r.err, 0);
}

UTEST(head, async_completes_at_end_of_headers) {
    AsyncResult r;
    async_req("HEAD", HEAD_REPLY, 1, &r);
    ASSERT_EQ(r.done, 1);
    ASSERT_EQ(r.err, 0);   /* was: waited for 10 bytes until the deadline */
    ASSERT_EQ(r.status, 200);
    ASSERT_EQ(r.has_probe, 1);
}

#define EARLY_HINTS_THEN_FINAL \
    "HTTP/1.1 103 Early Hints\r\nLink: </style.css>; rel=preload\r\n\r\n" \
    "HTTP/1.1 200 OK\r\nX-Probe: yes\r\nContent-Length: 5\r\n\r\nhello"

UTEST(interim, sync_reports_the_final_response) {
    KlHttpClientResponse r;
    ASSERT_EQ(sync_req("GET", EARLY_HINTS_THEN_FINAL, 1, &r), 0);
    ASSERT_EQ(r.status, 200);                            /* was: 103, with no body */
    ASSERT_EQ(r.body_len, (size_t)5);
    ASSERT_TRUE(r.body && memcmp(r.body, "hello", 5) == 0);
    ASSERT_TRUE(find_header(&r, "X-Probe") != NULL);
    ASSERT_TRUE(find_header(&r, "Link") == NULL);        /* the interim headers are not merged in */
    kl_http_client_response_free(&r);
}

UTEST(interim, async_reports_the_final_response) {
    AsyncResult r;
    async_req("GET", EARLY_HINTS_THEN_FINAL, 1, &r);
    ASSERT_EQ(r.done, 1);
    ASSERT_EQ(r.err, 0);
    ASSERT_EQ(r.status, 200);
    ASSERT_EQ(r.body_len, (size_t)5);
    ASSERT_EQ(memcmp(r.body, "hello", 5), 0);
    ASSERT_EQ(r.has_probe, 1);
}

/* Only an interim response, then the server closes: there is no response, so it is an error. */
UTEST(interim, sync_interim_then_close_fails) {
    KlHttpClientResponse r;
    ASSERT_EQ(sync_req("GET", "HTTP/1.1 100 Continue\r\n\r\n", 0, &r), -1);
    kl_http_client_response_free(&r);
}

UTEST(head, async_close_after_headers_is_not_a_truncation) {
    AsyncResult r;
    async_req("HEAD", HEAD_REPLY, 0, &r);
    ASSERT_EQ(r.done, 1);
    ASSERT_EQ(r.err, 0);
    ASSERT_EQ(r.status, 200);
    ASSERT_EQ(r.has_probe, 1);
}

/* ── Pooled requests: their own timers only, and a deadline ──────── */

static int g_unrelated_fired;
static void unrelated_timer(void *ud) { (void)ud; g_unrelated_fired = 1; }

UTEST(pooled, completion_leaves_unrelated_timer_alone) {
    static Peer p;   /* outlives an ASSERT early return: the peer thread uses it */
    ASSERT_EQ(peer_listen(&p, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK"), 0);
    peer_start(&p);
    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);
    g_unrelated_fired = 0;
    ASSERT_EQ(kl_timer_add(&ev, 300, unrelated_timer, NULL), (int64_t)0);   /* holds id 0 */
    KlHttpClientPool pool;
    ASSERT_EQ(kl_http_client_pool_init(&pool, NULL, &a, &ev), 0);
    AsyncResult r;
    memset(&r, 0, sizeof(r));
    KlHttpClient *c = kl_http_client_start_pooled(&pool, &ev, &a, NULL, "GET", url_for(&p),
                                                  NULL, 0, NULL, 0, async_done, &r);
    ASSERT_TRUE(c != NULL);
    for (int i = 0; i < 300 && (!r.done || !g_unrelated_fired); i++) (void)kl_event_ctx_run(&ev, 16, 10);
    ASSERT_EQ(r.done, 1);
    ASSERT_EQ(r.err, 0);
    ASSERT_EQ(g_unrelated_fired, 1);   /* was: cancelled by the pooled request's completion */
    kl_http_client_free(c);
    kl_http_client_pool_free(&pool);
    kl_event_ctx_free(&ev);
    peer_finish(&p);
}

UTEST(pooled, silent_server_times_out) {
    static Peer p;   /* outlives an ASSERT early return: the peer thread uses it */
    ASSERT_EQ(peer_listen(&p, NULL), 0);   /* accepts, reads the request, never answers */
    peer_start(&p);
    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);
    KlHttpClientPool pool;
    ASSERT_EQ(kl_http_client_pool_init(&pool, NULL, &a, &ev), 0);
    KlHttpClientConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.timeout_ms = 300;
    AsyncResult r;
    memset(&r, 0, sizeof(r));
    KlHttpClient *c = kl_http_client_start_pooled(&pool, &ev, &a, &cfg, "GET", url_for(&p),
                                                  NULL, 0, NULL, 0, async_done, &r);
    ASSERT_TRUE(c != NULL);
    for (int i = 0; i < 300 && !r.done; i++) (void)kl_event_ctx_run(&ev, 16, 10);
    ASSERT_EQ(r.done, 1);                        /* was: no deadline, never completed */
    ASSERT_EQ(r.err, (int)KL_ERR_TIMEOUT);
    kl_http_client_free(c);                      /* closes the socket: the silent peer returns */
    kl_http_client_pool_free(&pool);
    kl_event_ctx_free(&ev);
    peer_finish(&p);
}

UTEST_MAIN();
