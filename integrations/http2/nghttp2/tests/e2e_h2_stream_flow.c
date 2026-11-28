/*
 * e2e_h2_stream_flow.c: HTTP/2 streamed responses, per-stream flow control and per-stream cancel.
 *
 * A real KEEL server (KlHttpServerConfig.h2 with the nghttp2 factory, h2c prior knowledge, a real
 * loopback socket) serves two streamed responses on ONE connection:
 *   - /bulk: a producer outside the handler (a timer) writes 4 KiB chunks with
 *     kl_http_response_stream_try_write(), retrying from the writable edge on WOULD_BLOCK, and never
 *     ends (it is meant to be cancelled);
 *   - /events: server-sent events (kl_http_sse_*) from a timer, 200 events, then the end.
 *
 * The client is a raw nghttp2 session with automatic WINDOW_UPDATE turned off. It opens the
 * connection window and /events' stream window as it consumes their data, and never opens /bulk's:
 * /bulk's window is exhausted after its first INITIAL_WINDOW bytes. Then:
 *   1. /events keeps delivering while /bulk is stalled, /bulk's producer is refused (WOULD_BLOCK)
 *      with its buffer bounded by the stream capacity, and /bulk is not cancelled;
 *   2. the client resets /bulk (RST_STREAM CANCEL) while /events is still streaming: /bulk's
 *      producer hears completed=0 once, /events runs on to its end and its producer hears
 *      completed=1 once;
 *   3. a 3 MiB buffered (not streamed) response on the same connection arrives whole: on a
 *      completion loop it is larger than the output queue's producer bound, so the session holds
 *      frames back and a send completion has to resume them.
 * Exits non-zero on any failure.
 */
#include <keel/keel.h>
#include "keel_http2_nghttp2.h"
#include <nghttp2/nghttp2.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define CAPACITY       (16 * 1024)   /* the server's stream_capacity */
#define INITIAL_WINDOW (16 * 1024)   /* the client's SETTINGS_INITIAL_WINDOW_SIZE */
#define CHUNK          4096
#define EVENTS         200

static int fail(const char *m) { fprintf(stderr, "FAIL: %s\n", m); return 1; }
static void nap(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ── server side: two producers ───────────────────────────────────────────────────────────────── */

static KlHttpServer g_srv;

typedef struct {
    KlHttpResponse *res;
    KlHttpSse       sse;
    int             is_sse;
    int64_t         timer;
    int             timer_armed;
    int             sent_events;
    atomic_size_t   accepted;       /* body bytes the response took */
    atomic_int      would_block;    /* WOULD_BLOCK answers */
    atomic_size_t   max_pending;    /* the most the stream held at once */
    atomic_int      closed;         /* close notifications */
    atomic_int      completed;      /* the last notification's completed flag */
    atomic_int      wrote_after_close;
} Producer;

static Producer g_bulk, g_events;

static void producer_tick(void *ud);

static void producer_arm(Producer *p, uint64_t ms) {
    if (p->timer_armed || atomic_load(&p->closed)) return;
    p->timer = kl_timer_add(kl_http_server_event_ctx(&g_srv), ms, producer_tick, p);
    p->timer_armed = p->timer >= 0;
}

static void producer_writable(void *ud) { producer_arm(ud, 0); }

static void producer_closed(void *ud, int completed) {
    Producer *p = ud;
    atomic_store(&p->completed, completed);
    atomic_fetch_add(&p->closed, 1);
    if (p->timer_armed) kl_timer_cancel(kl_http_server_event_ctx(&g_srv), p->timer);
    p->timer_armed = 0;
    p->res = NULL;                   /* no longer ours */
}

static void note_pending(Producer *p) {
    size_t pend = kl_http_response_stream_pending(p->res);
    if (pend > atomic_load(&p->max_pending)) atomic_store(&p->max_pending, pend);
}

static void producer_tick(void *ud) {
    Producer *p = ud;
    p->timer_armed = 0;
    if (atomic_load(&p->closed)) { atomic_fetch_add(&p->wrote_after_close, 1); return; }
    if (p->is_sse) {
        char data[64];
        int n = snprintf(data, sizeof(data), "tick %d", p->sent_events);
        if (kl_http_sse_event(&p->sse, "tick", data, (size_t)n, NULL) == 0) {
            p->sent_events++;
            atomic_fetch_add(&p->accepted, (size_t)n);
        }
        note_pending(p);
        if (p->sent_events == EVENTS) { kl_http_sse_end(&p->sse); return; }
        producer_arm(p, 2);
        return;
    }
    static char chunk[CHUNK];
    memset(chunk, 'b', sizeof(chunk));
    for (;;) {                                   /* fill the stream until it refuses */
        KlHttpStreamStatus st = kl_http_response_stream_try_write(p->res, chunk, sizeof(chunk));
        note_pending(p);
        if (st == KL_HTTP_STREAM_ACCEPTED) { atomic_fetch_add(&p->accepted, sizeof(chunk)); continue; }
        if (st == KL_HTTP_STREAM_WOULD_BLOCK) { atomic_fetch_add(&p->would_block, 1); return; }
        return;                                  /* CLOSED / ERROR: the close notification follows */
    }
}

static void handle_bulk(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)req; (void)ud;
    KlHttpResponseWriteFn w; void *wctx;
    kl_http_response_header(res, "Content-Type", "application/octet-stream");
    if (kl_http_response_begin_stream(res, 200, &w, &wctx) < 0) return;
    g_bulk.res = res;
    kl_http_response_stream_on_close(res, producer_closed, &g_bulk);
    kl_http_response_stream_on_writable(res, producer_writable, &g_bulk);
    producer_arm(&g_bulk, 0);
}

