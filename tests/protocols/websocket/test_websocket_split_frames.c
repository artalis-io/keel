/*
 * test_websocket_split_frames.c: a WebSocket frame whose payload arrives over several reads is
 * reassembled whole, by the server and by the client.
 *
 * Both sides decided "is this a new message?" from the frame's opcode alone, so the second chunk of
 * the same TEXT frame looked like a new message: the server closed with 1002 (protocol error) and the
 * client kept only the tail. A control frame's payload split across reads was acted on from its last
 * chunk only. Loopback tests with small messages never split a frame.
 *
 * Cases, over real loopback TCP on whichever engine the build selects:
 *   - a large message round-trips through a real KlHttpServer WebSocket route and a real
 *     KlWsClientConn (large enough that TCP delivers it in several reads on both sides);
 *   - a raw client sends one masked TEXT frame in pieces with pauses, then a PING whose payload is
 *     split across two writes, and checks the echo and the PONG payload.
 */
#include "utest.h"
#include <keel/keel.h>
#include <keel/websocket_server.h>
#include <keel/websocket_client.h>
#include "net_compat.h"
#include "platform_thread.h"
#if !defined(_WIN32)
#include <netinet/tcp.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Echo server ────────────────────────────────────────────────────────────────────────────── */

static void srv_on_message(KlWsServerConn *ws, const char *data, size_t len, int is_binary, void *ud) {
    (void)ud;
    if (is_binary) kl_ws_server_send_binary(ws, data, len);
    else           kl_ws_server_send_text(ws, data, len);
}

static void server_thread_fn(void *arg) { kl_http_server_run((KlHttpServer *)arg); }

typedef struct { KlHttpServer srv; KlWsServerConfig ws_cfg; KlPlatThread t; int port; } Srv;

/* Each test's Srv is static: a failed ASSERT returns from the test without srv_stop, and a server
 * thread still running on a stack frame the next test reuses crashes the process, turning a clean
 * failure report into a segfault. */
static int srv_start(Srv *s) {
    KlHttpServerConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.port = 0;
    cfg.bind_addr = "127.0.0.1";
    if (kl_http_server_init(&s->srv, &cfg) != 0) return -1;
    kl_ws_server_config_init(&s->ws_cfg);
    s->ws_cfg.callbacks.on_message = srv_on_message;
    if (kl_http_server_ws_upgrade(&s->srv, "/ws", &s->ws_cfg) != 0) return -1;
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

static unsigned char pat(size_t i) { return (unsigned char)('a' + (i * 7 + (i >> 5)) % 26); }

/* ── Real client, large message ─────────────────────────────────────────────────────────────── */

typedef struct { int open, closed, errors; size_t got_len; char *got; } Cli;

static void cli_on_open(KlWsClientConn *ws, void *ud) { (void)ws; ((Cli *)ud)->open = 1; }
static void cli_on_message(KlWsClientConn *ws, const char *data, size_t len, int is_binary, void *ud) {
    (void)ws; (void)is_binary;
    Cli *c = ud;
    free(c->got);
    c->got = malloc(len ? len : 1);
    memcpy(c->got, data, len);
    c->got_len = len;
}
static void cli_on_close(KlWsClientConn *ws, uint16_t code, const char *r, size_t rl, void *ud) {
    (void)ws; (void)code; (void)r; (void)rl;
    ((Cli *)ud)->closed = 1;
}
static void cli_on_error(KlWsClientConn *ws, const char *msg, void *ud) {
    (void)ws;
    fprintf(stderr, "  ws client error: %s\n", msg);
    ((Cli *)ud)->errors++;
}

UTEST(ws_split_frames, large_message_round_trips_whole) {
    static Srv s; ASSERT_EQ(srv_start(&s), 0);
    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);
    char url[128];
    snprintf(url, sizeof url, "ws://127.0.0.1:%d/ws", s.port);
    Cli c; memset(&c, 0, sizeof c);
    KlWsClientCallbacks cbs = { .on_open = cli_on_open, .on_message = cli_on_message,
                                .on_close = cli_on_close, .on_error = cli_on_error };
    KlWsClientConn *ws = kl_ws_client_connect(&ev, &a, NULL, url, &cbs, &c);
    ASSERT_TRUE(ws != NULL);
    for (int i = 0; i < 300 && !c.open && !c.errors; i++) kl_event_ctx_run(&ev, 16, 10);
    ASSERT_TRUE(c.open);

    const size_t n = 60000;                     /* one frame, many TCP reads on both sides */
    char *msg = malloc(n);
    for (size_t i = 0; i < n; i++) msg[i] = (char)pat(i);
    ASSERT_EQ(kl_ws_client_send_text(ws, msg, n), 0);
    for (int i = 0; i < 500 && c.got_len == 0 && !c.closed && !c.errors; i++) kl_event_ctx_run(&ev, 16, 10);

    ASSERT_EQ(c.errors, 0);
    ASSERT_EQ(c.closed, 0);                     /* the server did not close with 1002 */
    ASSERT_EQ(c.got_len, n);                    /* the client kept the whole echo, not the tail */
    ASSERT_EQ(memcmp(c.got, msg, n), 0);

    free(msg);
    free(c.got);
    kl_ws_client_free(ws);
    kl_event_ctx_free(&ev);
    srv_stop(&s);
}

/* ── Raw client: one frame written in pieces, and a split PING ──────────────────────────────── */

