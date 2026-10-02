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
#include "mock_tls.h"
#if !defined(_WIN32)
#include <netinet/tcp.h>
#include <signal.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* PROBE (not for merge): log every failed raw receive with its wall-clock time, so a CI run can tell
 * "the server closed" (0 / ECONNRESET, quickly) from "the server never answered" (EAGAIN after the
 * 3 s receive timeout). */
#if !defined(_WIN32)
#include <errno.h>
#include <time.h>
static long probe_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000L + t.tv_nsec / 1000000L; }
static int probe_recv(KlSocketHandle fd, void *b, int n, int line) {
    long t0 = probe_ms();
    int k = (int)recv(fd, b, (size_t)n, 0);
    if (k <= 0) {
        int e = errno;
        fprintf(stderr, "PROBE recv line %d: k=%d errno=%d (%s) after %ld ms\n", line, k, k < 0 ? e : 0,
                k < 0 ? strerror(e) : "eof", probe_ms() - t0);
    }
    return k;
}
#define recv(fd, b, n, f) probe_recv((fd), (b), (int)(n), __LINE__)
#endif

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

/* ── Over TLS: a record split across reads ──────────────────────────────────────────────────
 * A readable event can carry part of a TLS record; the engine buffers it and read() returns 0,
 * WANT_READ (the KlTls contract; -1 is error or close). The WebSocket server read that 0 as end of
 * stream and dropped the connection. The mock TLS (identity, no crypto) simulates the split in
 * socket mode, so this exercises the readiness transport; a completion loop takes its own TLS path. */
static KlTlsConfig g_tls_cfg = { .ctx = NULL, .factory = mock_tls_create };

static int srv_start_tls(Srv *s) {
    KlHttpServerConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.port = 0;
    cfg.bind_addr = "127.0.0.1";
    cfg.tls = &g_tls_cfg;
    if (kl_http_server_init(&s->srv, &cfg) != 0) return -1;
    kl_ws_server_config_init(&s->ws_cfg);
    s->ws_cfg.callbacks.on_message = srv_on_message;
    if (kl_http_server_ws_upgrade(&s->srv, "/ws", &s->ws_cfg) != 0) return -1;
    if (kl_plat_thread_create(&s->t, server_thread_fn, &s->srv) != 0) return -1;
    for (int i = 0; i < 300 && s->srv.bound_port == 0; i++) kl_test_sleep_ms(10);
    s->port = s->srv.bound_port;
    return s->port > 0 ? 0 : -1;
}

