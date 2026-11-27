/*
 * e2e_h2_stream_control.c: a producer's abort of a stalled HTTP/2 stream, and a client's frames read
 * while the connection's output is held, on every event backend (readiness and completion alike).
 *
 * A real KEEL server (KlHttpServerConfig.h2 with the nghttp2 factory, h2c prior knowledge, a real
 * loopback socket). The client is a raw nghttp2 session that opens flow-control windows by hand.
 *
 *   1. Abort. /stuck's window is never opened, so its producer is refused (WOULD_BLOCK) and its
 *      writable edge never fires again. The producer aborts it from a timer: the client sees
 *      RST_STREAM CANCEL, the producer hears completed=0 once, writes after the abort fail, and the
 *      sibling /events (server-sent events) runs on to its end.
 *   2. Input while output is held. /flood is a 32 MiB body with windows far past it, so only the
 *      transport bounds it. The client stops reading: the socket fills, then the server's output, and
 *      /flood's producer is refused. With that output still held, the client sends a WINDOW_UPDATE
 *      for /slow (window-stalled) and then resets /victim (window-stalled, endless). The server must
 *      read both while /flood is still blocked: the session's input count grows by the
 *      WINDOW_UPDATE, and /victim's producer hears its cancel while /flood still has queued output.
 *      A completion loop used to read an HTTP/2 client only once its output queue was idle. Then the
 *      client reads again and /flood and /slow arrive whole.
 *
 * Exits non-zero on any failure. Builds on POSIX and on Windows (MinGW, Winsock).
 */
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif
#include <keel/keel.h>
#include "keel_http2_nghttp2.h"
#include <nghttp2/nghttp2.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
typedef SOCKET CliSock;
#define CLI_BAD_SOCK INVALID_SOCKET
static int cli_would_block(void) { return WSAGetLastError() == WSAEWOULDBLOCK; }
static void cli_close(CliSock s) { closesocket(s); }
static int cli_nonblock(CliSock s) { u_long on = 1; return ioctlsocket(s, FIONBIO, &on); }
static int cli_poll_in(CliSock s, int ms) {
    WSAPOLLFD p = { s, POLLRDNORM, 0 };
    return WSAPoll(&p, 1, ms);
}
static void nap(int ms) { Sleep((DWORD)ms); }
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
typedef int CliSock;
#define CLI_BAD_SOCK (-1)
static int cli_would_block(void) { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }
static void cli_close(CliSock s) { close(s); }
static int cli_nonblock(CliSock s) {
    int fl = fcntl(s, F_GETFL, 0);
    return fl < 0 ? -1 : fcntl(s, F_SETFL, fl | O_NONBLOCK);
}
static int cli_poll_in(CliSock s, int ms) {
    struct pollfd p = { s, POLLIN, 0 };
    return poll(&p, 1, ms);
}
static void nap(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}
#endif

#define CAPACITY       (16 * 1024)              /* the server's stream_capacity */
#define INITIAL_WINDOW (16 * 1024)              /* the client's SETTINGS_INITIAL_WINDOW_SIZE */
#define BIG_WINDOW     (128 * 1024 * 1024)
#define CHUNK          4096
#define EVENTS         150
#define FLOOD_TOTAL    ((size_t)32 * 1024 * 1024)
#define SLOW_TOTAL     ((size_t)64 * 1024)

static int fail(const char *m) { fprintf(stderr, "FAIL: %s\n", m); return 1; }

/* ── server side ──────────────────────────────────────────────────────────────────────────────── */

static KlHttpServer g_srv;

enum { K_SSE, K_ENDLESS, K_FINITE };

