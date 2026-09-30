/*
 * test_anon_pipe.c: anonymous pipe pairs (<keel/anon_pipe.h>).
 *
 * Both ends live in the test process: endpoint A is the KlPipeStream under test, driven only through
 * the KlStream API, and endpoint B (the child's end) is driven with plain blocking I/O on a helper
 * thread, the way a child process would use it. Spawning real children is the embedder's business.
 *
 * Engine matrix. Support follows the PLATFORM and what the loop can drive, not completion vs
 * readiness: Windows supports pairs on IOCP (completion) and refuses WSAPoll; POSIX supports them on
 * every engine that watches native descriptors (epoll, kqueue, poll, and io_uring / pollcomp through
 * their watcher relay). support_follows_platform_and_engine pins that on every engine; the semantic
 * cases then run wherever pairs are supported, in a Windows section (IOCP) and a POSIX section.
 *
 * Lifetime is checked twice: a counting allocator must return to its baseline once a stream has been
 * freed and its last op retired, and the process's open handles (Windows) or descriptors (POSIX) must
 * return to their baseline once both ends are closed, so no failure path or teardown order can leave a
 * pipe end open.
 */
#include "utest.h"
#include <keel/keel.h>
#include <keel/event_ctx.h>
#include <keel/stream.h>
#include <keel/pipe.h>
#include <keel/anon_pipe.h>
#include <keel/anon_pipe_native.h>
#include "../src/event_caps.h"   /* kl_event_caps: what can this loop drive? */
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* ── Counting allocator with failure injection ──────────────────────────────────────────────── */

typedef struct { long live_blocks; long long live_bytes; } Counts;
static Counts g_counts;
static int g_fail_nth;        /* > 0: the nth allocation from now fails */

static void *ca_malloc(void *c, size_t n) {
    (void)c;
    if (g_fail_nth > 0 && --g_fail_nth == 0) return NULL;
    void *p = malloc(n ? n : 1);
    if (p) { g_counts.live_blocks++; g_counts.live_bytes += (long long)n; }
    return p;
}
static void *ca_realloc(void *c, void *p, size_t o, size_t n) {
    (void)c;
    void *q = realloc(p, n ? n : 1);
    if (q) {
        if (!p) g_counts.live_blocks++;
        g_counts.live_bytes += (long long)n - (long long)(p ? o : 0);
    }
    return q;
}
static void ca_free(void *c, void *p, size_t n) {
    (void)c;
    if (!p) return;
    g_counts.live_blocks--; g_counts.live_bytes -= (long long)n;
    free(p);
}
static KlAllocator g_alloc = { ca_malloc, ca_realloc, ca_free, NULL };

/* ── Consumer-side recorder ─────────────────────────────────────────────────────────────────── */

typedef struct {
    char  *buf;
    size_t len, cap;
    int    data_calls, terminals, closes;
} Rec;

static void rec_on_data(void *ud, const char *b, size_t n, int ok) {
    Rec *r = ud;
    if (!ok) { r->terminals++; return; }
    r->data_calls++;
    if (r->len + n > r->cap) {
        size_t nc = r->cap ? r->cap * 2 : 4096;
        while (nc < r->len + n) nc *= 2;
        r->buf = realloc(r->buf, nc);
        r->cap = nc;
    }
    memcpy(r->buf + r->len, b, n);
    r->len += n;
}
static void rec_on_close(void *ud) { ((Rec *)ud)->closes++; }

static KlPipeConfig rec_cfg(Rec *r, size_t rcap, size_t wcap) {
    KlPipeConfig c;
    memset(&c, 0, sizeof(c));
    c.read_capacity = rcap;
    c.write_capacity = wcap;
    c.on_data = rec_on_data;
    c.on_close = rec_on_close;
    c.user_data = r;
    return c;
}

/* ── Everywhere: validation, empty ends, and the engine gate ───────────────────────────────── */

UTEST(anon_pipe, argument_validation) {
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &g_alloc), 0);
    Rec r; memset(&r, 0, sizeof r);
    KlPipeConfig cfg = rec_cfg(&r, 0, 0);
    KlPipeStream *p = (KlPipeStream *)1;
    KlAnonPipeEnd e; memset(&e, 0x5A, sizeof e);

    ASSERT_EQ(kl_anon_pipe_create(NULL, KL_ANON_PIPE_READS, &cfg, &p, &e), KL_PIPE_INVALID);
    ASSERT_TRUE(p == NULL);                                   /* outputs cleared even on INVALID */
    ASSERT_TRUE(e._handle == NULL && e._fd1 == 0);
    ASSERT_EQ(kl_anon_pipe_create(&ev, KL_ANON_PIPE_READS, NULL, &p, &e), KL_PIPE_INVALID);
    ASSERT_EQ(kl_anon_pipe_create(&ev, KL_ANON_PIPE_READS, &cfg, NULL, &e), KL_PIPE_INVALID);
    ASSERT_EQ(kl_anon_pipe_create(&ev, KL_ANON_PIPE_READS, &cfg, &p, NULL), KL_PIPE_INVALID);
    ASSERT_EQ(kl_anon_pipe_create(&ev, (KlAnonPipeDir)0, &cfg, &p, &e), KL_PIPE_INVALID);
    ASSERT_EQ(kl_anon_pipe_create(&ev, (KlAnonPipeDir)3, &cfg, &p, &e), KL_PIPE_INVALID);
    cfg.on_data = NULL;                                       /* a READS pair must deliver somewhere */
    ASSERT_EQ(kl_anon_pipe_create(&ev, KL_ANON_PIPE_READS, &cfg, &p, &e), KL_PIPE_INVALID);
    kl_event_ctx_free(&ev);
}

UTEST(anon_pipe, empty_end_is_safe) {
    KlAnonPipeEnd e; memset(&e, 0, sizeof e);                /* zero-initialized = empty */
    kl_anon_pipe_end_close(&e);
    kl_anon_pipe_end_close(&e);
    kl_anon_pipe_end_close(NULL);
#if defined(_WIN32)
    ASSERT_TRUE(kl_anon_pipe_end_handle(&e) == NULL);
    ASSERT_TRUE(kl_anon_pipe_end_handle(NULL) == NULL);
#else
    ASSERT_EQ(kl_anon_pipe_end_fd(&e), -1);                  /* never fd 0 (stdin) */
    ASSERT_EQ(kl_anon_pipe_end_fd(NULL), -1);
#endif
}

/* Pairs are supported where the platform's pipes can be driven by this loop: Windows on the
 * completion engine (IOCP), POSIX on any engine that watches native descriptors. */
static int pairs_expected(KlEventCtx *ev) {
    unsigned caps = kl_event_caps(&ev->loop);
#if defined(_WIN32)
    return (caps & KL_EVENT_CAP_COMPLETION) != 0;
#else
    return (caps & KL_EVENT_CAP_NATIVE_FD) != 0;
#endif
}

UTEST(anon_pipe, support_follows_platform_and_engine) {
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &g_alloc), 0);
    int expected = pairs_expected(&ev);
#if !defined(_WIN32)
    ASSERT_TRUE(expected);                                    /* every built-in POSIX engine */
