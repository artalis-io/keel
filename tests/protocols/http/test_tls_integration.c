#include "utest.h"
#include <keel/keel.h>
#include <keel/tls.h>
#include "net_compat.h"
#include "mock_tls.h"   /* shared identity TLS mock: completion-capable (feed_input/drain_output) */
#include <string.h>
#include "platform_thread.h"   /* Keel PAL threads: portable to MSVC */
#include <errno.h>

/* The passthrough TLS mock (identity, no crypto) now lives in tests/mock_tls.h and implements
 * the completion-mode ops too (feed_input/drain_output), so this suite runs over the completion
 * backend as well as readiness. The factory is mock_tls_create. */

/* ── Helpers ────────────────────────────────────────────────────────── */

static void handle_hello(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_http_response_json(res, 200, "{\"ok\":true}", 11);
}

static void wait_for_bind(KlHttpServer *s) {
    for (int i = 0; i < 200 && s->bound_port == 0; i++) kl_test_sleep_ms(10);
}

static void server_thread_fn(void *arg) {
    kl_http_server_run((KlHttpServer *)arg);
}

static int connect_to(int port) {
    int fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)port),
    };
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        kl_test_closesock(fd);
        return -1;
    }
    return fd;
}

/* As connect_to, with a 4 KiB receive buffer set BEFORE connecting, so the window the peer sees
 * is small from the start: a client that then never reads stops the server's sends quickly. */
static int connect_small_rcvbuf(int port) {
    int fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int rcv = 4096;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (const char *)&rcv, sizeof rcv);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)port),
    };
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        kl_test_closesock(fd);
        return -1;
    }
    return fd;
}

static kl_ssize_t read_response(int fd, char *buf, size_t buflen, int timeout_ms) {
    kl_ssize_t total = 0;
    while (total < (kl_ssize_t)buflen - 1) {
        int pr = kl_test_poll1(fd, 0, timeout_ms);
        if (pr <= 0) break;
        kl_ssize_t n = kl_test_sockread(fd, buf + total, buflen - (size_t)total - 1);
        if (n <= 0) break;
        total += n;
    }
    buf[total] = '\0';
    return total;
}

static kl_ssize_t read_one_response(int fd, char *buf, size_t buflen, int timeout_ms) {
    kl_ssize_t total = 0;
    while (total < (kl_ssize_t)buflen - 1) {
        int pr = kl_test_poll1(fd, 0, timeout_ms);
        if (pr <= 0) break;
        kl_ssize_t n = kl_test_sockread(fd, buf + total, buflen - (size_t)total - 1);
        if (n <= 0) break;
        total += n;
        buf[total] = '\0';
        char *hdr_end = strstr(buf, "\r\n\r\n");
        if (hdr_end) {
            hdr_end += 4;
            char *cl = strstr(buf, "Content-Length:");
            if (cl) {
                size_t cl_val = (size_t)strtol(cl + 15, NULL, 10);
                size_t body_rcvd = (size_t)(total - (hdr_end - buf));
                if (body_rcvd >= cl_val) break;
            }
        }
    }
    buf[total] = '\0';
    return total;
}

/* ═══════════════════════════════════════════════════════════════════
 * Tests
 * ═══════════════════════════════════════════════════════════════════ */

UTEST(tls_integration, hello_request) {
    KlTlsConfig tls_cfg = {
        .ctx     = NULL,
        .factory = mock_tls_create,
    };
    KlHttpServerConfig cfg = {
        .port = 0,
        .tls  = &tls_cfg,
    };
    KlHttpServer srv;
    ASSERT_EQ(0, kl_http_server_init(&srv, &cfg));
    kl_http_server_route(&srv, "GET", "/hello", handle_hello, NULL, NULL);

    KlPlatThread tid;
    kl_plat_thread_create(&tid, server_thread_fn, &srv);
    wait_for_bind(&srv);
    ASSERT_TRUE(srv.bound_port > 0);
    int port = srv.bound_port;

    /* Connect and send plain HTTP: passthrough TLS is transparent */
    int fd = connect_to(port);
    ASSERT_TRUE(fd >= 0);

    const char *req = "GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    kl_test_sockwrite(fd, req, strlen(req));

    char buf[2048];
    read_response(fd, buf, sizeof(buf), 2000);
    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);
    ASSERT_TRUE(strstr(buf, "{\"ok\":true}") != NULL);

    kl_test_closesock(fd);
    kl_http_server_stop(&srv);
    kl_plat_thread_join(&tid);
    kl_http_server_free(&srv);
}

