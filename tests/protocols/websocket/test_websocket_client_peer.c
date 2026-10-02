/*
 * test_websocket_client_peer.c: the WebSocket client against a raw peer that sends exactly the bytes
 * each case needs (audit L12).
 *
 * Freeing the connection from a callback. kl_ws_client_free from on_close, on_message or on_open is
 * the natural pattern; on_close used to run BEFORE the connection was closed, and the frame loop kept
 * using the connection after on_message, so the free was a use-after-free. The client's allocator
 * here POISONS and QUARANTINES every freed block (as in test_http_client_free_in_done): a later read
 * sees garbage and a later write is caught by the final check, with or without a sanitizer.
 *
 * Protocol violations from the server (RFC 6455): a masked frame (5.1) and an invalid close frame
 * (7.4: a code that may not appear on the wire, or a reason that is not UTF-8) must fail the
 * connection with a close frame carrying 1002 / 1007, not be accepted. An upgrade response that never
 * ends its headers must not grow the client's buffer without bound.
 *
 * The peer's state is static: a failed ASSERT returns without joining its thread, and a thread left
 * running on a reused stack frame would turn a clean failure into a crash.
 */
#include "utest.h"
#include <keel/keel.h>
#include <keel/websocket.h>
#include <keel/websocket_client.h>
#include "net_compat.h"
#include "platform_thread.h"
#include "platform_socket.h"   /* kl_plat_socket_runtime_init */
#include "sha1.h"
#include "base64.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Poisoning, quarantining allocator ──────────────────────────────────────────────────────── */

#define QMAX 4096
static struct { unsigned char *p; size_t n; } g_q[QMAX];
static int g_nq;

static void *q_malloc(void *c, size_t n) { (void)c; return malloc(n ? n : 1); }
static void *q_realloc(void *c, void *p, size_t o, size_t n) { (void)c; (void)o; return realloc(p, n ? n : 1); }
static void q_free(void *c, void *p, size_t n) {
    (void)c;
    if (!p) return;
    memset(p, 0xDD, n);
    if (g_nq < QMAX) { g_q[g_nq].p = p; g_q[g_nq].n = n; g_nq++; }
    else free(p);
}
static KlAllocator g_qa = { q_malloc, q_realloc, q_free, NULL };

static int quarantine_check_and_release(void) {
    int written = 0;
    for (int i = 0; i < g_nq; i++) {
        for (size_t k = 0; k < g_q[i].n; k++)
            if (g_q[i].p[k] != 0xDD) { written++; break; }
        free(g_q[i].p);
    }
    g_nq = 0;
    return written;
}

/* ── Raw peer ───────────────────────────────────────────────────────────────────────────────── */

typedef struct {
    KlSocketHandle listen_fd;
    int            port;
    const unsigned char *after;     /* bytes sent right after the 101, in the same write */
    size_t         after_len;
    int            raw_reply;       /* send `after` INSTEAD of a 101 (handshake cases) */
    const char    *upgrade_value;   /* the 101's Upgrade value (NULL = "websocket") */
    const char    *extra_headers;   /* extra 101 header lines, each ending in \r\n (NULL = none) */
    unsigned char  got[256];        /* what the client sent after the upgrade request */
    size_t         got_len;
    KlPlatThread   tid;
} Peer;

static int peer_listen(Peer *p) {
    memset(p, 0, sizeof(*p));
    if (kl_plat_socket_runtime_init() != 0) return -1;
    KlSocketHandle fd = (KlSocketHandle)socket(AF_INET, SOCK_STREAM, 0);
    if (!kl_handle_valid(fd)) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t al = sizeof(a);
    if (bind((int)fd, (struct sockaddr *)&a, sizeof(a)) != 0 || listen((int)fd, 4) != 0 ||
        getsockname((int)fd, (struct sockaddr *)&a, &al) != 0) {
        kl_test_closesock(fd);
        return -1;
    }
    p->listen_fd = fd;
    p->port = ntohs(a.sin_port);
    return 0;
}

static void accept_value(const char *req, char out[64]) {
    out[0] = '\0';
    const char *k = strstr(req, "Sec-WebSocket-Key: ");
    if (!k) return;
    k += strlen("Sec-WebSocket-Key: ");
    const char *e = strstr(k, "\r\n");
    if (!e || e - k > 64) return;
    char cat[128];
    size_t kl = (size_t)(e - k);
    memcpy(cat, k, kl);
    memcpy(cat + kl, KL_WS_MAGIC_GUID, strlen(KL_WS_MAGIC_GUID));
    uint8_t dig[20];
    kl_sha1(cat, kl + strlen(KL_WS_MAGIC_GUID), dig);
    size_t ol = 0;
    kl_base64_encode(dig, sizeof dig, out, &ol);
    out[ol] = '\0';
}

