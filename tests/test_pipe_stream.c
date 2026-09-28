/*
 * test_pipe_stream.c: Windows Named Pipes as a KlStream (<keel/pipe.h>).
 *
 * Everything after kl_pipe_connect goes through the KlStream API only (read_start / write / pause /
 * resume / close_begin / cancel), which is the claim under test: a named pipe differs from a socket
 * below KlStream and not at all above it. The framed-exchange cases drive a small u32be-length codec
 * written against KlStream alone (it never names a pipe), the shape a consumer such as an SSH-agent
 * client needs.
 *
 * The SERVER side is test scaffolding in raw Win32 (CreateNamedPipeW / ConnectNamedPipe / blocking
 * ReadFile / WriteFile on a helper thread). Test TUs are outside the Tier-1 gate, so that is allowed
 * here and nowhere in the library.
 *
 * Engine matrix. On the IOCP engine every case runs for real. On any other engine, and on POSIX, the
 * transport must refuse with KL_PIPE_UNSUPPORTED before touching the OS; that case runs everywhere.
 *
 * Lifetime is checked with a counting allocator: once a stream has been freed and its last op has
 * retired, the allocator must be back to the level it had before the connect. That is the observable
 * form of "logical close is not physical retirement" (I3): the memory goes only when the ops do.
 */
#include "utest.h"
#include <keel/keel.h>
#include <keel/event_ctx.h>
#include <keel/stream.h>
#include <keel/pipe.h>
#include "../src/event_caps.h"   /* kl_event_caps: is this loop the completion (IOCP) engine? */
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* ── Counting allocator ─────────────────────────────────────────────────────────────────────── */

typedef struct { long live_blocks; long long live_bytes; } Counts;
static Counts g_counts;

static void *ca_malloc(void *c, size_t n) {
    (void)c;
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

/* ── Consumer-side recorder (callbacks from the pipe) ──────────────────────────────────────── */

typedef struct {
    char  *buf;          /* accumulated delivered bytes */
    size_t len, cap;
    int    data_calls;   /* ok=1 deliveries */
    int    terminals;    /* ok=0 deliveries */
    int    closes;       /* on_close */
    int    late;         /* any callback after `sealed` was set */
    int    sealed;
} Rec;

static void rec_on_data(void *ud, const char *b, size_t n, int ok) {
    Rec *r = ud;
    if (r->sealed) r->late++;
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
static void rec_on_close(void *ud) {
    Rec *r = ud;
    if (r->sealed) r->late++;
    r->closes++;
}

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

/* ── Engine gate: runs on every platform and engine ────────────────────────────────────────── */

static int loop_is_completion(KlEventCtx *ev) {
    return (kl_event_caps(&ev->loop) & KL_EVENT_CAP_COMPLETION) != 0;
}

UTEST(pipe, argument_validation_and_status_names) {
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &g_alloc), 0);
    Rec r; memset(&r, 0, sizeof(r));
    KlPipeConfig cfg = rec_cfg(&r, 0, 0);
    KlPipeStream *p = (KlPipeStream *)1;
    ASSERT_EQ(kl_pipe_connect(NULL, "\\\\.\\pipe\\x", &cfg, &p), KL_PIPE_INVALID);
    ASSERT_TRUE(p == NULL);
    ASSERT_EQ(kl_pipe_connect(&ev, NULL, &cfg, &p), KL_PIPE_INVALID);
    ASSERT_EQ(kl_pipe_connect(&ev, "\\\\.\\pipe\\x", NULL, &p), KL_PIPE_INVALID);
    KlPipeConfig nodata = cfg; nodata.on_data = NULL;
    ASSERT_EQ(kl_pipe_connect(&ev, "\\\\.\\pipe\\x", &nodata, &p), KL_PIPE_INVALID);
    ASSERT_STREQ(kl_pipe_status_str(KL_PIPE_BUSY), "busy");
    ASSERT_STREQ(kl_pipe_status_str(KL_PIPE_UNSUPPORTED), "unsupported");
    ASSERT_TRUE(kl_pipe_stream(NULL) == NULL);
    kl_pipe_free(NULL);
    kl_event_ctx_free(&ev);
}

static void unsupported_on_accept(void *ud, KlPipeStream *p) { (void)ud; (void)p; }

/* Off the IOCP engine the transport refuses before any OS call; there is no readiness fallback. */
UTEST(pipe, unsupported_off_iocp) {
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &g_alloc), 0);
    if (loop_is_completion(&ev)) {
#if defined(_WIN32)
        kl_event_ctx_free(&ev);
        UTEST_SKIP("IOCP engine: the transport is supported here");
#endif
        /* A non-Windows completion engine (io_uring / pollcomp): still unsupported. */
    }
    Rec r; memset(&r, 0, sizeof(r));
    KlPipeConfig cfg = rec_cfg(&r, 0, 0);
    KlPipeStream *p = NULL;
    long before = g_counts.live_blocks;
    ASSERT_EQ(kl_pipe_connect(&ev, "\\\\.\\pipe\\keel-never-opened", &cfg, &p), KL_PIPE_UNSUPPORTED);
    ASSERT_TRUE(p == NULL);
    ASSERT_EQ(g_counts.live_blocks, before);   /* refused before allocating anything */
    /* The listener is refused the same way. */
    KlPipeListenConfig lc; memset(&lc, 0, sizeof lc);
    lc.on_accept = unsupported_on_accept;
    KlPipeListener *pl = (KlPipeListener *)1;
    ASSERT_EQ(kl_pipe_listen(&ev, "\\\\.\\pipe\\keel-never-opened", &lc, &pl), KL_PIPE_UNSUPPORTED);
    ASSERT_TRUE(pl == NULL);
    ASSERT_EQ(g_counts.live_blocks, before);
    kl_event_ctx_free(&ev);
}

#if defined(_WIN32)
#include <sddl.h>                     /* ConvertStringSecurityDescriptorToSecurityDescriptorW */
#include "../src/platform_thread.h"   /* the PAL thread seam, for the blocking pipe-server helper */

static void rec_free(Rec *r) { free(r->buf); memset(r, 0, sizeof(*r)); }

/* ── Harness: loop pump ─────────────────────────────────────────────────────────────────────── */

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

/* A pipe-free ctx must not grow: everything the stream allocated comes back only after retirement. */
static int g_baseline_set;
static long g_baseline_blocks;
static long long g_baseline_bytes;
static int cond_balanced(void *a) {
    (void)a;
    return g_counts.live_blocks == g_baseline_blocks && g_counts.live_bytes == g_baseline_bytes;
}
static void mark_baseline(void) {
    g_baseline_set = 1;
    g_baseline_blocks = g_counts.live_blocks;
    g_baseline_bytes = g_counts.live_bytes;
}

/* ── Harness: pipe names + raw Win32 server ─────────────────────────────────────────────────── */

static int g_seq;
static void pipe_name(char *out, size_t n, const char *tag) {
    snprintf(out, n, "\\\\.\\pipe\\keel-test-%lu-%s-%d", (unsigned long)GetCurrentProcessId(), tag,
             ++g_seq);
}
static HANDLE server_create(const char *name, DWORD max_inst, DWORD bufsz, const wchar_t *sddl) {
    wchar_t w[256];
    MultiByteToWideChar(CP_UTF8, 0, name, -1, w, 256);
    SECURITY_ATTRIBUTES sa, *psa = NULL;
    PSECURITY_DESCRIPTOR sd = NULL;
    if (sddl && ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, NULL)) {
        sa.nLength = sizeof(sa); sa.lpSecurityDescriptor = sd; sa.bInheritHandle = FALSE;
        psa = &sa;
    }
    HANDLE h = CreateNamedPipeW(w, PIPE_ACCESS_DUPLEX,
                                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                max_inst, bufsz, bufsz, 0, psa);
    if (sd) LocalFree(sd);
    return h;
}
static int server_accept(HANDLE h) {
    if (ConnectNamedPipe(h, NULL)) return 1;
    return GetLastError() == ERROR_PIPE_CONNECTED;   /* the client beat us to it: already connected */
}
static int read_full(HANDLE h, void *buf, DWORD n) {
    DWORD got = 0;
    while (got < n) {
        DWORD k = 0;
        if (!ReadFile(h, (char *)buf + got, n - got, &k, NULL) || k == 0) return 0;
        got += k;
    }
    return 1;
}
static int write_full(HANDLE h, const void *buf, DWORD n) {
    DWORD done = 0;
    while (done < n) {
        DWORD k = 0;
        if (!WriteFile(h, (const char *)buf + done, n - done, &k, NULL)) return 0;
        done += k;
    }
    return 1;
}

