/*
 * test_http_client_backpressure.c: response-body pause/resume on the async HTTP/1.1 client.
 *
 * A body callback that returns KL_HTTP_CLIENT_BODY_PAUSE accepts its fragment and stops delivery
 * until kl_http_client_resume(). These tests drive the public API against a raw loopback peer (a
 * thread), so each case controls exactly which bytes arrive, in which writes, and whether the peer
 * then closes or holds the connection. They check:
 *
 *   - delivery: the body is reconstructed byte for byte across Content-Length, chunked (with
 *     trailers) and close-delimited framing; a pause mid-read, on the first or the final fragment,
 *     on every fragment; nothing (no body callback, no on_complete, no on_done) while paused;
 *     resume never calls back synchronously; resume works with no new socket event;
 *   - decompression: a test codec that turns each compressed byte into several output fragments
 *     and emits more output while flushing, so a pause lands mid-feed and mid-flush;
 *   - TLS: a TLS engine that holds decrypted plaintext back (pending), so the resume must read it
 *     without a socket event;
 *   - transport backpressure: a producer peer counts the bytes it manages to write; while the client
 *     is paused that count stops growing, well short of the body, and the client's allocations stay
 *     bounded however large the body;
 *   - lifecycle: cancel / free while paused, cancel from a callback, resume twice, resume after the
 *     end, resume from inside the pausing callback, the request deadline while paused, a resume that
 *     cannot be scheduled, a watcher that cannot be re-registered, and the sync client's PAUSE.
 *
 * The suite runs on whichever engine the library was built with (readiness: epoll / kqueue / poll /
 * WSAPoll; completion: io_uring / IOCP / pollcomp), so the same expectations hold on both axes.
 */
#include "utest.h"
#include <keel/keel.h>
#include <keel/http_client.h>
#include <keel/decompress.h>
#include <keel/timer.h>
#include <keel/tls.h>
#include "net_compat.h"
#include "platform_thread.h"
#include "platform_socket.h"
#include "kl_atomic.h"
#include "event_caps.h" /* kl_event_caps: skip a readiness-only fault injection */
#include "socket.h"   /* kl_sockdef_recv / kl_sockdef_send / kl_sockdef_io_status (mock TLS) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── A scripted raw peer ─────────────────────────────────────────── */

#define MAX_PARTS 64

typedef struct {
    const char *data;
    size_t      len;
    unsigned    delay_ms;   /* before this part */
} Part;

typedef struct {
    KlSocketHandle listen_fd;
    int            port;
    Part           parts[MAX_PARTS];
    int            nparts;
    int            hold;          /* keep the connection open after the script (no EOF) */
    /* Producer mode: after `parts` (the headers), write `produce` bytes of pattern body. */
    size_t         produce;
    int            produced;      /* bytes written so far (atomic) */
    int            stop;          /* release a holding peer (atomic) */
    KlPlatThread   tid;
} Peer;

static char pattern_at(size_t i) { return (char)('a' + (i * 7 + i / 13) % 26); }

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

static void peer_add(Peer *p, const char *data, size_t len, unsigned delay_ms) {
    p->parts[p->nparts].data = data;
    p->parts[p->nparts].len = len;
    p->parts[p->nparts].delay_ms = delay_ms;
    p->nparts++;
}
static void peer_add_str(Peer *p, const char *s) { peer_add(p, s, strlen(s), 0); }

static int write_all(KlSocketHandle c, const char *d, size_t n) {
    while (n > 0) {
        long w = kl_test_sockwrite(c, d, n);
        if (w <= 0) return -1;
        d += w;
        n -= (size_t)w;
    }
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
    for (int i = 0; i < p->nparts; i++) {
        if (p->parts[i].delay_ms) kl_test_sleep_ms(p->parts[i].delay_ms);
        if (write_all(c, p->parts[i].data, p->parts[i].len) != 0) goto out;
    }
    if (p->produce) {
        char chunk[16384];
        size_t off = 0;
        while (off < p->produce) {
            size_t n = p->produce - off < sizeof chunk ? p->produce - off : sizeof chunk;
            for (size_t k = 0; k < n; k++) chunk[k] = pattern_at(off + k);
            if (write_all(c, chunk, n) != 0) goto out;   /* blocks while the client's window is shut */
            off += n;
            kl_atomic_store_int(&p->produced, (int)off);
        }
    }
    if (p->hold)   /* hold until the client closes or the test releases it */
        while (!kl_atomic_load_int(&p->stop) && kl_test_poll1(c, 0, 50) >= 0) {
            if (kl_test_poll1(c, 0, 0) > 0 && kl_test_sockread(c, buf, sizeof(buf)) <= 0) break;
        }
out:
    kl_test_closesock(c);
}

static void peer_start(Peer *p) { kl_plat_thread_create(&p->tid, peer_thread, p); }

static void peer_finish(Peer *p) {
    kl_atomic_store_int(&p->stop, 1);
    kl_plat_thread_join(&p->tid);
    kl_test_closesock(p->listen_fd);
}

static const char *url_for(const Peer *p, const char *scheme) {
    static char url[64];
    snprintf(url, sizeof(url), "%s://127.0.0.1:%d/", scheme, p->port);
    return url;
}

/* ── A counting allocator (peak bytes outstanding; optional failure) ─ */

typedef struct {
    size_t cur, peak;
    int    fail;   /* every allocation fails while set */
} Count;

static void *cnt_malloc(void *ctx, size_t n) {
    Count *c = ctx;
    if (c->fail) return NULL;
    void *p = malloc(n);
    if (p) { c->cur += n; if (c->cur > c->peak) c->peak = c->cur; }
    return p;
}
static void *cnt_realloc(void *ctx, void *p, size_t old, size_t n) {
    Count *c = ctx;
    if (c->fail) return NULL;
    void *q = realloc(p, n);
    if (q) { c->cur = c->cur - old + n; if (c->cur > c->peak) c->peak = c->cur; }
    return q;
}
static void cnt_free(void *ctx, void *p, size_t n) {
    Count *c = ctx;
    if (p) c->cur -= n;
    free(p);
}
static KlAllocator counting_alloc(Count *c) {
    memset(c, 0, sizeof(*c));
    KlAllocator a = { .malloc = cnt_malloc, .realloc = cnt_realloc, .free = cnt_free, .ctx = c };
    return a;
}

/* ── The consumer ────────────────────────────────────────────────── */

typedef struct {
    char        *got;
    size_t       len, cap;
    int          calls;
    int          pause_every;     /* pause after every Nth fragment (0 = never) */
    int          paused;          /* the last callback returned PAUSE and no resume was issued */
    int          completes;       /* stream on_complete calls */
    int          done;            /* on_done calls */
    KlError      err;
    int          status;
    int          zero_len_seen;
    int          cb_while_paused; /* a callback arrived while paused: must never happen */
    int          in_resume_call;  /* set around kl_http_client_resume: a callback here is re-entrant */
    int          reentered;
    /* lifecycle hooks */
    KlHttpClient *client;
    int          cancel_at;       /* cancel from the Nth callback (0 = never) */
    int          resume_inside;   /* call kl_http_client_resume from the pausing callback */
} Consumer;

static int on_body(const char *data, size_t len, void *ud) {
    Consumer *k = ud;
    if (k->paused) k->cb_while_paused = 1;
    if (k->in_resume_call) k->reentered = 1;
    if (len == 0) k->zero_len_seen = 1;
    if (k->len + len > k->cap) {
        size_t nc = k->cap ? k->cap : 1024;
        while (nc < k->len + len) nc *= 2;
        k->got = realloc(k->got, nc);
        k->cap = nc;
    }
    memcpy(k->got + k->len, data, len);
    k->len += len;
    k->calls++;
    if (k->cancel_at && k->calls == k->cancel_at) {
        kl_http_client_cancel(k->client);
        return KL_HTTP_CLIENT_BODY_CONTINUE;   /* cancellation alone must stop delivery */
    }
    if (k->pause_every && k->calls % k->pause_every == 0) {
        if (k->resume_inside)
            (void)kl_http_client_resume(k->client);   /* takes effect once this returns PAUSE */
        else
            k->paused = 1;
        return KL_HTTP_CLIENT_BODY_PAUSE;
    }
    return KL_HTTP_CLIENT_BODY_CONTINUE;
}

static void on_complete(void *ud) {
    Consumer *k = ud;
    if (k->paused) k->cb_while_paused = 1;
    if (k->in_resume_call) k->reentered = 1;
    k->completes++;
}

static void on_done(KlHttpClient *c, void *ud) {
    Consumer *k = ud;
    if (k->paused && kl_http_client_last_error(c) == KL_ERR_NONE) k->cb_while_paused = 1;
    k->done++;
    k->err = kl_http_client_last_error(c);
    const KlHttpClientResponse *r = kl_http_client_response(c);
    k->status = r ? r->status : 0;
}

