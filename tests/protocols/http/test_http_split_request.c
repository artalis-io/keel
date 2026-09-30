/*
 * test_http_split_request.c: an HTTP/1 request whose request line and headers arrive in more than
 * one read is parsed exactly as if it had arrived in one.
 *
 * The request parser is stateful: it keeps its place between calls. The server must therefore feed
 * it only the bytes it has not seen yet. Re-feeding the accumulated buffer from byte 0 made every
 * split request fail (a parse error, a phantom header set, or an out-of-bounds path length), on both
 * event models, while loopback tests that send a request in one write never noticed.
 *
 * Each case drives a real KlHttpServer over loopback and writes the request in pieces with a pause
 * between them, so each piece is a separate read. The handler echoes what it parsed (path, query,
 * one header's value, header count), so a request that parses "successfully" but differently fails
 * too. Cases cover every single split point, a byte-at-a-time client, a header block larger than the
 * base read buffer (the buffer-growth path), and a split request on a kept-alive connection. The
 * suite runs on whichever engine the build selects, readiness or completion.
 */
#include "utest.h"
#include <keel/keel.h>
#include "net_compat.h"
#if !defined(_WIN32)
#include <netinet/tcp.h>       /* TCP_NODELAY */
#endif
#include "platform_thread.h"   /* Keel PAL threads: portable to MSVC */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Server under test ──────────────────────────────────────────────────────────────────────── */

/* Echo what the parser produced: "path|query|x-probe value length:first 8 bytes|header count". */
static void handle_echo(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)ud;
    char body[256];
    size_t plen = 0;
    const char *probe = kl_http_request_header_len(req, "X-Probe", &plen);
    int n = snprintf(body, sizeof body, "%.*s|%.*s|%zu:%.*s|%d",
                     (int)req->path_len, req->path ? req->path : "",
                     (int)req->query_len, req->query ? req->query : "",
                     probe ? plen : 0, probe ? (int)(plen < 8 ? plen : 8) : 0, probe ? probe : "",
                     req->num_headers);
    kl_http_response_status(res, 200);
    kl_http_response_body_copy(res, body, (size_t)n);
}

static void server_thread_fn(void *arg) { kl_http_server_run((KlHttpServer *)arg); }

typedef struct { KlHttpServer srv; KlPlatThread t; int port; } Srv;

/* Each test's Srv is static: a failed ASSERT returns from the test without srv_stop, and a server
 * thread still running on a stack frame the next test reuses crashes the process, turning a clean
 * failure report into a segfault. */
static int srv_start(Srv *s, size_t max_header_size) {
    KlHttpServerConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.port = 0;
    cfg.bind_addr = "127.0.0.1";
    cfg.max_header_size = max_header_size;
    if (kl_http_server_init(&s->srv, &cfg) != 0) return -1;
    kl_http_server_route(&s->srv, "GET", "/echo", handle_echo, NULL, NULL);
    if (kl_plat_thread_create(&s->t, server_thread_fn, &s->srv) != 0) return -1;
    for (int i = 0; i < 300 && s->srv.bound_port == 0; i++) kl_test_sleep_ms(10);
    s->port = s->srv.bound_port;
    return s->port > 0 ? 0 : -1;
}

static void srv_stop(Srv *s) {
    kl_http_server_stop(&s->srv);
    kl_plat_thread_join(&s->t);
    kl_http_server_free(&s->srv);
}

/* ── Client helpers ─────────────────────────────────────────────────────────────────────────── */

static KlSocketHandle connect_to(int port) {
    KlSocketHandle fd = (KlSocketHandle)socket(AF_INET, SOCK_STREAM, 0);
    if (!kl_handle_valid(fd)) return KL_INVALID_SOCKET;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) { kl_test_closesock(fd); return KL_INVALID_SOCKET; }
    int one = 1;   /* each piece leaves as its own segment */
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    kl_test_set_rcvtimeo(fd, 3000);
    return fd;
}

