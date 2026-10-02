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
#include <keel/decompress.h>
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

/* ── A pooled connection is kept only if the response ended cleanly (audit L8) ──────────────────
 * Bytes after a complete response used to be dropped and the connection pooled anyway, so the next
 * request on it read them as the start of its own response; a response that ended at end of stream
 * (close-delimited) pooled a connection the server had already closed. This peer answers ONE request
 * per connection and counts connections: a second pooled request must get a fresh one. */

typedef struct {
    KlSocketHandle listen_fd;
    int            port;
    const char    *reply;
    int            hold;       /* keep each connection open after replying, until the client closes */
    int            accepts;
    char           first_line[256];   /* the first request line received */
    KlPlatThread   tid;
} MultiPeer;

static void multi_peer_thread(void *arg) {
    MultiPeer *p = arg;
    for (int n = 0; n < 2; n++) {
        if (kl_test_poll1(p->listen_fd, 0, 3000) <= 0) return;
        KlSocketHandle c = (KlSocketHandle)accept((int)p->listen_fd, NULL, NULL);
        if (!kl_handle_valid(c)) return;
        p->accepts++;
        char buf[2048];
        size_t got = 0;
        while (got < sizeof(buf) - 1 && kl_test_poll1(c, 0, 3000) > 0) {
            long r = kl_test_sockread(c, buf + got, sizeof(buf) - 1 - got);
            if (r <= 0) break;
            got += (size_t)r;
            buf[got] = '\0';
            if (strstr(buf, "\r\n\r\n")) break;
        }
        if (n == 0) {
            const char *e = strstr(buf, "\r\n");
            size_t l = e ? (size_t)(e - buf) : 0;
            if (l >= sizeof(p->first_line)) l = sizeof(p->first_line) - 1;
            memcpy(p->first_line, buf, l);
            p->first_line[l] = '\0';
        }
        (void)kl_test_sockwrite(c, p->reply, strlen(p->reply));
        if (p->hold)   /* answers one request only; a second request on this connection gets nothing */
            while (kl_test_poll1(c, 0, 1500) > 0 && kl_test_sockread(c, buf, sizeof(buf)) > 0) {}
        kl_test_closesock(c);
    }
}

static int multi_peer_start(MultiPeer *p, const char *reply, int hold) {
    Peer tmp;
    if (peer_listen(&tmp, NULL) != 0) return -1;
    memset(p, 0, sizeof(*p));
    p->listen_fd = tmp.listen_fd;
    p->port = tmp.port;
    p->reply = reply;
    p->hold = hold;
    return kl_plat_thread_create(&p->tid, multi_peer_thread, p);
}

static void multi_peer_finish(MultiPeer *p) {
    kl_plat_thread_join(&p->tid);
    kl_test_closesock(p->listen_fd);
}

/* Two sync pooled GETs to the same peer; returns how many succeeded (0, 1 or 2). */
static int two_pooled_gets(MultiPeer *p) {
    KlAllocator a = kl_allocator_default();
    KlHttpClientPool pool;
    if (kl_http_client_pool_init(&pool, NULL, &a, NULL) != 0) return -1;
    KlHttpClientConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.timeout_ms = 1000;
    char url[64];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/", p->port);
    int ok = 0;
    for (int i = 0; i < 2; i++) {
        KlHttpClientResponse r;
        memset(&r, 0, sizeof r);
        if (kl_http_client_request_pooled(&pool, &a, &cfg, "GET", url, NULL, 0, NULL, 0, &r) == 0 &&
            r.status == 200)
            ok++;
        kl_http_client_response_free(&r);
    }
    kl_http_client_pool_free(&pool);
    return ok;
}