typedef struct {
    int             kind;
    size_t          total;                /* K_FINITE: the body's size */
    int             abort_on_stall;
    KlHttpResponse *res;
    KlHttpSse       sse;
    size_t          written;
    int             sent_events;
    int64_t         timer;
    int             timer_armed;
    int             abort_armed;
    atomic_int      closed, completed;
    atomic_int      would_block, stalled, edges, wrote_after_close;
    atomic_size_t   pending_now;
    atomic_int      abort_rc, abort_again_rc, write_after_abort, end_after_abort;
    int             edges_at_stall;       /* the edge count at the last WOULD_BLOCK */
    atomic_int      edges_at_abort;       /* the edge count when the abort ran (-1: not yet) */
} Producer;

static Producer g_events, g_stuck, g_flood, g_victim, g_slow;
static atomic_int g_victim_saw_flood_held;      /* at /victim's close: /flood blocked, output held */

static KlEventCtx *ev(void) { return kl_http_server_event_ctx(&g_srv); }

static void producer_tick(void *ud);

static void producer_arm(Producer *p, uint64_t ms) {
    if (p->timer_armed || atomic_load(&p->closed)) return;
    p->timer = kl_timer_add(ev(), ms, producer_tick, p);
    p->timer_armed = p->timer >= 0;
}

static void producer_writable(void *ud) {
    Producer *p = ud;
    atomic_fetch_add(&p->edges, 1);
    producer_arm(p, 0);
}

static void producer_closed(void *ud, int completed) {
    Producer *p = ud;
    if (p == &g_victim) {
        int held = !atomic_load(&g_flood.closed) && atomic_load(&g_flood.stalled) &&
                   g_flood.res && kl_http_response_stream_pending(g_flood.res) > 0;
        atomic_store(&g_victim_saw_flood_held, held);
    }
    atomic_store(&p->completed, completed);
    atomic_fetch_add(&p->closed, 1);
    if (p->timer_armed) kl_timer_cancel(ev(), p->timer);
    p->timer_armed = 0;
    p->res = NULL;                              /* no longer ours */
}

/* The abort, from a timer, once the stream has stayed stalled with no writable edge since its last
 * refusal (its window exhausted): nothing on the stream progresses first. */
static void abort_tick(void *ud) {
    Producer *p = ud;
    if (atomic_load(&p->closed)) return;
    if (!atomic_load(&p->stalled) || atomic_load(&p->edges) != p->edges_at_stall) {
        (void)kl_timer_add(ev(), 30, abort_tick, p);   /* it moved since: wait for the real stall */
        return;
    }
    atomic_store(&p->edges_at_abort, atomic_load(&p->edges));
    atomic_store(&p->abort_rc, kl_http_response_stream_abort(p->res));
    atomic_store(&p->abort_again_rc, kl_http_response_stream_abort(p->res));
    atomic_store(&p->write_after_abort, (int)kl_http_response_stream_try_write(p->res, "x", 1));
    atomic_store(&p->end_after_abort, kl_http_response_end_stream(p->res));
}

static void producer_tick(void *ud) {
    Producer *p = ud;
    p->timer_armed = 0;
    if (atomic_load(&p->closed)) { atomic_fetch_add(&p->wrote_after_close, 1); return; }
    if (p->kind == K_SSE) {
        char data[64];
        int n = snprintf(data, sizeof(data), "tick %d", p->sent_events);
        if (kl_http_sse_event(&p->sse, "tick", data, (size_t)n, NULL) == 0) p->sent_events++;
        if (p->sent_events == EVENTS) { kl_http_sse_end(&p->sse); return; }
        producer_arm(p, 2);
        return;
    }
    static char chunk[CHUNK];
    memset(chunk, 'f', sizeof(chunk));
    for (;;) {
        if (p->kind == K_FINITE && p->written == p->total) {
            (void)kl_http_response_end_stream(p->res);
            return;
        }
        size_t n = CHUNK;
        if (p->kind == K_FINITE && p->total - p->written < n) n = p->total - p->written;
        KlHttpStreamStatus st = kl_http_response_stream_try_write(p->res, chunk, n);
        atomic_store(&p->pending_now, kl_http_response_stream_pending(p->res));
        if (st == KL_HTTP_STREAM_ACCEPTED) {
            p->written += n;
            atomic_store(&p->stalled, 0);
            continue;
        }
        if (st == KL_HTTP_STREAM_WOULD_BLOCK) {
            atomic_fetch_add(&p->would_block, 1);
            atomic_store(&p->stalled, 1);
            p->edges_at_stall = atomic_load(&p->edges);
            if (p->abort_on_stall && !p->abort_armed) {
                p->abort_armed = 1;
                (void)kl_timer_add(ev(), 30, abort_tick, p);
            }
        }
        return;                                 /* WOULD_BLOCK: the edge; else the close follows */
    }
}