#endif
    long before = g_counts.live_blocks;
    Rec r; memset(&r, 0, sizeof r);
    KlPipeConfig cfg = rec_cfg(&r, 0, 0);
    for (int d = KL_ANON_PIPE_READS; d <= KL_ANON_PIPE_WRITES; d++) {
        KlPipeStream *p = (KlPipeStream *)1;
        KlAnonPipeEnd e; memset(&e, 0x5A, sizeof e);
        KlPipeStatus st = kl_anon_pipe_create(&ev, (KlAnonPipeDir)d, &cfg, &p, &e);
        if (expected) {
            ASSERT_EQ((int)st, (int)KL_PIPE_OK);
            ASSERT_TRUE(p != NULL);
            kl_anon_pipe_end_close(&e);
            kl_pipe_free(p);
            for (int i = 0; i < 5; i++) (void)kl_event_ctx_run(&ev, 16, 1);
        } else {
            ASSERT_EQ((int)st, (int)KL_PIPE_UNSUPPORTED);
            ASSERT_TRUE(p == NULL);
            ASSERT_TRUE(e._handle == NULL && e._fd1 == 0);
        }
    }
    ASSERT_EQ(g_counts.live_blocks, before);                 /* refused, or created and fully reclaimed */
    kl_event_ctx_free(&ev);
}

#if defined(_WIN32)
#include <windows.h>
#include "../src/platform_thread.h"   /* the PAL thread seam, for the child-side helper */

static int loop_is_completion(KlEventCtx *ev) {
    return (kl_event_caps(&ev->loop) & KL_EVENT_CAP_COMPLETION) != 0;
}

static void rec_free(Rec *r) { free(r->buf); memset(r, 0, sizeof(*r)); }

/* ── Harness ────────────────────────────────────────────────────────────────────────────────── */

typedef int (*CondFn)(void *);
static int pump_until(KlEventCtx *ev, CondFn cond, void *arg, DWORD ms) {
    ULONGLONG end = GetTickCount64() + ms;
    for (;;) {
        if (cond && cond(arg)) return 1;
        if (GetTickCount64() >= end) return cond ? cond(arg) : 1;
        if (kl_event_ctx_run(ev, 16, 5) < 0) return 0;
    }
}
static void pump_for(KlEventCtx *ev, DWORD ms) { (void)pump_until(ev, NULL, NULL, ms); }
static int cond_closed(void *a)   { return ((Rec *)a)->closes > 0; }
static int cond_terminal(void *a) { return ((Rec *)a)->terminals > 0; }
typedef struct { Rec *r; size_t want; } LenWant;
static int cond_len(void *a)      { LenWant *w = a; return w->r->len >= w->want; }

static DWORD handle_count(void) {
    DWORD n = 0;
    GetProcessHandleCount(GetCurrentProcess(), &n);
    return n;
}
static unsigned char pat(size_t i) { return (unsigned char)((i * 131u + (i >> 9)) & 0xFF); }

/* The child's side, on a helper thread: plain blocking ReadFile / WriteFile on endpoint B. */
typedef struct {
    HANDLE       h;
    int          mode;      /* 0 = write `total` pattern bytes then stop; 1 = read to EOF */
    size_t       total;
    size_t       done;      /* bytes written or read */
    int          eof;       /* read: saw end of stream */
    int          bad;       /* read: bytes that did not match the pattern */
    DWORD        err;       /* last failing Win32 error */
    KlPlatThread t;
} Child;

static void child_main(void *arg) {
    Child *c = arg;
    if (c->mode == 0) {
        char b[7000];
        while (c->done < c->total) {
            DWORD n = (DWORD)(c->total - c->done < sizeof b ? c->total - c->done : sizeof b), k = 0;
            for (DWORD i = 0; i < n; i++) b[i] = (char)pat(c->done + i);
            if (!WriteFile(c->h, b, n, &k, NULL)) { c->err = GetLastError(); return; }
            c->done += k;
        }
    } else {
        char b[5000];
        for (;;) {
            DWORD k = 0;
            if (!ReadFile(c->h, b, sizeof b, &k, NULL)) {
                c->err = GetLastError();
                c->eof = (c->err == ERROR_BROKEN_PIPE);
                return;
            }
            if (k == 0) { c->eof = 1; return; }
            for (DWORD i = 0; i < k; i++) if ((unsigned char)b[i] != pat(c->done + i)) c->bad++;
            c->done += k;
        }
    }
}
static void child_start(Child *c) {
    if (kl_plat_thread_create(&c->t, child_main, c) != 0) { fprintf(stderr, "harness: thread\n"); abort(); }
}

/* ── Fixture ────────────────────────────────────────────────────────────────────────────────── */

struct anon_iocp { KlEventCtx ev; int skip; long blocks0; long long bytes0; DWORD handles0; };

UTEST_F_SETUP(anon_iocp) {
    ASSERT_EQ(kl_event_ctx_init(&utest_fixture->ev, &g_alloc), 0);
    utest_fixture->skip = !loop_is_completion(&utest_fixture->ev);
    if (!utest_fixture->skip) {
        /* DO NOT REDUCE THESE WARM-UPS. Windows lazily creates process and runtime handles during the
         * first pipe constructions (measured: about 7 on the first pair, 1 more on the second, then
         * flat). The allocator and handle baselines below are taken only after warm-up, so every case
         * measures Keel's lifetime balance, not process initialization. With a single warm-up, every
         * case run on its own reports a false one-handle leak. Three rounds leave margin. */
        Rec r; memset(&r, 0, sizeof r);
        KlPipeConfig cfg = rec_cfg(&r, 0, 0);
        for (int w = 0; w < 3; w++) {
            KlPipeStream *p = NULL; KlAnonPipeEnd e;
            if (kl_anon_pipe_create(&utest_fixture->ev, KL_ANON_PIPE_READS, &cfg, &p, &e) != KL_PIPE_OK)
                break;
            kl_anon_pipe_end_close(&e);
            kl_pipe_free(p);
            pump_for(&utest_fixture->ev, 20);
        }
    }
    utest_fixture->blocks0  = g_counts.live_blocks;
    utest_fixture->bytes0   = g_counts.live_bytes;
    utest_fixture->handles0 = handle_count();
}
UTEST_F_TEARDOWN(anon_iocp) {
    (void)utest_result;
    kl_event_ctx_free(&utest_fixture->ev);
}
#define NEED_IOCP() do { if (utest_fixture->skip) UTEST_SKIP("needs BACKEND=iocp"); } while (0)
/* Every case ends here: nothing allocated and no handle left open. */
#define ASSERT_RECLAIMED() do {                                                              \
        pump_for(&utest_fixture->ev, 30);                                                    \
        ASSERT_EQ(g_counts.live_blocks, utest_fixture->blocks0);                             \
        ASSERT_EQ(g_counts.live_bytes, utest_fixture->bytes0);                               \
        ASSERT_EQ((long)handle_count(), (long)utest_fixture->handles0);                      \
    } while (0)

/* ── The pair itself ────────────────────────────────────────────────────────────────────────── */

/* The child end's name, from the kernel: "\keel-anon-<pid>-<n>-<hex>". */
static int end_name(HANDLE h, wchar_t *out, size_t n) {
    union { FILE_NAME_INFO fi; unsigned char raw[sizeof(FILE_NAME_INFO) + 512 * sizeof(wchar_t)]; } u;
    if (!GetFileInformationByHandleEx(h, FileNameInfo, &u, sizeof u)) return -1;
    size_t wn = u.fi.FileNameLength / sizeof(wchar_t);
    if (wn + 1 > n) return -1;
    memcpy(out, u.fi.FileName, wn * sizeof(wchar_t));
    out[wn] = 0;
    return 0;
}