UTEST(tls_integration, keep_alive) {
    KlTlsConfig tls_cfg = {
        .ctx     = NULL,
        .factory = mock_tls_create,
    };
    KlHttpServerConfig cfg = {
        .port = 0,
        .tls  = &tls_cfg,
    };
    KlHttpServer srv;
    ASSERT_EQ(0, kl_http_server_init(&srv, &cfg));
    kl_http_server_route(&srv, "GET", "/hello", handle_hello, NULL, NULL);

    KlPlatThread tid;
    kl_plat_thread_create(&tid, server_thread_fn, &srv);
    wait_for_bind(&srv);
    int port = srv.bound_port;

    int fd = connect_to(port);
    ASSERT_TRUE(fd >= 0);

    /* First request (keep-alive) */
    const char *req1 = "GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n";
    kl_test_sockwrite(fd, req1, strlen(req1));

    char buf[2048];
    read_one_response(fd, buf, sizeof(buf), 2000);
    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);
    ASSERT_TRUE(strstr(buf, "{\"ok\":true}") != NULL);

    /* Second request on same connection (tests TLS reset()) */
    const char *req2 = "GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    kl_test_sockwrite(fd, req2, strlen(req2));

    read_response(fd, buf, sizeof(buf), 2000);
    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);
    ASSERT_TRUE(strstr(buf, "{\"ok\":true}") != NULL);

    kl_test_closesock(fd);
    kl_http_server_stop(&srv);
    kl_plat_thread_join(&tid);
    kl_http_server_free(&srv);
}

UTEST(tls_integration, concurrent) {
    KlTlsConfig tls_cfg = {
        .ctx     = NULL,
        .factory = mock_tls_create,
    };
    KlHttpServerConfig cfg = {
        .port = 0,
        .tls  = &tls_cfg,
    };
    KlHttpServer srv;
    ASSERT_EQ(0, kl_http_server_init(&srv, &cfg));
    kl_http_server_route(&srv, "GET", "/hello", handle_hello, NULL, NULL);

    KlPlatThread tid;
    kl_plat_thread_create(&tid, server_thread_fn, &srv);
    wait_for_bind(&srv);
    int port = srv.bound_port;

    /* 3 concurrent TLS connections */
    int fds[3];
    for (int i = 0; i < 3; i++) {
        fds[i] = connect_to(port);
        ASSERT_TRUE(fds[i] >= 0);
    }

    const char *req = "GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    for (int i = 0; i < 3; i++) {
        kl_test_sockwrite(fds[i], req, strlen(req));
    }

    for (int i = 0; i < 3; i++) {
        char buf[2048];
        read_response(fds[i], buf, sizeof(buf), 2000);
        ASSERT_TRUE_MSG(strstr(buf, "200 OK") != NULL,
                         "TLS connection %d should get 200 OK");
        ASSERT_TRUE_MSG(strstr(buf, "{\"ok\":true}") != NULL,
                         "TLS connection %d should have correct body");
        kl_test_closesock(fds[i]);
    }

    kl_http_server_stop(&srv);
    kl_plat_thread_join(&tid);
    kl_http_server_free(&srv);
}

/* ── A TLS record split across reads ────────────────────────────────────
 * On a real network a TLS record often spans several TCP segments, so a readable event can arrive
 * with only part of a record: the engine takes the bytes and read() returns 0, WANT_READ. That is
 * not end of stream (the KlTls contract: 0 = WANT_READ, -1 = error or closed). The server must
 * wait for the rest, in the header phase and in the body phase. mock_tls_split_record simulates
 * the split (socket mode only, so this exercises the readiness transport). */

static void handle_echo_body(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)ctx;
    KlHttpBufReader *br = (KlHttpBufReader *)req->body_reader;
    kl_http_response_status(res, 200);
    if (br && br->len > 0)
        (void)kl_http_response_body_copy(res, br->data, br->len);
}

