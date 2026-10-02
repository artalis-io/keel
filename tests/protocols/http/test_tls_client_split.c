/*
 * test_tls_client_split.c: Keel's clients over TLS when a record does not arrive in one piece.
 *
 * On a real network a TLS record often spans several TCP segments. A read that gets part of one
 * returns 0, WANT_READ, and a write into a full send buffer returns 0, WANT_WRITE: the KlTls
 * contract (-1 is error or close, with at_eof telling which). A record can also hold more plaintext
 * than the caller's read buffer, and the socket will not signal readable again for what the engine
 * already holds. Loopback tests rarely see any of this.
 *
 * The peer is a scripted raw TCP thread (the mock TLS is an identity transform, so it speaks plain
 * bytes); only the client runs the mock, whose knobs (mock_tls_split_record / _split_after /
 * _write_want) simulate the split record and the full send buffer.
 */
#include "utest.h"
#include <keel/keel.h>
#include <keel/http_client.h>
#include <keel/http_client_pool.h>
#include <keel/websocket.h>
#include <keel/websocket_client.h>
#include "net_compat.h"
#include "platform_thread.h"
#include "platform_socket.h"
#include "mock_tls.h"
#include "sha1.h"
#include "base64.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Scripted peer ──────────────────────────────────────────────────────────────────────────── */

enum { STEP_HEAD, STEP_SEND, STEP_PAUSE, STEP_WS_101, STEP_WS_101_SPLIT };
typedef struct { int kind; const char *data; size_t len; int ms; } Step;

typedef struct {
    KlSocketHandle lfd;
    int port;
    Step steps[16];
    int nsteps;
    char head[4096];
    size_t head_len;
    KlPlatThread t;
} Peer;

static int peer_listen(Peer *p) {
    if (kl_plat_socket_runtime_init() != 0) return -1;
    p->lfd = (KlSocketHandle)socket(AF_INET, SOCK_STREAM, 0);
    if (!kl_handle_valid(p->lfd)) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t al = sizeof a;
    if (bind((int)p->lfd, (struct sockaddr *)&a, sizeof a) != 0 || listen((int)p->lfd, 4) != 0 ||
        getsockname((int)p->lfd, (struct sockaddr *)&a, &al) != 0) return -1;
    p->port = ntohs(a.sin_port);
    return 0;
}

static int peer_read_head(int fd, Peer *p) {
    p->head_len = 0;
    while (p->head_len < sizeof p->head - 1) {
        int k = (int)recv(fd, p->head + p->head_len, 1, 0);
        if (k <= 0) return -1;
        p->head_len++;
        p->head[p->head_len] = '\0';
        if (p->head_len >= 4 && memcmp(p->head + p->head_len - 4, "\r\n\r\n", 4) == 0) return 0;
    }
    return -1;
}