static void peer_thread(void *arg) {
    Peer *p = arg;
    if (kl_test_poll1(p->listen_fd, 0, 5000) <= 0) return;
    KlSocketHandle c = (KlSocketHandle)accept((int)p->listen_fd, NULL, NULL);
    if (!kl_handle_valid(c)) return;
    char req[2048];
    size_t got = 0;
    while (got < sizeof(req) - 1 && kl_test_poll1(c, 0, 3000) > 0) {
        long n = kl_test_sockread(c, req + got, sizeof(req) - 1 - got);
        if (n <= 0) break;
        got += (size_t)n;
        req[got] = '\0';
        if (strstr(req, "\r\n\r\n")) break;
    }
    static unsigned char out[32 * 1024];
    size_t ol = 0;
    if (!p->raw_reply) {
        char acc[64];
        accept_value(req, acc);
        ol = (size_t)snprintf((char *)out, sizeof out,
                              "HTTP/1.1 101 Switching Protocols\r\nUpgrade: %s\r\n"
                              "Connection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n%s\r\n",
                              p->upgrade_value ? p->upgrade_value : "websocket", acc,
                              p->extra_headers ? p->extra_headers : "");
    }
    memcpy(out + ol, p->after, p->after_len);
    ol += p->after_len;
    (void)kl_test_sockwrite(c, out, ol);
    /* Record what the client sends back (its close frame), until it closes. In a handshake case
     * hold the connection longer than the client waits, so only the client's own limit can end
     * the handshake, not this peer's close. */
    while (kl_test_poll1(c, 0, p->raw_reply ? 8000 : 3000) > 0) {
        long n = kl_test_sockread(c, p->got + p->got_len, sizeof(p->got) - p->got_len);
        if (n <= 0) break;
        p->got_len += (size_t)n;
        if (p->got_len == sizeof(p->got)) break;
    }
    kl_test_closesock(c);
}

static void peer_finish(Peer *p) {
    kl_plat_thread_join(&p->tid);
    kl_test_closesock(p->listen_fd);
}

/* The status code of the first close frame the client sent (client frames are masked), or 0. */
static int client_close_code(const Peer *p) {
    if (p->got_len < 8 || (p->got[0] & 0x0F) != KL_WS_OP_CLOSE || !(p->got[1] & 0x80)) return 0;
    size_t plen = p->got[1] & 0x7F;
    if (plen < 2) return 0;
    const unsigned char *key = p->got + 2;
    return ((p->got[6] ^ key[0]) << 8) | (p->got[7] ^ key[1]);
}

/* ── Client side ────────────────────────────────────────────────────────────────────────────── */

typedef struct {
    int opened, messages, closed, errors;
    int free_in;   /* 1 = on_open, 2 = on_message, 3 = on_close: free the connection there */
    KlWsClientConn *ws;
} Cli;

static void c_open(KlWsClientConn *ws, void *ud) {
    Cli *c = ud; c->opened++;
    if (c->free_in == 1) { kl_ws_client_free(ws); c->ws = NULL; }
}
static void c_msg(KlWsClientConn *ws, const char *d, size_t n, int bin, void *ud) {
    (void)d; (void)n; (void)bin;
    Cli *c = ud; c->messages++;
    if (c->free_in == 2) { kl_ws_client_free(ws); c->ws = NULL; }
}
static void c_close(KlWsClientConn *ws, uint16_t code, const char *r, size_t rl, void *ud) {
    (void)code; (void)r; (void)rl;
    Cli *c = ud; c->closed++;
    if (c->free_in == 3) { kl_ws_client_free(ws); c->ws = NULL; }
}
static void c_err(KlWsClientConn *ws, const char *msg, void *ud) {
    (void)ws; (void)msg;
    ((Cli *)ud)->errors++;
}

static void run_case(Peer *p, Cli *c) {
    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    if (kl_event_ctx_init(&ev, &a) != 0) return;
    char url[64];
    snprintf(url, sizeof url, "ws://127.0.0.1:%d/", p->port);
    KlWsClientCallbacks cbs = { .on_open = c_open, .on_message = c_msg,
                                .on_close = c_close, .on_error = c_err };
    kl_plat_thread_create(&p->tid, peer_thread, p);
    c->ws = kl_ws_client_connect(&ev, &g_qa, NULL, url, &cbs, c);
    for (int i = 0; i < 300 && c->ws && !c->closed && !c->errors; i++)
        (void)kl_event_ctx_run(&ev, 16, 10);
    for (int i = 0; i < 5; i++) (void)kl_event_ctx_run(&ev, 16, 1);   /* stale events, if any */
    if (c->ws) kl_ws_client_free(c->ws);
    kl_event_ctx_free(&ev);
    peer_finish(p);
}