typedef enum { SRV_ECHO, SRV_SEND_CLOSE, SRV_SEND_ABORT, SRV_SINK_THEN_CLOSE, SRV_FRAMED } SrvMode;
typedef struct {
    HANDLE       h;
    SrvMode      mode;
    const char  *payload; DWORD payload_len;   /* SEND_* */
    size_t       sink_want;                     /* SINK_THEN_CLOSE: bytes to read before closing */
    size_t       got;                           /* bytes the server read */
    int          frames;                        /* FRAMED: frames answered */
    int          ok;                            /* the script ran to its end */
    KlPlatThread t;
} Srv;

static void srv_main(void *arg) {
    Srv *s = arg;
    if (!server_accept(s->h)) return;
    switch (s->mode) {
    case SRV_ECHO: {
        char b[3000];
        for (;;) {
            DWORD k = 0;
            if (!ReadFile(s->h, b, sizeof(b), &k, NULL) || k == 0) break;
            s->got += k;
            if (!write_full(s->h, b, k)) break;
        }
        s->ok = 1;
        break;
    }
    case SRV_SEND_CLOSE:
        s->ok = write_full(s->h, s->payload, s->payload_len);
        FlushFileBuffers(s->h);   /* orderly: returns once the client has read every byte */
        break;
    case SRV_SEND_ABORT:
        s->ok = write_full(s->h, s->payload, s->payload_len);
        break;                    /* abortive: DisconnectNamedPipe below discards anything unread */
    case SRV_SINK_THEN_CLOSE: {
        char b[4096];
        while (s->got < s->sink_want) {
            DWORD k = 0;
            if (!ReadFile(s->h, b, sizeof(b), &k, NULL) || k == 0) break;
            s->got += k;
        }
        s->ok = (s->got == s->sink_want);
        break;
    }
    case SRV_FRAMED: {
        /* Answer each u32be-length frame with the payload reversed, until the client disconnects. */
        for (;;) {
            unsigned char hdr[4];
            if (!read_full(s->h, hdr, 4)) break;
            DWORD n = ((DWORD)hdr[0] << 24) | ((DWORD)hdr[1] << 16) | ((DWORD)hdr[2] << 8) | hdr[3];
            char *pl = malloc(n ? n : 1);
            if (n && !read_full(s->h, pl, n)) { free(pl); break; }
            for (DWORD i = 0; i < n / 2; i++) { char c = pl[i]; pl[i] = pl[n - 1 - i]; pl[n - 1 - i] = c; }
            int w = write_full(s->h, hdr, 4) && (n == 0 || write_full(s->h, pl, n));
            free(pl);
            if (!w) break;
            s->frames++;
        }
        s->ok = 1;
        break;
    }
    }
    DisconnectNamedPipe(s->h);
}
static void srv_start(Srv *s) {
    if (kl_plat_thread_create(&s->t, srv_main, s) != 0) { fprintf(stderr, "harness: thread\n"); abort(); }
}
static void srv_join(Srv *s) { kl_plat_thread_join(&s->t); CloseHandle(s->h); }

/* ── Fixture ────────────────────────────────────────────────────────────────────────────────── */

struct pipe_iocp { KlEventCtx ev; int skip; };

UTEST_F_SETUP(pipe_iocp) {
    ASSERT_EQ(kl_event_ctx_init(&utest_fixture->ev, &g_alloc), 0);
    utest_fixture->skip = !loop_is_completion(&utest_fixture->ev);
}
UTEST_F_TEARDOWN(pipe_iocp) {
    (void)utest_result;
    kl_event_ctx_free(&utest_fixture->ev);
}
#define NEED_IOCP() do { if (utest_fixture->skip) UTEST_SKIP("needs BACKEND=iocp"); } while (0)

/* ── Connect outcomes ───────────────────────────────────────────────────────────────────────── */

UTEST_F(pipe_iocp, connect_absent) {
    NEED_IOCP();
    char name[128]; pipe_name(name, sizeof name, "absent");
    Rec r; memset(&r, 0, sizeof r);
    KlPipeConfig cfg = rec_cfg(&r, 0, 0);
    KlPipeStream *p = NULL;
    mark_baseline();
    ASSERT_EQ(kl_pipe_connect(&utest_fixture->ev, name, &cfg, &p), KL_PIPE_ABSENT);
    ASSERT_TRUE(p == NULL);
    ASSERT_TRUE(cond_balanced(NULL));   /* a failed connect leaves nothing behind */
}

UTEST_F(pipe_iocp, connect_rejects_non_local_names) {
    NEED_IOCP();
    Rec r; memset(&r, 0, sizeof r);
    KlPipeConfig cfg = rec_cfg(&r, 0, 0);
    KlPipeStream *p = NULL;
    const char *bad[] = { "", "pipe\\x", "C:\\temp\\x", "\\\\.\\pipe\\", "\\\\server\\pipe\\x",
                          "\\\\.\\mailslot\\x", "\\\\?\\pipe\\x" };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        ASSERT_EQ(kl_pipe_connect(&utest_fixture->ev, bad[i], &cfg, &p), KL_PIPE_INVALID);
}

UTEST_F(pipe_iocp, connect_busy) {
    NEED_IOCP();
    char name[128]; pipe_name(name, sizeof name, "busy");
    HANDLE srv = server_create(name, 1, 4096, NULL);
    ASSERT_TRUE(srv != INVALID_HANDLE_VALUE);
    wchar_t w[256]; MultiByteToWideChar(CP_UTF8, 0, name, -1, w, 256);
    HANDLE first = CreateFileW(w, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    ASSERT_TRUE(first != INVALID_HANDLE_VALUE);   /* takes the only instance */
    Rec r; memset(&r, 0, sizeof r);
    KlPipeConfig cfg = rec_cfg(&r, 0, 0);
    KlPipeStream *p = NULL;
    ASSERT_EQ(kl_pipe_connect(&utest_fixture->ev, name, &cfg, &p), KL_PIPE_BUSY);
    ASSERT_TRUE(p == NULL);
    CloseHandle(first);
    CloseHandle(srv);
}

UTEST_F(pipe_iocp, connect_denied_by_dacl) {
    NEED_IOCP();
    char name[128]; pipe_name(name, sizeof name, "denied");
    HANDLE srv = server_create(name, 1, 4096, L"D:(D;;GA;;;WD)");   /* deny Everyone */
    ASSERT_TRUE(srv != INVALID_HANDLE_VALUE);
    Rec r; memset(&r, 0, sizeof r);
    KlPipeConfig cfg = rec_cfg(&r, 0, 0);
    KlPipeStream *p = NULL;
    ASSERT_EQ(kl_pipe_connect(&utest_fixture->ev, name, &cfg, &p), KL_PIPE_DENIED);
    CloseHandle(srv);
}

/* The client is IDENTIFICATION-only: a server may learn who connected but must not act as them. */
UTEST_F(pipe_iocp, server_cannot_impersonate_client) {
    NEED_IOCP();
    char name[128]; pipe_name(name, sizeof name, "sqos");
    HANDLE srv = server_create(name, 1, 4096, NULL);
    ASSERT_TRUE(srv != INVALID_HANDLE_VALUE);
    Rec r; memset(&r, 0, sizeof r);
    KlPipeConfig cfg = rec_cfg(&r, 0, 0);
    KlPipeStream *p = NULL;
    ASSERT_EQ(kl_pipe_connect(&utest_fixture->ev, name, &cfg, &p), KL_PIPE_OK);
    ASSERT_TRUE(server_accept(srv));
    /* Write one byte so the server has read from the client, which impersonation requires. */
    ASSERT_EQ(kl_stream_write(kl_pipe_stream(p), "x", 1), KL_STREAM_ACCEPTED);
    char b; DWORD k = 0;
    pump_for(&utest_fixture->ev, 20);
    ASSERT_TRUE(ReadFile(srv, &b, 1, &k, NULL) && k == 1);
    ASSERT_TRUE(ImpersonateNamedPipeClient(srv));
    HANDLE tok = NULL;
    ASSERT_TRUE(OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &tok));
    SECURITY_IMPERSONATION_LEVEL lvl = SecurityAnonymous; DWORD rl = 0;
    ASSERT_TRUE(GetTokenInformation(tok, TokenImpersonationLevel, &lvl, sizeof lvl, &rl));
    CloseHandle(tok);
    RevertToSelf();
    ASSERT_EQ((int)lvl, (int)SecurityIdentification);
    kl_pipe_free(p);
    pump_for(&utest_fixture->ev, 20);
    CloseHandle(srv);
}