static KlHttpServer split_srv;

/* Send `req` in two writes split at `cut`, with the server told the record ends at the end of
 * the request; returns the response. */
static void split_round_trip(int port, const char *req, size_t cut, char *buf, size_t cap) {
    buf[0] = '\0';
    int fd = connect_to(port);
    if (fd < 0) return;
    (void)kl_test_sockwrite(fd, req, cut);
    kl_test_sleep_ms(150);                    /* the server sees the first part on its own */
    (void)kl_test_sockwrite(fd, req + cut, strlen(req) - cut);
    read_response(fd, buf, cap, 3000);
    kl_test_closesock(fd);
}

UTEST(tls_integration, record_split_in_headers_waits_for_the_rest) {
    KlTlsConfig tls_cfg = { .ctx = NULL, .factory = mock_tls_create };
    KlHttpServerConfig cfg = { .port = 0, .tls = &tls_cfg };
    ASSERT_EQ(0, kl_http_server_init(&split_srv, &cfg));
    kl_http_server_route(&split_srv, "GET", "/hello", handle_hello, NULL, NULL);
    KlPlatThread tid;
    kl_plat_thread_create(&tid, server_thread_fn, &split_srv);
    wait_for_bind(&split_srv);

    static char buf[2048];
    const char *req = "GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    mock_tls_split_after = 0;
    mock_tls_split_record = strlen(req);
    split_round_trip(split_srv.bound_port, req, 20, buf, sizeof buf);
    mock_tls_split_record = 0;

    kl_http_server_stop(&split_srv);
    kl_plat_thread_join(&tid);
    kl_http_server_free(&split_srv);
    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);
}

UTEST(tls_integration, record_split_in_body_waits_for_the_rest) {
    KlTlsConfig tls_cfg = { .ctx = NULL, .factory = mock_tls_create };
    KlHttpServerConfig cfg = { .port = 0, .tls = &tls_cfg };
    ASSERT_EQ(0, kl_http_server_init(&split_srv, &cfg));
    kl_http_server_route(&split_srv, "POST", "/echo", handle_echo_body,
                         (void *)(size_t)(64 * 1024), kl_http_body_reader_buffer);
    KlPlatThread tid;
    kl_plat_thread_create(&tid, server_thread_fn, &split_srv);
    wait_for_bind(&split_srv);

    static char buf[2048];
    const char *hdrs = "POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 10\r\n"
                       "Connection: close\r\n\r\n";
    static char req[256];
    snprintf(req, sizeof req, "%s0123456789", hdrs);
    mock_tls_split_after = strlen(hdrs);       /* the headers arrive whole; the body record splits */
    mock_tls_split_record = 10;
    /* headers alone first, then half the body, then the rest */
    int fd = connect_to(split_srv.bound_port);
    buf[0] = '\0';
    if (fd >= 0) {
        (void)kl_test_sockwrite(fd, req, strlen(hdrs));
        kl_test_sleep_ms(150);
        (void)kl_test_sockwrite(fd, req + strlen(hdrs), 4);
        kl_test_sleep_ms(150);
        (void)kl_test_sockwrite(fd, req + strlen(hdrs) + 4, 6);
        read_response(fd, buf, sizeof buf, 3000);
        kl_test_closesock(fd);
    }
    mock_tls_split_record = 0;
    mock_tls_split_after = 0;

    kl_http_server_stop(&split_srv);
    kl_plat_thread_join(&tid);
    kl_http_server_free(&split_srv);
    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);
    ASSERT_TRUE(strstr(buf, "0123456789") != NULL);
}

/* A paused body stays paused over TLS. The completion TLS drive posted the next receive whatever the
 * pause state, so kl_http_request_pause_body had no effect on HTTPS uploads on IOCP or io_uring:
 * every later chunk still reached on_data. */