/* An unmasked server frame: FIN + opcode, 7-bit length, payload. */
static size_t frame(unsigned char *o, int op, const void *pl, size_t n) {
    o[0] = (unsigned char)(0x80 | op);
    o[1] = (unsigned char)n;
    memcpy(o + 2, pl, n);
    return 2 + n;
}

/* ── Freeing from a callback ────────────────────────────────────────────────────────────────── */

UTEST(wsc_peer, free_in_on_close_is_safe) {
    static Peer p; ASSERT_EQ(peer_listen(&p), 0);
    static unsigned char f[16];
    const unsigned char code[2] = { 0x03, 0xE8 };            /* 1000 */
    p.after = f; p.after_len = frame(f, KL_WS_OP_CLOSE, code, 2);
    Cli c; memset(&c, 0, sizeof c); c.free_in = 3;
    run_case(&p, &c);
    ASSERT_EQ(c.closed, 1);
    ASSERT_EQ(quarantine_check_and_release(), 0);           /* was: written after free */
}

UTEST(wsc_peer, free_in_on_message_is_safe) {
    static Peer p; ASSERT_EQ(peer_listen(&p), 0);
    static unsigned char f[32];
    size_t n = frame(f, KL_WS_OP_TEXT, "one", 3);
    n += frame(f + n, KL_WS_OP_TEXT, "two", 3);              /* a second frame in the same read */
    p.after = f; p.after_len = n;
    Cli c; memset(&c, 0, sizeof c); c.free_in = 2;
    run_case(&p, &c);
    ASSERT_EQ(c.messages, 1);                                /* nothing after the free */
    ASSERT_EQ(quarantine_check_and_release(), 0);
}

UTEST(wsc_peer, free_in_on_open_with_frames_behind_is_safe) {
    static Peer p; ASSERT_EQ(peer_listen(&p), 0);
    static unsigned char f[16];
    p.after = f; p.after_len = frame(f, KL_WS_OP_TEXT, "hi", 2);   /* arrives with the 101 */
    Cli c; memset(&c, 0, sizeof c); c.free_in = 1;
    run_case(&p, &c);
    ASSERT_EQ(c.opened, 1);
    ASSERT_EQ(c.messages, 0);
    ASSERT_EQ(quarantine_check_and_release(), 0);
}

/* ── Protocol violations ────────────────────────────────────────────────────────────────────── */

UTEST(wsc_peer, masked_server_frame_fails_with_1002) {
    static Peer p; ASSERT_EQ(peer_listen(&p), 0);
    static const unsigned char masked[] = { 0x81, 0x82, 1, 2, 3, 4, 'h' ^ 1, 'i' ^ 2 };
    p.after = masked; p.after_len = sizeof masked;
    Cli c; memset(&c, 0, sizeof c);
    run_case(&p, &c);
    ASSERT_EQ(c.messages, 0);                                /* was: delivered as "hi" */
    ASSERT_EQ(c.errors, 1);
    ASSERT_EQ(client_close_code(&p), 1002);
    (void)quarantine_check_and_release();
}

UTEST(wsc_peer, close_code_not_allowed_on_the_wire_fails_with_1002) {
    static Peer p; ASSERT_EQ(peer_listen(&p), 0);
    static unsigned char f[16];
    const unsigned char code[2] = { 0x03, 0xED };            /* 1005: never sent on the wire */
    p.after = f; p.after_len = frame(f, KL_WS_OP_CLOSE, code, 2);
    Cli c; memset(&c, 0, sizeof c);
    run_case(&p, &c);
    ASSERT_EQ(c.closed, 0);                                  /* was: on_close(1005) */
    ASSERT_EQ(c.errors, 1);
    ASSERT_EQ(client_close_code(&p), 1002);                  /* was: the 1005 echoed back */
    (void)quarantine_check_and_release();
}

UTEST(wsc_peer, close_reason_not_utf8_fails_with_1007) {
    static Peer p; ASSERT_EQ(peer_listen(&p), 0);
    static unsigned char f[16];
    const unsigned char pl[4] = { 0x03, 0xE8, 0xC3, 0x28 };  /* 1000 + an invalid UTF-8 pair */
    p.after = f; p.after_len = frame(f, KL_WS_OP_CLOSE, pl, sizeof pl);
    Cli c; memset(&c, 0, sizeof c);
    run_case(&p, &c);
    ASSERT_EQ(c.closed, 0);
    ASSERT_EQ(c.errors, 1);
    ASSERT_EQ(client_close_code(&p), 1007);
    (void)quarantine_check_and_release();
}