UTEST(ws_split_frames, tls_record_split_across_reads_is_not_eof) {
    static Srv s; ASSERT_EQ(srv_start_tls(&s), 0);
    KlSocketHandle fd = raw_connect(s.port);
    int ok = kl_handle_valid(fd);

    static char req[512];
    int rn = snprintf(req, sizeof req,
                      "GET /ws HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nUpgrade: websocket\r\n"
                      "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                      "Sec-WebSocket-Version: 13\r\n\r\n", s.port);
    static unsigned char frame[64];
    static const unsigned char msg[] = "hello over a split record";
    size_t fl = build_masked(frame, 0x1, msg, sizeof msg - 1);
    mock_tls_split_after = (size_t)rn;         /* the upgrade request passes whole */
    mock_tls_split_record = fl;                /* the frame's record arrives in two parts */

    if (ok) ok = send_all(fd, req, (size_t)rn) == 0;
    static char resp[1024]; size_t rl = 0;
    while (ok && rl < sizeof resp - 1) {       /* read the 101 up to the blank line */
        int k = (int)recv(fd, resp + rl, 1, 0);
        if (k <= 0) { ok = 0; break; }
        rl++;
        resp[rl] = '\0';
        if (rl >= 4 && memcmp(resp + rl - 4, "\r\n\r\n", 4) == 0) break;
    }
    if (ok) ok = strncmp(resp, "HTTP/1.1 101", 12) == 0;
    if (ok) ok = send_all(fd, frame, 5) == 0;
    kl_test_sleep_ms(150);                     /* the server sees part of the record on its own */
    if (ok) ok = send_all(fd, frame + 5, fl - 5) == 0;
    int opcode = -1; static unsigned char echo[64]; size_t got = 0;
    int echoed = ok && read_frame(fd, &opcode, echo, sizeof echo, &got) == 0;

    mock_tls_split_record = 0;
    mock_tls_split_after = 0;
    if (kl_handle_valid(fd)) kl_test_closesock(fd);
    srv_stop(&s);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(echoed);
    ASSERT_EQ(opcode, 0x1);
    ASSERT_EQ(got, sizeof msg - 1);
    ASSERT_EQ(memcmp(echo, msg, got), 0);
}

/* A record larger than the server's 8 KiB read buffer: after the first read, the rest of the
 * plaintext sits inside the TLS engine and the socket will not signal readable again for it. The
 * server must drain tls->pending() itself, or the message waits for the peer's next send. */
UTEST(ws_split_frames, tls_record_larger_than_the_read_buffer_is_drained) {
    static Srv s; ASSERT_EQ(srv_start_tls(&s), 0);
    KlSocketHandle fd = raw_connect(s.port);
    int ok = kl_handle_valid(fd);

    static char req[512];
    int rn = snprintf(req, sizeof req,
                      "GET /ws HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nUpgrade: websocket\r\n"
                      "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                      "Sec-WebSocket-Version: 13\r\n\r\n", s.port);
    enum { N = 12000 };
    static unsigned char msg[N], frame[N + 16], echo[N];
    for (size_t i = 0; i < N; i++) msg[i] = pat(i);
    size_t fl = build_masked(frame, 0x2, msg, N);
    mock_tls_split_after = (size_t)rn;
    mock_tls_split_record = fl;                /* one record carrying the whole frame */

    if (ok) ok = send_all(fd, req, (size_t)rn) == 0;
    static char resp[1024]; size_t rl = 0;
    while (ok && rl < sizeof resp - 1) {
        int k = (int)recv(fd, resp + rl, 1, 0);
        if (k <= 0) { ok = 0; break; }
        rl++;
        resp[rl] = '\0';
        if (rl >= 4 && memcmp(resp + rl - 4, "\r\n\r\n", 4) == 0) break;
    }
    if (ok) ok = strncmp(resp, "HTTP/1.1 101", 12) == 0;
    if (ok) ok = send_all(fd, frame, 100) == 0;
    kl_test_sleep_ms(150);
    if (ok) ok = send_all(fd, frame + 100, fl - 100) == 0;   /* the last bytes the peer sends */
    int opcode = -1; size_t got = 0;
    int echoed = ok && read_frame(fd, &opcode, echo, sizeof echo, &got) == 0;

    mock_tls_split_record = 0;
    mock_tls_split_after = 0;
    if (kl_handle_valid(fd)) kl_test_closesock(fd);
    srv_stop(&s);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(echoed);
    ASSERT_EQ(opcode, 0x2);
    ASSERT_EQ(got, (size_t)N);
    ASSERT_EQ(memcmp(echo, msg, got), 0);
}

/* ── Close frames the server receives (RFC 6455 5.5.1, 7.4) ─────────────────────────────────
 * A close frame's payload is empty, or a valid status code optionally followed by a UTF-8
 * reason. The server echoed whatever arrived: an empty close was answered with status 1005 on
 * the wire (a code that must never be sent), and invalid codes or reasons were accepted instead of
 * failing the connection with 1002 / 1007, as the client does. */

static KlSocketHandle ws_open_raw(int port) {
    KlSocketHandle fd = raw_connect(port);
    if (!kl_handle_valid(fd)) return KL_INVALID_SOCKET;
    char req[512];
    int rn = snprintf(req, sizeof req,
                      "GET /ws HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nUpgrade: websocket\r\n"
                      "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                      "Sec-WebSocket-Version: 13\r\n\r\n", port);
    char resp[1024]; size_t rl = 0;
    if (send_all(fd, req, (size_t)rn) != 0) goto fail;
    while (rl < sizeof resp - 1) {
        int k = (int)recv(fd, resp + rl, 1, 0);
        if (k <= 0) goto fail;
        rl++;
        resp[rl] = '\0';
        if (rl >= 4 && memcmp(resp + rl - 4, "\r\n\r\n", 4) == 0) break;
    }
    if (strncmp(resp, "HTTP/1.1 101", 12) == 0) return fd;
fail:
    kl_test_closesock(fd);
    return KL_INVALID_SOCKET;
}

/* Send a CLOSE carrying `pl` and read the server's reply frame. Returns its payload length, or -1. */
static int close_round_trip(int port, const unsigned char *pl, size_t pl_len,
                            int *opcode, unsigned char *reply, size_t cap) {
    KlSocketHandle fd = ws_open_raw(port);
    if (!kl_handle_valid(fd)) return -1;
    unsigned char frame[160];
    size_t fl = build_masked(frame, 0x8, pl, pl_len);
    size_t got = 0;
    int r = -1;
    if (send_all(fd, frame, fl) == 0 && read_frame(fd, opcode, reply, cap, &got) == 0) r = (int)got;
    kl_test_closesock(fd);
    return r;
}

static unsigned close_code_of(const unsigned char *p, int n) {
    return n >= 2 ? ((unsigned)p[0] << 8) | p[1] : 0;
}

UTEST(ws_server_close_recv, empty_close_is_echoed_empty) {
    static Srv s; ASSERT_EQ(srv_start(&s), 0);
    int op = -1; unsigned char reply[128];
    int n = close_round_trip(s.port, NULL, 0, &op, reply, sizeof reply);
    srv_stop(&s);
    ASSERT_EQ(op, 0x8);
    ASSERT_EQ(n, 0);                            /* was: 2 bytes, status 1005 on the wire */
}

UTEST(ws_server_close_recv, invalid_close_payloads_fail_the_connection) {
    static Srv s; ASSERT_EQ(srv_start(&s), 0);
    static const unsigned char one_byte[]  = { 0x03 };
    static const unsigned char code_999[]  = { 0x03, 0xE7 };              /* below 1000 */
    static const unsigned char code_1005[] = { 0x03, 0xED };              /* reserved, never sent */
    static const unsigned char bad_utf8[]  = { 0x03, 0xE8, 0xC3, 0x28 };  /* 1000 + invalid UTF-8 */
    static const unsigned char normal[]    = { 0x03, 0xE8, 'o', 'k' };    /* 1000 "ok" */
    int op[5]; unsigned char rep[5][128]; int n[5];
    n[0] = close_round_trip(s.port, one_byte,  sizeof one_byte,  &op[0], rep[0], sizeof rep[0]);
    n[1] = close_round_trip(s.port, code_999,  sizeof code_999,  &op[1], rep[1], sizeof rep[1]);
    n[2] = close_round_trip(s.port, code_1005, sizeof code_1005, &op[2], rep[2], sizeof rep[2]);
    n[3] = close_round_trip(s.port, bad_utf8,  sizeof bad_utf8,  &op[3], rep[3], sizeof rep[3]);
    n[4] = close_round_trip(s.port, normal,    sizeof normal,    &op[4], rep[4], sizeof rep[4]);
    srv_stop(&s);
    ASSERT_EQ(close_code_of(rep[0], n[0]), 1002u);
    ASSERT_EQ(close_code_of(rep[1], n[1]), 1002u);
    ASSERT_EQ(close_code_of(rep[2], n[2]), 1002u);
    ASSERT_EQ(close_code_of(rep[3], n[3]), 1007u);
    ASSERT_EQ(close_code_of(rep[4], n[4]), 1000u);   /* a valid close is still echoed */
}

/* A route with no on_message (send-only) must still finish each message it receives: the second
 * message used to hit "previous message not finished" and close the connection with 1002. */
static int srv_start_no_on_message(Srv *s) {
    KlHttpServerConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.port = 0;
    cfg.bind_addr = "127.0.0.1";
    if (kl_http_server_init(&s->srv, &cfg) != 0) return -1;
    kl_ws_server_config_init(&s->ws_cfg);           /* no callbacks at all */
    if (kl_http_server_ws_upgrade(&s->srv, "/ws", &s->ws_cfg) != 0) return -1;
    if (kl_plat_thread_create(&s->t, server_thread_fn, &s->srv) != 0) return -1;
    for (int i = 0; i < 300 && s->srv.bound_port == 0; i++) kl_test_sleep_ms(10);
    s->port = s->srv.bound_port;
    return s->port > 0 ? 0 : -1;
}

UTEST(ws_server_close_recv, messages_without_on_message_are_finished) {
    static Srv s; ASSERT_EQ(srv_start_no_on_message(&s), 0);
    KlSocketHandle fd = ws_open_raw(s.port);
    int ok = kl_handle_valid(fd);
    static const unsigned char m1[] = "one", m2[] = "two";
    static const unsigned char bye[] = { 0x03, 0xE8 };
    unsigned char f[64]; size_t fl;
    fl = build_masked(f, 0x1, m1, 3); if (ok) ok = send_all(fd, f, fl) == 0;
    fl = build_masked(f, 0x1, m2, 3); if (ok) ok = send_all(fd, f, fl) == 0;
    fl = build_masked(f, 0x8, bye, 2); if (ok) ok = send_all(fd, f, fl) == 0;
    int op = -1; unsigned char reply[128]; size_t got = 0;
    if (ok) ok = read_frame(fd, &op, reply, sizeof reply, &got) == 0;
    if (kl_handle_valid(fd)) kl_test_closesock(fd);
    srv_stop(&s);
    ASSERT_TRUE(ok);
    ASSERT_EQ(op, 0x8);
    ASSERT_EQ(close_code_of(reply, (int)got), 1000u);   /* our close echoed, not a 1002 */
}

/* ── Fragmentation edge cases at the server (RFC 6455 5.4) ─────────────────────────────────── */
static KlSocketHandle frag_open(int port) {
    KlSocketHandle fd = raw_connect(port);
    if (!kl_handle_valid(fd)) return KL_INVALID_SOCKET;
    char req[512];
    int rn = snprintf(req, sizeof req,
                      "GET /ws HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nUpgrade: websocket\r\n"
                      "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                      "Sec-WebSocket-Version: 13\r\n\r\n", port);
    char resp[1024]; size_t rl = 0;
    if (send_all(fd, req, (size_t)rn) == 0) {
        while (rl < sizeof resp - 1) {
            int k = (int)recv(fd, resp + rl, 1, 0);
            if (k <= 0) break;
            rl++;
            resp[rl] = '\0';
            if (rl >= 4 && memcmp(resp + rl - 4, "\r\n\r\n", 4) == 0) break;
        }
        if (strncmp(resp, "HTTP/1.1 101", 12) == 0) return fd;
    }
    kl_test_closesock(fd);
    return KL_INVALID_SOCKET;
}

/* A masked client frame with an explicit FIN bit. */
static size_t frag_masked(unsigned char *out, int fin, int opcode, const void *pl, size_t len) {
    static const unsigned char key[4] = { 0x11, 0x22, 0x33, 0x44 };
    out[0] = (unsigned char)((fin ? 0x80 : 0) | opcode);
    out[1] = (unsigned char)(0x80 | len);
    memcpy(out + 2, key, 4);
    for (size_t i = 0; i < len; i++) out[6 + i] = ((const unsigned char *)pl)[i] ^ key[i & 3];
    return 6 + len;
}

/* Send frames, then read the server's reply frame. */
static int frag_case(const unsigned char *frames, size_t n, int *opcode, unsigned char *pl, size_t *pl_len) {
    static Srv s;
    if (srv_start(&s) != 0) return -1;
    KlSocketHandle fd = frag_open(s.port);
    int r = -1;
    if (kl_handle_valid(fd) && send_all(fd, frames, n) == 0)
        r = read_frame(fd, opcode, pl, 125, pl_len);
    if (kl_handle_valid(fd)) kl_test_closesock(fd);
    srv_stop(&s);
    return r;
}

/* An empty first fragment starts a message: it was not recorded, so the continuation was refused. */
UTEST(ws_server_frag, empty_first_fragment_starts_the_message) {
    unsigned char f[64]; size_t n = frag_masked(f, 0, 0x1, NULL, 0);
    n += frag_masked(f + n, 1, 0x0, "hi", 2);
    int op = -1; unsigned char pl[125]; size_t len = 0;
    ASSERT_EQ(frag_case(f, n, &op, pl, &len), 0);
    ASSERT_EQ(op, 0x1);                          /* was: a 1002 close */
    ASSERT_EQ(len, (size_t)2);
    ASSERT_EQ(memcmp(pl, "hi", 2), 0);
}

/* A final continuation with no message open is a protocol error (it was delivered as a message). */
UTEST(ws_server_frag, continuation_without_a_message_fails_with_1002) {
    unsigned char f[64]; size_t n = frag_masked(f, 1, 0x0, NULL, 0);
    int op = -1; unsigned char pl[125]; size_t len = 0;
    ASSERT_EQ(frag_case(f, n, &op, pl, &len), 0);
    ASSERT_EQ(op, 0x8);
    ASSERT_TRUE(len >= 2 && ((pl[0] << 8) | pl[1]) == 1002);
}

/* An empty new message while a fragmented one is open is a protocol error (it replaced it). */
UTEST(ws_server_frag, empty_message_inside_a_fragmented_one_fails_with_1002) {
    unsigned char f[64]; size_t n = frag_masked(f, 0, 0x1, "ab", 2);
    n += frag_masked(f + n, 1, 0x1, NULL, 0);
    int op = -1; unsigned char pl[125]; size_t len = 0;
    ASSERT_EQ(frag_case(f, n, &op, pl, &len), 0);
    ASSERT_EQ(op, 0x8);
    ASSERT_TRUE(len >= 2 && ((pl[0] << 8) | pl[1]) == 1002);
}

/* ── A frame the socket would not take whole (no drain) ───────────────────────────────────────
 * Without kl_ws_server_enable_drain, a send writes the frame directly; when the socket buffer
 * fills partway (the peer is not reading), the write gives up after part of the frame went out and
 * the connection stayed open. A later send, once the peer has read and the socket has room again,
 * then started a frame header in the middle of the old payload: the stream was desynced. Once a
 * frame was cut short, no further frame may be sent. */
static int g_sends_done, g_next_done;
static void big_on_message(KlWsServerConn *ws, const char *data, size_t len, int is_binary, void *ud) {
    (void)data; (void)len; (void)is_binary; (void)ud;
    if (!g_sends_done) {                            /* "go": fill the socket until a frame is cut */
        static char big[1024 * 1024];
        memset(big, 'B', sizeof big);
        for (int i = 0; i < 16; i++)
            if (kl_ws_server_send_binary(ws, big, sizeof big) < 0) break;
        g_sends_done = 1;
        return;
    }
    for (int i = 0; i < 3; i++)                     /* "more", after the peer has read: none may */
        (void)kl_ws_server_send_text(ws, "next", 4);    /* follow the cut frame */
    g_next_done = 1;
}

/* Parse what the peer received: every frame header must be a valid unmasked server frame (FIN,
 * no RSV, binary of 1 MiB or the text "next"). A cut-short last frame followed by EOF is fine; a
 * header found inside a payload is the desync. Returns 1 if the stream is well formed. */
static int stream_well_formed(const unsigned char *b, size_t n) {
    size_t p = 0;
    while (p < n) {
        if (n - p < 2) return 1;                    /* cut inside a header, then EOF */
        unsigned op = b[p] & 0x0F, len7 = b[p + 1] & 0x7F;
        if ((b[p] & 0xF0) != 0x80 || (b[p + 1] & 0x80)) return 0;
        size_t hl = 2, plen = len7;
        if (len7 == 126) { if (n - p < 4) return 1; plen = ((size_t)b[p + 2] << 8) | b[p + 3]; hl = 4; }
        else if (len7 == 127) {
            if (n - p < 10) return 1;
            plen = 0;
            for (int k = 2; k < 10; k++) plen = (plen << 8) | b[p + k];
            hl = 10;
        }
        if (op == 0x2 && plen != 1024 * 1024) return 0;
        if (op == 0x1 && (plen != 4 || (n - p >= hl + 4 && memcmp(b + p + hl, "next", 4) != 0)))
            return 0;
        if (op != 0x1 && op != 0x2) return 0;
        /* Every byte of a binary payload is 'B', up to the end of what arrived. A frame sent after
         * a cut one lands INSIDE the cut frame's declared payload, and the stream may end before
         * that payload would: so check each byte, not a sample, and check a cut payload too. */
        if (op == 0x2) {
            size_t have = (n - p - hl < plen) ? n - p - hl : plen;
            for (size_t k = 0; k < have; k++)
                if (b[p + hl + k] != 'B') return 0;
        }
        if (n - p < hl + plen) return 1;            /* cut inside a payload, then EOF */
        p += hl + plen;
    }
    return 1;
}

/* The checker itself must be able to fail: a frame inside a cut payload (the desync) is rejected,
 * and a clean stream, including one cut inside its last payload, is accepted. */
UTEST(ws_server_send, stream_checker_sees_a_frame_inside_a_payload) {
    static unsigned char s[4096];
    size_t n = 0;
    s[n++] = 0x82; s[n++] = 127;                    /* binary, 64-bit length = 1 MiB */
    for (int k = 0; k < 8; k++) s[n++] = (unsigned char)(k == 5 ? 0x10 : 0);
    memset(s + n, 'B', 1000); n += 1000;            /* the cut: 1000 of 1 MiB sent */
    size_t clean = n;
    s[n++] = 0x81; s[n++] = 4; memcpy(s + n, "next", 4); n += 4;   /* then a frame: the desync */
    memset(s + n, 'B', 500); n += 500;
    ASSERT_EQ(stream_well_formed(s, clean), 1);
    ASSERT_EQ(stream_well_formed(s, n), 0);
}

UTEST(ws_server_send, a_cut_short_frame_is_never_followed_by_another) {
    static Srv s;
    KlHttpServerConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.port = 0;
    cfg.bind_addr = "127.0.0.1";
    ASSERT_EQ(kl_http_server_init(&s.srv, &cfg), 0);
    kl_ws_server_config_init(&s.ws_cfg);
    s.ws_cfg.callbacks.on_message = big_on_message;
    ASSERT_EQ(kl_http_server_ws_upgrade(&s.srv, "/ws", &s.ws_cfg), 0);
    ASSERT_EQ(kl_plat_thread_create(&s.t, server_thread_fn, &s.srv), 0);
    for (int i = 0; i < 300 && s.srv.bound_port == 0; i++) kl_test_sleep_ms(10);
    s.port = s.srv.bound_port;

#if !defined(_WIN32)
    signal(SIGPIPE, SIG_IGN);                       /* the fixed server may close before "more" */
#endif
    g_sends_done = 0; g_next_done = 0;
    static unsigned char rx[20 * 1024 * 1024];
    size_t got = 0;
    KlSocketHandle fd = ws_open_raw(s.port);
    if (kl_handle_valid(fd)) {
        unsigned char f[32];
        size_t fl = build_masked(f, 0x1, (const unsigned char *)"go", 2);
        (void)send_all(fd, f, fl);                  /* then read nothing while the server sends */
        for (int i = 0; i < 100 && !g_sends_done; i++) kl_test_sleep_ms(20);
        for (int round = 0; round < 2; round++) {
            for (;;) {                              /* read what has been sent, to a lull */
                if (kl_test_poll1(fd, 0, round == 0 ? 300 : 1000) <= 0) break;
                long r = kl_test_sockread(fd, rx + got, sizeof rx - got);
                if (r <= 0) break;
                got += (size_t)r;
                if (got == sizeof rx) break;
            }
            if (round == 0) {                       /* the socket has room again: ask for more */
                fl = build_masked(f, 0x1, (const unsigned char *)"more", 4);
                (void)send_all(fd, f, fl);
                for (int i = 0; i < 100 && !g_next_done; i++) kl_test_sleep_ms(20);
            }
        }
        kl_test_closesock(fd);
    }
    srv_stop(&s);
    ASSERT_TRUE(g_sends_done);
    ASSERT_TRUE(got > 0);
    ASSERT_TRUE(stream_well_formed(rx, got));       /* was: a frame header inside a payload */
}

UTEST_MAIN();