typedef struct { KlHttpBodyReader base; KlAllocator *alloc; const KlHttpRequest *req; } TlsPauseReader;
static int g_tls_pause_calls;
static int tls_pause_on_data(KlHttpBodyReader *self, const char *data, size_t len) {
    (void)data; (void)len;
    g_tls_pause_calls++;
    kl_http_request_pause_body(((TlsPauseReader *)self)->req);
    return 0;
}
static void tls_pause_noop(KlHttpBodyReader *self) { (void)self; }
static void tls_pause_destroy(KlHttpBodyReader *self) {
    TlsPauseReader *r = (TlsPauseReader *)self;
    kl_free(r->alloc, r, sizeof(*r));
}
static KlHttpBodyReader *tls_pause_factory(KlAllocator *alloc, const KlHttpRequest *req, void *ud) {
    (void)ud;
    TlsPauseReader *r = kl_malloc(alloc, sizeof(*r));
    if (!r) return NULL;
    memset(r, 0, sizeof(*r));
    r->base.on_data = tls_pause_on_data;
    r->base.on_complete = tls_pause_noop;
    r->base.on_error = tls_pause_noop;
    r->base.destroy = tls_pause_destroy;
    r->alloc = alloc;
    r->req = req;
    return &r->base;
}
static void handle_never(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_http_response_status(res, 200);
}
static KlHttpServer pause_tls_srv;

UTEST(tls_integration, paused_body_stays_paused) {
    KlTlsConfig tls_cfg = { .ctx = NULL, .factory = mock_tls_create };
    KlHttpServerConfig cfg = { .port = 0, .tls = &tls_cfg, .body_timeout_ms = 10000 };
    ASSERT_EQ(0, kl_http_server_init(&pause_tls_srv, &cfg));
    kl_http_server_route(&pause_tls_srv, "POST", "/pause", handle_never, NULL, tls_pause_factory);
    KlPlatThread tid;
    kl_plat_thread_create(&tid, server_thread_fn, &pause_tls_srv);
    wait_for_bind(&pause_tls_srv);

    g_tls_pause_calls = 0;
    const char *h = "POST /pause HTTP/1.1\r\nHost: localhost\r\nContent-Length: 30\r\n\r\n";
    int fd = connect_to(pause_tls_srv.bound_port);
    if (fd >= 0) {
        (void)kl_test_sockwrite(fd, h, strlen(h));
        for (int i = 0; i < 3; i++) {                      /* three separate chunks */
            kl_test_sleep_ms(150);
            (void)kl_test_sockwrite(fd, "0123456789", 10);
        }
        kl_test_sleep_ms(300);                             /* time for any wrongly read chunk */
    }
    kl_http_server_stop(&pause_tls_srv);
    kl_plat_thread_join(&tid);
    kl_http_server_free(&pause_tls_srv);
    if (fd >= 0) kl_test_closesock(fd);
    ASSERT_EQ(g_tls_pause_calls, 1);                       /* was (completion): 3, the pause ignored */
}

/* ── TLS output on a completion loop never blocks the loop ─────────────────────────────────
 * On a completion loop TLS ciphertext was pushed with a synchronous send on the loop thread. A
 * client that stops reading a large TLS response then stalled EVERY connection on the loop: for
 * 30 s per stall on pollcomp (a bounded poll), indefinitely on IOCP and io_uring (accepted sockets
 * are blocking there). Another client's request must be answered meanwhile. Readiness loops already
 * use WRITE interest, so they pass either way. */
#define TQ_CHUNK  (64 * 1024)
#define TQ_CHUNKS 64                                       /* 4 MiB of payload */
static void handle_tq_bigstream(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)ctx;
    KlHttpResponseWriteFn w = NULL;
    void *wc = NULL;
    if (kl_http_response_begin_stream(res, 200, &w, &wc) < 0) return;
    static char chunk[TQ_CHUNK];
    memset(chunk, 'T', sizeof chunk);
    for (int i = 0; i < TQ_CHUNKS; i++) w(wc, chunk, sizeof chunk);
    kl_http_response_end_stream(res);
}
static KlHttpServer tq_srv;