static void consumer_free(Consumer *k) { free(k->got); k->got = NULL; }

/* Resume a paused consumer: never re-entrant (no callback from inside the call). */
static int consumer_resume(Consumer *k) {
    k->paused = 0;
    k->in_resume_call = 1;
    int rc = kl_http_client_resume(k->client);
    k->in_resume_call = 0;
    return rc;
}

/* Drive the loop until done (or `max_ticks`). Each time the consumer pauses, spin `idle_ticks`
 * loop ticks first (nothing may arrive while paused), then resume. Returns the number of pauses. */
static int drive(KlEventCtx *ev, Consumer *k, int idle_ticks, int max_ticks) {
    int pauses = 0;
    for (int i = 0; i < max_ticks && !k->done; i++) {
        (void)kl_event_ctx_run(ev, 16, 5);
        if (k->paused && !k->done) {
            pauses++;
            for (int j = 0; j < idle_ticks && !k->done; j++) (void)kl_event_ctx_run(ev, 16, 2);
            if (k->done) break;   /* only a terminal error may end a paused request */
            int calls = k->calls, completes = k->completes;
            if (consumer_resume(k) != 0) break;
            if (k->calls != calls || k->completes != completes) k->reentered = 1;
        }
    }
    return pauses;
}

/* One full request against a scripted peer. */
typedef struct {
    const KlHttpClientConfig *cfg;
    const char *scheme;
    int idle_ticks;
} RunOpts;

static int run_request(Peer *p, Consumer *k, const RunOpts *o, KlAllocator *ca) {
    KlAllocator da = kl_allocator_default();
    KlAllocator *a = ca ? ca : &da;
    KlEventCtx ev;
    if (kl_event_ctx_init(&ev, &da) != 0) return -1;
    peer_start(p);
    KlHttpClientStreamCfg sc;
    memset(&sc, 0, sizeof(sc));
    sc.on_body = on_body;
    sc.on_complete = on_complete;
    sc.user_data = k;
    KlHttpClientConfig defcfg;
    memset(&defcfg, 0, sizeof(defcfg));
    defcfg.timeout_ms = 10000;
    k->client = kl_http_client_start_s(&ev, a, o && o->cfg ? o->cfg : &defcfg, "GET",
                                       url_for(p, o && o->scheme ? o->scheme : "http"),
                                       NULL, 0, NULL, 0, &sc, on_done, k);
    int pauses = -1;
    if (k->client) {
        pauses = drive(&ev, k, o ? o->idle_ticks : 3, 20000);
        kl_http_client_free(k->client);
        k->client = NULL;
    }
    kl_event_ctx_free(&ev);
    peer_finish(p);
    return pauses;
}

#define ASSERT_CLEAN(k) do { \
    ASSERT_EQ((k).cb_while_paused, 0); \
    ASSERT_EQ((k).reentered, 0); \
    ASSERT_EQ((k).zero_len_seen, 0); \
} while (0)

/* ── Delivery ────────────────────────────────────────────────────── */

#define CL_REPLY "HTTP/1.1 200 OK\r\nContent-Length: 26\r\n\r\nabcdefghijklmnopqrstuvwxyz"

/* Many chunks in ONE write: a single read holds many body fragments, chunk boundaries, trailers and
 * the end of the message, so every pause leaves a residual that must be resumed without any socket
 * event (the peer holds the connection open and sends nothing more). */
#define CHUNKED_REPLY "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n" \
    "3\r\nabc\r\n1\r\nd\r\n5\r\nefghi\r\n2\r\njk\r\n4\r\nlmno\r\n" \
    "6\r\npqrstu\r\n5\r\nvwxyz\r\n0\r\nX-Trailer: t\r\n\r\n"

UTEST(delivery, chunked_pause_every_fragment_one_read) {
    static Peer p;
    ASSERT_EQ(peer_listen(&p), 0);
    peer_add_str(&p, CHUNKED_REPLY);
    p.hold = 1;
    Consumer k;
    memset(&k, 0, sizeof k);
    k.pause_every = 1;
    int pauses = run_request(&p, &k, NULL, NULL);
    ASSERT_EQ(k.done, 1);
    ASSERT_EQ(k.err, KL_ERR_NONE);
    ASSERT_EQ(k.status, 200);
    ASSERT_EQ(k.len, (size_t)26);
    ASSERT_EQ(memcmp(k.got, "abcdefghijklmnopqrstuvwxyz", 26), 0);
    ASSERT_EQ(k.calls, 7);
    ASSERT_EQ(pauses, 7);          /* every fragment paused, the last one included */
    ASSERT_EQ(k.completes, 1);
    ASSERT_CLEAN(k);
    consumer_free(&k);
}

UTEST(delivery, chunked_pause_mid_read_then_continue) {
    static Peer p;
    ASSERT_EQ(peer_listen(&p), 0);
    peer_add_str(&p, CHUNKED_REPLY);
    p.hold = 1;
    Consumer k;
    memset(&k, 0, sizeof k);
    k.pause_every = 3;             /* pause in the middle of the read, then run to the end */
    int pauses = run_request(&p, &k, NULL, NULL);
    ASSERT_EQ(k.done, 1);
    ASSERT_EQ(k.err, KL_ERR_NONE);
    ASSERT_EQ(k.len, (size_t)26);
    ASSERT_EQ(memcmp(k.got, "abcdefghijklmnopqrstuvwxyz", 26), 0);
    ASSERT_EQ(pauses, 2);
    ASSERT_CLEAN(k);
    consumer_free(&k);
}

UTEST(delivery, content_length_pause_on_only_and_final_fragment) {
    static Peer p;
    ASSERT_EQ(peer_listen(&p), 0);
    peer_add_str(&p, CL_REPLY);
    p.hold = 1;   /* no EOF: completion must come from the parser on resume, with zero bytes left */
    Consumer k;
    memset(&k, 0, sizeof k);
    k.pause_every = 1;
    int pauses = run_request(&p, &k, NULL, NULL);
    ASSERT_EQ(k.done, 1);
    ASSERT_EQ(k.err, KL_ERR_NONE);
    ASSERT_EQ(pauses, 1);
    ASSERT_EQ(k.calls, 1);
    ASSERT_EQ(k.len, (size_t)26);
    ASSERT_EQ(k.completes, 1);
    ASSERT_CLEAN(k);
    consumer_free(&k);
}

UTEST(delivery, close_delimited_fragmented_headers) {
    static Peer p;
    ASSERT_EQ(peer_listen(&p), 0);
    /* Headers split mid-line, then a close-delimited body in several writes, then EOF. */
    peer_add(&p, "HTTP/1.1 200 OK\r\nConn", 21, 0);
    peer_add(&p, "ection: close\r\n\r", 16, 20);
    peer_add(&p, "\nabcdefgh", 9, 20);
    peer_add(&p, "ijklmnop", 8, 20);
    peer_add(&p, "qrstuvwxyz", 10, 20);
    Consumer k;
    memset(&k, 0, sizeof k);
    k.pause_every = 1;
    RunOpts o = { NULL, NULL, 10 };   /* long pauses: the EOF arrives while paused */
    (void)run_request(&p, &k, &o, NULL);
    ASSERT_EQ(k.done, 1);
    ASSERT_EQ(k.err, KL_ERR_NONE);
    ASSERT_EQ(k.len, (size_t)26);
    ASSERT_EQ(memcmp(k.got, "abcdefghijklmnopqrstuvwxyz", 26), 0);
    ASSERT_EQ(k.completes, 1);
    ASSERT_CLEAN(k);
    consumer_free(&k);
}