static void begin(KlHttpResponse *res, Producer *p) {
    if (p->kind == K_SSE) {
        if (kl_http_sse_begin(res, &p->sse) < 0) return;
    } else {
        KlHttpResponseWriteFn w; void *wctx;
        kl_http_response_header(res, "Content-Type", "application/octet-stream");
        if (kl_http_response_begin_stream(res, 200, &w, &wctx) < 0) return;
    }
    p->res = res;
    kl_http_response_stream_on_close(res, producer_closed, p);
    kl_http_response_stream_on_writable(res, producer_writable, p);
    producer_arm(p, 0);
}

static void handle(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)req;
    begin(res, ud);
}

static void *srv_thread(void *a) { (void)a; kl_http_server_run(&g_srv); return NULL; }

/* The session's input, counted: frames the server read, whatever its output is doing. */
static atomic_size_t g_fed;
static kl_ssize_t (*g_real_recv)(KlHttp2ServerSession *, const void *, size_t);
static kl_ssize_t counting_recv(KlHttp2ServerSession *s, const void *d, size_t n) {
    kl_ssize_t r = g_real_recv(s, d, n);
    if (r > 0) atomic_fetch_add(&g_fed, (size_t)r);
    return r;
}
static KlHttp2ServerSession *counting_factory(KlAllocator *a, KlHttp2ServerCallbacks *cb, void *ud) {
    KlHttp2ServerSession *s = kl_http2_nghttp2_server_session(a, cb, ud);
    if (s) {
        g_real_recv = s->recv;
        s->recv = counting_recv;
    }
    return s;
}

/* ── client side ──────────────────────────────────────────────────────────────────────────────── */

typedef struct {
    CliSock fd;
    nghttp2_session *ng;
    int32_t events_id, stuck_id, flood_id, victim_id, slow_id;
    size_t events_bytes, stuck_bytes, flood_bytes, victim_bytes, slow_bytes;
    size_t events_at_stuck_close;
    int events_ended, flood_ended, slow_ended;
    int stuck_closed, victim_closed;
    uint32_t stuck_code;
    int statuses_ok;
    char events_tail[64];
} Cli;

static Cli c;

static ssize_t cli_send(nghttp2_session *s, const uint8_t *d, size_t n, int fl, void *ud) {
    (void)s; (void)fl; (void)ud;
    int len = n > 65536 ? 65536 : (int)n;
    int w = (int)send(c.fd, (const char *)d, len, 0);
    if (w < 0) return cli_would_block() ? NGHTTP2_ERR_WOULDBLOCK : NGHTTP2_ERR_CALLBACK_FAILURE;
    return w;
}

static int cli_on_header(nghttp2_session *s, const nghttp2_frame *f, const uint8_t *name, size_t nl,
                         const uint8_t *value, size_t vl, uint8_t fl, void *ud) {
    (void)s; (void)f; (void)fl; (void)ud;
    if (nl == 7 && memcmp(name, ":status", 7) == 0 && !(vl == 3 && memcmp(value, "200", 3) == 0))
        c.statuses_ok = 0;
    return 0;
}