static int send_all(KlSocketHandle fd, const char *p, size_t n) {
    while (n > 0) {
        int k = (int)send(fd, p, (int)n, 0);
        if (k <= 0) return -1;
        p += k; n -= (size_t)k;
    }
    return 0;
}

/* Read one response: the headers, then Content-Length body bytes. Returns the body length (into
 * `body`, NUL-terminated) with *status set, or -1 if the server closed or stalled first. */
static int read_response(KlSocketHandle fd, int *status, char *body, size_t body_cap) {
    static char buf[16384];
    size_t len = 0;
    char *eoh = NULL;
    while (!eoh) {
        if (len + 1 >= sizeof buf) return -1;
        int k = (int)recv(fd, buf + len, (int)(sizeof buf - 1 - len), 0);
        if (k <= 0) return -1;
        len += (size_t)k;
        buf[len] = '\0';
        eoh = strstr(buf, "\r\n\r\n");
    }
    if (sscanf(buf, "HTTP/1.1 %d", status) != 1) return -1;
    const char *cl = strstr(buf, "Content-Length:");
    if (!cl) cl = strstr(buf, "content-length:");
    size_t want = cl ? (size_t)strtoul(cl + 15, NULL, 10) : 0;
    size_t have = len - (size_t)(eoh + 4 - buf);
    if (want >= body_cap) return -1;
    memcpy(body, eoh + 4, have < want ? have : want);
    while (have < want) {
        int k = (int)recv(fd, body + have, (int)(want - have), 0);
        if (k <= 0) return -1;
        have += (size_t)k;
    }
    body[want] = '\0';
    return (int)want;
}

/* Send `req` in pieces cut at the given offsets (ascending, within the request), pausing between
 * pieces so each is a separate read, then read one response. */
static int send_in_pieces(KlSocketHandle fd, const char *req, size_t n, const size_t *cuts, int ncuts,
                          unsigned pause_ms) {
    size_t at = 0;
    for (int i = 0; i <= ncuts; i++) {
        size_t end = i < ncuts ? cuts[i] : n;
        if (end > at && send_all(fd, req + at, end - at) != 0) return -1;
        at = end;
        if (i < ncuts) kl_test_sleep_ms(pause_ms);
    }
    return 0;
}

static const char REQ[] =
    "GET /echo?k=v HTTP/1.1\r\n"
    "Host: localhost\r\n"
    "X-Probe: abcdefgh\r\n"
    "User-Agent: keel-split-test\r\n"
    "Connection: close\r\n"
    "\r\n";
static const char EXPECT[] = "/echo|k=v|8:abcdefgh|4";

/* ── Cases ──────────────────────────────────────────────────────────────────────────────────── */

UTEST(http_split_request, one_write_baseline) {
    static Srv s; ASSERT_EQ(srv_start(&s, 0), 0);
    KlSocketHandle fd = connect_to(s.port);
    ASSERT_TRUE(kl_handle_valid(fd));
    ASSERT_EQ(send_all(fd, REQ, sizeof REQ - 1), 0);
    int status = 0; char body[512];
    ASSERT_GE(read_response(fd, &status, body, sizeof body), 0);
    ASSERT_EQ(status, 200);
    ASSERT_STREQ(body, EXPECT);
    kl_test_closesock(fd);
    srv_stop(&s);
}

UTEST(http_split_request, every_single_split_point) {
    static Srv s; ASSERT_EQ(srv_start(&s, 0), 0);
    const size_t n = sizeof REQ - 1;
    int wrong = 0;
    for (size_t cut = 1; cut < n; cut++) {
        KlSocketHandle fd = connect_to(s.port);
        ASSERT_TRUE(kl_handle_valid(fd));
        ASSERT_EQ(send_in_pieces(fd, REQ, n, &cut, 1, 15), 0);
        int status = 0; char body[512];
        int r = read_response(fd, &status, body, sizeof body);
        if (r < 0 || status != 200 || strcmp(body, EXPECT) != 0) {
            wrong++;
            fprintf(stderr, "  split at %zu: %s\n", cut, r < 0 ? "no response" : body);
        }
        kl_test_closesock(fd);
    }
    ASSERT_EQ(wrong, 0);
    srv_stop(&s);
}