UTEST(delivery, chunk_boundaries_split_across_writes) {
    static Peer p;
    ASSERT_EQ(peer_listen(&p), 0);
    peer_add_str(&p, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1");
    peer_add(&p, "0\r\nabcdefgh", 11, 15);       /* chunk size line split: "1" + "0" = 16 */
    peer_add(&p, "ijklmnop\r", 9, 15);
    peer_add(&p, "\na\r\nqrstuvwxyz\r\n0\r", 18, 15);
    peer_add(&p, "\nX-T: 1\r\n\r\n", 11, 15);
    Consumer k;
    memset(&k, 0, sizeof k);
    k.pause_every = 1;
    (void)run_request(&p, &k, NULL, NULL);
    ASSERT_EQ(k.done, 1);
    ASSERT_EQ(k.err, KL_ERR_NONE);
    ASSERT_EQ(k.len, (size_t)26);
    ASSERT_EQ(memcmp(k.got, "abcdefghijklmnopqrstuvwxyz", 26), 0);
    ASSERT_CLEAN(k);
    consumer_free(&k);
}

UTEST(delivery, zero_length_body) {
    static Peer p;
    ASSERT_EQ(peer_listen(&p), 0);
    peer_add_str(&p, "HTTP/1.1 204 No Content\r\n\r\n");
    p.hold = 1;
    Consumer k;
    memset(&k, 0, sizeof k);
    k.pause_every = 1;
    int pauses = run_request(&p, &k, NULL, NULL);
    ASSERT_EQ(k.done, 1);
    ASSERT_EQ(k.err, KL_ERR_NONE);
    ASSERT_EQ(k.status, 204);
    ASSERT_EQ(k.calls, 0);
    ASSERT_EQ(pauses, 0);
    ASSERT_EQ(k.completes, 1);
    consumer_free(&k);
}

UTEST(delivery, continue_only_callers_unchanged) {
    static Peer p;
    ASSERT_EQ(peer_listen(&p), 0);
    peer_add_str(&p, CHUNKED_REPLY);
    p.hold = 1;
    Consumer k;
    memset(&k, 0, sizeof k);   /* never pauses: the pre-existing contract */
    int pauses = run_request(&p, &k, NULL, NULL);
    ASSERT_EQ(k.done, 1);
    ASSERT_EQ(k.err, KL_ERR_NONE);
    ASSERT_EQ(pauses, 0);
    ASSERT_EQ(k.len, (size_t)26);
    ASSERT_EQ(memcmp(k.got, "abcdefghijklmnopqrstuvwxyz", 26), 0);
    ASSERT_EQ(k.zero_len_seen, 0);
    consumer_free(&k);
}

static int abort_ret;
static int abort_on_body(const char *d, size_t n, void *ud) {
    (void)d; (void)n;
    (*(int *)ud)++;
    return abort_ret;
}

/* ABORT (-1), and any value that is neither CONTINUE nor PAUSE, fails the request as before. */
static void run_abort(int ret, int *calls, int *done, KlError *err) {
    static Peer p;
    *calls = 0; *done = 0; *err = KL_ERR_NONE;
    if (peer_listen(&p) != 0) return;
    peer_add_str(&p, CHUNKED_REPLY);
    p.hold = 1;
    abort_ret = ret;
    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    if (kl_event_ctx_init(&ev, &a) != 0) return;
    peer_start(&p);
    Consumer k;
    memset(&k, 0, sizeof k);
    KlHttpClientStreamCfg sc;
    memset(&sc, 0, sizeof sc);
    sc.on_body = abort_on_body;
    sc.user_data = calls;
    KlHttpClient *c = kl_http_client_start_s(&ev, &a, NULL, "GET", url_for(&p, "http"), NULL, 0,
                                             NULL, 0, &sc, on_done, &k);
    for (int i = 0; c && i < 400 && !k.done; i++) (void)kl_event_ctx_run(&ev, 16, 5);
    *done = k.done;
    *err = k.err;
    kl_http_client_free(c);
    kl_event_ctx_free(&ev);
    peer_finish(&p);
}

UTEST(delivery, abort_unchanged) {
    int rets[] = { KL_HTTP_CLIENT_BODY_ABORT, 2, 21 /* llhttp's HPE_PAUSED value */, -7 };
    for (size_t i = 0; i < sizeof rets / sizeof rets[0]; i++) {
        int calls, done;
        KlError err;
        run_abort(rets[i], &calls, &done, &err);
        ASSERT_EQ(calls, 1);
        ASSERT_EQ(done, 1);
        ASSERT_EQ(err, KL_ERR_PARSE);
    }
}

/* ── Decompression: a codec whose every input byte yields several output fragments ── */

/* "x-rep": each input byte c becomes "cc" "cc" (two emits); the final flush emits "!!" then "??"
 * (two emits), or fails if a '#' was seen (a corrupt stream, as a bad checksum would be). */
typedef struct { KlDecompress base; KlAllocator *alloc; int bad; size_t fed; } RepDecomp;

static int rep_decompress(KlDecompress *s, const char *in, size_t n, char **out, size_t *on,
                          KlAllocator *a) {
    (void)s; (void)in; (void)n; (void)out; (void)on; (void)a;
    return -1;
}
static int rep_dfeed(KlDecompress *self, const char *d, size_t n, int flush,
                     int (*emit)(void *, const char *, size_t), void *ctx) {
    RepDecomp *r = (RepDecomp *)self;
    for (size_t i = 0; i < n; i++) {
        char two[2] = { d[i], d[i] };
        if (d[i] == '#') r->bad = 1;
        r->fed++;
        if (emit(ctx, two, 2) < 0 || emit(ctx, two, 2) < 0) return -1;
    }
    if (flush) {
        if (r->bad) return -1;
        if (emit(ctx, "!!", 2) < 0 || emit(ctx, "??", 2) < 0) return -1;
    }
    return 0;
}
static const char *rep_encoding(KlDecompress *s) { (void)s; return "x-rep"; }
static void rep_destroy(KlDecompress *s) {
    RepDecomp *r = (RepDecomp *)s;
    kl_free(r->alloc, r, sizeof(*r));
}
static KlDecompress *rep_factory(KlCompressCtx *ctx, KlAllocator *alloc) {
    (void)ctx;
    RepDecomp *r = kl_malloc(alloc, sizeof(*r));
    if (!r) return NULL;
    memset(r, 0, sizeof(*r));
    r->alloc = alloc;
    r->base.decompress = rep_decompress;
    r->base.dfeed = rep_dfeed;
    r->base.encoding = rep_encoding;
    r->base.destroy = rep_destroy;
    return &r->base;
}

static void expand_rep(const char *in, char *out) {   /* the expected decoded body */
    size_t o = 0;
    for (size_t i = 0; in[i]; i++) { for (int j = 0; j < 4; j++) out[o++] = in[i]; }
    memcpy(out + o, "!!??", 4);
    out[o + 4] = '\0';
}

#define REP_BODY "abcdefghij"
#define REP_REPLY "HTTP/1.1 200 OK\r\nContent-Encoding: x-rep\r\nTransfer-Encoding: chunked\r\n\r\n" \
    "4\r\nabcd\r\n6\r\nefghij\r\n0\r\n\r\n"

static void run_rep(Peer *p, Consumer *k, int pause_every) {
    KlDecompressConfig dc = { .ctx = NULL, .factory = rep_factory, .ctx_destroy = NULL };
    KlHttpClientConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.timeout_ms = 10000;
    cfg.decompress = &dc;
    memset(k, 0, sizeof *k);
    k->pause_every = pause_every;
    RunOpts o = { &cfg, NULL, 2 };
    (void)run_request(p, k, &o, NULL);
}

UTEST(decompress, pause_every_output_fragment_including_flush) {
    static Peer p;
    ASSERT_EQ(peer_listen(&p), 0);
    peer_add_str(&p, REP_REPLY);
    p.hold = 1;
    Consumer k;
    run_rep(&p, &k, 1);   /* every emit pauses: mid-feed, mid-slice and during the final flush */
    char want[64];
    expand_rep(REP_BODY, want);
    ASSERT_EQ(k.done, 1);
    ASSERT_EQ(k.err, KL_ERR_NONE);
    ASSERT_EQ(k.len, strlen(want));
    ASSERT_EQ(memcmp(k.got, want, k.len), 0);
    ASSERT_EQ(k.completes, 1);
    ASSERT_CLEAN(k);
    consumer_free(&k);
}

UTEST(decompress, pause_every_third_fragment) {
    static Peer p;
    ASSERT_EQ(peer_listen(&p), 0);
    peer_add_str(&p, REP_REPLY);
    p.hold = 1;
    Consumer k;
    run_rep(&p, &k, 3);
    char want[64];
    expand_rep(REP_BODY, want);
    ASSERT_EQ(k.done, 1);
    ASSERT_EQ(k.err, KL_ERR_NONE);
    ASSERT_EQ(k.len, strlen(want));
    ASSERT_EQ(memcmp(k.got, want, k.len), 0);
    ASSERT_CLEAN(k);
    consumer_free(&k);
}

UTEST(decompress, corrupt_stream_fails_after_paused_delivery) {
    static Peer p;
    ASSERT_EQ(peer_listen(&p), 0);
    peer_add_str(&p, "HTTP/1.1 200 OK\r\nContent-Encoding: x-rep\r\nContent-Length: 4\r\n\r\nab#c");
    p.hold = 1;
    Consumer k;
    run_rep(&p, &k, 1);
    ASSERT_EQ(k.done, 1);
    ASSERT_EQ(k.err, KL_ERR_COMPRESS);   /* the final check still runs, after every pause */
    ASSERT_EQ(k.len, (size_t)16);        /* all decoded output was delivered first */
    ASSERT_EQ(k.cb_while_paused, 0);
    consumer_free(&k);
}

/* A large compressed body: one read of compressed input spans many slices; the stash is used and
 * then released. */
UTEST(decompress, large_body_repeated_pause_bounded) {
    static Peer p;
    static char reply[64 * 1024];
    static char body[40000];
    ASSERT_EQ(peer_listen(&p), 0);
    for (size_t i = 0; i < sizeof body; i++) body[i] = (char)('A' + i % 23);
    int hl = snprintf(reply, sizeof reply,
                      "HTTP/1.1 200 OK\r\nContent-Encoding: x-rep\r\nContent-Length: %zu\r\n\r\n",
                      sizeof body);
    memcpy(reply + hl, body, sizeof body);
    peer_add(&p, reply, (size_t)hl + sizeof body, 0);
    p.hold = 1;
    Count cnt;
    KlAllocator ca = counting_alloc(&cnt);
    KlDecompressConfig dc = { .ctx = NULL, .factory = rep_factory, .ctx_destroy = NULL };
    KlHttpClientConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.timeout_ms = 20000;
    cfg.decompress = &dc;
    cfg.max_response_size = 1u << 20;
    Consumer k;
    memset(&k, 0, sizeof k);
    k.pause_every = 7;
    RunOpts o = { &cfg, NULL, 0 };
    (void)run_request(&p, &k, &o, &ca);
    ASSERT_EQ(k.done, 1);
    ASSERT_EQ(k.err, KL_ERR_NONE);
    ASSERT_EQ(k.len, sizeof body * 4 + 4);
    for (size_t i = 0; i < sizeof body; i++)
        ASSERT_EQ(k.got[i * 4 + 3], body[i]);
    ASSERT_EQ(memcmp(k.got + sizeof body * 4, "!!??", 4), 0);
    /* One slice (512 B) expands to 2 KiB here; the client holds far less than the 160 KB body. */
    ASSERT_LT(cnt.peak, (size_t)48 * 1024);
    ASSERT_EQ(cnt.cur, (size_t)0);
    ASSERT_CLEAN(k);
    consumer_free(&k);
}

/* ── Stoppable decoders (KlDecompress.dfeed_stoppable) ───────────── */

/* "x-srep": each input byte c becomes `expand` fragments of `frag` bytes of c; the final flush emits
 * "!!" then "??" and then validates (a '#' anywhere fails it, as a bad checksum would). With
 * `stoppable` it supplies dfeed_stoppable: a stop leaves the fragments still owed for the current
 * byte, and the flush's progress, inside the session. `misbehave` breaks the contract on purpose.
 * The configuration and the observations travel in the decoder context. */
enum { SR_OK, SR_STOP_UNASKED, SR_SHORT_DONE, SR_EMIT_AFTER_STOP, SR_OVER_CONSUME };
typedef struct {
    int    stoppable, misbehave;
    size_t expand, frag;
    int    calls;
    int    zero_input_resumes;     /* a call with no input that still had output to give */
    int    stops_with_input_left;  /* a stop that left part of the call's input untaken */
    int    flush_resumes;          /* a flush call continuing a stopped flush */
} SrepCfg;

typedef struct {
    KlDecompress  base;
    KlAllocator  *alloc;
    SrepCfg      *cfg;
    int           bad, fl_started, fl_owed;
    char          cur;
    size_t        owed;
} Srep;

static char g_srep_fill[16384];

static int srep_run(Srep *r, const char *d, size_t n, int flush, size_t *consumed,
                    int (*emit)(void *, const char *, size_t), void *ctx, int stoppable) {
    SrepCfg *c = r->cfg;
    c->calls++;
    if (n == 0 && !flush && r->owed > 0) c->zero_input_resumes++;
    if (flush && r->fl_started && r->fl_owed > 0) c->flush_resumes++;
    if (stoppable && c->misbehave == SR_STOP_UNASKED) { *consumed = 0; return 1; }
    size_t i = 0;
    for (;;) {
        while (r->owed > 0) {
            memset(g_srep_fill, r->cur, c->frag);
            r->owed--;
            int e = emit(ctx, g_srep_fill, c->frag);
            if (e < 0) return -1;
            if (stoppable && e > 0) {
                if (c->misbehave == SR_EMIT_AFTER_STOP && emit(ctx, g_srep_fill, 1) < 0) return -1;
                if (i < n) c->stops_with_input_left++;
                *consumed = c->misbehave == SR_OVER_CONSUME ? n + 1 : i;
                return 1;
            }
        }
        if (i >= n) break;
        r->cur = d[i++];
        if (r->cur == '#') r->bad = 1;
        r->owed = c->expand;
    }
    if (flush) {
        if (!r->fl_started) { r->fl_started = 1; r->fl_owed = 2; }
        while (r->fl_owed > 0) {
            const char *t = r->fl_owed == 2 ? "!!" : "??";
            r->fl_owed--;
            int e = emit(ctx, t, 2);
            if (e < 0) return -1;
            if (stoppable && e > 0) { *consumed = n; return 1; }
        }
        if (r->bad) return -1;
    }
    if (stoppable && c->misbehave == SR_SHORT_DONE && n > 1) { *consumed = n - 1; return 0; }
    *consumed = n;
    return 0;
}
static int srep_dfeed(KlDecompress *self, const char *d, size_t n, int flush,
                      int (*emit)(void *, const char *, size_t), void *ctx) {
    size_t consumed;
    return srep_run((Srep *)self, d, n, flush, &consumed, emit, ctx, 0);
}
static int srep_dfeed_stoppable(KlDecompress *self, const char *d, size_t n, int flush,
                                size_t *consumed, int (*emit)(void *, const char *, size_t),
                                void *ctx) {
    return srep_run((Srep *)self, d, n, flush, consumed, emit, ctx, 1);
}
static const char *srep_encoding(KlDecompress *s) { (void)s; return "x-srep"; }
static void srep_destroy(KlDecompress *s) { Srep *r = (Srep *)s; kl_free(r->alloc, r, sizeof(*r)); }
static KlDecompress *srep_factory(KlCompressCtx *ctx, KlAllocator *alloc) {
    Srep *r = kl_malloc(alloc, sizeof(*r));
    if (!r) return NULL;
    memset(r, 0, sizeof(*r));
    r->alloc = alloc;
    r->cfg = (SrepCfg *)(void *)ctx;
    r->base.decompress = rep_decompress;
    r->base.dfeed = srep_dfeed;
    r->base.encoding = srep_encoding;
    r->base.destroy = srep_destroy;
    if (r->cfg->stoppable) r->base.dfeed_stoppable = srep_dfeed_stoppable;
    return &r->base;
}

static void run_srep(Peer *p, Consumer *k, SrepCfg *sc, int pause_every, KlAllocator *ca) {
    KlDecompressConfig dc = { .ctx = (KlCompressCtx *)(void *)sc, .factory = srep_factory,
                              .ctx_destroy = NULL };
    KlHttpClientConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.timeout_ms = 20000;
    cfg.decompress = &dc;
    cfg.max_response_size = (size_t)64 * 1024 * 1024;
    memset(k, 0, sizeof *k);
    k->pause_every = pause_every;
    RunOpts o = { &cfg, NULL, 0 };
    (void)run_request(p, k, &o, ca);
}

/* The expected x-srep output for `in` (NUL-terminated): caller frees. */
static char *srep_expected(const char *in, size_t expand, size_t frag, size_t *len) {
    size_t n = strlen(in), per = expand * frag;
    char *o = malloc(n * per + 4);
    for (size_t i = 0; i < n; i++) memset(o + i * per, in[i], per);
    memcpy(o + n * per, "!!??", 4);
    *len = n * per + 4;
    return o;
}

/* Every fragment pauses, so the decoder is stopped: with compressed input still untaken, holding
 * output it owes for a byte already taken (continued by a call with no input), and inside the final
 * flush (continued, then validated). Delivery is exact, never re-entrant, nothing while paused. */
UTEST(stoppable, pause_every_fragment_exact) {
    static Peer p;
    ASSERT_EQ(peer_listen(&p), 0);
    peer_add_str(&p, "HTTP/1.1 200 OK\r\nContent-Encoding: x-srep\r\nTransfer-Encoding: chunked\r\n\r\n"
                     "4\r\nabcd\r\n6\r\nefghij\r\n0\r\n\r\n");
    p.hold = 1;
    SrepCfg sc = { .stoppable = 1, .expand = 3, .frag = 5 };
    Consumer k;
    run_srep(&p, &k, &sc, 1, NULL);
    size_t wl;
    char *want = srep_expected("abcdefghij", 3, 5, &wl);
    ASSERT_EQ(k.done, 1);
    ASSERT_EQ(k.err, KL_ERR_NONE);
    ASSERT_EQ(k.len, wl);
    ASSERT_EQ(memcmp(k.got, want, wl), 0);
    ASSERT_EQ(k.calls, 10 * 3 + 2);       /* one callback per decoded fragment: none replayed */
    ASSERT_EQ(k.completes, 1);
    ASSERT_GT(sc.stops_with_input_left, 0);
    ASSERT_GT(sc.zero_input_resumes, 0);
    ASSERT_GT(sc.flush_resumes, 0);
    ASSERT_CLEAN(k);
    free(want);
    consumer_free(&k);
}

/* A pause inside the final flush keeps the session: the validation still runs after the resume and
 * fails the request, after every decoded byte was delivered, without on_complete. */
UTEST(stoppable, pause_in_final_flush_then_validation_fails) {
    static Peer p;
    ASSERT_EQ(peer_listen(&p), 0);
    peer_add_str(&p, "HTTP/1.1 200 OK\r\nContent-Encoding: x-srep\r\nContent-Length: 4\r\n\r\nab#c");
    p.hold = 1;
    SrepCfg sc = { .stoppable = 1, .expand = 2, .frag = 2 };
    Consumer k;
    run_srep(&p, &k, &sc, 1, NULL);
    ASSERT_EQ(k.done, 1);
    ASSERT_EQ(k.err, KL_ERR_COMPRESS);
    ASSERT_EQ(k.len, (size_t)(4 * 4 + 4));   /* the body, then "!!??" from the flush */
    ASSERT_EQ(memcmp(k.got + 16, "!!??", 4), 0);
    ASSERT_GT(sc.flush_resumes, 0);
    ASSERT_EQ(k.completes, 0);
    ASSERT_EQ(k.cb_while_paused, 0);
    consumer_free(&k);
}

/* One compressed byte expands to 1 MiB. A stoppable decoder streams 16 MiB through a few KiB of
 * client memory whatever the pause pattern; a decoder without the op cannot stop, so pausing on its
 * first fragment overflows the bounded hold-back and fails the request (the documented limit of the
 * legacy path), with nothing leaked and only an in-order prefix delivered. */
UTEST(stoppable, high_expansion_bounded_memory_and_legacy_limit) {
    static Peer p;
    const char *in = "0123456789abcdef";
    static char reply[256];
    int hl = snprintf(reply, sizeof reply,
                      "HTTP/1.1 200 OK\r\nContent-Encoding: x-srep\r\nContent-Length: %zu\r\n\r\n%s",
                      strlen(in), in);
    size_t wl;
    char *want = srep_expected(in, 64, 16384, &wl);

    ASSERT_EQ(peer_listen(&p), 0);
    peer_add(&p, reply, (size_t)hl, 0);
    p.hold = 1;
    Count cnt;
    KlAllocator ca = counting_alloc(&cnt);
    SrepCfg sc = { .stoppable = 1, .expand = 64, .frag = 16384 };
    Consumer k;
    run_srep(&p, &k, &sc, 1, &ca);
    ASSERT_EQ(k.done, 1);
    ASSERT_EQ(k.err, KL_ERR_NONE);
    ASSERT_EQ(k.len, wl);
    ASSERT_EQ(memcmp(k.got, want, wl), 0);
    ASSERT_LT(cnt.peak, (size_t)32 * 1024);   /* 16 MiB decoded; nothing held by the client */
    ASSERT_EQ(cnt.cur, (size_t)0);
    ASSERT_CLEAN(k);
    consumer_free(&k);

    static Peer q;
    ASSERT_EQ(peer_listen(&q), 0);
    peer_add(&q, reply, (size_t)hl, 0);
    q.hold = 1;
    Count cnt2;
    KlAllocator ca2 = counting_alloc(&cnt2);
    SrepCfg lc = { .stoppable = 0, .expand = 64, .frag = 16384 };
    run_srep(&q, &k, &lc, 1, &ca2);
    ASSERT_EQ(k.done, 1);
    ASSERT_EQ(k.err, KL_ERR_PARSE);
    ASSERT_LT(k.len, wl);
    ASSERT_EQ(memcmp(k.got, want, k.len), 0);
    ASSERT_EQ(k.completes, 0);
    ASSERT_EQ(cnt2.cur, (size_t)0);
    ASSERT_EQ(k.cb_while_paused, 0);
    free(want);
    consumer_free(&k);
}

/* A decoder that breaks the dfeed_stoppable contract fails the request at once: it can neither
 * spin the client through resumes that make no progress, nor deliver while paused, nor make it
 * skip or replay input. */
UTEST(stoppable, contract_violations_fail_promptly) {
    /* user_calls: fragments the user sees before the failure. SR_SHORT_DONE delivers all 12 (6 bytes
     * x 2) and then reports one byte untaken: that byte is not fed again (no 13th fragment). */
    static const struct { int misbehave, pause_every, user_calls; } cases[] = {
        { SR_STOP_UNASKED, 1, 0 }, { SR_STOP_UNASKED, 0, 0 }, { SR_SHORT_DONE, 0, 12 },
        { SR_EMIT_AFTER_STOP, 1, 1 }, { SR_OVER_CONSUME, 1, 1 },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        static Peer p;
        ASSERT_EQ(peer_listen(&p), 0);
        peer_add_str(&p, "HTTP/1.1 200 OK\r\nContent-Encoding: x-srep\r\nContent-Length: 6\r\n\r\n"
                         "abcdef");
        p.hold = 1;
        SrepCfg sc = { .stoppable = 1, .misbehave = cases[i].misbehave, .expand = 2, .frag = 3 };
        Consumer k;
        run_srep(&p, &k, &sc, cases[i].pause_every, NULL);
        ASSERT_EQ(k.done, 1);
        ASSERT_EQ(k.err, KL_ERR_PARSE);   /* not KL_ERR_TIMEOUT: no spinning until the deadline */
        ASSERT_LE(sc.calls, 2);
        ASSERT_EQ(k.calls, cases[i].user_calls);
        ASSERT_EQ(k.completes, 0);
        ASSERT_EQ(k.cb_while_paused, 0);
        consumer_free(&k);
    }
}

/* Cancelling from a callback while the decoder is stopped mid-output: nothing more is delivered and
 * the session is released without being called again. */
UTEST(stoppable, cancel_while_decoder_holds_output) {
    static Peer p;
    ASSERT_EQ(peer_listen(&p), 0);
    peer_add_str(&p, "HTTP/1.1 200 OK\r\nContent-Encoding: x-srep\r\nContent-Length: 3\r\n\r\nabc");
    p.hold = 1;
    Count cnt;
    KlAllocator ca = counting_alloc(&cnt);
    KlAllocator da = kl_allocator_default();
    SrepCfg sc = { .stoppable = 1, .expand = 4, .frag = 8 };
    KlDecompressConfig dc = { .ctx = (KlCompressCtx *)(void *)&sc, .factory = srep_factory,
                              .ctx_destroy = NULL };
    KlHttpClientConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.timeout_ms = 10000;
    cfg.decompress = &dc;
    Consumer k;
    memset(&k, 0, sizeof k);
    k.pause_every = 1;
    k.cancel_at = 3;   /* the 2nd fragment paused with the decoder owing 2 more; cancel on the 3rd */
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &da), 0);
    peer_start(&p);
    KlHttpClientStreamCfg st;
    memset(&st, 0, sizeof st);
    st.on_body = on_body;
    st.on_complete = on_complete;
    st.user_data = &k;
    k.client = kl_http_client_start_s(&ev, &ca, &cfg, "GET", url_for(&p, "http"), NULL, 0, NULL, 0,
                                      &st, on_done, &k);
    ASSERT_TRUE(k.client != NULL);
    for (int i = 0; i < 400 && k.calls < 3; i++) {
        (void)kl_event_ctx_run(&ev, 16, 5);
        if (k.paused && k.calls < 3) (void)consumer_resume(&k);
    }
    int dcalls = sc.calls;
    for (int i = 0; i < 20; i++) (void)kl_event_ctx_run(&ev, 16, 2);
    ASSERT_EQ(k.calls, 3);
    ASSERT_EQ(sc.calls, dcalls);     /* the stopped session is never called again */
    ASSERT_EQ(k.done, 0);            /* a cancelled request reports nothing */
    ASSERT_EQ(k.completes, 0);
    ASSERT_EQ(k.cb_while_paused, 0);
    kl_http_client_free(k.client);
    ASSERT_EQ(cnt.cur, (size_t)0);
    kl_event_ctx_free(&ev);
    peer_finish(&p);
    consumer_free(&k);
}