static int cli_on_data(nghttp2_session *s, uint8_t fl, int32_t sid, const uint8_t *d, size_t n,
                       void *ud) {
    (void)fl; (void)ud;
    nghttp2_session_consume_connection(s, n);   /* the connection window stays open */
    if (sid == c.events_id) {
        nghttp2_session_consume_stream(s, sid, n);
        c.events_bytes += n;
        size_t keep = n < sizeof(c.events_tail) - 1 ? n : sizeof(c.events_tail) - 1;
        memcpy(c.events_tail, d + n - keep, keep);
        c.events_tail[keep] = 0;
    } else if (sid == c.flood_id) {
        nghttp2_session_consume_stream(s, sid, n);
        c.flood_bytes += n;
    } else if (sid == c.stuck_id) {
        c.stuck_bytes += n;                     /* never opened again */
    } else if (sid == c.victim_id) {
        c.victim_bytes += n;                    /* never opened again */
    } else if (sid == c.slow_id) {
        c.slow_bytes += n;                      /* opened once, by hand (phase 2) */
    }
    return 0;
}

static int cli_on_frame_recv(nghttp2_session *s, const nghttp2_frame *f, void *ud) {
    (void)s; (void)ud;
    if ((f->hd.type == NGHTTP2_DATA || f->hd.type == NGHTTP2_HEADERS) &&
        (f->hd.flags & NGHTTP2_FLAG_END_STREAM)) {
        if (f->hd.stream_id == c.events_id) c.events_ended = 1;
        if (f->hd.stream_id == c.flood_id) c.flood_ended = 1;
        if (f->hd.stream_id == c.slow_id) c.slow_ended = 1;
    }
    return 0;
}

static int cli_on_close(nghttp2_session *s, int32_t sid, uint32_t code, void *ud) {
    (void)s; (void)ud;
    if (sid == c.stuck_id) {
        c.stuck_closed = 1;
        c.stuck_code = code;
        c.events_at_stuck_close = c.events_bytes;
    }
    if (sid == c.victim_id) c.victim_closed = 1;
    return 0;
}

/* Pump for up to `ms` (or until *until): send what the session has, and read only when `reading`. */
static int cli_pump(int ms, const int *until, int reading) {
    for (int waited = 0; waited < ms && !(until && *until); waited += 5) {
        if (nghttp2_session_send(c.ng) != 0) return -1;
        if (!reading) { nap(5); continue; }
        if (cli_poll_in(c.fd, 5) > 0) {
            char buf[16384];
            int n = (int)recv(c.fd, buf, (int)sizeof(buf), 0);
            if (n == 0) return -1;
            if (n < 0) { if (cli_would_block()) continue; return -1; }
            if (nghttp2_session_mem_recv(c.ng, (const uint8_t *)buf, (size_t)n) < 0) return -1;
        }
    }
    return nghttp2_session_send(c.ng) == 0 ? 0 : -1;
}

/* Wait (not reading) for a server-side flag. */
static int wait_flag(atomic_int *flag, int want, int ms) {
    for (int waited = 0; waited < ms; waited += 5) {
        if (atomic_load(flag) == want) return 1;
        if (nghttp2_session_send(c.ng) != 0) return 0;
        nap(5);
    }
    return atomic_load(flag) == want;
}

static int32_t cli_get(const char *path) {
    const nghttp2_nv nva[] = {
        { (uint8_t *)":method", (uint8_t *)"GET", 7, 3, NGHTTP2_NV_FLAG_NONE },
        { (uint8_t *)":scheme", (uint8_t *)"http", 7, 4, NGHTTP2_NV_FLAG_NONE },
        { (uint8_t *)":authority", (uint8_t *)"localhost", 10, 9, NGHTTP2_NV_FLAG_NONE },
        { (uint8_t *)":path", (uint8_t *)path, 5, strlen(path), NGHTTP2_NV_FLAG_NONE },
    };
    return nghttp2_submit_request(c.ng, NULL, nva, 4, NULL, NULL);
}