UTEST(http_split_request, byte_at_a_time) {
    static Srv s; ASSERT_EQ(srv_start(&s, 0), 0);
    const size_t n = sizeof REQ - 1;
    size_t cuts[sizeof REQ];
    for (size_t i = 0; i + 1 < n; i++) cuts[i] = i + 1;
    KlSocketHandle fd = connect_to(s.port);
    ASSERT_TRUE(kl_handle_valid(fd));
    ASSERT_EQ(send_in_pieces(fd, REQ, n, cuts, (int)(n - 1), 3), 0);
    int status = 0; char body[512];
    ASSERT_GE(read_response(fd, &status, body, sizeof body), 0);
    ASSERT_EQ(status, 200);
    ASSERT_STREQ(body, EXPECT);
    kl_test_closesock(fd);
    srv_stop(&s);
}

/* A header block larger than the 8 KiB base read buffer, split so that the parser has already seen
 * part of it when the buffer grows (and may move): the growth path must restart the parse on the
 * moved buffer rather than keep pointers into the old one. */
UTEST(http_split_request, header_block_larger_than_the_read_buffer) {
    static Srv s; ASSERT_EQ(srv_start(&s, 64 * 1024), 0);
    static char req[24 * 1024];
    static char big[20000];
    memset(big, 'q', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    int n = snprintf(req, sizeof req,
                     "GET /echo?k=v HTTP/1.1\r\nHost: localhost\r\nX-Probe: %s\r\n"
                     "User-Agent: keel-split-test\r\nConnection: close\r\n\r\n", big);
    ASSERT_GT(n, 8192);
    const size_t cutsets[][3] = { { 100, 9000, 17000 }, { 4000, 8192, 12000 }, { 8190, 8193, 16384 } };
    for (size_t k = 0; k < sizeof cutsets / sizeof cutsets[0]; k++) {
        KlSocketHandle fd = connect_to(s.port);
        ASSERT_TRUE(kl_handle_valid(fd));
        ASSERT_EQ(send_in_pieces(fd, req, (size_t)n, cutsets[k], 3, 20), 0);
        int status = 0; char body[512];
        ASSERT_GE(read_response(fd, &status, body, sizeof body), 0);
        ASSERT_EQ(status, 200);
        char want[64];
        snprintf(want, sizeof want, "/echo|k=v|%zu:qqqqqqqq|4", sizeof big - 1);
        ASSERT_STREQ(body, want);
        kl_test_closesock(fd);
    }
    srv_stop(&s);
}

/* The second request on a kept-alive connection, split: the per-request parse state must start
 * fresh after the first response. */
UTEST(http_split_request, split_request_after_keep_alive) {
    static Srv s; ASSERT_EQ(srv_start(&s, 0), 0);
    static const char KA[] =
        "GET /echo?k=v HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "X-Probe: abcdefgh\r\n"
        "User-Agent: keel-split-test\r\n"
        "Connection: keep-alive\r\n"
        "\r\n";
    const size_t n = sizeof KA - 1;
    KlSocketHandle fd = connect_to(s.port);
    ASSERT_TRUE(kl_handle_valid(fd));
    int status = 0; char body[512];
    size_t cut1 = 7;
    ASSERT_EQ(send_in_pieces(fd, KA, n, &cut1, 1, 15), 0);
    ASSERT_GE(read_response(fd, &status, body, sizeof body), 0);
    ASSERT_EQ(status, 200);
    ASSERT_STREQ(body, EXPECT);
    size_t cut2[2] = { 30, 60 };
    ASSERT_EQ(send_in_pieces(fd, KA, n, cut2, 2, 15), 0);
    ASSERT_GE(read_response(fd, &status, body, sizeof body), 0);
    ASSERT_EQ(status, 200);
    ASSERT_STREQ(body, EXPECT);
    kl_test_closesock(fd);
    srv_stop(&s);
}

UTEST_MAIN();