/* ── TLS: decrypted plaintext held inside the engine ─────────────── */

/* A pass-through "TLS" that reads the socket in big gulps but hands out at most 7 bytes per read,
 * so plaintext stays pending inside it with nothing left on the socket to signal readiness. */
typedef struct {
    KlTls base;
    KlAllocator *alloc;
    char  buf[16384];
    size_t off, len;
    int   eof;
} HoldTls;

static KlTlsResult ht_handshake(KlTls *s, KlSocketHandle fd) { (void)s; (void)fd; return KL_TLS_OK; }
static kl_ssize_t ht_read(KlTls *self, KlSocketHandle fd, void *out, size_t n) {
    HoldTls *t = (HoldTls *)self;
    if (t->off == t->len) {
        kl_ssize_t r = kl_sockdef_recv(fd, t->buf, sizeof t->buf);
        if (r == 0) { t->eof = 1; return -1; }
        if (r < 0) return kl_sockdef_io_status() == KL_IO_WOULD_BLOCK ? 0 : -1;
        t->off = 0;
        t->len = (size_t)r;
    }
    size_t k = t->len - t->off;
    if (k > 7) k = 7;
    if (k > n) k = n;
    memcpy(out, t->buf + t->off, k);
    t->off += k;
    return (kl_ssize_t)k;
}
static kl_ssize_t ht_write(KlTls *s, KlSocketHandle fd, const void *b, size_t n) {
    (void)s;
    kl_ssize_t w = kl_sockdef_send(fd, b, n);
    if (w < 0 && kl_sockdef_io_status() == KL_IO_WOULD_BLOCK) return 0;
    return w;
}
static KlTlsResult ht_shutdown(KlTls *s, KlSocketHandle fd) { (void)s; (void)fd; return KL_TLS_OK; }
static size_t ht_pending(KlTls *s) { HoldTls *t = (HoldTls *)s; return t->len - t->off; }
static void ht_reset(KlTls *s) { (void)s; }
static void ht_destroy(KlTls *s) { HoldTls *t = (HoldTls *)s; kl_free(t->alloc, t, sizeof(*t)); }
static int ht_set_hostname(KlTls *s, const char *h) { (void)s; (void)h; return 0; }
static int ht_at_eof(KlTls *s) { return ((HoldTls *)s)->eof; }
static KlTls *ht_create(KlTlsCtx *ctx, KlAllocator *alloc) {
    (void)ctx;
    HoldTls *t = kl_malloc(alloc, sizeof(*t));
    if (!t) return NULL;
    memset(t, 0, sizeof(*t));
    t->alloc = alloc;
    t->base.handshake = ht_handshake;
    t->base.read = ht_read;
    t->base.write = ht_write;
    t->base.shutdown = ht_shutdown;
    t->base.pending = ht_pending;
    t->base.reset = ht_reset;
    t->base.destroy = ht_destroy;
    t->base.set_hostname = ht_set_hostname;
    t->base.at_eof = ht_at_eof;
    return &t->base;
}