static int run_client(void) {
    /* 1. Abort a window-stalled stream while its sibling streams. */
    c.events_id = cli_get("/events");
    c.stuck_id = cli_get("/stuck");
    for (int i = 0; i < 600 && !(c.events_ended && c.stuck_closed); i++)
        if (cli_pump(10, NULL, 1) < 0) return fail("client I/O (abort)");
    for (int i = 0; i < 100 && (!atomic_load(&g_stuck.closed) || !atomic_load(&g_events.closed)); i++)
        cli_pump(10, NULL, 1);
    printf("abort: /stuck %zu bytes, closed %d (code %u), producer closed %d completed %d, abort %d "
           "again %d write %d end %d, edges %d (%d at the abort); /events %zu bytes (%zu at the reset), ended %d\n",
           c.stuck_bytes, c.stuck_closed, c.stuck_code, atomic_load(&g_stuck.closed),
           atomic_load(&g_stuck.completed), atomic_load(&g_stuck.abort_rc),
           atomic_load(&g_stuck.abort_again_rc), atomic_load(&g_stuck.write_after_abort),
           atomic_load(&g_stuck.end_after_abort), atomic_load(&g_stuck.edges),
           atomic_load(&g_stuck.edges_at_abort), c.events_bytes,
           c.events_at_stuck_close, c.events_ended);
    if (!c.statuses_ok) return fail("a response was not 200");
    if (atomic_load(&g_stuck.would_block) == 0) return fail("/stuck's producer was never refused");
    if (!c.stuck_closed || c.stuck_code != NGHTTP2_CANCEL) return fail("/stuck not reset with CANCEL");
    if (atomic_load(&g_stuck.abort_rc) != 0) return fail("abort refused");
    if (atomic_load(&g_stuck.abort_again_rc) != -1) return fail("a second abort accepted");
    if (atomic_load(&g_stuck.write_after_abort) != KL_HTTP_STREAM_ERROR)
        return fail("a write after the abort was not refused");
    if (atomic_load(&g_stuck.end_after_abort) != -1) return fail("an end after the abort succeeded");
    if (atomic_load(&g_stuck.edges_at_abort) < 0) return fail("/stuck was never aborted");
    if (atomic_load(&g_stuck.edges) != atomic_load(&g_stuck.edges_at_abort))
        return fail("/stuck got a writable edge after its abort");
    if (atomic_load(&g_stuck.closed) != 1 || atomic_load(&g_stuck.completed) != 0)
        return fail("/stuck's producer not told completed=0 exactly once");
    if (c.stuck_bytes > INITIAL_WINDOW) return fail("/stuck exceeded its window");
    if (!c.events_ended || !strstr(c.events_tail, "tick 149")) return fail("/events did not finish");
    if (c.events_bytes <= c.events_at_stuck_close) return fail("/events stopped at the abort");
    if (atomic_load(&g_events.closed) != 1 || atomic_load(&g_events.completed) != 1)
        return fail("/events' producer not completed exactly once");

    /* 2. Output held; the client's WINDOW_UPDATE and RST_STREAM must still be read. */
    c.flood_id = cli_get("/flood");
    c.victim_id = cli_get("/victim");
    c.slow_id = cli_get("/slow");
    if (nghttp2_session_send(c.ng) != 0 ||
        nghttp2_session_set_local_window_size(c.ng, NGHTTP2_FLAG_NONE, 0, BIG_WINDOW) != 0 ||
        nghttp2_session_set_local_window_size(c.ng, NGHTTP2_FLAG_NONE, c.flood_id, BIG_WINDOW) != 0)
        return fail("open /flood's windows");
    for (int i = 0; i < 1000 && !(c.flood_bytes > (1u << 20) && atomic_load(&g_victim.stalled) &&
                                  atomic_load(&g_slow.stalled)); i++)
        if (cli_pump(10, NULL, 1) < 0) return fail("client I/O (start)");
    if (c.flood_bytes <= (1u << 20) || !atomic_load(&g_victim.stalled) || !atomic_load(&g_slow.stalled))
        return fail("streams did not start (test premise)");
    size_t slow_before = c.slow_bytes;

    /* Stop reading: the socket fills, then the server's output, and /flood's producer is refused. */
    if (!wait_flag(&g_flood.stalled, 1, 10000)) return fail("/flood never blocked (test premise)");
    nap(200);
    if (!atomic_load(&g_flood.stalled)) return fail("/flood resumed with the client not reading");
    /* A receive the server may have posted before the stall takes this PING; the frames that count
     * come after it. */
    nghttp2_submit_ping(c.ng, NGHTTP2_FLAG_NONE, NULL);
    cli_pump(300, NULL, 0);
    size_t fed0 = atomic_load(&g_fed);

    if (nghttp2_submit_window_update(c.ng, NGHTTP2_FLAG_NONE, c.slow_id, (int32_t)SLOW_TOTAL) != 0)
        return fail("submit WINDOW_UPDATE");
    cli_pump(0, NULL, 0);
    int wu_read = 0;
    for (int i = 0; i < 600 && !wu_read; i++) {
        wu_read = atomic_load(&g_fed) >= fed0 + 13;   /* a WINDOW_UPDATE frame is 13 bytes */
        if (!wu_read) nap(5);
    }
    int flood_held = atomic_load(&g_flood.stalled) && !atomic_load(&g_flood.closed) &&
                     atomic_load(&g_flood.pending_now) > 0;
    printf("input while output held: WINDOW_UPDATE read %d (fed %zu -> %zu), /flood still held %d "
           "(%zu of %zu bytes at the client)\n", wu_read, fed0, (size_t)atomic_load(&g_fed),
           flood_held, c.flood_bytes, FLOOD_TOTAL);
    if (!wu_read) return fail("WINDOW_UPDATE not read while output was held");
    if (!flood_held) return fail("/flood's output drained with the client not reading (test premise)");

    nghttp2_submit_rst_stream(c.ng, NGHTTP2_FLAG_NONE, c.victim_id, NGHTTP2_CANCEL);
    cli_pump(0, NULL, 0);
    int victim_cancelled = wait_flag(&g_victim.closed, 1, 3000);
    printf("input while output held: /victim's producer closed %d (completed %d), /flood held at "
           "that moment %d\n", atomic_load(&g_victim.closed), atomic_load(&g_victim.completed),
           atomic_load(&g_victim_saw_flood_held));
    if (!victim_cancelled) return fail("RST_STREAM not read while output was held");
    if (atomic_load(&g_victim.completed) != 0) return fail("/victim's producer told completed");
    if (!atomic_load(&g_victim_saw_flood_held))
        return fail("/victim cancelled only once /flood's output drained");

    /* Read again: everything held arrives. */
    for (int i = 0; i < 6000 && !(c.flood_ended && c.slow_ended); i++)
        if (cli_pump(10, NULL, 1) < 0) return fail("client I/O (drain)");
    for (int i = 0; i < 100 && (!atomic_load(&g_flood.closed) || !atomic_load(&g_slow.closed)); i++)
        cli_pump(10, NULL, 1);
    printf("drain: /flood %zu of %zu bytes, ended %d, producer completed %d; /slow %zu of %zu "
           "(%zu before the update), ended %d, producer completed %d; /victim %zu bytes\n",
           c.flood_bytes, FLOOD_TOTAL, c.flood_ended, atomic_load(&g_flood.completed),
           c.slow_bytes, SLOW_TOTAL, slow_before, c.slow_ended, atomic_load(&g_slow.completed),
           c.victim_bytes);
    if (!c.flood_ended || c.flood_bytes != FLOOD_TOTAL) return fail("/flood did not arrive whole");
    if (!c.slow_ended || c.slow_bytes != SLOW_TOTAL) return fail("/slow did not arrive whole");
    if (atomic_load(&g_flood.closed) != 1 || atomic_load(&g_flood.completed) != 1)
        return fail("/flood's producer not completed exactly once");
    if (atomic_load(&g_slow.closed) != 1 || atomic_load(&g_slow.completed) != 1)
        return fail("/slow's producer not completed exactly once");
    if (atomic_load(&g_victim.closed) != 1) return fail("/victim's producer closed more than once");
    if (c.victim_bytes > INITIAL_WINDOW) return fail("/victim exceeded its window");
    Producer *all[] = { &g_events, &g_stuck, &g_flood, &g_victim, &g_slow };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++)
        if (atomic_load(&all[i]->wrote_after_close)) return fail("a producer ran after its close");
    return 0;
}