static KlSocketHandle raw_connect(int port) {
    KlSocketHandle fd = (KlSocketHandle)socket(AF_INET, SOCK_STREAM, 0);
    if (!kl_handle_valid(fd)) return KL_INVALID_SOCKET;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) { kl_test_closesock(fd); return KL_INVALID_SOCKET; }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    kl_test_set_rcvtimeo(fd, 3000);
    return fd;
}
static int send_all(KlSocketHandle fd, const void *p, size_t n) {
    const char *b = p;
    while (n > 0) {
        int k = (int)send(fd, b, (int)n, 0);
        if (k <= 0) return -1;
        b += k; n -= (size_t)k;
    }
    return 0;
}
static int recv_exact(KlSocketHandle fd, void *p, size_t n) {
    char *b = p;
    while (n > 0) {
        int k = (int)recv(fd, b, (int)n, 0);
        if (k <= 0) return -1;
        b += k; n -= (size_t)k;
    }
    return 0;
}

/* Build a masked client frame (FIN set) into out; returns its length. */
static size_t build_masked(unsigned char *out, int opcode, const unsigned char *payload, size_t len) {
    static const unsigned char key[4] = { 0x11, 0x22, 0x33, 0x44 };
    size_t h = 0;
    out[h++] = (unsigned char)(0x80 | opcode);
    if (len < 126) {
        out[h++] = (unsigned char)(0x80 | len);
    } else {
        out[h++] = 0x80 | 126;
        out[h++] = (unsigned char)(len >> 8);
        out[h++] = (unsigned char)len;
    }
    memcpy(out + h, key, 4); h += 4;
    for (size_t i = 0; i < len; i++) out[h + i] = payload[i] ^ key[i & 3];
    return h + len;
}

/* Read one unmasked server frame header + payload (payload < 65536). */
static int read_frame(KlSocketHandle fd, int *opcode, unsigned char *payload, size_t cap, size_t *len) {
    unsigned char hdr[4];
    if (recv_exact(fd, hdr, 2) != 0) return -1;
    *opcode = hdr[0] & 0x0F;
    size_t l = hdr[1] & 0x7F;
    if (l == 126) {
        if (recv_exact(fd, hdr + 2, 2) != 0) return -1;
        l = ((size_t)hdr[2] << 8) | hdr[3];
    } else if (l == 127) {
        return -1;
    }
    if (l > cap) return -1;
    if (l && recv_exact(fd, payload, l) != 0) return -1;
    *len = l;
    return 0;
}

UTEST(ws_split_frames, raw_frame_in_pieces_and_split_ping) {
    static Srv s; ASSERT_EQ(srv_start(&s), 0);
    KlSocketHandle fd = raw_connect(s.port);
    ASSERT_TRUE(kl_handle_valid(fd));

    char req[512];
    int rn = snprintf(req, sizeof req,
                      "GET /ws HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nUpgrade: websocket\r\n"
                      "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                      "Sec-WebSocket-Version: 13\r\n\r\n", s.port);
    ASSERT_EQ(send_all(fd, req, (size_t)rn), 0);
    char resp[1024]; size_t rl = 0;
    while (rl < sizeof resp - 1) {                /* read the 101 up to the blank line */
        int k = (int)recv(fd, resp + rl, 1, 0);
        ASSERT_GT(k, 0);
        rl++;
        resp[rl] = '\0';
        if (rl >= 4 && memcmp(resp + rl - 4, "\r\n\r\n", 4) == 0) break;
    }
    ASSERT_TRUE(strncmp(resp, "HTTP/1.1 101", 12) == 0);

    /* One 3000-byte TEXT frame, written as header, a few payload pieces, rest: each its own read. */
    static unsigned char payload[3000], frame[3100];
    for (size_t i = 0; i < sizeof payload; i++) payload[i] = pat(i);
    size_t fl = build_masked(frame, 0x1, payload, sizeof payload);
    const size_t cuts[] = { 3, 9, 700, 1500, 2999 };
    size_t at = 0;
    for (size_t i = 0; i <= sizeof cuts / sizeof cuts[0]; i++) {
        size_t end = i < sizeof cuts / sizeof cuts[0] ? cuts[i] : fl;
        ASSERT_EQ(send_all(fd, frame + at, end - at), 0);
        at = end;
        kl_test_sleep_ms(15);
    }
    int op = 0; size_t got = 0;
    static unsigned char echo[4000];
    ASSERT_EQ(read_frame(fd, &op, echo, sizeof echo, &got), 0);
    ASSERT_EQ(op, 0x1);                           /* a TEXT echo, not a 1002 close */
    ASSERT_EQ(got, sizeof payload);
    ASSERT_EQ(memcmp(echo, payload, got), 0);

    /* A PING with a 100-byte payload, its payload split across two writes: the PONG must carry all
     * 100 bytes, not the last chunk. */
    unsigned char ping_pl[100];
    for (size_t i = 0; i < sizeof ping_pl; i++) ping_pl[i] = (unsigned char)('A' + i % 26);
    unsigned char pf[128];
    size_t pl = build_masked(pf, 0x9, ping_pl, sizeof ping_pl);
    ASSERT_EQ(send_all(fd, pf, 6 + 40), 0);       /* header (2 + mask 4) + first 40 bytes */
    kl_test_sleep_ms(20);
    ASSERT_EQ(send_all(fd, pf + 46, pl - 46), 0);
    ASSERT_EQ(read_frame(fd, &op, echo, sizeof echo, &got), 0);
    ASSERT_EQ(op, 0xA);                           /* PONG */
    ASSERT_EQ(got, sizeof ping_pl);
    ASSERT_EQ(memcmp(echo, ping_pl, got), 0);

    kl_test_closesock(fd);
    srv_stop(&s);
}

UTEST_MAIN();