UTEST(tls, pending_plaintext_is_read_on_resume_without_socket_event) {
    static Peer p;
    ASSERT_EQ(peer_listen(&p), 0);
    peer_add_str(&p, CHUNKED_REPLY);
    p.hold = 1;   /* everything is inside the TLS engine after its first read; the socket is idle */
    KlTlsConfig tc = { .ctx = NULL, .factory = ht_create, .ctx_destroy = NULL };
    KlHttpClientConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.timeout_ms = 10000;
    cfg.tls = &tc;
    Consumer k;
    memset(&k, 0, sizeof k);
    k.pause_every = 1;
    RunOpts o = { &cfg, "https", 2 };
    (void)run_request(&p, &k, &o, NULL);
    ASSERT_EQ(k.done, 1);
    ASSERT_EQ(k.err, KL_ERR_NONE);
    ASSERT_EQ(k.len, (size_t)26);
    ASSERT_EQ(memcmp(k.got, "abcdefghijklmnopqrstuvwxyz", 26), 0);
    ASSERT_CLEAN(k);
    consumer_free(&k);
}

UTEST(tls, close_delimited_eof_after_pause) {
    static Peer p;
    ASSERT_EQ(peer_listen(&p), 0);
    peer_add_str(&p, "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nabcdefghijklmnopqrstuvwxyz");
    KlTlsConfig tc = { .ctx = NULL, .factory = ht_create, .ctx_destroy = NULL };
    KlHttpClientConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.timeout_ms = 10000;
    cfg.tls = &tc;
    Consumer k;
    memset(&k, 0, sizeof k);
    k.pause_every = 1;
    RunOpts o = { &cfg, "https", 5 };
    (void)run_request(&p, &k, &o, NULL);
    ASSERT_EQ(k.done, 1);
    ASSERT_EQ(k.err, KL_ERR_NONE);
    ASSERT_EQ(k.len, (size_t)26);
    ASSERT_EQ(memcmp(k.got, "abcdefghijklmnopqrstuvwxyz", 26), 0);
    ASSERT_EQ(k.completes, 1);
    ASSERT_CLEAN(k);
    consumer_free(&k);
}