int main(void) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return fail("WSAStartup");
#endif
    g_events.kind = K_SSE;
    g_stuck.kind = K_ENDLESS;
    g_stuck.abort_on_stall = 1;
    g_flood.kind = K_FINITE;
    g_flood.total = FLOOD_TOTAL;
    g_victim.kind = K_ENDLESS;
    g_slow.kind = K_FINITE;
    g_slow.total = SLOW_TOTAL;
    atomic_store(&g_stuck.abort_rc, 99);
    atomic_store(&g_stuck.abort_again_rc, 99);
    atomic_store(&g_stuck.write_after_abort, 99);
    atomic_store(&g_stuck.end_after_abort, 99);
    atomic_store(&g_stuck.edges_at_abort, -1);

    KlHttp2ServerConfig h2cfg = { .factory = counting_factory };
    KlHttpServerConfig cfg = { .port = 0, .bind_addr = "127.0.0.1", .h2 = &h2cfg,
                               .stream_capacity = CAPACITY };
    if (kl_http_server_init(&g_srv, &cfg) < 0) return fail("server init");
    kl_http_server_route(&g_srv, "GET", "/events", handle, &g_events, NULL);
    kl_http_server_route(&g_srv, "GET", "/stuck", handle, &g_stuck, NULL);
    kl_http_server_route(&g_srv, "GET", "/flood", handle, &g_flood, NULL);
    kl_http_server_route(&g_srv, "GET", "/victim", handle, &g_victim, NULL);
    kl_http_server_route(&g_srv, "GET", "/slow", handle, &g_slow, NULL);
    pthread_t tid;
    if (pthread_create(&tid, NULL, srv_thread, NULL) != 0) return fail("server thread");
    for (int i = 0; i < 400 && g_srv.bound_port == 0; i++) nap(5);
    if (g_srv.bound_port <= 0) return fail("server never bound");

    memset(&c, 0, sizeof(c));
    c.statuses_ok = 1;
    c.events_id = c.stuck_id = c.flood_id = c.victim_id = c.slow_id = -1;
    c.fd = socket(AF_INET, SOCK_STREAM, 0);
    if (c.fd == CLI_BAD_SOCK) return fail("socket");
    int rcvbuf = 64 * 1024;                     /* a small receive buffer: the stall comes sooner */
    (void)setsockopt(c.fd, SOL_SOCKET, SO_RCVBUF, (const char *)&rcvbuf, sizeof(rcvbuf));
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)g_srv.bound_port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(c.fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) return fail("connect");
    if (cli_nonblock(c.fd) != 0) return fail("nonblocking");

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
    nghttp2_session_client_new2(&c.ng, cbs, NULL, opt);
    nghttp2_option_del(opt);
    nghttp2_session_callbacks_del(cbs);
    nghttp2_settings_entry iv[] = { { NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, INITIAL_WINDOW } };
    nghttp2_submit_settings(c.ng, NGHTTP2_FLAG_NONE, iv, 1);

    int rc = run_client();

    nghttp2_session_del(c.ng);
    cli_close(c.fd);
    kl_http_server_stop(&g_srv);
    pthread_join(tid, NULL);
    kl_http_server_free(&g_srv);
    if (rc == 0) printf("nghttp2 stream control: abort and input while output is held OK\n");
    return rc;
}