UTEST(tls_integration, a_stalled_reader_does_not_block_other_connections) {
    KlTlsConfig tls_cfg = { .ctx = NULL, .factory = mock_tls_create };
    KlHttpServerConfig cfg = { .port = 0, .tls = &tls_cfg, .max_connections = 8 };
    ASSERT_EQ(0, kl_http_server_init(&tq_srv, &cfg));
    kl_http_server_route(&tq_srv, "GET", "/big", handle_tq_bigstream, NULL, NULL);
    kl_http_server_route(&tq_srv, "GET", "/hello", handle_hello, NULL, NULL);
    KlPlatThread tid;
    kl_plat_thread_create(&tid, server_thread_fn, &tq_srv);
    wait_for_bind(&tq_srv);
    int port = tq_srv.bound_port;

    int a = connect_to(port);                              /* asks for 4 MiB, then reads nothing */
    if (a >= 0) {
        int rcv = 4096;
        (void)setsockopt(a, SOL_SOCKET, SO_RCVBUF, (const char *)&rcv, sizeof rcv);
        const char *rq = "GET /big HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
        (void)kl_test_sockwrite(a, rq, strlen(rq));
    }
    kl_test_sleep_ms(300);                                 /* the server is now stuck on A, or not */

    char buf[1024];
    buf[0] = '\0';
    int b = connect_to(port);
    if (b >= 0) {
        const char *rq = "GET /hello HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
        (void)kl_test_sockwrite(b, rq, strlen(rq));
        read_response(b, buf, sizeof buf, 2000);
        kl_test_closesock(b);
    }
    if (a >= 0) kl_test_closesock(a);                      /* unblocks a loop stuck on A */
    kl_http_server_stop(&tq_srv);
    kl_plat_thread_join(&tid);
    kl_http_server_free(&tq_srv);
    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);            /* was (completion): no answer in 2 s */
}

/* A request rejected on a TLS completion connection gets its response. The rejection wrote the 413
 * through the TLS engine (on a completion loop, into its output ring) and then half-closed the
 * socket for the drain without sending that ring: the client saw FIN and no status. */
static KlHttpServer tq_rej_srv;

UTEST(tls_integration, a_rejected_upload_gets_its_413) {
    KlTlsConfig tls_cfg = { .ctx = NULL, .factory = mock_tls_create };
    KlHttpServerConfig cfg = { .port = 0, .tls = &tls_cfg };
    ASSERT_EQ(0, kl_http_server_init(&tq_rej_srv, &cfg));
    kl_http_server_route(&tq_rej_srv, "POST", "/up", handle_hello,
                         (void *)(size_t)1024, kl_http_body_reader_buffer);   /* a 1 KiB reader */
    KlPlatThread tid;
    kl_plat_thread_create(&tid, server_thread_fn, &tq_rej_srv);
    wait_for_bind(&tq_rej_srv);

    char buf[2048];
    buf[0] = '\0';
    int fd = connect_to(tq_rej_srv.bound_port);
    if (fd >= 0) {
        const char *h = "POST /up HTTP/1.1\r\nHost: x\r\nContent-Length: 20000\r\n\r\n";
        (void)kl_test_sockwrite(fd, h, strlen(h));
        static char body[20000];
        memset(body, 'b', sizeof body);
        (void)kl_test_sockwrite(fd, body, sizeof body);    /* the declared body, no more */
        read_response(fd, buf, sizeof buf, 2000);
        kl_test_closesock(fd);
    }
    kl_http_server_stop(&tq_rej_srv);
    kl_plat_thread_join(&tid);
    kl_http_server_free(&tq_rej_srv);
    ASSERT_TRUE(strstr(buf, "413") != NULL);               /* was (completion + TLS): FIN, no status */
}

/* A connection that is waiting for its output to go before it closes is still timed out. A TLS
 * response with Connection: close leaves it closing once the output queue is empty, a state the
 * idle sweep skipped, so a client that never reads held the slot for good: with one slot, nobody
 * else was served again. (IOCP over loopback takes even a 16 MiB send whole, so this shows on the
 * POSIX completion backends, whose sends stop at a peer that does not read.) */
static KlHttpServer tq_hold_srv;
static void handle_tq_hugestream(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)ctx;
    KlHttpResponseWriteFn w = NULL;
    void *wc = NULL;
    if (kl_http_response_begin_stream(res, 200, &w, &wc) < 0) return;
    static char chunk[TQ_CHUNK];
    memset(chunk, 'H', sizeof chunk);
    for (int i = 0; i < 4 * TQ_CHUNKS; i++) w(wc, chunk, sizeof chunk);   /* 16 MiB */
    kl_http_response_end_stream(res);
}