/* ── Byte stream ────────────────────────────────────────────────────────────────────────────── */

UTEST_F(pipe_iocp, echo_roundtrip) {
    NEED_IOCP();
    char name[128]; pipe_name(name, sizeof name, "echo");
    Srv s; memset(&s, 0, sizeof s);
    s.h = server_create(name, 1, 4096, NULL); s.mode = SRV_ECHO;
    ASSERT_TRUE(s.h != INVALID_HANDLE_VALUE);
    srv_start(&s);
    mark_baseline();
    Rec r; memset(&r, 0, sizeof r);
    KlPipeConfig cfg = rec_cfg(&r, 0, 0);
    KlPipeStream *p = NULL;
    ASSERT_EQ(kl_pipe_connect(&utest_fixture->ev, name, &cfg, &p), KL_PIPE_OK);
    KlStream *st = kl_pipe_stream(p);
    ASSERT_EQ(kl_stream_read_start(st), 0);
    ASSERT_EQ(kl_stream_write(st, "hello, pipe", 11), KL_STREAM_ACCEPTED);
    LenWant w = { &r, 11 };
    ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_len, &w, 5000));
    ASSERT_EQ(r.len, (size_t)11);
    ASSERT_EQ(memcmp(r.buf, "hello, pipe", 11), 0);
    ASSERT_EQ(kl_stream_cancel(st), 0);
    ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_closed, &r, 5000));
    ASSERT_EQ(r.closes, 1);
    kl_pipe_free(p);
    ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_balanced, NULL, 5000));
    srv_join(&s);
    rec_free(&r);
}

/* 1 MiB through small pipe buffers, a 1000-byte client read buffer and a 64 KiB write queue: many
 * partial reads, WOULD_BLOCK backpressure on the write side, byte-exact order. */
UTEST_F(pipe_iocp, large_fragmented_transfer) {
    NEED_IOCP();
    const size_t total = 1u << 20;
    char name[128]; pipe_name(name, sizeof name, "large");
    Srv s; memset(&s, 0, sizeof s);
    s.h = server_create(name, 1, 4096, NULL); s.mode = SRV_ECHO;
    ASSERT_TRUE(s.h != INVALID_HANDLE_VALUE);
    srv_start(&s);
    Rec r; memset(&r, 0, sizeof r);
    KlPipeConfig cfg = rec_cfg(&r, 1000, 64 * 1024);
    KlPipeStream *p = NULL;
    ASSERT_EQ(kl_pipe_connect(&utest_fixture->ev, name, &cfg, &p), KL_PIPE_OK);
    KlStream *st = kl_pipe_stream(p);
    ASSERT_EQ(kl_stream_read_start(st), 0);

    char *src = malloc(total);
    for (size_t i = 0; i < total; i++) src[i] = (char)((i * 131u + (i >> 9)) & 0xFF);
    size_t sent = 0; int would_block = 0;
    ULONGLONG end = GetTickCount64() + 30000;
    while ((sent < total || r.len < total) && GetTickCount64() < end) {
        while (sent < total) {
            size_t n = total - sent < 16384 ? total - sent : 16384;
            KlStreamWriteStatus ws = kl_stream_write(st, src + sent, n);
            if (ws == KL_STREAM_WOULD_BLOCK) { would_block++; break; }
            ASSERT_EQ(ws, KL_STREAM_ACCEPTED);
            sent += n;
        }
        (void)kl_event_ctx_run(&utest_fixture->ev, 16, 5);
    }
    ASSERT_EQ(sent, total);
    ASSERT_EQ(r.len, total);
    ASSERT_EQ(memcmp(r.buf, src, total), 0);
    ASSERT_GT(would_block, 0);        /* the bounded queue pushed back rather than growing */
    ASSERT_GT(r.data_calls, 1000);    /* delivered in >= 1 MiB / 1000 B fragments */
    ASSERT_EQ(kl_stream_write(st, src, 64 * 1024 + 1), KL_STREAM_TOO_LARGE);
    free(src);
    kl_pipe_free(p);
    pump_for(&utest_fixture->ev, 50);
    srv_join(&s);
    rec_free(&r);
}

UTEST_F(pipe_iocp, server_orderly_disconnect_delivers_data_then_eof) {
    NEED_IOCP();
    char name[128]; pipe_name(name, sizeof name, "orderly");
    Srv s; memset(&s, 0, sizeof s);
    s.h = server_create(name, 1, 4096, NULL); s.mode = SRV_SEND_CLOSE;
    s.payload = "goodbye"; s.payload_len = 7;
    srv_start(&s);
    Rec r; memset(&r, 0, sizeof r);
    KlPipeConfig cfg = rec_cfg(&r, 0, 0);
    KlPipeStream *p = NULL;
    ASSERT_EQ(kl_pipe_connect(&utest_fixture->ev, name, &cfg, &p), KL_PIPE_OK);
    ASSERT_EQ(kl_stream_read_start(kl_pipe_stream(p)), 0);
    ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_terminal, &r, 5000));
    ASSERT_EQ(r.len, (size_t)7);
    ASSERT_EQ(memcmp(r.buf, "goodbye", 7), 0);
    ASSERT_EQ(r.terminals, 1);
    ASSERT_EQ(r.closes, 0);           /* EOF is not detachment: the owner decides to close */
    ASSERT_EQ(kl_stream_close_begin(kl_pipe_stream(p)), 0);
    ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_closed, &r, 5000));
    ASSERT_EQ(r.closes, 1);
    ASSERT_EQ(r.terminals, 1);
    kl_pipe_free(p);
    srv_join(&s);
    rec_free(&r);
}

UTEST_F(pipe_iocp, server_abort_delivers_one_terminal) {
    NEED_IOCP();
    char name[128]; pipe_name(name, sizeof name, "abort");
    Srv s; memset(&s, 0, sizeof s);
    s.h = server_create(name, 1, 4096, NULL); s.mode = SRV_SEND_ABORT;
    s.payload = "partial"; s.payload_len = 7;
    srv_start(&s);
    Rec r; memset(&r, 0, sizeof r);
    KlPipeConfig cfg = rec_cfg(&r, 0, 0);
    KlPipeStream *p = NULL;
    ASSERT_EQ(kl_pipe_connect(&utest_fixture->ev, name, &cfg, &p), KL_PIPE_OK);
    srv_join(&s);                     /* the server has written and forcibly disconnected */
    ASSERT_EQ(kl_stream_read_start(kl_pipe_stream(p)), 0);
    ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_terminal, &r, 5000));
    pump_for(&utest_fixture->ev, 50);
    ASSERT_EQ(r.terminals, 1);        /* exactly one terminal, whatever arrived before it */
    ASSERT_LE(r.len, (size_t)7);
    /* A write after the peer vanished fails, sticky, once its completion reports the broken pipe. */
    KlStream *st = kl_pipe_stream(p);
    KlStreamWriteStatus ws = kl_stream_write(st, "x", 1);
    pump_for(&utest_fixture->ev, 50);
    if (ws == KL_STREAM_ACCEPTED) ws = kl_stream_write(st, "y", 1);
    ASSERT_EQ(ws, KL_STREAM_ERROR);
    kl_pipe_free(p);
    pump_for(&utest_fixture->ev, 20);
    rec_free(&r);
}