static void handle_events(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)req; (void)ud;
    if (kl_http_sse_begin(res, &g_events.sse) < 0) return;
    g_events.res = res;
    g_events.is_sse = 1;
    kl_http_response_stream_on_close(res, producer_closed, &g_events);
    producer_arm(&g_events, 0);
}

#define BIG (3u * 1024u * 1024u)
static char g_big[BIG];
static void handle_big(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)req; (void)ud;
    kl_http_response_status(res, 200);
    kl_http_response_body_borrow(res, g_big, sizeof(g_big));
}

static void *srv_thread(void *a) { (void)a; kl_http_server_run(&g_srv); return NULL; }

/* ── client side: a raw nghttp2 session that opens windows by hand ───────────────────────────── */

typedef struct {
    int fd;
    nghttp2_session *ng;
    int32_t bulk_id, events_id, big_id;
    size_t bulk_bytes, events_bytes, big_bytes;
    int big_ended, big_bad;
    int bulk_status, events_status;
    int events_ended, bulk_ended, bulk_closed;
    uint32_t bulk_close_code;
    char events_tail[64];
} Cli;

static ssize_t cli_send(nghttp2_session *s, const uint8_t *d, size_t n, int fl, void *ud) {
    (void)s; (void)fl;
    Cli *c = ud;
    ssize_t w = send(c->fd, d, n, 0);
    return w < 0 ? NGHTTP2_ERR_WOULDBLOCK : w;
}

static int cli_on_header(nghttp2_session *s, const nghttp2_frame *f, const uint8_t *name, size_t nl,
                         const uint8_t *value, size_t vl, uint8_t fl, void *ud) {
    (void)s; (void)fl;
    Cli *c = ud;
    if (nl == 7 && memcmp(name, ":status", 7) == 0 && vl == 3) {
        int st = (value[0] - '0') * 100 + (value[1] - '0') * 10 + (value[2] - '0');
        if (f->hd.stream_id == c->bulk_id) c->bulk_status = st;
        if (f->hd.stream_id == c->events_id) c->events_status = st;
    }
    return 0;
}

static int cli_on_data(nghttp2_session *s, uint8_t fl, int32_t sid, const uint8_t *d, size_t n,
                       void *ud) {
    (void)fl;
    Cli *c = ud;
    nghttp2_session_consume_connection(s, n);    /* the connection window stays open */
    if (sid == c->events_id) {
        nghttp2_session_consume_stream(s, sid, n);   /* /events: opened as consumed */
        c->events_bytes += n;
        size_t keep = n < sizeof(c->events_tail) - 1 ? n : sizeof(c->events_tail) - 1;
        memcpy(c->events_tail, d + n - keep, keep);
        c->events_tail[keep] = 0;
    } else if (sid == c->big_id) {
        nghttp2_session_consume_stream(s, sid, n);
        for (size_t i = 0; i < n; i++)
            if ((unsigned char)d[i] != (unsigned char)((c->big_bytes + i) % 251)) c->big_bad = 1;
        c->big_bytes += n;
    } else if (sid == c->bulk_id) {
        c->bulk_bytes += n;                      /* /bulk: its window is never opened again */
    }
    return 0;
}

static int cli_on_frame_recv(nghttp2_session *s, const nghttp2_frame *f, void *ud) {
    (void)s;
    Cli *c = ud;
    if ((f->hd.type == NGHTTP2_DATA || f->hd.type == NGHTTP2_HEADERS) &&
        (f->hd.flags & NGHTTP2_FLAG_END_STREAM)) {
        if (f->hd.stream_id == c->events_id) c->events_ended = 1;
        if (f->hd.stream_id == c->bulk_id) c->bulk_ended = 1;
        if (f->hd.stream_id == c->big_id) c->big_ended = 1;
    }
    return 0;
}