UTEST_F(anon_iocp, pair_is_private_directional_and_not_inheritable) {
    NEED_IOCP();
    for (int d = KL_ANON_PIPE_READS; d <= KL_ANON_PIPE_WRITES; d++) {
        Rec r; memset(&r, 0, sizeof r);
        KlPipeConfig cfg = rec_cfg(&r, 0, 0);
        KlPipeStream *p = NULL; KlAnonPipeEnd e;
        ASSERT_EQ(kl_anon_pipe_create(&utest_fixture->ev, (KlAnonPipeDir)d, &cfg, &p, &e), KL_PIPE_OK);
        HANDLE ch = (HANDLE)kl_anon_pipe_end_handle(&e);
        ASSERT_TRUE(ch != NULL && ch != INVALID_HANDLE_VALUE);
        ASSERT_EQ((int)GetFileType(ch), (int)FILE_TYPE_PIPE);

        DWORD flags = 1;
        ASSERT_TRUE(GetHandleInformation(ch, &flags));
        ASSERT_EQ((int)(flags & HANDLE_FLAG_INHERIT), 0);   /* the spawner opts it in, per child */

        /* The child end is synchronous: a plain ReadFile / WriteFile with no OVERLAPPED is valid. And
         * it has only its own direction. */
        char b[4]; DWORD k = 0;
        if (d == KL_ANON_PIPE_READS) {
            ASSERT_FALSE(ReadFile(ch, b, sizeof b, &k, NULL));       /* the child end writes only */
            ASSERT_EQ((int)GetLastError(), (int)ERROR_ACCESS_DENIED);
            ASSERT_EQ((int)kl_stream_write(kl_pipe_stream(p), "x", 1), (int)KL_STREAM_ERROR);
        } else {
            ASSERT_FALSE(WriteFile(ch, "x", 1, &k, NULL));           /* the child end reads only */
            ASSERT_EQ((int)GetLastError(), (int)ERROR_ACCESS_DENIED);
            ASSERT_EQ(kl_stream_read_start(kl_pipe_stream(p)), -1);
        }

        /* Private: a random name, and the single instance is already taken. */
        wchar_t nm[600];
        ASSERT_EQ(end_name(ch, nm, 600), 0);
        ASSERT_EQ(wcsncmp(nm, L"\\keel-anon-", 11), 0);
        ASSERT_GE((int)wcslen(nm), 11 + 32);
        wchar_t full[700];
        wcscpy(full, L"\\\\.\\pipe");
        wcscat(full, nm);
        /* Ask for exactly the access a second child end would need, so the refusal is the instance
         * limit and not the wrong direction. */
        HANDLE other = CreateFileW(full, d == KL_ANON_PIPE_READS ? GENERIC_WRITE : GENERIC_READ, 0, NULL,
                                   OPEN_EXISTING, 0, NULL);
        ASSERT_TRUE(other == INVALID_HANDLE_VALUE);
        ASSERT_EQ((int)GetLastError(), (int)ERROR_PIPE_BUSY);
        /* ... and no second server instance can be added under the name (one instance only), even by
         * this same user, whom the DACL otherwise admits. */
        HANDLE second = CreateNamedPipeW(full, d == KL_ANON_PIPE_READS ? PIPE_ACCESS_INBOUND : PIPE_ACCESS_OUTBOUND,
                                         PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                         PIPE_UNLIMITED_INSTANCES, 4096, 4096, 0, NULL);
        if (second != INVALID_HANDLE_VALUE) CloseHandle(second);
        ASSERT_TRUE(second == INVALID_HANDLE_VALUE);

        kl_anon_pipe_end_close(&e);
        ASSERT_TRUE(kl_anon_pipe_end_handle(&e) == NULL);
        kl_anon_pipe_end_close(&e);                               /* idempotent */
        kl_pipe_free(p);
        rec_free(&r);
    }
    ASSERT_RECLAIMED();
}

UTEST_F(anon_iocp, names_are_distinct) {
    NEED_IOCP();
    Rec r; memset(&r, 0, sizeof r);
    KlPipeConfig cfg = rec_cfg(&r, 0, 0);
    KlPipeStream *p1 = NULL, *p2 = NULL; KlAnonPipeEnd e1, e2;
    ASSERT_EQ(kl_anon_pipe_create(&utest_fixture->ev, KL_ANON_PIPE_READS, &cfg, &p1, &e1), KL_PIPE_OK);
    ASSERT_EQ(kl_anon_pipe_create(&utest_fixture->ev, KL_ANON_PIPE_READS, &cfg, &p2, &e2), KL_PIPE_OK);
    wchar_t n1[600], n2[600];
    ASSERT_EQ(end_name((HANDLE)kl_anon_pipe_end_handle(&e1), n1, 600), 0);
    ASSERT_EQ(end_name((HANDLE)kl_anon_pipe_end_handle(&e2), n2, 600), 0);
    ASSERT_NE(wcscmp(n1, n2), 0);
    kl_anon_pipe_end_close(&e1); kl_anon_pipe_end_close(&e2);
    kl_pipe_free(p1); kl_pipe_free(p2);
    ASSERT_RECLAIMED();
}

/* ── Data, EOF and broken pipe ──────────────────────────────────────────────────────────────── */

UTEST_F(anon_iocp, reads_large_transfer_then_eof) {
    NEED_IOCP();
    const size_t total = 1u << 20;
    Rec r; memset(&r, 0, sizeof r);
    KlPipeConfig cfg = rec_cfg(&r, 1000, 0);                  /* small buffer: many fragments */
    KlPipeStream *p = NULL; KlAnonPipeEnd e;
    ASSERT_EQ(kl_anon_pipe_create(&utest_fixture->ev, KL_ANON_PIPE_READS, &cfg, &p, &e), KL_PIPE_OK);
    ASSERT_EQ(kl_stream_read_start(kl_pipe_stream(p)), 0);
    Child c; memset(&c, 0, sizeof c);
    c.h = (HANDLE)kl_anon_pipe_end_handle(&e); c.mode = 0; c.total = total;
    child_start(&c);
    LenWant w = { &r, total };
    ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_len, &w, 30000));
    kl_plat_thread_join(&c.t);
    ASSERT_EQ((int)c.err, 0);
    ASSERT_EQ(r.terminals, 0);                                /* B still open: no EOF yet */
    kl_anon_pipe_end_close(&e);                               /* the child exits */
    ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_terminal, &r, 5000));
    ASSERT_EQ(r.terminals, 1);
    ASSERT_EQ(r.len, total);
    int bad = 0;
    for (size_t i = 0; i < total; i++) if ((unsigned char)r.buf[i] != pat(i)) bad++;
    ASSERT_EQ(bad, 0);
    ASSERT_GT(r.data_calls, 1000);
    ASSERT_EQ(kl_stream_close_begin(kl_pipe_stream(p)), 0);   /* read side ended: detaches at once */
    ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_closed, &r, 2000));
    ASSERT_EQ(r.closes, 1);
    kl_pipe_free(p);
    rec_free(&r);
    ASSERT_RECLAIMED();
}

typedef struct { int ready; int edges; } Edge;
static void edge_writable(void *ud) { Edge *g = ud; g->ready = 1; g->edges++; }