/* ── Transport backpressure ──────────────────────────────────────── */

/* A producer streams a body far larger than any socket buffer. While the client stays paused, the
 * producer's written count stops growing well short of the body (TCP flow control), and the
 * client's own allocations stay a few KiB whatever the body size. */
UTEST(transport, pause_stops_the_producer_and_memory_stays_bounded) {
    static Peer p;
    static char hdr[128];
    const size_t total = (size_t)48 * 1024 * 1024;
    ASSERT_EQ(peer_listen(&p), 0);
    int hl = snprintf(hdr, sizeof hdr, "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n\r\n", total);
    peer_add(&p, hdr, (size_t)hl, 0);
    p.produce = total;
    Count cnt;
    KlAllocator ca = counting_alloc(&cnt);
    KlAllocator da = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &da), 0);
    peer_start(&p);
    Consumer k;
    memset(&k, 0, sizeof k);
    KlHttpClientStreamCfg sc;
    memset(&sc, 0, sizeof sc);
    sc.on_body = on_body;
    sc.user_data = &k;
    KlHttpClientConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.timeout_ms = 120000;
    cfg.max_response_size = total;
    k.client = kl_http_client_start_s(&ev, &ca, &cfg, "GET", url_for(&p, "http"), NULL, 0, NULL, 0,
                                      &sc, on_done, &k);
    ASSERT_TRUE(k.client != NULL);
    k.pause_every = 1;
    /* Run until the first pause, then hold it and watch the producer. */
    for (int i = 0; i < 2000 && !k.paused; i++) (void)kl_event_ctx_run(&ev, 16, 5);
    ASSERT_EQ(k.paused, 1);
    int stalled = 0, last = -1;
    for (int i = 0; i < 400 && !stalled; i++) {          /* up to ~4 s for the window to shut */
        for (int j = 0; j < 5; j++) (void)kl_event_ctx_run(&ev, 16, 2);
        int now = kl_atomic_load_int(&p.produced);
        if (now == last) stalled++; else { stalled = 0; last = now; }
        if (stalled) {   /* confirm over a further quiet period */
            kl_test_sleep_ms(200);
            for (int j = 0; j < 20; j++) (void)kl_event_ctx_run(&ev, 16, 2);
            stalled = kl_atomic_load_int(&p.produced) == now;
        }
    }
    ASSERT_EQ(stalled, 1);
    size_t at_stall = (size_t)kl_atomic_load_int(&p.produced);
    ASSERT_LT(at_stall, total / 2);   /* the producer is held back by TCP, not drained into us */
    ASSERT_EQ(k.calls, 1);            /* and nothing more was delivered while paused */
    ASSERT_EQ(k.cb_while_paused, 0);
    /* Then stream the rest, pausing on every fragment. */
    int calls = k.calls;
    ASSERT_EQ(consumer_resume(&k), 0);
    ASSERT_EQ(k.calls, calls);        /* never re-entrant */
    (void)drive(&ev, &k, 0, 2000000);
    ASSERT_EQ(k.done, 1);
    ASSERT_EQ(k.err, KL_ERR_NONE);
    ASSERT_EQ(k.len, total);
    for (size_t i = 0; i < total; i += 4093)
        ASSERT_EQ(k.got[i], pattern_at(i));
    ASSERT_EQ(k.got[total - 1], pattern_at(total - 1));
    ASSERT_LT(cnt.peak, (size_t)32 * 1024);   /* 48 MiB through a few KiB of client memory */
    ASSERT_CLEAN(k);
    kl_http_client_free(k.client);
    ASSERT_EQ(cnt.cur, (size_t)0);
    kl_event_ctx_free(&ev);
    peer_finish(&p);
    consumer_free(&k);
}

/* ── Lifecycle ───────────────────────────────────────────────────── */

typedef struct {
    Peer *p;
    KlEventCtx ev;
    Consumer k;
} Live;