/* Strict pause holds a completed read undelivered; resume delivers it exactly once. */
UTEST_F(pipe_iocp, pause_holds_resume_delivers_once) {
    NEED_IOCP();
    char name[128]; pipe_name(name, sizeof name, "pause");
    Srv s; memset(&s, 0, sizeof s);
    s.h = server_create(name, 1, 4096, NULL); s.mode = SRV_ECHO;
    srv_start(&s);
    Rec r; memset(&r, 0, sizeof r);
    KlPipeConfig cfg = rec_cfg(&r, 0, 0);
    KlPipeStream *p = NULL;
    ASSERT_EQ(kl_pipe_connect(&utest_fixture->ev, name, &cfg, &p), KL_PIPE_OK);
    KlStream *st = kl_pipe_stream(p);
    ASSERT_EQ(kl_stream_read_start(st), 0);
    kl_stream_pause(st);
    ASSERT_EQ(kl_stream_write(st, "held", 4), KL_STREAM_ACCEPTED);
    pump_for(&utest_fixture->ev, 200);
    ASSERT_EQ(r.data_calls, 0);
    ASSERT_EQ(kl_stream_read_held(st), 1);
    ASSERT_EQ(kl_stream_resume(st), 0);
    ASSERT_EQ(r.data_calls, 1);
    ASSERT_EQ(r.len, (size_t)4);
    ASSERT_EQ(memcmp(r.buf, "held", 4), 0);
    kl_pipe_free(p);
    pump_for(&utest_fixture->ev, 20);
    srv_join(&s);
    rec_free(&r);
}

/* ── Cancellation and close (I3: logical close is not physical retirement) ──────────────────── */

UTEST_F(pipe_iocp, cancel_with_read_pending) {
    NEED_IOCP();
    char name[128]; pipe_name(name, sizeof name, "cancel-read");
    HANDLE srv = server_create(name, 1, 4096, NULL);   /* connects, never writes */
    mark_baseline();
    Rec r; memset(&r, 0, sizeof r);
    KlPipeConfig cfg = rec_cfg(&r, 0, 0);
    KlPipeStream *p = NULL;
    ASSERT_EQ(kl_pipe_connect(&utest_fixture->ev, name, &cfg, &p), KL_PIPE_OK);
    KlStream *st = kl_pipe_stream(p);
    ASSERT_EQ(kl_stream_read_start(st), 0);
    pump_for(&utest_fixture->ev, 30);
    ASSERT_EQ(kl_stream_cancel(st), 0);
    ASSERT_EQ(r.closes, 0);           /* the ReadFile is still physically outstanding */
    ASSERT_EQ((int)kl_stream_close_state(st), (int)KL_STREAM_STATE_CLOSING);
    ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_closed, &r, 5000));
    ASSERT_EQ(r.closes, 1);
    ASSERT_EQ(r.data_calls, 0);
    ASSERT_EQ(r.terminals, 0);        /* a logically closed read is dropped, not delivered */
    kl_pipe_free(p);
    ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_balanced, NULL, 5000));
    CloseHandle(srv);
}

UTEST_F(pipe_iocp, cancel_with_write_pending) {
    NEED_IOCP();
    char name[128]; pipe_name(name, sizeof name, "cancel-write");
    HANDLE srv = server_create(name, 1, 1024, NULL);   /* tiny quota, never read */
    mark_baseline();
    Rec r; memset(&r, 0, sizeof r);
    KlPipeConfig cfg = rec_cfg(&r, 0, 256 * 1024);
    KlPipeStream *p = NULL;
    ASSERT_EQ(kl_pipe_connect(&utest_fixture->ev, name, &cfg, &p), KL_PIPE_OK);
    KlStream *st = kl_pipe_stream(p);
    static char big[200 * 1024];
    ASSERT_EQ(kl_stream_write(st, big, sizeof big), KL_STREAM_ACCEPTED);
    pump_for(&utest_fixture->ev, 100);
    ASSERT_GT(kl_stream_write_pending(st), (size_t)0);   /* the WriteFile cannot finish */
    ASSERT_EQ(kl_stream_cancel(st), 0);
    ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_closed, &r, 5000));
    ASSERT_EQ(r.closes, 1);
    kl_pipe_free(p);
    ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_balanced, NULL, 5000));
    CloseHandle(srv);
}

/* Graceful close drains queued output, then waits for the outstanding read to retire (here: the
 * server reads everything and disconnects). on_close fires once; nothing is delivered after it. */
UTEST_F(pipe_iocp, graceful_close_drains_output) {
    NEED_IOCP();
    const size_t total = 40000;
    char name[128]; pipe_name(name, sizeof name, "graceful");
    Srv s; memset(&s, 0, sizeof s);
    s.h = server_create(name, 1, 2048, NULL); s.mode = SRV_SINK_THEN_CLOSE; s.sink_want = total;
    srv_start(&s);
    Rec r; memset(&r, 0, sizeof r);
    KlPipeConfig cfg = rec_cfg(&r, 0, 64 * 1024);
    KlPipeStream *p = NULL;
    ASSERT_EQ(kl_pipe_connect(&utest_fixture->ev, name, &cfg, &p), KL_PIPE_OK);
    KlStream *st = kl_pipe_stream(p);
    ASSERT_EQ(kl_stream_read_start(st), 0);
    char *buf = calloc(1, total);
    ASSERT_EQ(kl_stream_write(st, buf, total), KL_STREAM_ACCEPTED);
    ASSERT_EQ(kl_stream_close_begin(st), 0);
    ASSERT_EQ(kl_stream_write(st, "late", 4), KL_STREAM_CLOSED);
    ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_closed, &r, 10000));
    srv_join(&s);
    ASSERT_EQ(s.got, total);          /* every queued byte reached the peer before detachment */
    ASSERT_EQ(r.closes, 1);
    ASSERT_EQ(r.terminals, 0);
    free(buf);
    kl_pipe_free(p);
    rec_free(&r);
}

/* Free with both a read and a write outstanding: no callback after the free, and the memory comes
 * back only once the kernel has given both ops back. */
UTEST_F(pipe_iocp, free_with_outstanding_ops_then_no_callbacks) {
    NEED_IOCP();
    char name[128]; pipe_name(name, sizeof name, "free-outstanding");
    HANDLE srv = server_create(name, 1, 1024, NULL);
    mark_baseline();
    Rec r; memset(&r, 0, sizeof r);
    KlPipeConfig cfg = rec_cfg(&r, 0, 128 * 1024);
    KlPipeStream *p = NULL;
    ASSERT_EQ(kl_pipe_connect(&utest_fixture->ev, name, &cfg, &p), KL_PIPE_OK);
    KlStream *st = kl_pipe_stream(p);
    ASSERT_EQ(kl_stream_read_start(st), 0);
    static char big[100 * 1024];
    ASSERT_EQ(kl_stream_write(st, big, sizeof big), KL_STREAM_ACCEPTED);
    pump_for(&utest_fixture->ev, 30);
    ASSERT_GT(g_counts.live_blocks, g_baseline_blocks);
    kl_pipe_free(p);
    r.sealed = 1;
    ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_balanced, NULL, 5000));
    /* The server now writes and disconnects: nothing may reach the freed stream's consumer. */
    DWORD k = 0; WriteFile(srv, "zzz", 3, &k, NULL);
    DisconnectNamedPipe(srv);
    pump_for(&utest_fixture->ev, 50);
    ASSERT_EQ(r.late, 0);
    ASSERT_EQ(r.closes, 0);
    CloseHandle(srv);
}

/* Free from inside the terminal delivery: the stream memory must survive the rest of the dispatch. */
static void free_on_terminal(void *ud, const char *b, size_t n, int ok) {
    (void)b; (void)n;
    KlPipeStream **pp = ud;
    if (!ok && *pp) { kl_pipe_free(*pp); *pp = NULL; }
}
UTEST_F(pipe_iocp, free_from_inside_callback) {
    NEED_IOCP();
    char name[128]; pipe_name(name, sizeof name, "free-in-cb");
    HANDLE srv = server_create(name, 1, 4096, NULL);
    mark_baseline();
    KlPipeStream *p = NULL;
    KlPipeConfig cfg; memset(&cfg, 0, sizeof cfg);
    cfg.on_data = free_on_terminal; cfg.user_data = &p;
    ASSERT_EQ(kl_pipe_connect(&utest_fixture->ev, name, &cfg, &p), KL_PIPE_OK);
    ASSERT_TRUE(server_accept(srv));
    ASSERT_EQ(kl_stream_read_start(kl_pipe_stream(p)), 0);
    DisconnectNamedPipe(srv);         /* → broken pipe → terminal → free inside the callback */
    ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_balanced, NULL, 5000));
    ASSERT_TRUE(p == NULL);
    CloseHandle(srv);
}

/* Tearing the loop down with ops still posted (after kl_pipe_free) releases everything through the
 * IOCP quiesce path: no dispatch, no leak. */