static int cli_on_close(nghttp2_session *s, int32_t sid, uint32_t code, void *ud) {
    (void)s;
    Cli *c = ud;
    if (sid == c->bulk_id) { c->bulk_closed = 1; c->bulk_close_code = code; }
    return 0;
}

/* Pump the client for up to `ms` or until *until is set. */
static int cli_pump(Cli *c, int ms, const int *until) {
    for (int waited = 0; waited < ms && !(until && *until); waited += 5) {
        if (nghttp2_session_send(c->ng) != 0) return -1;
        struct pollfd pfd = { c->fd, POLLIN, 0 };
        if (poll(&pfd, 1, 5) > 0) {
            uint8_t buf[16384];
            ssize_t n = recv(c->fd, buf, sizeof(buf), 0);
            if (n <= 0) return -1;
            if (nghttp2_session_mem_recv(c->ng, buf, (size_t)n) < 0) return -1;
        }
    }
    return nghttp2_session_send(c->ng) == 0 ? 0 : -1;
}

static int32_t cli_get(Cli *c, const char *path) {
    const nghttp2_nv nva[] = {
        { (uint8_t *)":method", (uint8_t *)"GET", 7, 3, NGHTTP2_NV_FLAG_NONE },
        { (uint8_t *)":scheme", (uint8_t *)"http", 7, 4, NGHTTP2_NV_FLAG_NONE },
        { (uint8_t *)":authority", (uint8_t *)"localhost", 10, 9, NGHTTP2_NV_FLAG_NONE },
        { (uint8_t *)":path", (uint8_t *)path, 5, strlen(path), NGHTTP2_NV_FLAG_NONE },
    };
    return nghttp2_submit_request(c->ng, NULL, nva, 4, NULL, NULL);
}