UTEST_F(anon_iocp, writes_edge_driven_transfer_then_child_sees_eof) {
    NEED_IOCP();
    const size_t total = 4u << 20;
    Rec r; memset(&r, 0, sizeof r);
    KlPipeConfig cfg = rec_cfg(&r, 0, 16 * 1024);
    cfg.on_data = NULL;                                       /* allowed for a WRITES pair */
    KlPipeStream *p = NULL; KlAnonPipeEnd e;
    ASSERT_EQ(kl_anon_pipe_create(&utest_fixture->ev, KL_ANON_PIPE_WRITES, &cfg, &p, &e), KL_PIPE_OK);
    KlStream *st = kl_pipe_stream(p);
    Edge g = { 1, 0 };
    ASSERT_EQ(kl_stream_on_writable(st, edge_writable, &g), 0);
    Child c; memset(&c, 0, sizeof c);
    c.h = (HANDLE)kl_anon_pipe_end_handle(&e); c.mode = 1;
    child_start(&c);

    static char chunk[6000];
    size_t sent = 0; int would_block = 0;
    ULONGLONG end = GetTickCount64() + 30000;
    while (sent < total && GetTickCount64() < end) {
        while (g.ready && sent < total) {
            size_t n = total - sent < sizeof chunk ? total - sent : sizeof chunk;
            for (size_t i = 0; i < n; i++) chunk[i] = (char)pat(sent + i);
            KlStreamWriteStatus ws = kl_stream_write(st, chunk, n);
            if (ws == KL_STREAM_WOULD_BLOCK) { would_block++; g.ready = 0; break; }
            ASSERT_EQ((int)ws, (int)KL_STREAM_ACCEPTED);
            sent += n;
        }
        (void)kl_event_ctx_run(&utest_fixture->ev, 16, 5);
    }
    ASSERT_EQ(sent, total);
    ASSERT_GT(would_block, 10);
    ASSERT_EQ(g.edges, would_block);                          /* each block resumed by one edge */

    /* Stdin EOF for the child: a graceful close drains, detaches, and kl_pipe_free closes A. */
    ASSERT_EQ(kl_stream_close_begin(st), 0);
    ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_closed, &r, 10000));
    ASSERT_EQ(r.closes, 1);
    ASSERT_EQ(r.terminals + r.data_calls, 0);                 /* no read facet: nothing delivered */
    kl_pipe_free(p);
    pump_for(&utest_fixture->ev, 20);
    kl_plat_thread_join(&c.t);
    ASSERT_EQ(c.done, total);
    ASSERT_EQ(c.bad, 0);
    ASSERT_EQ(c.eof, 1);
    kl_anon_pipe_end_close(&e);
    rec_free(&r);
    ASSERT_RECLAIMED();
}

UTEST_F(anon_iocp, reads_child_end_closed_without_data_is_eof) {
    NEED_IOCP();
    Rec r; memset(&r, 0, sizeof r);
    KlPipeConfig cfg = rec_cfg(&r, 0, 0);
    KlPipeStream *p = NULL; KlAnonPipeEnd e;
    ASSERT_EQ(kl_anon_pipe_create(&utest_fixture->ev, KL_ANON_PIPE_READS, &cfg, &p, &e), KL_PIPE_OK);
    ASSERT_EQ(kl_stream_read_start(kl_pipe_stream(p)), 0);
    kl_anon_pipe_end_close(&e);                               /* the child never started */
    ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_terminal, &r, 5000));
    ASSERT_EQ(r.terminals, 1);
    ASSERT_EQ(r.len, 0u);
    kl_pipe_free(p);
    rec_free(&r);
    ASSERT_RECLAIMED();
}

UTEST_F(anon_iocp, writes_child_gone_errors_and_graceful_close_still_detaches) {
    NEED_IOCP();
    Rec r; memset(&r, 0, sizeof r);
    KlPipeConfig cfg = rec_cfg(&r, 0, 64 * 1024);
    KlPipeStream *p = NULL; KlAnonPipeEnd e;
    ASSERT_EQ(kl_anon_pipe_create(&utest_fixture->ev, KL_ANON_PIPE_WRITES, &cfg, &p, &e), KL_PIPE_OK);
    KlStream *st = kl_pipe_stream(p);
    kl_anon_pipe_end_close(&e);                               /* the child is gone */
    static char chunk[16 * 1024];
    memset(chunk, 'q', sizeof chunk);
    int saw_error = 0;
    for (int i = 0; i < 200 && !saw_error; i++) {
        KlStreamWriteStatus ws = kl_stream_write(st, chunk, sizeof chunk);
        if (ws == KL_STREAM_ERROR) saw_error = 1;
        (void)kl_event_ctx_run(&utest_fixture->ev, 16, 5);
    }
    ASSERT_EQ(saw_error, 1);                                  /* broken pipe surfaced, sticky */
    ASSERT_EQ(kl_stream_close_begin(st), 0);                  /* graceful, with output undeliverable */
    ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_closed, &r, 5000));
    ASSERT_EQ(r.closes, 1);
    kl_pipe_free(p);
    rec_free(&r);
    ASSERT_RECLAIMED();
}

UTEST_F(anon_iocp, pause_holds_resume_delivers) {
    NEED_IOCP();
    Rec r; memset(&r, 0, sizeof r);
    KlPipeConfig cfg = rec_cfg(&r, 0, 0);
    KlPipeStream *p = NULL; KlAnonPipeEnd e;
    ASSERT_EQ(kl_anon_pipe_create(&utest_fixture->ev, KL_ANON_PIPE_READS, &cfg, &p, &e), KL_PIPE_OK);
    KlStream *st = kl_pipe_stream(p);
    ASSERT_EQ(kl_stream_read_start(st), 0);
    kl_stream_pause(st);
    DWORD k = 0;
    ASSERT_TRUE(WriteFile((HANDLE)kl_anon_pipe_end_handle(&e), "hello", 5, &k, NULL));
    pump_for(&utest_fixture->ev, 100);
    ASSERT_EQ(r.len, 0u);                                     /* held, not delivered */
    ASSERT_EQ(kl_stream_resume(st), 0);
    LenWant w = { &r, 5 };
    ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_len, &w, 2000));
    ASSERT_EQ(memcmp(r.buf, "hello", 5), 0);
    kl_anon_pipe_end_close(&e);
    kl_pipe_free(p);
    rec_free(&r);
    ASSERT_RECLAIMED();
}

/* ── Lifetime ───────────────────────────────────────────────────────────────────────────────── */

UTEST_F(anon_iocp, cancel_with_read_pending) {
    NEED_IOCP();
    Rec r; memset(&r, 0, sizeof r);
    KlPipeConfig cfg = rec_cfg(&r, 0, 0);
    KlPipeStream *p = NULL; KlAnonPipeEnd e;
    ASSERT_EQ(kl_anon_pipe_create(&utest_fixture->ev, KL_ANON_PIPE_READS, &cfg, &p, &e), KL_PIPE_OK);
    ASSERT_EQ(kl_stream_read_start(kl_pipe_stream(p)), 0);
    ASSERT_EQ(kl_stream_cancel(kl_pipe_stream(p)), 0);
    ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_closed, &r, 2000));
    ASSERT_EQ(r.closes, 1);
    kl_pipe_free(p);
    kl_anon_pipe_end_close(&e);
    rec_free(&r);
    ASSERT_RECLAIMED();
}

static KlPipeStream *g_free_me;
static int g_freed_in_cb;
static void free_on_terminal(void *ud, const char *b, size_t n, int ok) {
    (void)ud; (void)b; (void)n;
    if (!ok && g_free_me) { kl_pipe_free(g_free_me); g_free_me = NULL; g_freed_in_cb++; }
}

