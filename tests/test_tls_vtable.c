/*
 * test_tls_vtable.c — kl_server_init must reject a malformed server TLS config WITHOUT
 * crashing during error cleanup (regression for the S-7-review Finding 1):
 *   - a NULL factory pointer,
 *   - a factory that returns a session missing a REQUIRED vtable op (esp. destroy/shutdown).
 * Each case must return -1 with KL_ERR_TLS_VTABLE and free cleanly (kl_conn_pool_free must
 * not call a NULL destroy/shutdown), and a fully-valid vtable must still init + free.
 */
#include "utest.h"
#include <keel/keel.h>
#include <string.h>

/* ── a configurable stub TLS session (one required op omitted per test) ──────── */
static KlTlsResult stub_handshake(KlTls *s, KlSocketHandle fd) { (void)s; (void)fd; return KL_TLS_OK; }
static kl_ssize_t  stub_read(KlTls *s, KlSocketHandle fd, void *b, size_t n) { (void)s;(void)fd;(void)b; return (kl_ssize_t)n; }
static kl_ssize_t  stub_write(KlTls *s, KlSocketHandle fd, const void *b, size_t n) { (void)s;(void)fd;(void)b; return (kl_ssize_t)n; }
static KlTlsResult stub_shutdown(KlTls *s, KlSocketHandle fd) { (void)s; (void)fd; return KL_TLS_OK; }
static size_t      stub_pending(KlTls *s) { (void)s; return 0; }
static void        stub_reset(KlTls *s) { (void)s; }
static int         g_destroy_calls;
static void        stub_destroy(KlTls *s) { (void)s; g_destroy_calls++; }

/* omit selector: which REQUIRED op the factory leaves NULL (-1 = all present / valid). */
enum { OMIT_NONE = -1, OMIT_HANDSHAKE, OMIT_READ, OMIT_WRITE, OMIT_SHUTDOWN,
       OMIT_PENDING, OMIT_RESET, OMIT_DESTROY };
static int   g_omit;
static KlTls g_session;   /* shared across slots — the stub ops are stateless/idempotent */

static KlTls *vtable_factory(KlTlsCtx *ctx, KlAllocator *alloc) {
    (void)ctx; (void)alloc;
    memset(&g_session, 0, sizeof(g_session));
    g_session.handshake = stub_handshake;
    g_session.read      = stub_read;
    g_session.write     = stub_write;
    g_session.shutdown  = stub_shutdown;
    g_session.pending   = stub_pending;
    g_session.reset     = stub_reset;
    g_session.destroy   = stub_destroy;
    switch (g_omit) {
        case OMIT_HANDSHAKE: g_session.handshake = NULL; break;
        case OMIT_READ:      g_session.read      = NULL; break;
        case OMIT_WRITE:     g_session.write     = NULL; break;
        case OMIT_SHUTDOWN:  g_session.shutdown  = NULL; break;
        case OMIT_PENDING:   g_session.pending   = NULL; break;
        case OMIT_RESET:     g_session.reset     = NULL; break;
        case OMIT_DESTROY:   g_session.destroy   = NULL; break;
        default: break;
    }
    return &g_session;
}

static int init_with_tls(KlServer *s, KlTlsConfig *tls) {
    KlConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.port = 0;
    cfg.max_connections = 2;   /* >1 so cleanup iterates multiple slots */
    cfg.tls = tls;
    return kl_server_init(s, &cfg);
}

/* NULL factory → rejected up front (would crash on the first factory() call otherwise). */
UTEST(tls_vtable, null_factory_rejected) {
    KlServer s;
    KlTlsConfig tls; memset(&tls, 0, sizeof(tls));
    tls.factory = NULL;
    ASSERT_EQ(init_with_tls(&s, &tls), -1);
    ASSERT_EQ((int)s.last_error, (int)KL_ERR_TLS_VTABLE);
}

/* Missing destroy → detected as invalid; cleanup must NOT call the NULL destroy. THE
 * regression: previously kl_conn_pool_free crashed here. */
UTEST(tls_vtable, missing_destroy_no_crash) {
    KlServer s;
    KlTlsConfig tls; memset(&tls, 0, sizeof(tls));
    tls.factory = vtable_factory;
    g_omit = OMIT_DESTROY;
    ASSERT_EQ(init_with_tls(&s, &tls), -1);
    ASSERT_EQ((int)s.last_error, (int)KL_ERR_TLS_VTABLE);
    /* survived cleanup — no crash. */
}

/* Missing shutdown → invalid; destroy IS present, so init frees the bad session via it. */
UTEST(tls_vtable, missing_shutdown_no_crash) {
    KlServer s;
    KlTlsConfig tls; memset(&tls, 0, sizeof(tls));
    tls.factory = vtable_factory;
    g_omit = OMIT_SHUTDOWN;
    g_destroy_calls = 0;
    ASSERT_EQ(init_with_tls(&s, &tls), -1);
    ASSERT_EQ((int)s.last_error, (int)KL_ERR_TLS_VTABLE);
    ASSERT_EQ(g_destroy_calls, 1);   /* the one bad session was destroyed exactly once */
}

/* Each other required op missing → rejected, no crash. */
UTEST(tls_vtable, each_missing_required_op_rejected) {
    int cases[] = { OMIT_HANDSHAKE, OMIT_READ, OMIT_WRITE, OMIT_PENDING, OMIT_RESET };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        KlServer s;
        KlTlsConfig tls; memset(&tls, 0, sizeof(tls));
        tls.factory = vtable_factory;
        g_omit = cases[i];
        ASSERT_EQ(init_with_tls(&s, &tls), -1);
        ASSERT_EQ((int)s.last_error, (int)KL_ERR_TLS_VTABLE);
    }
}

/* A fully-valid vtable still inits + frees cleanly (no false rejection). */
UTEST(tls_vtable, valid_vtable_inits_and_frees) {
    KlServer s;
    KlTlsConfig tls; memset(&tls, 0, sizeof(tls));
    tls.factory = vtable_factory;
    g_omit = OMIT_NONE;
    g_destroy_calls = 0;
    ASSERT_EQ(init_with_tls(&s, &tls), 0);
    kl_server_free(&s);
    ASSERT_GT(g_destroy_calls, 0);   /* sessions were destroyed on free */
}

UTEST_MAIN();