UTEST(pipe, loop_teardown_reclaims_outstanding_ops) {
    KlEventCtx ev;
    long b0 = g_counts.live_blocks; long long y0 = g_counts.live_bytes;
    ASSERT_EQ(kl_event_ctx_init(&ev, &g_alloc), 0);
    if (!loop_is_completion(&ev)) { kl_event_ctx_free(&ev); UTEST_SKIP("needs BACKEND=iocp"); }
    char name[128]; pipe_name(name, sizeof name, "teardown");
    HANDLE srv = server_create(name, 1, 1024, NULL);
    Rec r; memset(&r, 0, sizeof r);
    KlPipeConfig cfg = rec_cfg(&r, 0, 0);
    KlPipeStream *p = NULL;
    ASSERT_EQ(kl_pipe_connect(&ev, name, &cfg, &p), KL_PIPE_OK);
    ASSERT_EQ(kl_stream_read_start(kl_pipe_stream(p)), 0);
    static char big[60 * 1024];
    ASSERT_EQ(kl_stream_write(kl_pipe_stream(p), big, sizeof big), KL_STREAM_ACCEPTED);
    kl_pipe_free(p);                  /* cancels are requested but NOT drained */
    r.sealed = 1;
    kl_event_ctx_free(&ev);           /* quiesce: cancel + dequeue + release each op's ref */
    ASSERT_EQ(r.late, 0);
    ASSERT_EQ(g_counts.live_blocks, b0);
    ASSERT_EQ(g_counts.live_bytes, y0);
    CloseHandle(srv);
}

UTEST_F(pipe_iocp, reconnect_with_fresh_stream) {
    NEED_IOCP();
    char name[128]; pipe_name(name, sizeof name, "reconnect");
    for (int round = 0; round < 3; round++) {
        Srv s; memset(&s, 0, sizeof s);
        s.h = server_create(name, 1, 4096, NULL); s.mode = SRV_ECHO;
        ASSERT_TRUE(s.h != INVALID_HANDLE_VALUE);
        srv_start(&s);
        Rec r; memset(&r, 0, sizeof r);
        KlPipeConfig cfg = rec_cfg(&r, 0, 0);
        KlPipeStream *p = NULL;
        ASSERT_EQ(kl_pipe_connect(&utest_fixture->ev, name, &cfg, &p), KL_PIPE_OK);
        KlStream *st = kl_pipe_stream(p);
        ASSERT_EQ(kl_stream_read_start(st), 0);
        char msg[16]; int n = snprintf(msg, sizeof msg, "round-%d", round);
        ASSERT_EQ(kl_stream_write(st, msg, (size_t)n), KL_STREAM_ACCEPTED);
        LenWant w = { &r, (size_t)n };
        ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_len, &w, 5000));
        ASSERT_EQ(memcmp(r.buf, msg, (size_t)n), 0);
        ASSERT_EQ(kl_stream_cancel(st), 0);
        ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_closed, &r, 5000));
        kl_pipe_free(p);
        srv_join(&s);                 /* the old instance is gone before the next round creates one */
        rec_free(&r);
    }
}

/* Cancel/close races, repeated: cancel at varying points relative to in-flight reads and writes.
 * Every round must detach exactly once, never call back after the free, and give memory back. */
UTEST_F(pipe_iocp, cancel_close_races_repeated) {
    NEED_IOCP();
    for (int i = 0; i < 200; i++) {
        char name[128]; pipe_name(name, sizeof name, "race");
        Srv s; memset(&s, 0, sizeof s);
        s.h = server_create(name, 1, 2048, NULL); s.mode = SRV_ECHO;
        ASSERT_TRUE(s.h != INVALID_HANDLE_VALUE);
        srv_start(&s);
        mark_baseline();
        Rec r; memset(&r, 0, sizeof r);
        KlPipeConfig cfg = rec_cfg(&r, 512, 16 * 1024);
        KlPipeStream *p = NULL;
        ASSERT_EQ(kl_pipe_connect(&utest_fixture->ev, name, &cfg, &p), KL_PIPE_OK);
        KlStream *st = kl_pipe_stream(p);
        ASSERT_EQ(kl_stream_read_start(st), 0);
        char buf[8192]; memset(buf, 'a' + (i % 26), sizeof buf);
        (void)kl_stream_write(st, buf, (size_t)(1 + (i * 37) % sizeof buf));
        for (int t = 0; t < i % 4; t++) (void)kl_event_ctx_run(&utest_fixture->ev, 16, 1);
        switch (i % 3) {
        case 0: (void)kl_stream_cancel(st); break;
        case 1: (void)kl_stream_close_begin(st); (void)kl_stream_cancel(st); break;
        case 2: break;                /* free directly with whatever is outstanding */
        }
        if (i % 3 != 2) {
            ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_closed, &r, 5000));
            ASSERT_EQ(r.closes, 1);
        }
        kl_pipe_free(p);
        r.sealed = 1;
        ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_balanced, NULL, 5000));
        srv_join(&s);
        pump_for(&utest_fixture->ev, 1);
        ASSERT_EQ(r.late, 0);
        ASSERT_LE(r.closes, 1);
        rec_free(&r);
    }
}

/* ── A consumer that knows only KlStream: u32be-length framing ─────────────────────────────── */

/* The codec below never names a pipe. It is the whole of what an SSH-agent-style client needs from
 * the transport: write one framed request, reassemble framed replies from arbitrary fragments. */
typedef struct {
    KlStream     *s;
    unsigned char acc[70000];
    size_t        n;
    int           frames;
    int           eof;
    char          last[70000];
    size_t        last_len;
} Framer;

static int framer_send(KlStream *s, const void *payload, uint32_t len) {
    unsigned char *m = malloc(4 + (size_t)len);
    m[0] = (unsigned char)(len >> 24); m[1] = (unsigned char)(len >> 16);
    m[2] = (unsigned char)(len >> 8);  m[3] = (unsigned char)len;
    if (len) memcpy(m + 4, payload, len);
    KlStreamWriteStatus ws = kl_stream_write(s, (const char *)m, 4 + (size_t)len);   /* atomic */
    free(m);
    return ws == KL_STREAM_ACCEPTED ? 0 : -1;
}
static void framer_on_data(void *ud, const char *b, size_t len, int ok) {
    Framer *f = ud;
    if (!ok) { f->eof = 1; return; }
    if (f->n + len > sizeof f->acc) { f->eof = 1; return; }
    memcpy(f->acc + f->n, b, len); f->n += len;
    while (f->n >= 4) {
        uint32_t fl = ((uint32_t)f->acc[0] << 24) | ((uint32_t)f->acc[1] << 16) |
                      ((uint32_t)f->acc[2] << 8) | f->acc[3];
        if (f->n < 4 + (size_t)fl) break;
        memcpy(f->last, f->acc + 4, fl); f->last_len = fl;
        f->frames++;
        memmove(f->acc, f->acc + 4 + fl, f->n - 4 - fl);
        f->n -= 4 + fl;
    }
}
typedef struct { Framer *f; int want; } FrameWant;
static int cond_frames(void *a) { FrameWant *w = a; return w->f->frames >= w->want || w->f->eof; }

UTEST_F(pipe_iocp, framed_exchange_through_kl_stream_only) {
    NEED_IOCP();
    char name[128]; pipe_name(name, sizeof name, "framed");
    Srv s; memset(&s, 0, sizeof s);
    s.h = server_create(name, 1, 4096, NULL); s.mode = SRV_FRAMED;
    srv_start(&s);
    static Framer f; memset(&f, 0, sizeof f);
    KlPipeConfig cfg; memset(&cfg, 0, sizeof cfg);
    cfg.read_capacity = 777;          /* odd size: frames always straddle deliveries */
    cfg.on_data = framer_on_data; cfg.user_data = &f;
    KlPipeStream *p = NULL;
    ASSERT_EQ(kl_pipe_connect(&utest_fixture->ev, name, &cfg, &p), KL_PIPE_OK);
    f.s = kl_pipe_stream(p);
    ASSERT_EQ(kl_stream_read_start(f.s), 0);

    const uint32_t sizes[] = { 0, 1, 5, 4096, 33333, 60000 };
    static char pl[60000];
    for (size_t k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
        for (uint32_t i = 0; i < sizes[k]; i++) pl[i] = (char)('A' + (i + k) % 23);
        ASSERT_EQ(framer_send(f.s, pl, sizes[k]), 0);
        FrameWant w = { &f, (int)k + 1 };
        ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_frames, &w, 10000));
        ASSERT_EQ(f.eof, 0);
        ASSERT_EQ(f.last_len, (size_t)sizes[k]);
        for (uint32_t i = 0; i < sizes[k]; i++)            /* the server reversed it */
            ASSERT_EQ(f.last[i], pl[sizes[k] - 1 - i]);
    }
    ASSERT_EQ(kl_stream_cancel(f.s), 0);
    pump_for(&utest_fixture->ev, 50);
    kl_pipe_free(p);
    srv_join(&s);
    ASSERT_EQ(s.frames, (int)(sizeof sizes / sizeof sizes[0]));
}
/* ── Listener (server side), KlListener's object handoff family ────────────────────────────── */