UTEST(wsc_peer, valid_close_is_echoed_and_reported) {
    static Peer p; ASSERT_EQ(peer_listen(&p), 0);
    static unsigned char f[16];
    const unsigned char pl[4] = { 0x0F, 0xA0, 'o', 'k' };    /* 4000 (application range) */
    p.after = f; p.after_len = frame(f, KL_WS_OP_CLOSE, pl, sizeof pl);
    Cli c; memset(&c, 0, sizeof c);
    run_case(&p, &c);
    ASSERT_EQ(c.closed, 1);
    ASSERT_EQ(c.errors, 0);
    ASSERT_EQ(client_close_code(&p), 4000);
    (void)quarantine_check_and_release();
}

UTEST(wsc_peer, endless_handshake_response_is_capped) {
    static Peer p; ASSERT_EQ(peer_listen(&p), 0);
    static unsigned char big[24 * 1024];
    const char *h = "HTTP/1.1 101 Switching Protocols\r\nX-Pad: ";
    size_t hl = strlen(h);
    memcpy(big, h, hl);
    memset(big + hl, 'a', sizeof big - hl);                  /* the headers never end */
    p.after = big; p.after_len = sizeof big; p.raw_reply = 1;
    Cli c; memset(&c, 0, sizeof c);
    run_case(&p, &c);
    ASSERT_EQ(c.opened, 0);
    ASSERT_EQ(c.errors, 1);                                  /* was: kept reading and growing */
    (void)quarantine_check_and_release();
}

/* ── The handshake is a WebSocket handshake ─────────────────────────────────────────────────── */
/* A 101 with the right accept value was enough. It must also upgrade to "websocket", and must not
 * negotiate an extension the client never offered (its frames would then carry RSV bits the client
 * cannot read). */
UTEST(wsc_peer, upgrade_to_another_protocol_is_refused) {
    static Peer p; ASSERT_EQ(peer_listen(&p), 0);
    p.upgrade_value = "h2c";
    Cli c; memset(&c, 0, sizeof c);
    run_case(&p, &c);
    ASSERT_EQ(c.opened, 0);                                  /* was: opened */
    ASSERT_EQ(c.errors, 1);
    (void)quarantine_check_and_release();
}

UTEST(wsc_peer, unoffered_extension_is_refused) {
    static Peer p; ASSERT_EQ(peer_listen(&p), 0);
    p.extra_headers = "Sec-WebSocket-Extensions: permessage-deflate\r\n";
    Cli c; memset(&c, 0, sizeof c);
    run_case(&p, &c);
    ASSERT_EQ(c.opened, 0);                                  /* was: opened */
    ASSERT_EQ(c.errors, 1);
    (void)quarantine_check_and_release();
}

/* ── Fragmentation (RFC 6455 5.4) ───────────────────────────────────────────────────────────── */
static size_t frag(unsigned char *o, int fin, int op, const void *pl, size_t n) {
    o[0] = (unsigned char)((fin ? 0x80 : 0) | op);
    o[1] = (unsigned char)n;
    if (n) memcpy(o + 2, pl, n);
    return 2 + n;
}

/* A new data frame while a fragmented message is open is a protocol error; it used to replace the
 * open message silently. */
UTEST(wsc_peer, new_message_inside_a_fragmented_one_fails_with_1002) {
    static Peer p; ASSERT_EQ(peer_listen(&p), 0);
    static unsigned char f[32];
    size_t n = frag(f, 0, KL_WS_OP_TEXT, "a", 1);
    n += frag(f + n, 1, KL_WS_OP_TEXT, "b", 1);
    p.after = f; p.after_len = n;
    Cli c; memset(&c, 0, sizeof c);
    run_case(&p, &c);
    ASSERT_EQ(c.messages, 0);                                /* was: "b" delivered */
    ASSERT_EQ(c.errors, 1);
    ASSERT_EQ(client_close_code(&p), 1002);
    (void)quarantine_check_and_release();
}

/* An empty first fragment starts a message; its continuation completes it. */
UTEST(wsc_peer, empty_first_fragment_starts_the_message) {
    static Peer p; ASSERT_EQ(peer_listen(&p), 0);
    static unsigned char f[32];
    size_t n = frag(f, 0, KL_WS_OP_TEXT, NULL, 0);
    n += frag(f + n, 1, KL_WS_OP_CONTINUATION, "x", 1);
    p.after = f; p.after_len = n;
    Cli c; memset(&c, 0, sizeof c);
    run_case(&p, &c);
    ASSERT_EQ(c.errors, 0);                                  /* was: "continuation without start" */
    ASSERT_EQ(c.messages, 1);
    (void)quarantine_check_and_release();
}

UTEST_MAIN();