UTEST_F(anon_iocp, free_from_inside_callback) {
    NEED_IOCP();
    KlPipeConfig cfg; memset(&cfg, 0, sizeof cfg);
    cfg.on_data = free_on_terminal;
    KlPipeStream *p = NULL; KlAnonPipeEnd e;
    ASSERT_EQ(kl_anon_pipe_create(&utest_fixture->ev, KL_ANON_PIPE_READS, &cfg, &p, &e), KL_PIPE_OK);
    ASSERT_EQ(kl_stream_read_start(kl_pipe_stream(p)), 0);
    g_free_me = p; g_freed_in_cb = 0;
    kl_anon_pipe_end_close(&e);
    pump_for(&utest_fixture->ev, 200);
    ASSERT_EQ(g_freed_in_cb, 1);
    ASSERT_RECLAIMED();
}

UTEST_F(anon_iocp, repeated_create_close_races_reclaim_everything) {
    NEED_IOCP();
    for (int i = 0; i < 200; i++) {
        Rec r; memset(&r, 0, sizeof r);
        KlPipeConfig cfg = rec_cfg(&r, 0, 0);
        KlPipeStream *p = NULL; KlAnonPipeEnd e;
        KlAnonPipeDir d = (i & 1) ? KL_ANON_PIPE_WRITES : KL_ANON_PIPE_READS;
        ASSERT_EQ(kl_anon_pipe_create(&utest_fixture->ev, d, &cfg, &p, &e), KL_PIPE_OK);
        KlStream *st = kl_pipe_stream(p);
        if (d == KL_ANON_PIPE_READS) ASSERT_EQ(kl_stream_read_start(st), 0);
        else ASSERT_EQ((int)kl_stream_write(st, "abc", 3), (int)KL_STREAM_ACCEPTED);
        switch (i % 4) {                                      /* vary who goes first */
        case 0: kl_anon_pipe_end_close(&e); kl_pipe_free(p); break;
        case 1: kl_pipe_free(p); kl_anon_pipe_end_close(&e); break;
        case 2: (void)kl_stream_cancel(st); kl_pipe_free(p); kl_anon_pipe_end_close(&e); break;
        default: (void)kl_stream_close_begin(st); kl_anon_pipe_end_close(&e); kl_pipe_free(p); break;
        }
        (void)kl_event_ctx_run(&utest_fixture->ev, 16, 0);
        rec_free(&r);
    }
    ASSERT_RECLAIMED();
}

UTEST_F(anon_iocp, allocation_failure_leaves_nothing_open) {
    NEED_IOCP();
    int saw_nomem = 0, saw_ok = 0;
    for (int nth = 1; nth <= 12 && !saw_ok; nth++) {
        Rec r; memset(&r, 0, sizeof r);
        KlPipeConfig cfg = rec_cfg(&r, 0, 0);
        KlPipeStream *p = (KlPipeStream *)1; KlAnonPipeEnd e;
        g_fail_nth = nth;
        KlPipeStatus st = kl_anon_pipe_create(&utest_fixture->ev, (nth & 1) ? KL_ANON_PIPE_READS
                                                                            : KL_ANON_PIPE_WRITES,
                                              &cfg, &p, &e);
        g_fail_nth = 0;
        if (st == KL_PIPE_OK) {
            saw_ok = 1;
            kl_anon_pipe_end_close(&e);
            kl_pipe_free(p);
        } else {
            ASSERT_EQ((int)st, (int)KL_PIPE_NOMEM);
            ASSERT_TRUE(p == NULL);
            ASSERT_TRUE(kl_anon_pipe_end_handle(&e) == NULL);
            saw_nomem++;
            /* Both ends closed inside create: nothing charged to this attempt. */
            ASSERT_EQ((long)handle_count(), (long)utest_fixture->handles0);
        }
        rec_free(&r);
    }
    ASSERT_GE(saw_nomem, 2);                                  /* several failure points exercised */
    ASSERT_EQ(saw_ok, 1);
    ASSERT_RECLAIMED();
}

#else /* POSIX */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include "../src/platform_thread.h"   /* the PAL thread seam, for the child-side helper */
#if defined(__COSMOPOLITAN__)
#include <cosmo.h>                    /* IsWindows(): the Cosmopolitan-on-Windows known defect */
#endif

static void rec_free(Rec *r) { free(r->buf); memset(r, 0, sizeof(*r)); }

/* ── Harness ────────────────────────────────────────────────────────────────────────────────── */

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
typedef int (*CondFn)(void *);
static int pump_until(KlEventCtx *ev, CondFn cond, void *arg, unsigned ms) {
    uint64_t end = now_ms() + ms;
    for (;;) {
        if (cond && cond(arg)) return 1;
        if (now_ms() >= end) return cond ? cond(arg) : 1;
        if (kl_event_ctx_run(ev, 16, 5) < 0) return 0;
    }
}
static void pump_for(KlEventCtx *ev, unsigned ms) { (void)pump_until(ev, NULL, NULL, ms); }
static int cond_closed(void *a)   { return ((Rec *)a)->closes > 0; }
static int cond_terminal(void *a) { return ((Rec *)a)->terminals > 0; }
typedef struct { Rec *r; size_t want; } LenWant;
static int cond_len(void *a)      { LenWant *w = a; return w->r->len >= w->want; }

static int fd_count(void) {
    int n = 0;
    for (int fd = 0; fd < 1024; fd++) if (fcntl(fd, F_GETFD) != -1) n++;
    return n;
}
static unsigned char pat(size_t i) { return (unsigned char)((i * 131u + (i >> 9)) & 0xFF); }

/* SIGPIPE must never reach the process: the tests run with the DEFAULT disposition, so a leaked one
 * kills the test binary. */
static void sigpipe_default(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPIPE, &sa, NULL);
}
static int sigpipe_is_default(void) {
    struct sigaction cur;
    return sigaction(SIGPIPE, NULL, &cur) == 0 && cur.sa_handler == SIG_DFL;
}
static int sigpipe_pending(void) {
    sigset_t p;
    return sigpending(&p) == 0 && sigismember(&p, SIGPIPE) == 1;
}

/* The child's side, on a helper thread: plain blocking read / write on endpoint B. */
typedef struct {
    int          fd;
    int          mode;      /* 0 = write `total` pattern bytes then stop; 1 = read to EOF */
    size_t       total, done;
    int          eof, bad, err;
    KlPlatThread t;
} Child;

static void child_main(void *arg) {
    Child *c = arg;
    if (c->mode == 0) {
        char b[7000];
        while (c->done < c->total) {
            size_t n = c->total - c->done < sizeof b ? c->total - c->done : sizeof b;
            for (size_t i = 0; i < n; i++) b[i] = (char)pat(c->done + i);
            ssize_t k = write(c->fd, b, n);
            if (k < 0) { if (errno == EINTR) continue; c->err = errno; return; }
            c->done += (size_t)k;
        }
    } else {
        char b[5000];
        for (;;) {
            ssize_t k = read(c->fd, b, sizeof b);
            if (k < 0) { if (errno == EINTR) continue; c->err = errno; return; }
            if (k == 0) { c->eof = 1; return; }
            for (ssize_t i = 0; i < k; i++) if ((unsigned char)b[i] != pat(c->done + (size_t)i)) c->bad++;
            c->done += (size_t)k;
        }
    }
}
static void child_start(Child *c) {
    if (kl_plat_thread_create(&c->t, child_main, c) != 0) { fprintf(stderr, "harness: thread\n"); abort(); }
}

typedef struct { int ready; int edges; } Edge;
static void edge_writable(void *ud) { Edge *g = ud; g->ready = 1; g->edges++; }
static int cond_edge(void *a) { return ((Edge *)a)->edges > 0; }