/* Start a request and run it to its first pause. */
static int live_to_first_pause(Live *l, Peer *p, const KlHttpClientConfig *cfg) {
    static KlAllocator a;   /* the client keeps this pointer: it must outlive the call */
    a = kl_allocator_default();
    l->p = p;
    memset(&l->k, 0, sizeof l->k);
    l->k.pause_every = 1;
    if (kl_event_ctx_init(&l->ev, &a) != 0) return -1;
    peer_start(p);
    static KlHttpClientStreamCfg sc;
    memset(&sc, 0, sizeof sc);
    sc.on_body = on_body;
    sc.on_complete = on_complete;
    sc.user_data = &l->k;
    l->k.client = kl_http_client_start_s(&l->ev, &a, cfg, "GET", url_for(p, "http"), NULL, 0, NULL, 0,
                                         &sc, on_done, &l->k);
    if (!l->k.client) return -1;
    for (int i = 0; i < 2000 && !l->k.paused && !l->k.done; i++) (void)kl_event_ctx_run(&l->ev, 16, 5);
    return l->k.paused ? 0 : -1;
}
static void live_spin(Live *l, int ticks) {
    for (int i = 0; i < ticks; i++) (void)kl_event_ctx_run(&l->ev, 16, 2);
}
static void live_end(Live *l) {
    if (l->k.client) kl_http_client_free(l->k.client);
    l->k.client = NULL;
    kl_event_ctx_free(&l->ev);
    peer_finish(l->p);
    consumer_free(&l->k);
}

UTEST(lifecycle, cancel_while_paused) {
    static Peer p;
    static Live l;
    ASSERT_EQ(peer_listen(&p), 0);
    peer_add_str(&p, CHUNKED_REPLY);
    p.hold = 1;
    ASSERT_EQ(live_to_first_pause(&l, &p, NULL), 0);
    ASSERT_EQ(kl_http_client_resume(l.k.client), 0);   /* scheduled ... */
    kl_http_client_cancel(l.k.client);                  /* ... and cancellation wins */
    l.k.paused = 0;
    live_spin(&l, 20);
    ASSERT_EQ(l.k.calls, 1);
    ASSERT_EQ(l.k.done, 0);                             /* no on_done after a cancel */
    ASSERT_EQ(kl_http_client_resume(l.k.client), -1);  /* never revives the request */
    live_end(&l);
}

UTEST(lifecycle, free_while_paused_with_resume_scheduled) {
    static Peer p;
    static Live l;
    ASSERT_EQ(peer_listen(&p), 0);
    peer_add_str(&p, CHUNKED_REPLY);
    p.hold = 1;
    ASSERT_EQ(live_to_first_pause(&l, &p, NULL), 0);
    ASSERT_EQ(kl_http_client_resume(l.k.client), 0);
    kl_http_client_free(l.k.client);   /* the scheduled continuation must not fire into it */
    l.k.client = NULL;
    live_spin(&l, 20);
    ASSERT_EQ(l.k.calls, 1);
    live_end(&l);
}

UTEST(lifecycle, resume_twice_and_after_done) {
    static Peer p;
    static Live l;
    ASSERT_EQ(peer_listen(&p), 0);
    peer_add_str(&p, CHUNKED_REPLY);
    p.hold = 1;
    ASSERT_EQ(live_to_first_pause(&l, &p, NULL), 0);
    l.k.paused = 0;
    l.k.pause_every = 0;                                  /* run to the end after this */
    ASSERT_EQ(kl_http_client_resume(l.k.client), 0);
    ASSERT_EQ(kl_http_client_resume(l.k.client), 0);     /* already scheduled: no-op */
    ASSERT_EQ(l.k.calls, 1);                              /* nothing synchronous */
    for (int i = 0; i < 400 && !l.k.done; i++) (void)kl_event_ctx_run(&l.ev, 16, 5);
    ASSERT_EQ(l.k.done, 1);
    ASSERT_EQ(l.k.err, KL_ERR_NONE);
    ASSERT_EQ(l.k.len, (size_t)26);
    ASSERT_EQ(memcmp(l.k.got, "abcdefghijklmnopqrstuvwxyz", 26), 0);
    ASSERT_EQ(kl_http_client_resume(l.k.client), -1);    /* after termination: no effect */
    ASSERT_EQ(kl_http_client_resume(NULL), -1);
    live_end(&l);
}

UTEST(lifecycle, resume_when_not_paused_is_a_noop) {
    static Peer p;
    ASSERT_EQ(peer_listen(&p), 0);
    peer_add_str(&p, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n");
    peer_add(&p, "OK", 2, 100);
    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);
    peer_start(&p);
    Consumer k;
    memset(&k, 0, sizeof k);
    KlHttpClientStreamCfg sc;
    memset(&sc, 0, sizeof sc);
    sc.on_body = on_body;
    sc.user_data = &k;
    k.client = kl_http_client_start_s(&ev, &a, NULL, "GET", url_for(&p, "http"), NULL, 0, NULL, 0,
                                      &sc, on_done, &k);
    ASSERT_TRUE(k.client != NULL);
    ASSERT_EQ(kl_http_client_resume(k.client), -1);   /* still connecting */
    for (int i = 0; i < 400 && !k.done; i++) {
        (void)kl_event_ctx_run(&ev, 16, 5);
        if (!k.done) ASSERT_EQ(kl_http_client_resume(k.client), -1);
    }
    ASSERT_EQ(k.done, 1);
    ASSERT_EQ(k.err, KL_ERR_NONE);
    kl_http_client_free(k.client);
    kl_event_ctx_free(&ev);
    peer_finish(&p);
    consumer_free(&k);
}

UTEST(lifecycle, cancel_from_callback_stops_delivery) {
    static Peer p;
    ASSERT_EQ(peer_listen(&p), 0);
    peer_add_str(&p, CHUNKED_REPLY);
    p.hold = 1;
    Consumer k;
    memset(&k, 0, sizeof k);
    k.cancel_at = 2;   /* cancels and returns CONTINUE: the rest of the read must not be delivered */
    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);
    peer_start(&p);
    KlHttpClientStreamCfg sc;
    memset(&sc, 0, sizeof sc);
    sc.on_body = on_body;
    sc.on_complete = on_complete;
    sc.user_data = &k;
    k.client = kl_http_client_start_s(&ev, &a, NULL, "GET", url_for(&p, "http"), NULL, 0, NULL, 0,
                                      &sc, on_done, &k);
    ASSERT_TRUE(k.client != NULL);
    for (int i = 0; i < 200 && k.calls < 2; i++) (void)kl_event_ctx_run(&ev, 16, 5);
    for (int i = 0; i < 20; i++) (void)kl_event_ctx_run(&ev, 16, 2);
    ASSERT_EQ(k.calls, 2);
    ASSERT_EQ(k.completes, 0);
    ASSERT_EQ(k.done, 0);
    kl_http_client_free(k.client);
    kl_event_ctx_free(&ev);
    peer_finish(&p);
    consumer_free(&k);
}

UTEST(lifecycle, cancel_from_callback_with_decompression) {
    static Peer p;
    ASSERT_EQ(peer_listen(&p), 0);
    peer_add_str(&p, REP_REPLY);
    p.hold = 1;
    KlDecompressConfig dc = { .ctx = NULL, .factory = rep_factory, .ctx_destroy = NULL };
    KlHttpClientConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.timeout_ms = 10000;
    cfg.decompress = &dc;
    Consumer k;
    memset(&k, 0, sizeof k);
    k.cancel_at = 3;   /* mid-feed: the decoder still has output for this slice */
    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);
    peer_start(&p);
    KlHttpClientStreamCfg sc;
    memset(&sc, 0, sizeof sc);
    sc.on_body = on_body;
    sc.on_complete = on_complete;
    sc.user_data = &k;
    k.client = kl_http_client_start_s(&ev, &a, &cfg, "GET", url_for(&p, "http"), NULL, 0, NULL, 0,
                                      &sc, on_done, &k);
    ASSERT_TRUE(k.client != NULL);
    for (int i = 0; i < 200 && k.calls < 3; i++) (void)kl_event_ctx_run(&ev, 16, 5);
    for (int i = 0; i < 20; i++) (void)kl_event_ctx_run(&ev, 16, 2);
    ASSERT_EQ(k.calls, 3);
    ASSERT_EQ(k.completes, 0);
    ASSERT_EQ(k.done, 0);
    kl_http_client_free(k.client);
    kl_event_ctx_free(&ev);
    peer_finish(&p);
    consumer_free(&k);
}