#include "../src/completion.h"        /* white-box: KL_COMP_PIPE_ACCEPT at the seam */
#include "../src/completion_pipe.h"
#include "../src/platform_pipe.h"
#include "../src/completion_life.h"
#include <aclapi.h>                   /* GetSecurityInfo */

/* A server-side echo: every accepted stream is bound to one of these, writing back what it reads. */
typedef struct {
    KlPipeStream *p;
    size_t        got;
    int           terminals, closes;
} Echo;
typedef struct {
    Echo  e[64];
    int   n;                 /* accepted so far */
    int   closes;            /* listener on_close */
    int   bind_on_accept;    /* 1 = bind + start reading inside on_accept */
    KlPipeListener *pl;
    int   free_in_close;     /* free the listener from inside its own on_close */
} Srv2;
static void echo_on_data(void *ud, const char *b, size_t n, int ok) {
    Echo *e = ud;
    if (!ok) { e->terminals++; return; }
    e->got += n;
    (void)kl_stream_write(kl_pipe_stream(e->p), b, n);
}
static void echo_on_close(void *ud) { ((Echo *)ud)->closes++; }
static void srv2_on_accept(void *ud, KlPipeStream *p) {
    Srv2 *s = ud;
    Echo *e = &s->e[s->n++];
    e->p = p;
    if (s->bind_on_accept) {
        kl_pipe_bind(p, echo_on_data, echo_on_close, e);
        (void)kl_stream_read_start(kl_pipe_stream(p));
    }
}
static void srv2_on_close(void *ud) {
    Srv2 *s = ud;
    s->closes++;
    if (s->free_in_close && kl_pipe_listener_free(s->pl) == 0) s->pl = NULL;
}
static KlPipeListenConfig srv2_cfg(Srv2 *s, int instances) {
    KlPipeListenConfig c; memset(&c, 0, sizeof c);
    c.instances = instances; c.on_accept = srv2_on_accept; c.on_close = srv2_on_close; c.user_data = s;
    return c;
}
static int cond_listener_closed(void *a) { return ((Srv2 *)a)->closes > 0; }
typedef struct { Srv2 *s; int want; } AccWant;
static int cond_accepted(void *a) { AccWant *w = a; return w->s->n >= w->want; }
typedef struct { Echo *e; } TermWant;
static int cond_echo_terminal(void *a) { return ((TermWant *)a)->e->terminals > 0; }
typedef struct { Echo *e; size_t want; } GotWant;
static int cond_echo_got(void *a) { GotWant *w = a; return w->e->got >= w->want; }

/* Connect a Keel client, retrying while every waiting instance is taken (the loop refills them). */
static KlPipeStatus connect_retry(KlEventCtx *ev, const char *name, KlPipeConfig *cfg, KlPipeStream **p) {
    for (int i = 0; i < 500; i++) {
        KlPipeStatus st = kl_pipe_connect(ev, name, cfg, p);
        if (st != KL_PIPE_BUSY) return st;
        (void)kl_event_ctx_run(ev, 16, 2);
    }
    return KL_PIPE_BUSY;
}

UTEST_F(pipe_iocp, listen_accept_echo_keel_on_both_ends) {
    NEED_IOCP();
    char name[128]; pipe_name(name, sizeof name, "listen-echo");
    mark_baseline();
    Srv2 s; memset(&s, 0, sizeof s); s.bind_on_accept = 1;
    KlPipeListenConfig lc = srv2_cfg(&s, 2);
    ASSERT_EQ(kl_pipe_listen(&utest_fixture->ev, name, &lc, &s.pl), KL_PIPE_OK);

    Rec r; memset(&r, 0, sizeof r);
    KlPipeConfig cc = rec_cfg(&r, 0, 0);
    KlPipeStream *c = NULL;
    ASSERT_EQ(kl_pipe_connect(&utest_fixture->ev, name, &cc, &c), KL_PIPE_OK);
    ASSERT_EQ(kl_stream_read_start(kl_pipe_stream(c)), 0);
    ASSERT_EQ(kl_stream_write(kl_pipe_stream(c), "ping over a keel listener", 25), KL_STREAM_ACCEPTED);
    LenWant w = { &r, 25 };
    ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_len, &w, 5000));
    ASSERT_EQ(memcmp(r.buf, "ping over a keel listener", 25), 0);
    ASSERT_EQ(s.n, 1);
    ASSERT_EQ(s.e[0].got, (size_t)25);

    kl_pipe_free(c);                                  /* the client leaves */
    ASSERT_EQ(kl_pipe_listener_close(s.pl), 0);
    ASSERT_EQ(kl_pipe_listener_free(s.pl), -1);       /* waiting instances not yet retired */
    ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_listener_closed, &s, 5000));
    ASSERT_EQ(s.closes, 1);
    ASSERT_EQ(kl_pipe_listener_free(s.pl), 0);
    kl_pipe_free(s.e[0].p);
    ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_balanced, NULL, 5000));
    rec_free(&r);
}

UTEST_F(pipe_iocp, client_disconnect_gives_server_stream_eof) {
    NEED_IOCP();
    char name[128]; pipe_name(name, sizeof name, "listen-eof");
    Srv2 s; memset(&s, 0, sizeof s); s.bind_on_accept = 1;
    KlPipeListenConfig lc = srv2_cfg(&s, 1);
    ASSERT_EQ(kl_pipe_listen(&utest_fixture->ev, name, &lc, &s.pl), KL_PIPE_OK);
    Rec r; memset(&r, 0, sizeof r);
    KlPipeConfig cc = rec_cfg(&r, 0, 0);
    KlPipeStream *c = NULL;
    ASSERT_EQ(kl_pipe_connect(&utest_fixture->ev, name, &cc, &c), KL_PIPE_OK);
    AccWant aw = { &s, 1 };
    ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_accepted, &aw, 5000));
    kl_pipe_free(c);
    TermWant tw = { &s.e[0] };
    ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_echo_terminal, &tw, 5000));
    ASSERT_EQ(s.e[0].terminals, 1);
    kl_pipe_free(s.e[0].p);
    kl_pipe_listener_close(s.pl);
    ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_listener_closed, &s, 5000));
    ASSERT_EQ(kl_pipe_listener_free(s.pl), 0);
    rec_free(&r);
}

UTEST_F(pipe_iocp, sequential_clients) {
    NEED_IOCP();
    char name[128]; pipe_name(name, sizeof name, "listen-seq");
    Srv2 s; memset(&s, 0, sizeof s); s.bind_on_accept = 1;
    KlPipeListenConfig lc = srv2_cfg(&s, 1);
    ASSERT_EQ(kl_pipe_listen(&utest_fixture->ev, name, &lc, &s.pl), KL_PIPE_OK);
    for (int i = 0; i < 5; i++) {
        Rec r; memset(&r, 0, sizeof r);
        KlPipeConfig cc = rec_cfg(&r, 0, 0);
        KlPipeStream *c = NULL;
        ASSERT_EQ(connect_retry(&utest_fixture->ev, name, &cc, &c), KL_PIPE_OK);
        ASSERT_EQ(kl_stream_read_start(kl_pipe_stream(c)), 0);
        char msg[32]; int n = snprintf(msg, sizeof msg, "client %d", i);
        ASSERT_EQ(kl_stream_write(kl_pipe_stream(c), msg, (size_t)n), KL_STREAM_ACCEPTED);
        LenWant w = { &r, (size_t)n };
        ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_len, &w, 5000));
        ASSERT_EQ(memcmp(r.buf, msg, (size_t)n), 0);
        kl_pipe_free(c);
        rec_free(&r);
    }
    ASSERT_EQ(s.n, 5);
    for (int i = 0; i < s.n; i++) kl_pipe_free(s.e[i].p);
    kl_pipe_listener_close(s.pl);
    ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_listener_closed, &s, 5000));
    ASSERT_EQ(kl_pipe_listener_free(s.pl), 0);
}