int main(void) {
    KlHttp2ServerConfig h2cfg = { .factory = kl_http2_nghttp2_server_session };
    KlHttpServerConfig cfg = { .port = 0, .bind_addr = "127.0.0.1", .h2 = &h2cfg,
                               .stream_capacity = CAPACITY };
    if (kl_http_server_init(&g_srv, &cfg) < 0) return fail("server init");
    kl_http_server_route(&g_srv, "GET", "/bulk", handle_bulk, NULL, NULL);
    kl_http_server_route(&g_srv, "GET", "/events", handle_events, NULL, NULL);
    kl_http_server_route(&g_srv, "GET", "/big", handle_big, NULL, NULL);
    for (size_t i = 0; i < sizeof(g_big); i++) g_big[i] = (char)(i % 251);
    pthread_t tid;
    if (pthread_create(&tid, NULL, srv_thread, NULL) != 0) return fail("server thread");
    for (int i = 0; i < 400 && kl_http_server_bound_port(&g_srv) == 0; i++) nap(5);
    if (kl_http_server_bound_port(&g_srv) <= 0) return fail("server never bound");

    Cli c;
    memset(&c, 0, sizeof(c));
    c.fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons((uint16_t)kl_http_server_bound_port(&g_srv)) };
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(c.fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) return fail("connect");

    nghttp2_session_callbacks *cbs;
    nghttp2_session_callbacks_new(&cbs);
    nghttp2_session_callbacks_set_send_callback(cbs, cli_send);
    nghttp2_session_callbacks_set_on_header_callback(cbs, cli_on_header);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cbs, cli_on_data);
    nghttp2_session_callbacks_set_on_frame_recv_callback(cbs, cli_on_frame_recv);
    nghttp2_session_callbacks_set_on_stream_close_callback(cbs, cli_on_close);
    nghttp2_option *opt;
    nghttp2_option_new(&opt);
    nghttp2_option_set_no_auto_window_update(opt, 1);
    nghttp2_session_client_new2(&c.ng, cbs, &c, opt);
    nghttp2_option_del(opt);
    nghttp2_session_callbacks_del(cbs);

    nghttp2_settings_entry iv[] = { { NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, INITIAL_WINDOW } };
    nghttp2_submit_settings(c.ng, NGHTTP2_FLAG_NONE, iv, 1);
    c.bulk_id = cli_get(&c, "/bulk");
    c.events_id = cli_get(&c, "/events");

    int rc = 0;
    /* 1. /bulk's window runs out; /events keeps coming. Wait until a quarter of the events. */
    for (int i = 0; i < 400 && c.events_bytes < (size_t)EVENTS * 20 / 4; i++)
        if (cli_pump(&c, 10, NULL) < 0) { rc = fail("client I/O (phase 1)"); goto out; }
    cli_pump(&c, 100, NULL);                     /* let /bulk's producer hit its bound */
    size_t bulk_seen = c.bulk_bytes;
    size_t events_mid = c.events_bytes;
    printf("phase 1: /bulk %zu bytes (window %d), /events %zu bytes, bulk WOULD_BLOCK %d, "
           "bulk max pending %zu (capacity %d)\n", bulk_seen, INITIAL_WINDOW, events_mid,
           atomic_load(&g_bulk.would_block), atomic_load(&g_bulk.max_pending), CAPACITY);
    if (c.bulk_status != 200 || c.events_status != 200) { rc = fail("statuses"); goto out; }
    if (bulk_seen > INITIAL_WINDOW) { rc = fail("/bulk exceeded its flow-control window"); goto out; }
    if (bulk_seen == 0) { rc = fail("/bulk delivered nothing"); goto out; }
    if (atomic_load(&g_bulk.would_block) == 0) { rc = fail("/bulk producer never refused"); goto out; }
    if (atomic_load(&g_bulk.max_pending) > CAPACITY) { rc = fail("/bulk buffer past capacity"); goto out; }
    if (atomic_load(&g_bulk.closed) != 0) { rc = fail("/bulk cancelled while only stalled"); goto out; }
    if (c.events_ended) { rc = fail("/events finished before the reset (test timing)"); goto out; }

    /* 2. Reset the stalled /bulk while /events still streams. */
    nghttp2_submit_rst_stream(c.ng, NGHTTP2_FLAG_NONE, c.bulk_id, NGHTTP2_CANCEL);
    for (int i = 0; i < 400 && !c.events_ended; i++)
        if (cli_pump(&c, 10, &c.events_ended) < 0) { rc = fail("client I/O (phase 2)"); goto out; }
    for (int i = 0; i < 100 && (atomic_load(&g_bulk.closed) == 0 || atomic_load(&g_events.closed) == 0); i++)
        cli_pump(&c, 10, NULL);
    printf("phase 2: /events %zu bytes, ended %d, tail \"%s\"; /bulk closed %d (completed %d), "
           "/events closed %d (completed %d)\n", c.events_bytes, c.events_ended, c.events_tail,
           atomic_load(&g_bulk.closed), atomic_load(&g_bulk.completed),
           atomic_load(&g_events.closed), atomic_load(&g_events.completed));
    if (!c.events_ended) { rc = fail("/events did not finish after /bulk's reset"); goto out; }
    if (c.events_bytes <= events_mid) { rc = fail("/events stopped at the reset"); goto out; }
    if (!strstr(c.events_tail, "tick 199")) { rc = fail("/events lost its last event"); goto out; }
    if (atomic_load(&g_bulk.closed) != 1 || atomic_load(&g_bulk.completed) != 0) {
        rc = fail("/bulk producer not cancelled exactly once"); goto out; }
    if (atomic_load(&g_events.closed) != 1 || atomic_load(&g_events.completed) != 1) {
        rc = fail("/events producer not completed exactly once"); goto out; }
    if (c.bulk_bytes != bulk_seen) { rc = fail("/bulk kept sending past its window"); goto out; }
    if (atomic_load(&g_bulk.wrote_after_close) || atomic_load(&g_events.wrote_after_close)) {
        rc = fail("a producer ran after its close notification"); goto out; }

    /* 3. A whole body past the completion queue's bound. */
    c.big_id = cli_get(&c, "/big");
    /* Windows far past the body, so the server may send it all at once: only the transport bounds
     * it. Set once the stream is open (its HEADERS sent): before that nghttp2 has no stream to set. */
    if (nghttp2_session_send(c.ng) != 0 ||
        nghttp2_session_set_local_window_size(c.ng, NGHTTP2_FLAG_NONE, 0, 16 * 1024 * 1024) != 0 ||
        nghttp2_session_set_local_window_size(c.ng, NGHTTP2_FLAG_NONE, c.big_id, 16 * 1024 * 1024) != 0) {
        rc = fail("open /big's windows"); goto out; }
    for (int i = 0; i < 1000 && !c.big_ended; i++)
        if (cli_pump(&c, 10, &c.big_ended) < 0) { rc = fail("client I/O (phase 3)"); goto out; }
    printf("phase 3: /big %zu of %u bytes, ended %d\n", c.big_bytes, BIG, c.big_ended);
    if (!c.big_ended || c.big_bytes != BIG || c.big_bad) { rc = fail("/big did not arrive whole"); goto out; }

out:
    nghttp2_session_del(c.ng);
    close(c.fd);
    kl_http_server_stop(&g_srv);
    pthread_join(tid, NULL);
    kl_http_server_free(&g_srv);
    if (rc == 0) printf("nghttp2 streamed responses: per-stream flow control and cancel OK\n");
    return rc;
}