UTEST(tls_integration, a_client_that_never_reads_does_not_hold_its_slot) {
    KlTlsConfig tls_cfg = { .ctx = NULL, .factory = mock_tls_create };
    KlHttpServerConfig cfg = { .port = 0, .tls = &tls_cfg, .max_connections = 1,
                               .read_timeout_ms = 300 };
    ASSERT_EQ(0, kl_http_server_init(&tq_hold_srv, &cfg));
    kl_http_server_route(&tq_hold_srv, "GET", "/big", handle_tq_hugestream, NULL, NULL);
    kl_http_server_route(&tq_hold_srv, "GET", "/hello", handle_hello, NULL, NULL);
    KlPlatThread tid;
    kl_plat_thread_create(&tid, server_thread_fn, &tq_hold_srv);
    wait_for_bind(&tq_hold_srv);
    int port = tq_hold_srv.bound_port;

    int a = connect_small_rcvbuf(port);                    /* asks for 16 MiB, never reads */
    if (a >= 0) {
        const char *rq = "GET /big HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
        (void)kl_test_sockwrite(a, rq, strlen(rq));
    }
    kl_test_sleep_ms(1500);                                /* well past the 300 ms idle timeout */

    char buf[1024];
    buf[0] = '\0';
    int b = connect_to(port);
    if (b >= 0) {
        const char *rq = "GET /hello HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
        (void)kl_test_sockwrite(b, rq, strlen(rq));
        read_response(b, buf, sizeof buf, 2000);
        kl_test_closesock(b);
    }
    if (a >= 0) kl_test_closesock(a);
    kl_http_server_stop(&tq_hold_srv);
    kl_plat_thread_join(&tid);
    kl_http_server_free(&tq_hold_srv);
    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);            /* was (completion + TLS): never served */
}

/* Input the engine already holds is read before the network is asked for more. A real engine hands
 * out one record per read, so headers and body that came in one receive come out over several reads;
 * once the headers were parsed, the body read posted a network receive and left the rest of the body
 * in the engine, waiting for bytes the client had already sent. mock_tls_read_max hands out 48 bytes
 * per read. */
static KlHttpServer tq_rec_srv;

UTEST(tls_integration, a_body_that_came_with_its_headers_is_not_left_in_the_engine) {
    KlTlsConfig tls_cfg = { .ctx = NULL, .factory = mock_tls_create };
    KlHttpServerConfig cfg = { .port = 0, .tls = &tls_cfg };
    ASSERT_EQ(0, kl_http_server_init(&tq_rec_srv, &cfg));
    kl_http_server_route(&tq_rec_srv, "POST", "/echo", handle_echo_body,
                         (void *)(size_t)4096, kl_http_body_reader_buffer);
    mock_tls_read_max = 48;
    KlPlatThread tid;
    kl_plat_thread_create(&tid, server_thread_fn, &tq_rec_srv);
    wait_for_bind(&tq_rec_srv);

    char buf[4096];
    buf[0] = '\0';
    int fd = connect_to(tq_rec_srv.bound_port);
    if (fd >= 0) {
        static char rq[2048];
        int hl = snprintf(rq, sizeof rq,
                          "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 1000\r\n"
                          "Connection: close\r\n\r\n");
        memset(rq + hl, 'e', 1000);
        (void)kl_test_sockwrite(fd, rq, (size_t)hl + 1000);   /* headers and body in one write */
        read_response(fd, buf, sizeof buf, 1500);
        kl_test_closesock(fd);
    }
    kl_http_server_stop(&tq_rec_srv);
    kl_plat_thread_join(&tid);
    kl_http_server_free(&tq_rec_srv);
    mock_tls_read_max = 0;
    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);            /* was (completion + TLS): no answer */
}

/* A request that times out over TLS gets its 408. The sweep wrote it through the TLS engine (on a
 * completion loop, into its output ring) and nothing ever sent it. */
static KlHttpServer tq_408_srv;