/* More simultaneous clients than waiting instances: the ones that find every instance taken see
 * KL_PIPE_BUSY and retry while the loop refills the window. Every client gets its own echo. */
UTEST_F(pipe_iocp, concurrent_clients_beyond_the_window) {
    NEED_IOCP();
    enum { N = 8 };
    char name[128]; pipe_name(name, sizeof name, "listen-conc");
    mark_baseline();
    Srv2 s; memset(&s, 0, sizeof s); s.bind_on_accept = 1;
    KlPipeListenConfig lc = srv2_cfg(&s, 2);
    ASSERT_EQ(kl_pipe_listen(&utest_fixture->ev, name, &lc, &s.pl), KL_PIPE_OK);
    Rec r[N]; KlPipeStream *c[N];
    for (int i = 0; i < N; i++) {
        memset(&r[i], 0, sizeof r[i]);
        KlPipeConfig cc = rec_cfg(&r[i], 0, 0);
        ASSERT_EQ(connect_retry(&utest_fixture->ev, name, &cc, &c[i]), KL_PIPE_OK);
        ASSERT_EQ(kl_stream_read_start(kl_pipe_stream(c[i])), 0);
    }
    for (int i = 0; i < N; i++) {
        char msg[16]; int n = snprintf(msg, sizeof msg, "c%02d", i);
        ASSERT_EQ(kl_stream_write(kl_pipe_stream(c[i]), msg, (size_t)n), KL_STREAM_ACCEPTED);
    }
    for (int i = 0; i < N; i++) {
        LenWant w = { &r[i], 3 };
        ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_len, &w, 5000));
        char msg[16]; snprintf(msg, sizeof msg, "c%02d", i);
        ASSERT_EQ(memcmp(r[i].buf, msg, 3), 0);       /* each client got ITS bytes back */
    }
    ASSERT_EQ(s.n, N);
    for (int i = 0; i < N; i++) { kl_pipe_free(c[i]); rec_free(&r[i]); }
    for (int i = 0; i < s.n; i++) kl_pipe_free(s.e[i].p);
    kl_pipe_listener_close(s.pl);
    ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_listener_closed, &s, 5000));
    ASSERT_EQ(kl_pipe_listener_free(s.pl), 0);
    ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_balanced, NULL, 5000));
}

/* Accepted streams outlive the listener: close and free it, and the connection keeps working. */
UTEST_F(pipe_iocp, accepted_stream_outlives_listener) {
    NEED_IOCP();
    char name[128]; pipe_name(name, sizeof name, "listen-outlive");
    Srv2 s; memset(&s, 0, sizeof s); s.bind_on_accept = 1; s.free_in_close = 1;
    KlPipeListenConfig lc = srv2_cfg(&s, 3);
    ASSERT_EQ(kl_pipe_listen(&utest_fixture->ev, name, &lc, &s.pl), KL_PIPE_OK);
    Rec r; memset(&r, 0, sizeof r);
    KlPipeConfig cc = rec_cfg(&r, 0, 0);
    KlPipeStream *c = NULL;
    ASSERT_EQ(kl_pipe_connect(&utest_fixture->ev, name, &cc, &c), KL_PIPE_OK);
    ASSERT_EQ(kl_stream_read_start(kl_pipe_stream(c)), 0);
    AccWant aw = { &s, 1 };
    ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_accepted, &aw, 5000));
    kl_pipe_listener_close(s.pl);                     /* freed from inside its own on_close */
    ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_listener_closed, &s, 5000));
    ASSERT_TRUE(s.pl == NULL);
    ASSERT_EQ(kl_stream_write(kl_pipe_stream(c), "still here", 10), KL_STREAM_ACCEPTED);
    LenWant w = { &r, 10 };
    ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_len, &w, 5000));
    ASSERT_EQ(memcmp(r.buf, "still here", 10), 0);
    kl_pipe_free(c);
    kl_pipe_free(s.e[0].p);
    pump_for(&utest_fixture->ev, 20);
    rec_free(&r);
}

/* An accepted stream arrives unbound and not reading: nothing is delivered until the owner binds it
 * and starts the read side. */
UTEST_F(pipe_iocp, accepted_stream_is_unbound_until_bind) {
    NEED_IOCP();
    char name[128]; pipe_name(name, sizeof name, "listen-bind");
    Srv2 s; memset(&s, 0, sizeof s); s.bind_on_accept = 0;
    KlPipeListenConfig lc = srv2_cfg(&s, 1);
    ASSERT_EQ(kl_pipe_listen(&utest_fixture->ev, name, &lc, &s.pl), KL_PIPE_OK);
    Rec r; memset(&r, 0, sizeof r);
    KlPipeConfig cc = rec_cfg(&r, 0, 0);
    KlPipeStream *c = NULL;
    ASSERT_EQ(kl_pipe_connect(&utest_fixture->ev, name, &cc, &c), KL_PIPE_OK);
    ASSERT_EQ(kl_stream_write(kl_pipe_stream(c), "early", 5), KL_STREAM_ACCEPTED);
    AccWant aw = { &s, 1 };
    ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_accepted, &aw, 5000));
    pump_for(&utest_fixture->ev, 50);
    ASSERT_EQ(s.e[0].got, (size_t)0);                 /* not bound, not reading: nothing delivered */
    ASSERT_EQ(kl_pipe_bind(s.e[0].p, NULL, NULL, NULL), -1);
    ASSERT_EQ(kl_pipe_bind(s.e[0].p, echo_on_data, echo_on_close, &s.e[0]), 0);
    ASSERT_EQ(kl_stream_read_start(kl_pipe_stream(s.e[0].p)), 0);
    GotWant gw = { &s.e[0], 5 };
    ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_echo_got, &gw, 5000));
    ASSERT_EQ(s.e[0].got, (size_t)5);                 /* the bytes waited in the pipe */
    kl_pipe_free(c);
    kl_pipe_free(s.e[0].p);
    kl_pipe_listener_close(s.pl);
    ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_listener_closed, &s, 5000));
    ASSERT_EQ(kl_pipe_listener_free(s.pl), 0);
    rec_free(&r);
}

/* Close with every instance still waiting: each ConnectNamedPipe is cancelled and retires; on_close
 * fires once; free is refused until then; nothing is left allocated. */
UTEST_F(pipe_iocp, listener_close_with_pending_instances) {
    NEED_IOCP();
    char name[128]; pipe_name(name, sizeof name, "listen-close");
    mark_baseline();
    Srv2 s; memset(&s, 0, sizeof s);
    KlPipeListenConfig lc = srv2_cfg(&s, 5);
    ASSERT_EQ(kl_pipe_listen(&utest_fixture->ev, name, &lc, &s.pl), KL_PIPE_OK);
    pump_for(&utest_fixture->ev, 20);
    ASSERT_EQ(kl_pipe_listener_close(s.pl), 0);
    ASSERT_EQ(kl_pipe_listener_close(s.pl), 0);       /* idempotent */
    ASSERT_EQ(s.closes, 0);                           /* the five connects are still physical */
    ASSERT_EQ(kl_pipe_listener_free(s.pl), -1);
    ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_listener_closed, &s, 5000));
    pump_for(&utest_fixture->ev, 20);
    ASSERT_EQ(s.closes, 1);
    ASSERT_EQ(s.n, 0);
    ASSERT_EQ(kl_pipe_listener_free(s.pl), 0);
    ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_balanced, NULL, 5000));
    /* the name is free again once every instance handle has closed */
    Srv2 s2; memset(&s2, 0, sizeof s2);
    KlPipeListenConfig lc2 = srv2_cfg(&s2, 1);
    ASSERT_EQ(kl_pipe_listen(&utest_fixture->ev, name, &lc2, &s2.pl), KL_PIPE_OK);
    kl_pipe_listener_close(s2.pl);
    ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_listener_closed, &s2, 5000));
    ASSERT_EQ(kl_pipe_listener_free(s2.pl), 0);
}

/* The first instance claims the name: a pre-existing pipe (a squatter), or a second Keel listener,
 * is refused rather than shared. */