/* ── Fixture ────────────────────────────────────────────────────────────────────────────────── */

struct anon_px { KlEventCtx ev; long blocks0; long long bytes0; int fds0; };

UTEST_F_SETUP(anon_px) {
    sigpipe_default();
    ASSERT_EQ(kl_event_ctx_init(&utest_fixture->ev, &g_alloc), 0);
    utest_fixture->blocks0 = g_counts.live_blocks;
    utest_fixture->bytes0  = g_counts.live_bytes;
    utest_fixture->fds0    = fd_count();
}
UTEST_F_TEARDOWN(anon_px) {
    (void)utest_result;
    kl_event_ctx_free(&utest_fixture->ev);
}
/* Every case ends here: nothing allocated, no descriptor left open, SIGPIPE never delivered. */
#define ASSERT_RECLAIMED() do {                                                              \
        pump_for(&utest_fixture->ev, 30);                                                    \
        ASSERT_EQ(g_counts.live_blocks, utest_fixture->blocks0);                             \
        ASSERT_EQ(g_counts.live_bytes, utest_fixture->bytes0);                               \
        ASSERT_EQ(fd_count(), utest_fixture->fds0);                                          \
        ASSERT_TRUE(sigpipe_is_default());                                                   \
        ASSERT_FALSE(sigpipe_pending());                                                     \
    } while (0)

/* ── The pair itself ────────────────────────────────────────────────────────────────────────── */

UTEST_F(anon_px, pair_is_directional_blocking_for_the_child_and_close_on_exec) {
    for (int d = KL_ANON_PIPE_READS; d <= KL_ANON_PIPE_WRITES; d++) {
        Rec r; memset(&r, 0, sizeof r);
        KlPipeConfig cfg = rec_cfg(&r, 0, 0);
        KlPipeStream *p = NULL; KlAnonPipeEnd e;
        ASSERT_EQ(kl_anon_pipe_create(&utest_fixture->ev, (KlAnonPipeDir)d, &cfg, &p, &e), KL_PIPE_OK);
        int cfd = kl_anon_pipe_end_fd(&e);
        ASSERT_GE(cfd, 0);
        ASSERT_TRUE((fcntl(cfd, F_GETFD) & FD_CLOEXEC) != 0);    /* the spawner opts it in */
        ASSERT_EQ((fcntl(cfd, F_GETFL) & O_NONBLOCK), 0);         /* children expect blocking stdio */
        ASSERT_EQ(fd_count(), utest_fixture->fds0 + 2);           /* exactly the two ends */
        char b[4];
        if (d == KL_ANON_PIPE_READS) {
            ASSERT_EQ((int)read(cfd, b, sizeof b), -1);           /* the child end writes only */
            ASSERT_EQ((int)kl_stream_write(kl_pipe_stream(p), "x", 1), (int)KL_STREAM_ERROR);
        } else {
            ASSERT_EQ((int)write(cfd, "x", 1), -1);               /* the child end reads only */
            ASSERT_EQ(kl_stream_read_start(kl_pipe_stream(p)), -1);
        }
        kl_anon_pipe_end_close(&e);
        ASSERT_EQ(kl_anon_pipe_end_fd(&e), -1);
        ASSERT_EQ(fcntl(cfd, F_GETFD), -1);                       /* really closed */
        kl_anon_pipe_end_close(&e);                               /* idempotent */
        kl_pipe_free(p);
        rec_free(&r);
    }
    ASSERT_RECLAIMED();
}

/* ── Data, EOF and broken pipe ──────────────────────────────────────────────────────────────── */

UTEST_F(anon_px, reads_large_transfer_then_eof) {
    const size_t total = 1u << 20;
    Rec r; memset(&r, 0, sizeof r);
    KlPipeConfig cfg = rec_cfg(&r, 1000, 0);                  /* small buffer: many fragments */
    KlPipeStream *p = NULL; KlAnonPipeEnd e;
    ASSERT_EQ(kl_anon_pipe_create(&utest_fixture->ev, KL_ANON_PIPE_READS, &cfg, &p, &e), KL_PIPE_OK);
    ASSERT_EQ(kl_stream_read_start(kl_pipe_stream(p)), 0);
    Child c; memset(&c, 0, sizeof c);
    c.fd = kl_anon_pipe_end_fd(&e); c.mode = 0; c.total = total;
    child_start(&c);
    LenWant w = { &r, total };
    ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_len, &w, 30000));
    kl_plat_thread_join(&c.t);
    ASSERT_EQ(c.err, 0);
    ASSERT_EQ(r.terminals, 0);                                /* B still open: no EOF yet */
    kl_anon_pipe_end_close(&e);                               /* the child exits */
    ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_terminal, &r, 5000));
    ASSERT_EQ(r.terminals, 1);
    ASSERT_EQ(r.len, total);
    int bad = 0;
    for (size_t i = 0; i < total; i++) if ((unsigned char)r.buf[i] != pat(i)) bad++;
    ASSERT_EQ(bad, 0);
    ASSERT_GT(r.data_calls, 1000);
    ASSERT_EQ(kl_stream_close_begin(kl_pipe_stream(p)), 0);   /* read side ended: detaches at once */
    ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_closed, &r, 2000));
    ASSERT_EQ(r.closes, 1);
    kl_pipe_free(p);
    rec_free(&r);
    ASSERT_RECLAIMED();
}

UTEST_F(anon_px, writes_edge_driven_transfer_then_child_sees_eof) {
    const size_t total = 4u << 20;
    Rec r; memset(&r, 0, sizeof r);
    KlPipeConfig cfg = rec_cfg(&r, 0, 16 * 1024);
    cfg.on_data = NULL;                                       /* allowed for a WRITES pair */
    KlPipeStream *p = NULL; KlAnonPipeEnd e;
    ASSERT_EQ(kl_anon_pipe_create(&utest_fixture->ev, KL_ANON_PIPE_WRITES, &cfg, &p, &e), KL_PIPE_OK);
    KlStream *st = kl_pipe_stream(p);
    Edge g = { 1, 0 };
    ASSERT_EQ(kl_stream_on_writable(st, edge_writable, &g), 0);
    Child c; memset(&c, 0, sizeof c);
    c.fd = kl_anon_pipe_end_fd(&e); c.mode = 1;
    child_start(&c);

    static char chunk[6000];
    size_t sent = 0; int would_block = 0;
    uint64_t end = now_ms() + 30000;
    while (sent < total && now_ms() < end) {
        while (g.ready && sent < total) {
            size_t n = total - sent < sizeof chunk ? total - sent : sizeof chunk;
            for (size_t i = 0; i < n; i++) chunk[i] = (char)pat(sent + i);
            KlStreamWriteStatus ws = kl_stream_write(st, chunk, n);
            if (ws == KL_STREAM_WOULD_BLOCK) { would_block++; g.ready = 0; break; }
            ASSERT_EQ((int)ws, (int)KL_STREAM_ACCEPTED);
            sent += n;
        }
        (void)kl_event_ctx_run(&utest_fixture->ev, 16, 5);
    }
    ASSERT_EQ(sent, total);
    ASSERT_GT(would_block, 0);
    ASSERT_EQ(g.edges, would_block);                          /* each block resumed by one edge */

    ASSERT_EQ(kl_stream_close_begin(st), 0);                  /* end of input for the child */
    ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_closed, &r, 10000));
    ASSERT_EQ(r.closes, 1);
    kl_pipe_free(p);                                          /* closes A: the child reads EOF */
    kl_plat_thread_join(&c.t);
    ASSERT_EQ(c.done, total);
    ASSERT_EQ(c.bad, 0);
    ASSERT_EQ(c.eof, 1);
    kl_anon_pipe_end_close(&e);
    rec_free(&r);
    ASSERT_RECLAIMED();
}

