/*
 * test_http1_parser_vtable.c: kl_http_server_init must reject a malformed HTTP/1 request-parser
 * WITHOUT crashing during error cleanup. The parser factory runs once per connection-slot at init;
 * a table missing a REQUIRED op (parse/reset/destroy) is rejected before the server accepts traffic.
 *
 * Failure semantics (distinct from the TLS validator's KL_ERR_TLS_VTABLE):
 *   - NULL factory return  -> KL_ERR_ALLOC (an allocation failure; pre-existing behavior).
 *   - malformed vtable      -> KL_ERR_INVALID_ARG (a bad caller-supplied table).
 * Each rejection must return -1 and free cleanly (kl_http_conn_pool_free must not call a NULL
 * destroy); a fully-valid vtable must still init + free.
 */
#include "utest.h"
#include "../../../src/protocols/http/http_conn_internal.h"
#include <keel/keel.h>
#include <string.h>

/* ── a configurable stub request parser (one required op omitted per test) ──────── */
static KlHttp1ParseResult stub_parse(KlHttp1RequestParser *s, KlHttpRequest *req,
                                     const char *buf, size_t len, size_t *consumed) {
    (void)s; (void)req; (void)buf; (void)len;
    if (consumed) *consumed = 0;
    return KL_HTTP1_PARSE_INCOMPLETE;
}
static void stub_reset(KlHttp1RequestParser *s) { (void)s; }
static int  g_destroy_calls;
static void stub_destroy(KlHttp1RequestParser *s) { (void)s; g_destroy_calls++; }

/* omit selector: which REQUIRED op the factory leaves NULL (-1 = all present / valid);
 * RETURN_NULL makes the factory itself fail (allocation-failure path). */
enum { OMIT_NONE = -1, OMIT_PARSE, OMIT_RESET, OMIT_DESTROY, RETURN_NULL };
static int g_omit;
static KlHttp1RequestParser g_parser;   /* shared across slots; ops are stateless/idempotent */

static KlHttp1Parser *vtable_factory(KlAllocator *alloc) {
    (void)alloc;
    if (g_omit == RETURN_NULL) return NULL;
    memset(&g_parser, 0, sizeof(g_parser));
    g_parser.parse   = stub_parse;
    g_parser.reset   = stub_reset;
    g_parser.destroy = stub_destroy;
    switch (g_omit) {
        case OMIT_PARSE:   g_parser.parse   = NULL; break;
        case OMIT_RESET:   g_parser.reset   = NULL; break;
        case OMIT_DESTROY: g_parser.destroy = NULL; break;
        default: break;
    }
    return &g_parser;
}

static int init_with_parser(KlHttpServer *s, KlHttp1ParserFactory parser) {
    KlHttpServerConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.port = 0;
    cfg.max_connections = 2;   /* >1 so cleanup iterates multiple slots */
    cfg.parser = parser;
    return kl_http_server_init(s, &cfg);
}

/* NULL factory return -> allocation-failure path (KL_ERR_ALLOC), pre-existing. */
UTEST(http1_parser_vtable, null_return_is_alloc_error) {
    KlHttpServer s;
    g_omit = RETURN_NULL;
    ASSERT_EQ(init_with_parser(&s, vtable_factory), -1);
    ASSERT_EQ((int)s.last_error, (int)KL_ERR_ALLOC);
}

/* Missing destroy -> invalid; cleanup must NOT call the NULL destroy. */
UTEST(http1_parser_vtable, missing_destroy_no_crash) {
    KlHttpServer s;
    g_omit = OMIT_DESTROY;
    ASSERT_EQ(init_with_parser(&s, vtable_factory), -1);
    ASSERT_EQ((int)s.last_error, (int)KL_ERR_INVALID_ARG);
    /* survived cleanup; no crash. */
}

/* Missing reset -> invalid; destroy IS present, so init frees the bad parser via it exactly once. */
UTEST(http1_parser_vtable, missing_reset_frees_via_destroy) {
    KlHttpServer s;
    g_omit = OMIT_RESET;
    g_destroy_calls = 0;
    ASSERT_EQ(init_with_parser(&s, vtable_factory), -1);
    ASSERT_EQ((int)s.last_error, (int)KL_ERR_INVALID_ARG);
    ASSERT_EQ(g_destroy_calls, 1);   /* the one bad parser was destroyed exactly once */
}

/* Missing parse -> invalid, rejected, no crash. */
UTEST(http1_parser_vtable, missing_parse_rejected) {
    KlHttpServer s;
    g_omit = OMIT_PARSE;
    ASSERT_EQ(init_with_parser(&s, vtable_factory), -1);
    ASSERT_EQ((int)s.last_error, (int)KL_ERR_INVALID_ARG);
}

/* A fully-valid vtable still inits + frees cleanly (no false rejection). */
UTEST(http1_parser_vtable, valid_vtable_inits_and_frees) {
    KlHttpServer s;
    g_omit = OMIT_NONE;
    g_destroy_calls = 0;
    ASSERT_EQ(init_with_parser(&s, vtable_factory), 0);
    kl_http_server_free(&s);
    ASSERT_GT(g_destroy_calls, 0);   /* parsers were destroyed on free */
}

UTEST_MAIN();