UTEST_F(pipe_iocp, listen_refuses_a_name_already_in_use) {
    NEED_IOCP();
    char name[128]; pipe_name(name, sizeof name, "listen-squat");
    HANDLE squatter = server_create(name, 4, 4096, NULL);
    ASSERT_TRUE(squatter != INVALID_HANDLE_VALUE);
    Srv2 s; memset(&s, 0, sizeof s);
    KlPipeListenConfig lc = srv2_cfg(&s, 1);
    KlPipeListener *pl = (KlPipeListener *)1;
    long before = g_counts.live_blocks;
    ASSERT_EQ(kl_pipe_listen(&utest_fixture->ev, name, &lc, &pl), KL_PIPE_IN_USE);
    ASSERT_TRUE(pl == NULL);
    ASSERT_EQ(g_counts.live_blocks, before);
    CloseHandle(squatter);

    char name2[128]; pipe_name(name2, sizeof name2, "listen-twice");
    ASSERT_EQ(kl_pipe_listen(&utest_fixture->ev, name2, &lc, &s.pl), KL_PIPE_OK);
    Srv2 s2; memset(&s2, 0, sizeof s2);
    KlPipeListenConfig lc2 = srv2_cfg(&s2, 1);
    ASSERT_EQ(kl_pipe_listen(&utest_fixture->ev, name2, &lc2, &pl), KL_PIPE_IN_USE);
    kl_pipe_listener_close(s.pl);
    ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_listener_closed, &s, 5000));
    ASSERT_EQ(kl_pipe_listener_free(s.pl), 0);
    ASSERT_STREQ(kl_pipe_status_str(KL_PIPE_IN_USE), "in_use");
}

/* White-box, at the PAL: a server instance's DACL grants the current user and SYSTEM only. */
UTEST_F(pipe_iocp, server_instance_dacl_is_user_and_system_only) {
    NEED_IOCP();
    char name[128]; pipe_name(name, sizeof name, "listen-dacl");
    KlPipeHandle *h = NULL;
    ASSERT_EQ(kl_plat_pipe_create_instance(name, 1, &h), KL_PIPE_OPEN_OK);
    PACL dacl = NULL; PSECURITY_DESCRIPTOR sd = NULL;
    ASSERT_EQ(GetSecurityInfo((HANDLE)h, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL,
                              &dacl, NULL, &sd), (DWORD)ERROR_SUCCESS);
    ASSERT_TRUE(dacl != NULL);
    ASSERT_EQ((int)dacl->AceCount, 2);
    HANDLE tok = NULL;
    ASSERT_TRUE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok));
    union { TOKEN_USER u; unsigned char raw[256]; } tu; DWORD len = 0;
    ASSERT_TRUE(GetTokenInformation(tok, TokenUser, &tu, sizeof tu, &len));
    CloseHandle(tok);
    unsigned char sys_sid[SECURITY_MAX_SID_SIZE]; DWORD ssz = sizeof sys_sid;
    ASSERT_TRUE(CreateWellKnownSid(WinLocalSystemSid, NULL, sys_sid, &ssz));
    int saw_user = 0, saw_system = 0;
    for (DWORD i = 0; i < dacl->AceCount; i++) {
        ACCESS_ALLOWED_ACE *ace = NULL;
        ASSERT_TRUE(GetAce(dacl, i, (void **)&ace));
        ASSERT_EQ((int)ace->Header.AceType, (int)ACCESS_ALLOWED_ACE_TYPE);
        PSID sid = (PSID)&ace->SidStart;
        if (EqualSid(sid, tu.u.User.Sid)) saw_user = 1;
        else if (EqualSid(sid, sys_sid)) saw_system = 1;
    }
    ASSERT_EQ(saw_user, 1);
    ASSERT_EQ(saw_system, 1);                          /* ...and nothing else: 2 ACEs, no Everyone */
    LocalFree(sd);
    kl_plat_pipe_close(h);
}

/* White-box, at the completion seam: a client that connects BEFORE the ConnectNamedPipe is issued
 * (ERROR_PIPE_CONNECTED, which queues no packet) still yields exactly one asynchronous, successful
 * ACCEPT completion. Deterministic: the client connects first by construction. */
typedef struct { int accepts, ok, finals; } RaceObs;
static void race_dispatch(void *target, const KlCompletionEvent *ev) {
    RaceObs *o = target;
    if (ev->kind == KL_COMP_PIPE_ACCEPT) { o->accepts++; o->ok = ev->ok; }
    if (!ev->retain_life) kl_comp_life_release(ev->life);
}
static void race_final(void *ctx) { RaceObs *o = ctx; o->finals++; }
static int cond_race(void *a) { return ((RaceObs *)a)->accepts > 0; }

UTEST_F(pipe_iocp, pipe_connected_race_completes_once_as_success) {
    NEED_IOCP();
    char name[128]; pipe_name(name, sizeof name, "listen-race");
    KlPipeHandle *h = NULL;
    ASSERT_EQ(kl_plat_pipe_create_instance(name, 1, &h), KL_PIPE_OPEN_OK);
    wchar_t w[256]; MultiByteToWideChar(CP_UTF8, 0, name, -1, w, 256);
    HANDLE client = CreateFileW(w, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    ASSERT_TRUE(client != INVALID_HANDLE_VALUE);       /* connected BEFORE any ConnectNamedPipe */
    ASSERT_EQ(kl_comp_pipe_attach(&utest_fixture->ev, h), 0);
    RaceObs o; memset(&o, 0, sizeof o);
    KlCompLife *life = kl_comp_life_create(&g_alloc, &o, race_final, &o, race_dispatch);
    ASSERT_TRUE(life != NULL);
    KlPipeIoOp op; memset(&op, 0, sizeof op);
    op.h = h; op.kind = KL_PIPE_OP_ACCEPT; op.life = life;
    kl_comp_life_retain(life);
    ASSERT_EQ(kl_comp_pipe_post(&utest_fixture->ev, &op), 0);
    ASSERT_EQ(o.accepts, 0);                           /* never inline, even though it is already done */
    ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_race, &o, 5000));
    pump_for(&utest_fixture->ev, 20);
    ASSERT_EQ(o.accepts, 1);                           /* exactly one completion */
    ASSERT_EQ(o.ok, 1);                                /* and it is a success */
    kl_comp_life_release(life);                        /* owner ref: final */
    ASSERT_EQ(o.finals, 1);
    CloseHandle(client);
    kl_plat_pipe_close(h);
}

/* Listener close racing connects and accepts, repeated: every round detaches exactly once and gives
 * every byte of memory back. */
UTEST_F(pipe_iocp, listener_close_races_repeated) {
    NEED_IOCP();
    for (int i = 0; i < 100; i++) {
        char name[128]; pipe_name(name, sizeof name, "listen-race-rep");
        mark_baseline();
        Srv2 s; memset(&s, 0, sizeof s); s.bind_on_accept = (i % 2);
        KlPipeListenConfig lc = srv2_cfg(&s, 1 + i % 3);
        ASSERT_EQ(kl_pipe_listen(&utest_fixture->ev, name, &lc, &s.pl), KL_PIPE_OK);
        Rec r[3]; KlPipeStream *c[3] = { NULL, NULL, NULL };
        int nc = i % 4 < 3 ? i % 4 : 1;
        for (int k = 0; k < nc; k++) {
            memset(&r[k], 0, sizeof r[k]);
            KlPipeConfig cc = rec_cfg(&r[k], 0, 0);
            if (kl_pipe_connect(&utest_fixture->ev, name, &cc, &c[k]) != KL_PIPE_OK) c[k] = NULL;
        }
        for (int t = 0; t < i % 5; t++) (void)kl_event_ctx_run(&utest_fixture->ev, 16, 1);
        ASSERT_EQ(kl_pipe_listener_close(s.pl), 0);
        ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_listener_closed, &s, 5000));
        ASSERT_EQ(s.closes, 1);
        ASSERT_EQ(kl_pipe_listener_free(s.pl), 0);
        for (int k = 0; k < nc; k++) { kl_pipe_free(c[k]); rec_free(&r[k]); }
        for (int k = 0; k < s.n; k++) kl_pipe_free(s.e[k].p);
        ASSERT_TRUE(pump_until(&utest_fixture->ev, cond_balanced, NULL, 5000));
    }
}

#endif /* _WIN32 */

UTEST_MAIN();