UTEST_F(anon_px, reads_child_end_closed_without_data_is_eof) {
    Rec r; memset(&r, 0, sizeof r);
    KlPipeConfig cfg = rec_cfg(&r, 0, 0);
    KlPipeStream *p = NULL; KlAnonPipeEnd e;
    ASSERT_EQ(kl_anon_pipe_create(&utest_fixture->ev, KL_ANON_PIPE_READS, &cfg, &p, &e), KL_PIPE_OK);
    ASSERT_EQ(kl_stream_read_start(kl_pipe_stream(p)), 0);
    kl_anon_pipe_end_close(&e);                               /* the child never started */
    ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_terminal, &r, 5000));
    ASSERT_EQ(r.terminals, 1);
    ASSERT_EQ(r.len, 0u);
    kl_pipe_free(p);
    rec_free(&r);
    ASSERT_RECLAIMED();
}

/* Fill the pair until the stream refuses: the kernel pipe is full and so is the bounded queue. */
static int fill_until_would_block(KlStream *st) {
    static char chunk[4096];
    memset(chunk, 'f', sizeof chunk);
    for (int i = 0; i < 100000; i++) {
        KlStreamWriteStatus ws = kl_stream_write(st, chunk, sizeof chunk);
        if (ws == KL_STREAM_WOULD_BLOCK) return 1;
        if (ws != KL_STREAM_ACCEPTED) return 0;
    }
    return 0;
}

/* The scenario that ties the stream fixes together: a producer blocked on a full pipe, and the child
 * disappears. The vanished reader arrives in whatever native form the engine reports (HUP / ERR, often
 * as READ to a WRITE watcher); the adapter attempts the write, write() says EPIPE (with no SIGPIPE),
 * the writable edge wakes the blocked producer exactly once, its retry reports the error, and a
 * graceful close still completes. */
UTEST_F(anon_px, blocked_writer_peer_gone_wakes_once_errors_and_closes) {
    Rec r; memset(&r, 0, sizeof r);
    KlPipeConfig cfg = rec_cfg(&r, 0, 16 * 1024);
    KlPipeStream *p = NULL; KlAnonPipeEnd e;
    ASSERT_EQ(kl_anon_pipe_create(&utest_fixture->ev, KL_ANON_PIPE_WRITES, &cfg, &p, &e), KL_PIPE_OK);
    KlStream *st = kl_pipe_stream(p);
    Edge g = { 0, 0 };
    ASSERT_EQ(kl_stream_on_writable(st, edge_writable, &g), 0);
    ASSERT_EQ(fill_until_would_block(st), 1);                 /* blocked: the edge is armed */
    ASSERT_GT((int)kl_stream_write_pending(st), 0);
    pump_for(&utest_fixture->ev, 50);
    ASSERT_EQ(g.edges, 0);                                    /* nothing drained: no wake */

    kl_anon_pipe_end_close(&e);                               /* the child goes away */
    ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_edge, &g, 5000));
    ASSERT_EQ(g.edges, 1);                                    /* woken once ... */
    ASSERT_EQ((int)kl_stream_write(st, "x", 1), (int)KL_STREAM_ERROR);   /* ... the retry reports it */
    pump_for(&utest_fixture->ev, 50);
    ASSERT_EQ(g.edges, 1);                                    /* ... and never again */

    ASSERT_EQ(kl_stream_close_begin(st), 0);                  /* undeliverable queue: still detaches */
    ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_closed, &r, 2000));
    ASSERT_EQ(r.closes, 1);
    kl_pipe_free(p);
    rec_free(&r);
    ASSERT_RECLAIMED();                                       /* includes: no SIGPIPE delivered */
}

UTEST_F(anon_px, write_after_child_gone_errors_without_sigpipe) {
    Rec r; memset(&r, 0, sizeof r);
    KlPipeConfig cfg = rec_cfg(&r, 0, 0);
    KlPipeStream *p = NULL; KlAnonPipeEnd e;
    ASSERT_EQ(kl_anon_pipe_create(&utest_fixture->ev, KL_ANON_PIPE_WRITES, &cfg, &p, &e), KL_PIPE_OK);
    KlStream *st = kl_pipe_stream(p);
    kl_anon_pipe_end_close(&e);
    ASSERT_EQ((int)kl_stream_write(st, "abc", 3), (int)KL_STREAM_ERROR);   /* EPIPE, directly */
    ASSERT_EQ((int)kl_stream_write(st, "abc", 3), (int)KL_STREAM_ERROR);   /* sticky */
    ASSERT_EQ(kl_stream_close_begin(st), 0);
    ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_closed, &r, 2000));
    kl_pipe_free(p);
    rec_free(&r);
    ASSERT_RECLAIMED();
}

/* The local SIGPIPE handling consumes only the signal its own write raised: an embedder's SIGPIPE that
 * was already pending (blocked in this thread) is still pending afterwards. */
UTEST_F(anon_px, embedder_pending_sigpipe_is_left_alone) {
    sigset_t pipe_only, old;
    sigemptyset(&pipe_only);
    sigaddset(&pipe_only, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &pipe_only, &old);
    raise(SIGPIPE);                                           /* the embedder's own, now pending */
    ASSERT_TRUE(sigpipe_pending());

    Rec r; memset(&r, 0, sizeof r);
    KlPipeConfig cfg = rec_cfg(&r, 0, 0);
    KlPipeStream *p = NULL; KlAnonPipeEnd e;
    ASSERT_EQ(kl_anon_pipe_create(&utest_fixture->ev, KL_ANON_PIPE_WRITES, &cfg, &p, &e), KL_PIPE_OK);
    kl_anon_pipe_end_close(&e);
    ASSERT_EQ((int)kl_stream_write(kl_pipe_stream(p), "abc", 3), (int)KL_STREAM_ERROR);
    int still = sigpipe_pending();

    const struct timespec zero = { 0, 0 };                    /* consume it; restore the mask */
    while (sigtimedwait(&pipe_only, NULL, &zero) >= 0) { }
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    ASSERT_TRUE(still);
    kl_pipe_free(p);
    rec_free(&r);
    ASSERT_RECLAIMED();
}

UTEST_F(anon_px, pause_holds_resume_delivers) {
    Rec r; memset(&r, 0, sizeof r);
    KlPipeConfig cfg = rec_cfg(&r, 0, 0);
    KlPipeStream *p = NULL; KlAnonPipeEnd e;
    ASSERT_EQ(kl_anon_pipe_create(&utest_fixture->ev, KL_ANON_PIPE_READS, &cfg, &p, &e), KL_PIPE_OK);
    KlStream *st = kl_pipe_stream(p);
    ASSERT_EQ(kl_stream_read_start(st), 0);
    kl_stream_pause(st);
    ASSERT_EQ((int)write(kl_anon_pipe_end_fd(&e), "hello", 5), 5);
    pump_for(&utest_fixture->ev, 100);
    ASSERT_EQ(r.len, 0u);                                     /* not read while paused */
    ASSERT_EQ(kl_stream_resume(st), 0);
    LenWant w = { &r, 5 };
    ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_len, &w, 2000));
    ASSERT_EQ(memcmp(r.buf, "hello", 5), 0);
    kl_anon_pipe_end_close(&e);
    kl_pipe_free(p);
    rec_free(&r);
    ASSERT_RECLAIMED();
}