UTEST(tls_integration, a_timed_out_request_gets_its_408) {
    KlTlsConfig tls_cfg = { .ctx = NULL, .factory = mock_tls_create };
    KlHttpServerConfig cfg = { .port = 0, .tls = &tls_cfg, .read_timeout_ms = 300 };
    ASSERT_EQ(0, kl_http_server_init(&tq_408_srv, &cfg));
    kl_http_server_route(&tq_408_srv, "GET", "/hello", handle_hello, NULL, NULL);
    KlPlatThread tid;
    kl_plat_thread_create(&tid, server_thread_fn, &tq_408_srv);
    wait_for_bind(&tq_408_srv);

    char buf[1024];
    buf[0] = '\0';
    int fd = connect_to(tq_408_srv.bound_port);
    if (fd >= 0) {
        const char *part = "GET /hello HTTP/1.1\r\nHost: x\r\n";   /* and then nothing */
        (void)kl_test_sockwrite(fd, part, strlen(part));
        read_response(fd, buf, sizeof buf, 2500);
        kl_test_closesock(fd);
    }
    kl_http_server_stop(&tq_408_srv);
    kl_plat_thread_join(&tid);
    kl_http_server_free(&tq_408_srv);
    ASSERT_TRUE(strstr(buf, "408") != NULL);               /* was (completion + TLS): no status */
}

/* A rejected chunked upload over TLS drains on its plaintext. The drain fed the bytes in read_buf to
 * the chunked decoder, but a TLS receive lands its ciphertext in another buffer (the engine's input),
 * so the decoder worked on stale bytes, past the end of read_buf for a receive larger than it. It must
 * end at the terminal chunk: the connection closes then, not at the drain deadline. */
static KlHttpServer tq_chk_srv;

UTEST(tls_integration, a_rejected_chunked_upload_drains_to_its_terminal_chunk) {
    KlTlsConfig tls_cfg = { .ctx = NULL, .factory = mock_tls_create };
    KlHttpServerConfig cfg = { .port = 0, .tls = &tls_cfg, .reject_drain_timeout_ms = 5000 };
    ASSERT_EQ(0, kl_http_server_init(&tq_chk_srv, &cfg));
    kl_http_server_route(&tq_chk_srv, "POST", "/up", handle_hello,
                         (void *)(size_t)1024, kl_http_body_reader_buffer);   /* a 1 KiB reader */
    KlPlatThread tid;
    kl_plat_thread_create(&tid, server_thread_fn, &tq_chk_srv);
    wait_for_bind(&tq_chk_srv);

    char buf[2048];
    buf[0] = '\0';
    int closed = 0;
    int fd = connect_to(tq_chk_srv.bound_port);
    if (fd >= 0) {
        static char big[16384 + 64];
        const char *h = "POST /up HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n";
        (void)kl_test_sockwrite(fd, h, strlen(h));
        int n = snprintf(big, sizeof big, "%x\r\n", 2000);     /* over the reader's 1 KiB: 413 */
        memset(big + n, 'c', 2000);
        memcpy(big + n + 2000, "\r\n", 2);
        (void)kl_test_sockwrite(fd, big, (size_t)n + 2002);
        read_one_response(fd, buf, sizeof buf, 2000);          /* the 413 */
        for (int i = 0; i < 3; i++) {                          /* more body, 16 KiB chunks */
            n = snprintf(big, sizeof big, "%x\r\n", 16384);
            memset(big + n, 'c', 16384);
            memcpy(big + n + 16384, "\r\n", 2);
            (void)kl_test_sockwrite(fd, big, (size_t)n + 16386);
        }
        (void)kl_test_sockwrite(fd, "0\r\n\r\n", 5);           /* the terminal chunk */
        char sink[512];
        for (int i = 0; i < 20; i++) {                         /* EOF well before the deadline */
            if (kl_test_poll1(fd, 0, 1500) <= 0) break;
            long r = kl_test_sockread(fd, sink, sizeof sink);
            if (r <= 0) { closed = 1; break; }
        }
        kl_test_closesock(fd);
    }
    kl_http_server_stop(&tq_chk_srv);
    kl_plat_thread_join(&tid);
    kl_http_server_free(&tq_chk_srv);
    fprintf(stderr, "PROBE client: got %zu bytes, closed=%d, first line: %.40s\n", strlen(buf), closed, buf);
    ASSERT_TRUE(strstr(buf, "413") != NULL);
    ASSERT_EQ(closed, 1);                                  /* was: open until the 5 s deadline */
}

UTEST_MAIN();