/* The 101 for the head just read, with the accept value for its Sec-WebSocket-Key. */
static size_t ws_101(const char *head, char *out, size_t cap) {
    const char *k = strstr(head, "Sec-WebSocket-Key: ");
    if (!k) return 0;
    k += strlen("Sec-WebSocket-Key: ");
    const char *e = strstr(k, "\r\n");
    char cat[128];
    size_t kl = (size_t)(e - k);
    if (!e || kl + sizeof(KL_WS_MAGIC_GUID) > sizeof cat) return 0;
    memcpy(cat, k, kl);
    memcpy(cat + kl, KL_WS_MAGIC_GUID, sizeof(KL_WS_MAGIC_GUID) - 1);
    uint8_t dig[20];
    kl_sha1(cat, kl + sizeof(KL_WS_MAGIC_GUID) - 1, dig);
    char acc[32];
    size_t al = 0;
    kl_base64_encode(dig, sizeof dig, acc, &al);
    int n = snprintf(out, cap, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                     "Connection: Upgrade\r\nSec-WebSocket-Accept: %.*s\r\n\r\n", (int)al, acc);
    return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

static void peer_thread(void *arg) {
    Peer *p = arg;
    int fd = (int)accept((int)p->lfd, NULL, NULL);
    if (fd < 0) return;
    kl_test_set_rcvtimeo((KlSocketHandle)fd, 5000);
    for (int i = 0; i < p->nsteps; i++) {
        const Step *s = &p->steps[i];
        if (s->kind == STEP_HEAD) {
            if (peer_read_head(fd, p) != 0) break;
        } else if (s->kind == STEP_SEND) {
            (void)kl_test_sockwrite(fd, s->data, s->len);
        } else if (s->kind == STEP_PAUSE) {
            kl_test_sleep_ms((unsigned)s->ms);
        } else if (s->kind == STEP_WS_101 || s->kind == STEP_WS_101_SPLIT) {
            static char r[512];
            size_t n = ws_101(p->head, r, sizeof r);
            if (!n) break;
            if (s->kind == STEP_WS_101) {
                (void)kl_test_sockwrite(fd, r, n);
            } else {
                (void)kl_test_sockwrite(fd, r, 10);   /* part of the record ... */
                kl_test_sleep_ms(150);
                (void)kl_test_sockwrite(fd, r + 10, n - 10);   /* ... and the rest */
            }
        }
    }
    char sink[256];
    while (kl_test_sockread(fd, sink, sizeof sink) > 0) {}   /* until the client lets go */
    kl_test_closesock(fd);
}

static void peer_start(Peer *p) { (void)kl_plat_thread_create(&p->t, peer_thread, p); }
static void peer_join(Peer *p) { kl_plat_thread_join(&p->t); kl_test_closesock((int)p->lfd); }
static void step(Peer *p, int kind, const char *data, size_t len, int ms) {
    p->steps[p->nsteps++] = (Step){ kind, data, len, ms };
}

static KlTlsConfig g_tls = { .ctx = NULL, .factory = mock_tls_create };
static void knobs_off(void) {
    mock_tls_split_record = 0;
    mock_tls_split_after = 0;
    mock_tls_write_want = 0;
}

/* ── Sync client ────────────────────────────────────────────────────────────────────────────── */

/* A response record larger than the client's 8 KiB read buffer: after the first read, the rest is
 * plaintext inside the engine, and polling the socket for it waits until the timeout. */
UTEST(tls_client_split, sync_reads_buffered_plaintext_without_waiting) {
    static Peer p;
    memset(&p, 0, sizeof p);
    ASSERT_EQ(peer_listen(&p), 0);
    enum { BODY = 12000 };
    static char resp[BODY + 128];
    int hl = snprintf(resp, sizeof resp, "HTTP/1.1 200 OK\r\nContent-Length: %d\r\nConnection: close\r\n\r\n", BODY);
    memset(resp + hl, 'b', BODY);
    size_t rl = (size_t)hl + BODY;
    step(&p, STEP_HEAD, NULL, 0, 0);
    step(&p, STEP_SEND, resp, rl, 0);
    peer_start(&p);

    mock_tls_split_record = rl;                 /* one record carrying the whole response */
    KlAllocator a = kl_allocator_default();
    KlHttpClientConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.tls = &g_tls;
    cfg.timeout_ms = 1500;
    char url[64];
    snprintf(url, sizeof url, "https://127.0.0.1:%d/", p.port);
    KlHttpClientResponse r;
    memset(&r, 0, sizeof r);
    int rc = kl_http_client_request(&a, &cfg, "GET", url, NULL, 0, NULL, 0, &r);
    knobs_off();
    int status = r.status;
    size_t blen = r.body_len;
    kl_http_client_response_free(&r);
    peer_join(&p);

    ASSERT_EQ(rc, 0);
    ASSERT_EQ(status, 200);
    ASSERT_EQ(blen, (size_t)BODY);
}

/* A pooled connection comes back non-blocking (the pool's liveness peek sets it). A read there that
 * gets part of a record returns 0, WANT_READ, which the sync client took for end of stream. */
UTEST(tls_client_split, sync_pooled_read_want_is_not_end_of_stream) {
    static Peer p;
    memset(&p, 0, sizeof p);
    ASSERT_EQ(peer_listen(&p), 0);
    static const char r1[] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
    static const char r2[] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi";
    step(&p, STEP_HEAD, NULL, 0, 0);
    step(&p, STEP_SEND, r1, sizeof r1 - 1, 0);
    step(&p, STEP_HEAD, NULL, 0, 0);
    step(&p, STEP_SEND, r2, 12, 0);            /* part of the second response's record ... */
    step(&p, STEP_PAUSE, NULL, 0, 150);
    step(&p, STEP_SEND, r2 + 12, sizeof r2 - 1 - 12, 0);   /* ... and the rest */
    peer_start(&p);

    KlAllocator a = kl_allocator_default();
    KlHttpClientPool pool;
    int pool_ok = kl_http_client_pool_init(&pool, NULL, &a, NULL) == 0;
    KlHttpClientConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.tls = &g_tls;
    cfg.timeout_ms = 2000;
    char url[64];
    snprintf(url, sizeof url, "https://127.0.0.1:%d/", p.port);
    KlHttpClientResponse a1, a2;
    memset(&a1, 0, sizeof a1);
    memset(&a2, 0, sizeof a2);
    int rc1 = pool_ok ? kl_http_client_request_pooled(&pool, &a, &cfg, "GET", url, NULL, 0, NULL, 0, &a1) : -1;
    mock_tls_split_after = sizeof r1 - 1;       /* the split starts with the second response */
    mock_tls_split_record = sizeof r2 - 1;
    int rc2 = pool_ok ? kl_http_client_request_pooled(&pool, &a, &cfg, "GET", url, NULL, 0, NULL, 0, &a2) : -1;
    knobs_off();
    int s2 = a2.status;
    int body_ok = a2.body_len == 2 && a2.body && memcmp(a2.body, "hi", 2) == 0;
    kl_http_client_response_free(&a1);
    kl_http_client_response_free(&a2);
    if (pool_ok) kl_http_client_pool_free(&pool);
    peer_join(&p);

    ASSERT_EQ(rc1, 0);
    ASSERT_EQ(rc2, 0);
    ASSERT_EQ(s2, 200);
    ASSERT_TRUE(body_ok);
}

/* ── Async client ───────────────────────────────────────────────────────────────────────────── */

typedef struct { int done; int error; int status; } AsyncDone;
static void async_done(KlHttpClient *c, void *ud) {
    AsyncDone *d = ud;
    d->done = 1;
    d->error = kl_http_client_error(c);
    const KlHttpClientResponse *r = kl_http_client_response(c);
    d->status = r ? r->status : 0;
}

/* A write into a full send buffer returns 0, WANT_WRITE: wait for writable and retry, not fail. */
UTEST(tls_client_split, async_write_want_is_retried) {
    static Peer p;
    memset(&p, 0, sizeof p);
    ASSERT_EQ(peer_listen(&p), 0);
    static const char resp[] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
    step(&p, STEP_HEAD, NULL, 0, 0);
    step(&p, STEP_SEND, resp, sizeof resp - 1, 0);
    peer_start(&p);

    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    int ev_ok = kl_event_ctx_init(&ev, &a) == 0;
    KlHttpClientConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.tls = &g_tls;
    cfg.timeout_ms = 3000;
    char url[64];
    snprintf(url, sizeof url, "https://127.0.0.1:%d/", p.port);
    AsyncDone d = { 0, 0, 0 };
    mock_tls_write_want = 1;                    /* the first write finds the send buffer full */
    KlHttpClient *c = ev_ok ? kl_http_client_start(&ev, &a, &cfg, "GET", url, NULL, 0, NULL, 0,
                                                   async_done, &d) : NULL;
    for (int i = 0; c && i < 300 && !d.done; i++) (void)kl_event_ctx_run(&ev, 16, 10);
    knobs_off();
    kl_http_client_free(c);
    if (ev_ok) kl_event_ctx_free(&ev);
    peer_join(&p);

    ASSERT_TRUE(c != NULL);
    ASSERT_TRUE(d.done);
    ASSERT_EQ(d.error, 0);
    ASSERT_EQ(d.status, 200);
}

/* ── WebSocket client ───────────────────────────────────────────────────────────────────────── */

typedef struct { int open, closed, errors; size_t got_len; unsigned char *got; } Ws;
static void ws_open(KlWsClientConn *ws, void *ud) { (void)ws; ((Ws *)ud)->open = 1; }
static void ws_msg(KlWsClientConn *ws, const char *data, size_t len, int bin, void *ud) {
    (void)ws; (void)bin;
    Ws *w = ud;
    free(w->got);
    w->got = malloc(len ? len : 1);
    memcpy(w->got, data, len);
    w->got_len = len;
}
static void ws_close(KlWsClientConn *ws, uint16_t code, const char *r, size_t rl, void *ud) {
    (void)ws; (void)code; (void)r; (void)rl;
    ((Ws *)ud)->closed = 1;
}
static void ws_err(KlWsClientConn *ws, const char *msg, void *ud) {
    (void)ws; (void)msg;
    ((Ws *)ud)->errors++;
}

/* Run a wss:// client against the scripted peer until it has a message (or gives up). */
static void ws_run(Peer *p, Ws *w) {
    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    if (kl_event_ctx_init(&ev, &a) != 0) return;
    KlWsClientConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.tls = &g_tls;
    char url[64];
    snprintf(url, sizeof url, "wss://127.0.0.1:%d/ws", p->port);
    KlWsClientCallbacks cbs = { .on_open = ws_open, .on_message = ws_msg,
                                .on_close = ws_close, .on_error = ws_err };
    KlWsClientConn *ws = kl_ws_client_connect(&ev, &a, &cfg, url, &cbs, w);
    for (int i = 0; ws && i < 300 && !w->got_len && !w->closed && !w->errors; i++)
        (void)kl_event_ctx_run(&ev, 16, 10);
    kl_ws_client_free(ws);
    kl_event_ctx_free(&ev);
}

UTEST(tls_client_split, ws_client_handshake_record_split) {
    static Peer p;
    memset(&p, 0, sizeof p);
    ASSERT_EQ(peer_listen(&p), 0);
    static const unsigned char frame[] = { 0x81, 0x02, 'h', 'i' };
    step(&p, STEP_HEAD, NULL, 0, 0);
    step(&p, STEP_WS_101_SPLIT, NULL, 0, 0);
    step(&p, STEP_SEND, (const char *)frame, sizeof frame, 0);
    peer_start(&p);

    mock_tls_split_record = 20;                 /* the 101 arrives as a record split after 10 bytes */
    Ws w;
    memset(&w, 0, sizeof w);
    ws_run(&p, &w);
    knobs_off();
    peer_join(&p);
    int got_hi = w.got_len == 2 && memcmp(w.got, "hi", 2) == 0;
    free(w.got);

    ASSERT_EQ(w.errors, 0);
    ASSERT_TRUE(w.open);
    ASSERT_TRUE(got_hi);
}

/* A frame record that arrives in two parts; and a record larger than the client's 8 KiB read
 * buffer, whose tail only a tls->pending() drain can deliver. */
static void ws_frame_case(int *utest_result, size_t n) {
    static Peer p;
    memset(&p, 0, sizeof p);
    ASSERT_EQ(peer_listen(&p), 0);
    static unsigned char frame[16 + 16384];
    size_t h = 0;
    frame[h++] = 0x82;
    if (n < 126) frame[h++] = (unsigned char)n;
    else { frame[h++] = 126; frame[h++] = (unsigned char)(n >> 8); frame[h++] = (unsigned char)n; }
    for (size_t i = 0; i < n; i++) frame[h + i] = (unsigned char)('a' + i % 26);
    size_t fl = h + n;
    step(&p, STEP_HEAD, NULL, 0, 0);
    step(&p, STEP_WS_101, NULL, 0, 0);          /* the 101 whole, then the frame in two parts */
    step(&p, STEP_PAUSE, NULL, 0, 100);
    step(&p, STEP_SEND, (const char *)frame, 3, 0);
    step(&p, STEP_PAUSE, NULL, 0, 150);
    step(&p, STEP_SEND, (const char *)frame + 3, fl - 3, 0);
    peer_start(&p);

    /* the 101 is served normally; the record split begins with the frame. The 101's length is
     * fixed (the accept value is always 28 characters). */
    static char probe[512];
    size_t l101 = ws_101("Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n", probe, sizeof probe);
    mock_tls_split_after = l101;
    mock_tls_split_record = fl;
    Ws w;
    memset(&w, 0, sizeof w);
    ws_run(&p, &w);
    knobs_off();
    peer_join(&p);
    int whole = w.got_len == n && memcmp(w.got, frame + h, n) == 0;
    free(w.got);

    ASSERT_EQ(w.errors, 0);
    ASSERT_EQ(w.closed, 0);
    ASSERT_TRUE(whole);
}

UTEST(tls_client_split, ws_client_frame_record_split) { ws_frame_case(utest_result, 5); }
UTEST(tls_client_split, ws_client_record_larger_than_read_buffer) { ws_frame_case(utest_result, 12000); }

UTEST_MAIN();