UTEST(lifecycle, resume_from_inside_the_pausing_callback) {
    static Peer p;
    ASSERT_EQ(peer_listen(&p), 0);
    peer_add_str(&p, CHUNKED_REPLY);
    p.hold = 1;
    Consumer k;
    memset(&k, 0, sizeof k);
    k.pause_every = 1;
    k.resume_inside = 1;   /* pause + resume in one callback: continues on its own, deferred */
    int pauses = run_request(&p, &k, NULL, NULL);
    ASSERT_EQ(pauses, 0);  /* the test never had to resume */
    ASSERT_EQ(k.done, 1);
    ASSERT_EQ(k.err, KL_ERR_NONE);
    ASSERT_EQ(k.len, (size_t)26);
    ASSERT_EQ(memcmp(k.got, "abcdefghijklmnopqrstuvwxyz", 26), 0);
    ASSERT_EQ(k.calls, 7);
    consumer_free(&k);
}

/* The same through the decompressor, where the last pauses land in its final flush (the parse then
 * ends OK with completion held): the in-callback resume must still be honoured there. */
UTEST(lifecycle, resume_from_inside_the_pausing_callback_during_final_flush) {
    static Peer p;
    ASSERT_EQ(peer_listen(&p), 0);
    peer_add_str(&p, REP_REPLY);
    p.hold = 1;
    KlDecompressConfig dc = { .ctx = NULL, .factory = rep_factory, .ctx_destroy = NULL };
    KlHttpClientConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.timeout_ms = 3000;   /* a lost resume would surface as KL_ERR_TIMEOUT */
    cfg.decompress = &dc;
    Consumer k;
    memset(&k, 0, sizeof k);
    k.pause_every = 1;
    k.resume_inside = 1;
    RunOpts o = { &cfg, NULL, 0 };
    int pauses = run_request(&p, &k, &o, NULL);
    char want[64];
    expand_rep(REP_BODY, want);
    ASSERT_EQ(pauses, 0);
    ASSERT_EQ(k.done, 1);
    ASSERT_EQ(k.err, KL_ERR_NONE);
    ASSERT_EQ(k.len, strlen(want));
    ASSERT_EQ(memcmp(k.got, want, k.len), 0);
    ASSERT_EQ(k.completes, 1);
    consumer_free(&k);
}

UTEST(lifecycle, deadline_still_applies_while_paused) {
    static Peer p;
    static Live l;
    ASSERT_EQ(peer_listen(&p), 0);
    peer_add_str(&p, CHUNKED_REPLY);
    p.hold = 1;
    KlHttpClientConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.timeout_ms = 300;
    ASSERT_EQ(live_to_first_pause(&l, &p, &cfg), 0);
    for (int i = 0; i < 400 && !l.k.done; i++) (void)kl_event_ctx_run(&l.ev, 16, 5);
    ASSERT_EQ(l.k.done, 1);                          /* a terminal error outranks the pause */
    ASSERT_EQ(l.k.err, KL_ERR_TIMEOUT);
    ASSERT_EQ(l.k.calls, 1);
    ASSERT_EQ(kl_http_client_resume(l.k.client), -1);
    l.k.paused = 0;
    live_end(&l);
}

/* Fault injection: the event context's allocator fails. */
static void idle_timer(void *ud) { (void)ud; }
UTEST(lifecycle, resume_that_cannot_be_scheduled_stays_paused) {
    static Peer p;
    ASSERT_EQ(peer_listen(&p), 0);
    peer_add_str(&p, CHUNKED_REPLY);
    p.hold = 1;
    Count cnt;
    KlAllocator fa = counting_alloc(&cnt);
    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &fa), 0);
    peer_start(&p);
    Consumer k;
    memset(&k, 0, sizeof k);
    k.pause_every = 1;
    KlHttpClientStreamCfg sc;
    memset(&sc, 0, sizeof sc);
    sc.on_body = on_body;
    sc.user_data = &k;
    k.client = kl_http_client_start_s(&ev, &a, NULL, "GET", url_for(&p, "http"), NULL, 0, NULL, 0,
                                      &sc, on_done, &k);
    ASSERT_TRUE(k.client != NULL);
    for (int i = 0; i < 2000 && !k.paused; i++) (void)kl_event_ctx_run(&ev, 16, 5);
    ASSERT_EQ(k.paused, 1);
    /* Fill the timer heap to capacity, then refuse its growth. */
    int64_t ids[64];
    int nids = 0;
    cnt.fail = 1;
    while (nids < 64) {
        int64_t id = kl_timer_add(&ev, 60000, idle_timer, NULL);
        if (id < 0) break;
        ids[nids++] = id;
    }
    ASSERT_LT(nids, 64);
    ASSERT_EQ(kl_http_client_resume(k.client), -1);   /* could not schedule ... */
    for (int i = 0; i < 10; i++) (void)kl_event_ctx_run(&ev, 16, 2);
    ASSERT_EQ(k.calls, 1);                             /* ... so it stays paused */
    ASSERT_EQ(k.done, 0);
    ASSERT_EQ(kl_timer_cancel(&ev, ids[0]), 0);
    k.paused = 0;
    ASSERT_EQ(kl_http_client_resume(k.client), 0);    /* a retry succeeds */
    cnt.fail = 0;
    for (int i = 1; i < nids; i++) kl_timer_cancel(&ev, ids[i]);
    k.pause_every = 0;
    for (int i = 0; i < 400 && !k.done; i++) (void)kl_event_ctx_run(&ev, 16, 5);
    ASSERT_EQ(k.done, 1);
    ASSERT_EQ(k.err, KL_ERR_NONE);
    ASSERT_EQ(k.len, (size_t)26);
    kl_http_client_free(k.client);
    kl_event_ctx_free(&ev);
    peer_finish(&p);
    consumer_free(&k);
}

UTEST(lifecycle, watcher_reregistration_failure_fails_the_request) {
    static Peer p;
    ASSERT_EQ(peer_listen(&p), 0);
    /* One chunk, then silence: after the pause the residual parses to INCOMPLETE and the client
     * must register its watcher again to wait for more. */
    peer_add_str(&p, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n");
    p.hold = 1;
    Count cnt;
    KlAllocator fa = counting_alloc(&cnt);
    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &fa), 0);
    /* A completion engine (pollcomp builds its poll set per drain) allocates from the same
     * allocator on every loop tick, so failing it stops the loop itself before the client is
     * reached. The client's re-registration path is engine-independent; inject on readiness. */
    if (kl_event_caps(kl_event_ctx_loop(&ev)) & KL_EVENT_CAP_COMPLETION) {
        kl_event_ctx_free(&ev);
        kl_test_closesock(p.listen_fd);
        UTEST_SKIP("allocator fault injection here would fail the completion engine, not the client");
    }
    peer_start(&p);
    Consumer k;
    memset(&k, 0, sizeof k);
    k.pause_every = 1;
    KlHttpClientStreamCfg sc;
    memset(&sc, 0, sizeof sc);
    sc.on_body = on_body;
    sc.user_data = &k;
    k.client = kl_http_client_start_s(&ev, &a, NULL, "GET", url_for(&p, "http"), NULL, 0, NULL, 0,
                                      &sc, on_done, &k);
    ASSERT_TRUE(k.client != NULL);
    for (int i = 0; i < 2000 && !k.paused; i++) (void)kl_event_ctx_run(&ev, 16, 5);
    ASSERT_EQ(k.paused, 1);
    ASSERT_EQ(consumer_resume(&k), 0);   /* scheduled while the allocator still works */
    cnt.fail = 1;                        /* the watcher node cannot be allocated again */
    for (int i = 0; i < 400 && !k.done; i++) (void)kl_event_ctx_run(&ev, 16, 5);
    cnt.fail = 0;
    ASSERT_EQ(k.done, 1);
    ASSERT_EQ(k.err, KL_ERR_ALLOC);
    kl_http_client_free(k.client);
    kl_event_ctx_free(&ev);
    peer_finish(&p);
    consumer_free(&k);
}

/* ── The sync client has no resume: PAUSE fails the request ──────── */

static int sync_on_body(const char *d, size_t n, void *ud) {
    (void)d; (void)n;
    (*(int *)ud)++;
    return KL_HTTP_CLIENT_BODY_PAUSE;
}

UTEST(sync, pause_is_abort) {
    static Peer p;
    ASSERT_EQ(peer_listen(&p), 0);
    peer_add_str(&p, CHUNKED_REPLY);
    peer_start(&p);
    KlAllocator a = kl_allocator_default();
    KlHttpClientConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.timeout_ms = 3000;
    int calls = 0;
    KlHttpClientStreamCfg sc;
    memset(&sc, 0, sizeof sc);
    sc.on_body = sync_on_body;
    sc.user_data = &calls;
    KlHttpClientResponse r;
    memset(&r, 0, sizeof r);
    int rc = kl_http_client_request_s(&a, &cfg, "GET", url_for(&p, "http"), NULL, 0, NULL, 0, &sc, &r);
    ASSERT_EQ(rc, -1);
    ASSERT_EQ(calls, 1);   /* nothing delivered past the pause */
    kl_http_client_response_free(&r);
    peer_finish(&p);
}

UTEST_MAIN()