/* A writer blocked on a full pipe must SLEEP until the reader makes room. A correct kernel does not
 * report a full pipe as writable, so the loop has nothing to do; a runtime that does (measured:
 * Cosmopolitan on a Windows host, whose poll reports POLLOUT on a full pipe) turns the blocked writer
 * into a busy loop. Keel does not paper over that (no timer backoff, no message-size rule). This case
 * asserts the correct behaviour everywhere; on Cosmopolitan-on-Windows it is a TRACKED KNOWN DEFECT:
 * seeing the spin skips with that label, and NOT seeing it fails, so the fix upstream is noticed and
 * the exemption removed. */
UTEST_F(anon_px, blocked_writer_does_not_busy_wake) {
    Rec r; memset(&r, 0, sizeof r);
    KlPipeConfig cfg = rec_cfg(&r, 0, 16 * 1024);
    KlPipeStream *p = NULL; KlAnonPipeEnd e;
    ASSERT_EQ(kl_anon_pipe_create(&utest_fixture->ev, KL_ANON_PIPE_WRITES, &cfg, &p, &e), KL_PIPE_OK);
    ASSERT_EQ(fill_until_would_block(kl_pipe_stream(p)), 1);
    /* 200 ms of loop time with nobody reading. Asleep: about zero events (a kernel may report the last
     * few bytes of room once or twice while the queue tops the pipe up). Spinning: every tick returns
     * at once, so hundreds. The thresholds sit far from both. */
    int events = 0;
    uint64_t until = now_ms() + 200;
    while (now_ms() < until) {
        int n = kl_event_ctx_run(&utest_fixture->ev, 16, 20);
        if (n > 0) events += n;
    }
    kl_pipe_free(p);
    kl_anon_pipe_end_close(&e);
    rec_free(&r);
#if defined(__COSMOPOLITAN__)
    if (IsWindows()) {
        ASSERT_RECLAIMED();
        if (events > 50) UTEST_SKIP("KNOWN DEFECT (Cosmopolitan on Windows): a full pipe is reported writable");
        ASSERT_GT(events, 50);   /* reached only when the defect is gone: drop this exemption */
    }
#endif
    ASSERT_LE(events, 5);                                     /* asleep, not spinning */
    ASSERT_RECLAIMED();
}

/* ── Lifetime ───────────────────────────────────────────────────────────────────────────────── */

UTEST_F(anon_px, cancel_with_read_pending) {
    Rec r; memset(&r, 0, sizeof r);
    KlPipeConfig cfg = rec_cfg(&r, 0, 0);
    KlPipeStream *p = NULL; KlAnonPipeEnd e;
    ASSERT_EQ(kl_anon_pipe_create(&utest_fixture->ev, KL_ANON_PIPE_READS, &cfg, &p, &e), KL_PIPE_OK);
    ASSERT_EQ(kl_stream_read_start(kl_pipe_stream(p)), 0);
    ASSERT_EQ(kl_stream_cancel(kl_pipe_stream(p)), 0);
    ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_closed, &r, 2000));
    ASSERT_EQ(r.closes, 1);
    kl_pipe_free(p);
    kl_anon_pipe_end_close(&e);
    rec_free(&r);
    ASSERT_RECLAIMED();
}

static KlPipeStream *g_free_me;
static int g_freed_in_cb;
static void free_on_terminal(void *ud, const char *b, size_t n, int ok) {
    (void)ud; (void)b; (void)n;
    if (!ok && g_free_me) { kl_pipe_free(g_free_me); g_free_me = NULL; g_freed_in_cb++; }
}

UTEST_F(anon_px, free_from_inside_callback) {
    KlPipeConfig cfg; memset(&cfg, 0, sizeof cfg);
    cfg.on_data = free_on_terminal;
    KlPipeStream *p = NULL; KlAnonPipeEnd e;
    ASSERT_EQ(kl_anon_pipe_create(&utest_fixture->ev, KL_ANON_PIPE_READS, &cfg, &p, &e), KL_PIPE_OK);
    ASSERT_EQ(kl_stream_read_start(kl_pipe_stream(p)), 0);
    g_free_me = p; g_freed_in_cb = 0;
    kl_anon_pipe_end_close(&e);
    pump_for(&utest_fixture->ev, 200);
    ASSERT_EQ(g_freed_in_cb, 1);
    ASSERT_RECLAIMED();
}

UTEST_F(anon_px, repeated_create_close_races_reclaim_everything) {
    for (int i = 0; i < 200; i++) {
        Rec r; memset(&r, 0, sizeof r);
        KlPipeConfig cfg = rec_cfg(&r, 0, 0);
        KlPipeStream *p = NULL; KlAnonPipeEnd e;
        KlAnonPipeDir d = (i & 1) ? KL_ANON_PIPE_WRITES : KL_ANON_PIPE_READS;
        ASSERT_EQ(kl_anon_pipe_create(&utest_fixture->ev, d, &cfg, &p, &e), KL_PIPE_OK);
        KlStream *st = kl_pipe_stream(p);
        if (d == KL_ANON_PIPE_READS) ASSERT_EQ(kl_stream_read_start(st), 0);
        else ASSERT_EQ((int)kl_stream_write(st, "abc", 3), (int)KL_STREAM_ACCEPTED);
        switch (i % 4) {                                      /* vary who goes first */
        case 0: kl_anon_pipe_end_close(&e); kl_pipe_free(p); break;
        case 1: kl_pipe_free(p); kl_anon_pipe_end_close(&e); break;
        case 2: (void)kl_stream_cancel(st); kl_pipe_free(p); kl_anon_pipe_end_close(&e); break;
        default: (void)kl_stream_close_begin(st); kl_anon_pipe_end_close(&e); kl_pipe_free(p); break;
        }
        (void)kl_event_ctx_run(&utest_fixture->ev, 16, 0);
        rec_free(&r);
    }
    ASSERT_RECLAIMED();
}

UTEST_F(anon_px, allocation_failure_leaves_nothing_open) {
    int saw_nomem = 0, saw_ok = 0;
    for (int nth = 1; nth <= 12 && !saw_ok; nth++) {
        Rec r; memset(&r, 0, sizeof r);
        KlPipeConfig cfg = rec_cfg(&r, 0, 0);
        KlPipeStream *p = (KlPipeStream *)1; KlAnonPipeEnd e;
        g_fail_nth = nth;
        KlPipeStatus st = kl_anon_pipe_create(&utest_fixture->ev, (nth & 1) ? KL_ANON_PIPE_READS
                                                                            : KL_ANON_PIPE_WRITES,
                                              &cfg, &p, &e);
        g_fail_nth = 0;
        if (st == KL_PIPE_OK) {
            saw_ok = 1;
            kl_anon_pipe_end_close(&e);
            kl_pipe_free(p);
        } else {
            ASSERT_EQ((int)st, (int)KL_PIPE_NOMEM);
            ASSERT_TRUE(p == NULL);
            ASSERT_EQ(kl_anon_pipe_end_fd(&e), -1);
            saw_nomem++;
            ASSERT_EQ(fd_count(), utest_fixture->fds0);       /* both ends closed inside create */
        }
        rec_free(&r);
    }
    ASSERT_GE(saw_nomem, 2);
    ASSERT_EQ(saw_ok, 1);
    ASSERT_RECLAIMED();
}

#endif /* _WIN32 / POSIX */

UTEST_MAIN();