UTEST(pooled, bytes_after_the_response_are_not_pooled) {
    static MultiPeer p;
    ASSERT_EQ(multi_peer_start(&p, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOKextra", 1), 0);
    int ok = two_pooled_gets(&p);
    multi_peer_finish(&p);
    ASSERT_EQ(ok, 2);            /* was: 1, the second request reused the tainted connection */
    ASSERT_EQ(p.accepts, 2);     /* a fresh connection for the second request */
}

/* The async pooled client, same property. */
UTEST(pooled, async_bytes_after_the_response_are_not_pooled) {
    static MultiPeer p;
    ASSERT_EQ(multi_peer_start(&p, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOKextra", 1), 0);
    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);
    KlHttpClientPool pool;
    ASSERT_EQ(kl_http_client_pool_init(&pool, NULL, &a, &ev), 0);
    KlHttpClientConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.timeout_ms = 1000;
    char url[64];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/", p.port);
    int ok = 0;
    for (int i = 0; i < 2; i++) {
        AsyncResult r;
        memset(&r, 0, sizeof(r));
        KlHttpClient *c = kl_http_client_start_pooled(&pool, &ev, &a, &cfg, "GET", url,
                                                      NULL, 0, NULL, 0, async_done, &r);
        for (int k = 0; k < 300 && c && !r.done; k++) (void)kl_event_ctx_run(&ev, 16, 10);
        if (r.done && r.err == 0 && r.status == 200) ok++;
        if (c) kl_http_client_free(c);
    }
    kl_http_client_pool_free(&pool);
    kl_event_ctx_free(&ev);
    multi_peer_finish(&p);
    ASSERT_EQ(ok, 2);            /* was: 1 */
    ASSERT_EQ(p.accepts, 2);
}

/* A pooled request with a proxy configured goes through the proxy (audit L9). The pool is keyed by
 * the target and connected to it directly, so the proxy used to be silently bypassed. The target here
 * is a closed port: reaching it directly fails, reaching it through the proxy succeeds. */
UTEST(pooled, a_proxied_request_goes_through_the_proxy) {
    static MultiPeer p;
    ASSERT_EQ(multi_peer_start(&p, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK",
                               0), 0);
    KlAllocator a = kl_allocator_default();
    KlHttpClientPool pool;
    ASSERT_EQ(kl_http_client_pool_init(&pool, NULL, &a, NULL), 0);
    KlHttpProxyConfig proxy = { .host = "127.0.0.1", .port = (uint16_t)p.port, .auth = NULL };
    KlHttpClientConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.timeout_ms = 1000;
    cfg.proxy = &proxy;
    KlHttpClientResponse r;
    memset(&r, 0, sizeof r);
    int rc = kl_http_client_request_pooled(&pool, &a, &cfg, "GET", "http://127.0.0.1:9/target",
                                           NULL, 0, NULL, 0, &r);
    int status = r.status;
    kl_http_client_response_free(&r);
    kl_http_client_pool_free(&pool);
    kl_test_closesock(p.listen_fd);   /* no second connection is coming: release the peer's accept */
    kl_plat_thread_join(&p.tid);
    ASSERT_EQ(rc, 0);                 /* was: a direct connect to the closed port, refused */
    ASSERT_EQ(status, 200);
    ASSERT_EQ(p.accepts, 1);
    ASSERT_TRUE(strstr(p.first_line, "GET http://127.0.0.1:9/target HTTP/1.1") != NULL);
}

/* A response ended at end of stream: the pool already notices a connection the server closed before
 * reusing it, so this passed before the fix too; it guards that the clients now also decline to pool
 * such a connection in the first place. */
UTEST(pooled, a_response_ended_at_eof_is_not_pooled) {
    static MultiPeer p;
    ASSERT_EQ(multi_peer_start(&p, "HTTP/1.1 200 OK\r\n\r\nhello", 0), 0);   /* close-delimited */
    int ok = two_pooled_gets(&p);
    multi_peer_finish(&p);
    ASSERT_EQ(ok, 2);
    ASSERT_EQ(p.accepts, 2);
}

UTEST(pooled, a_clean_response_is_still_reused) {
    static MultiPeer p;
    ASSERT_EQ(multi_peer_start(&p, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK", 1), 0);
    KlAllocator a = kl_allocator_default();
    KlHttpClientPool pool;
    ASSERT_EQ(kl_http_client_pool_init(&pool, NULL, &a, NULL), 0);
    KlHttpClientConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.timeout_ms = 1000;
    char url[64];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/", p.port);
    KlHttpClientResponse r;
    memset(&r, 0, sizeof r);
    int rc = kl_http_client_request_pooled(&pool, &a, &cfg, "GET", url, NULL, 0, NULL, 0, &r);
    kl_http_client_response_free(&r);
    int idle = kl_http_client_pool_idle_count(&pool);
    kl_http_client_pool_free(&pool);   /* closes the pooled connection: the peer moves on */
    multi_peer_finish(&p);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(idle, 1);          /* the guard: a clean keep-alive response is pooled as before */
}

/* ── Connection reuse: what a response says about its connection ─────────────────────────── */

/* Connection is a comma-separated token list (RFC 9110 7.6.1): "close" among other tokens still
 * closes. Only an exact "Connection: close" was recognised, so the connection was pooled and the
 * next request went to a peer that had finished with it. */
UTEST(pooled, connection_close_among_other_tokens_is_not_pooled) {
    static MultiPeer p;
    ASSERT_EQ(multi_peer_start(&p, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
                                   "Connection: Keep-Alive, close\r\n\r\nOK", 1), 0);
    int ok = two_pooled_gets(&p);
    multi_peer_finish(&p);
    ASSERT_EQ(ok, 2);
    ASSERT_EQ(p.accepts, 2);     /* was 1: the closing connection was reused */
}

/* A 101 hands the connection to another protocol: it is never reusable for HTTP. */
UTEST(pooled, switching_protocols_is_not_pooled) {
    static MultiPeer p;
    ASSERT_EQ(multi_peer_start(&p, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: x\r\n"
                                   "Connection: Upgrade\r\n\r\n", 1), 0);
    (void)two_pooled_gets(&p);
    multi_peer_finish(&p);
    ASSERT_EQ(p.accepts, 2);     /* was 1: the second request was written into the switched protocol */
}

/* ── Decompression ───────────────────────────────────────────────────────────────────────────── */

/* A mock decompressor: doubles every byte; its streaming feed fails at the final flush when
 * g_md_fail_flush is set (a truncated stream), and both forms fail when g_md_fail is set. */
static int g_md_fail, g_md_fail_flush;
static int md_decompress(KlDecompress *self, const char *in, size_t in_len, char **out,
                         size_t *out_len, KlAllocator *alloc) {
    (void)self;
    if (g_md_fail) return -1;
    char *b = kl_malloc(alloc, in_len * 2);
    if (!b) return -1;
    for (size_t i = 0; i < in_len; i++) { b[2 * i] = in[i]; b[2 * i + 1] = in[i]; }
    *out = b;
    *out_len = in_len * 2;
    return 0;
}
static int md_dfeed(KlDecompress *self, const char *d, size_t n, int flush,
                    int (*emit)(void *, const char *, size_t), void *ctx) {
    (void)self;
    if (g_md_fail) return -1;                    /* a failing decompressor fails in either form */
    for (size_t i = 0; i < n; i++) {
        char two[2] = { d[i], d[i] };
        if (emit(ctx, two, 2) != 0) return -1;
    }
    if (flush && g_md_fail_flush) return -1;
    return 0;
}
static const char *md_encoding(KlDecompress *self) { (void)self; return "x-double"; }
static void md_reset(KlDecompress *self) { (void)self; }
static void md_destroy(KlDecompress *self) { (void)self; }
static KlDecompress g_md = { md_decompress, md_dfeed, md_encoding, md_reset, md_destroy };
static KlDecompress *md_factory(KlCompressCtx *ctx, KlAllocator *alloc) { (void)ctx; (void)alloc; return &g_md; }
static KlDecompressConfig g_md_cfg = { .ctx = NULL, .factory = md_factory };

#define DOUBLED_8 "HTTP/1.1 200 OK\r\nContent-Encoding: x-double\r\nContent-Length: 8\r\n\r\nabcdefgh"

/* max_response_size bounds what the caller receives: the decompressed body, not just the bytes on
 * the wire (8 bytes inflate to 16 here, over a limit of 10). */
UTEST(decompress, sync_decompressed_body_over_the_limit_fails) {
    Peer p;
    ASSERT_EQ(peer_listen(&p, DOUBLED_8), 0);
    peer_start(&p);
    KlAllocator a = kl_allocator_default();
    KlHttpClientConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.timeout_ms = 3000;
    cfg.max_response_size = 10;
    cfg.decompress = &g_md_cfg;
    KlHttpClientResponse r;
    memset(&r, 0, sizeof r);
    int rc = kl_http_client_request(&a, &cfg, "GET", url_for(&p), NULL, 0, NULL, 0, &r);
    KlError err = r.error;
    size_t len = r.body_len;
    kl_http_client_response_free(&r);
    peer_finish(&p);
    ASSERT_EQ(rc, -1);                           /* was 0, with a 16-byte body */
    ASSERT_EQ((int)err, (int)KL_ERR_TOO_LARGE);
    (void)len;
}

/* The async client ignored a failed decompression and delivered the encoded body as a success. */
UTEST(decompress, async_decompression_failure_fails_the_request) {
    Peer p;
    ASSERT_EQ(peer_listen(&p, DOUBLED_8), 0);
    peer_start(&p);
    AsyncResult res;
    memset(&res, 0, sizeof res);
    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);
    KlHttpClientConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.timeout_ms = 3000;
    cfg.decompress = &g_md_cfg;
    g_md_fail = 1;
    KlHttpClient *c = kl_http_client_start(&ev, &a, &cfg, "GET", url_for(&p), NULL, 0, NULL, 0,
                                           async_done, &res);
    for (int i = 0; i < 400 && c && !res.done; i++) (void)kl_event_ctx_run(&ev, 16, 10);
    g_md_fail = 0;
    if (c) kl_http_client_free(c);
    kl_event_ctx_free(&ev);
    peer_finish(&p);
    ASSERT_TRUE(res.done);
    ASSERT_NE(res.err, 0);                       /* was 0: success, with the body still encoded */
}

/* A streaming response whose decompression fails at the final flush (a truncated stream) must not
 * end as a success; the flush result was ignored. */
static int sink_body(const char *d, size_t n, void *ud) { (void)d; *(size_t *)ud += n; return 0; }
UTEST(decompress, sync_stream_final_flush_failure_fails_the_request) {
    Peer p;
    ASSERT_EQ(peer_listen(&p, DOUBLED_8), 0);
    peer_start(&p);
    KlAllocator a = kl_allocator_default();
    KlHttpClientConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.timeout_ms = 3000;
    cfg.decompress = &g_md_cfg;
    size_t got = 0;
    KlHttpClientStreamCfg st;
    memset(&st, 0, sizeof st);
    st.on_body = sink_body;
    st.user_data = &got;
    KlHttpClientResponse r;
    memset(&r, 0, sizeof r);
    g_md_fail_flush = 1;
    int rc = kl_http_client_request_s(&a, &cfg, "GET", url_for(&p), NULL, 0, NULL, 0, &st, &r);
    g_md_fail_flush = 0;
    kl_http_client_response_free(&r);
    peer_finish(&p);
    ASSERT_EQ(got, (size_t)16);
    ASSERT_EQ(rc, -1);                           /* was 0 */
}

/* A buffered body must be refused while inflating, not after. The limit was compared only once the
 * whole body had been decompressed (up to the backend's own cap, 256 MB for miniz), so a small
 * compressed body still cost its full inflated size first. This "bomb" inflates each input byte to
 * 1 MiB: its one-shot form allocates all of it, its streaming form emits it in 64 KiB pieces from a
 * static buffer. With a 64-byte limit the client's live memory must stay small. */
static long g_peak, g_cur;
static void *pk_malloc(void *c, size_t n) {
    (void)c; void *p = malloc(n ? n + sizeof(size_t) : sizeof(size_t));
    if (!p) return NULL;
    *(size_t *)p = n; g_cur += (long)n; if (g_cur > g_peak) g_peak = g_cur;
    return (char *)p + sizeof(size_t);
}
static void pk_free(void *c, void *p, size_t n) {
    (void)c; (void)n;
    if (!p) return;
    char *b = (char *)p - sizeof(size_t);
    g_cur -= (long)*(size_t *)b;
    free(b);
}
static void *pk_realloc(void *c, void *p, size_t o, size_t n) {
    (void)o;
    void *q = pk_malloc(c, n);
    if (!q) return NULL;
    if (p) {
        size_t old = *(size_t *)((char *)p - sizeof(size_t));
        memcpy(q, p, old < n ? old : n);
        pk_free(c, p, old);
    }
    return q;
}
static int bomb_decompress(KlDecompress *self, const char *in, size_t in_len, char **out,
                           size_t *out_len, KlAllocator *alloc) {
    (void)self; (void)in;
    size_t n = in_len * (1u << 20);
    char *b = kl_malloc(alloc, n);
    if (!b) return -1;
    memset(b, 'b', n);
    *out = b;
    *out_len = n;
    return 0;
}
static int bomb_dfeed(KlDecompress *self, const char *d, size_t n, int flush,
                      int (*emit)(void *, const char *, size_t), void *ctx) {
    (void)self; (void)d; (void)flush;
    static char piece[64 * 1024];
    memset(piece, 'b', sizeof piece);
    for (size_t i = 0; i < n; i++)
        for (int k = 0; k < 16; k++)
            if (emit(ctx, piece, sizeof piece) != 0) return -1;
    return 0;
}
static const char *bomb_encoding(KlDecompress *self) { (void)self; return "x-bomb"; }
static void bomb_reset(KlDecompress *self) { (void)self; }
static void bomb_destroy(KlDecompress *self) { (void)self; }
static KlDecompress g_bomb = { bomb_decompress, bomb_dfeed, bomb_encoding, bomb_reset, bomb_destroy };
static KlDecompress *bomb_factory(KlCompressCtx *ctx, KlAllocator *alloc) { (void)ctx; (void)alloc; return &g_bomb; }
static KlDecompressConfig g_bomb_cfg = { .ctx = NULL, .factory = bomb_factory };

UTEST(decompress, buffered_limit_stops_inflation_early) {
    Peer p;
    ASSERT_EQ(peer_listen(&p, "HTTP/1.1 200 OK\r\nContent-Encoding: x-bomb\r\nContent-Length: 32\r\n\r\n"
                              "abcdefghabcdefghabcdefghabcdefgh"), 0);   /* 32 MiB once inflated */
    peer_start(&p);
    KlAllocator a = { pk_malloc, pk_realloc, pk_free, NULL };
    KlHttpClientConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.timeout_ms = 3000;
    cfg.max_response_size = 64;                  /* the 32 wire bytes fit; the inflated body cannot */
    cfg.decompress = &g_bomb_cfg;
    KlHttpClientResponse r;
    memset(&r, 0, sizeof r);
    g_peak = g_cur = 0;
    int rc = kl_http_client_request(&a, &cfg, "GET", url_for(&p), NULL, 0, NULL, 0, &r);
    KlError err = r.error;
    kl_http_client_response_free(&r);
    peer_finish(&p);
    ASSERT_EQ(rc, -1);
    ASSERT_EQ((int)err, (int)KL_ERR_TOO_LARGE);
    ASSERT_LT(g_peak, (1L << 20));               /* was: 32 MiB inflated before the check */
}

UTEST_MAIN();
