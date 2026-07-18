#include "utest.h"
#include <keel/dns_resolver.h>
#include <keel/resolver.h>
#include <keel/udp_server.h>
#include <keel/event_ctx.h>
#include <keel/allocator.h>
#include <string.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>

/* ── A canned A-record response for a.com → 1.2.3.4, id 0x1234 ────────── */
static const uint8_t A_RESP[] = {
    0x12,0x34, 0x81,0x80, 0x00,0x01, 0x00,0x01, 0x00,0x00, 0x00,0x00,
    0x01,0x61, 0x03,0x63,0x6f,0x6d, 0x00, 0x00,0x01, 0x00,0x01,
    0xc0,0x0c, 0x00,0x01, 0x00,0x01, 0x00,0x00,0x01,0x00, 0x00,0x04, 0x01,0x02,0x03,0x04
};

/* ── Parser unit tests ───────────────────────────────────────────────── */

UTEST(dns_parse, valid_a_record) {
    KlResolveResult r;
    memset(&r, 0, sizeof(r));
    ASSERT_EQ(0, kl_dns_parse_response(A_RESP, sizeof(A_RESP), 0x1234,
                                       KL_DNS_TYPE_A, &r));
    ASSERT_EQ(AF_INET, r.ai_family);
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &((struct sockaddr_in *)&r.addr)->sin_addr, ip, sizeof(ip));
    ASSERT_STREQ("1.2.3.4", ip);
}

UTEST(dns_parse, id_mismatch) {
    KlResolveResult r;
    ASSERT_EQ(-1, kl_dns_parse_response(A_RESP, sizeof(A_RESP), 0x9999,
                                        KL_DNS_TYPE_A, &r));
}

UTEST(dns_parse, wrong_type_no_record) {
    KlResolveResult r;
    ASSERT_EQ(-1, kl_dns_parse_response(A_RESP, sizeof(A_RESP), 0x1234,
                                        KL_DNS_TYPE_AAAA, &r));
}

UTEST(dns_parse, truncated_is_safe) {
    KlResolveResult r;
    for (size_t n = 0; n < sizeof(A_RESP); n++)
        ASSERT_EQ(-1, kl_dns_parse_response(A_RESP, n, 0x1234, KL_DNS_TYPE_A, &r));
}

UTEST(dns_parse, compression_loop_is_safe) {
    /* Question name is a pointer to itself → must fail, not loop/crash. */
    uint8_t pkt[] = {
        0x12,0x34, 0x81,0x80, 0x00,0x01, 0x00,0x00, 0x00,0x00, 0x00,0x00,
        0xc0,0x0c,                          /* name @12 points to @12 */
        0x00,0x01, 0x00,0x01
    };
    KlResolveResult r;
    ASSERT_EQ(-1, kl_dns_parse_response(pkt, sizeof(pkt), 0x1234, KL_DNS_TYPE_A, &r));
}

/* ── Mock nameserver + resolver integration ──────────────────────────── */

static int g_answer_a;      /* reply with an A record for A queries   */
static int g_answer_aaaa;   /* reply with an AAAA record for AAAA queries */
static int g_rcode;         /* rcode to set (0 = NOERROR, 3 = NXDOMAIN) */
static int g_silent;        /* drop queries (to exercise timeout)      */
static int g_queries;

/* Parse qtype from the question and emit a canned response. */
static void mock_ns(KlUdpServer *s, const void *data, size_t len,
                    const struct sockaddr *src, socklen_t src_len, void *ud) {
    (void)ud;
    if (len < 12) return;
    g_queries++;
    if (g_silent) return;

    const uint8_t *q = data;
    /* Walk the question name to find qtype. */
    size_t off = 12;
    while (off < len && q[off] != 0) {
        if (q[off] & 0xC0) return;          /* no compression in our queries */
        off += 1 + q[off];
    }
    off += 1;                                /* root label */
    if (off + 4 > len) return;
    int qtype = (q[off] << 8) | q[off + 1];
    size_t qend = off + 4;                   /* end of question */

    uint8_t resp[512];
    size_t n = 0;
    resp[0] = q[0]; resp[1] = q[1];          /* echo id */
    resp[2] = 0x81;                          /* QR + RD */
    resp[3] = (uint8_t)(0x80 | (g_rcode & 0x0F));  /* RA + rcode */
    resp[4] = 0; resp[5] = 1;                /* qdcount */
    int answer = (qtype == KL_DNS_TYPE_A && g_answer_a) ||
                 (qtype == KL_DNS_TYPE_AAAA && g_answer_aaaa);
    resp[6] = 0; resp[7] = (uint8_t)(answer ? 1 : 0);   /* ancount */
    resp[8] = 0; resp[9] = 0; resp[10] = 0; resp[11] = 0;
    n = 12;
    memcpy(resp + n, q + 12, qend - 12);     /* echo question */
    n += qend - 12;

    if (answer) {
        resp[n++] = 0xc0; resp[n++] = 0x0c;  /* name → question */
        resp[n++] = (uint8_t)(qtype >> 8); resp[n++] = (uint8_t)(qtype & 0xFF);
        resp[n++] = 0x00; resp[n++] = 0x01;  /* IN */
        resp[n++] = 0; resp[n++] = 0; resp[n++] = 0x01; resp[n++] = 0x00; /* ttl */
        if (qtype == KL_DNS_TYPE_A) {
            resp[n++] = 0; resp[n++] = 4;
            resp[n++] = 10; resp[n++] = 1; resp[n++] = 2; resp[n++] = 3;
        } else {
            resp[n++] = 0; resp[n++] = 16;
            memset(resp + n, 0, 16);
            resp[n + 15] = 1;                /* ::1 */
            n += 16;
        }
    }
    kl_udp_server_reply(s, resp, n, src, src_len);
}

static int             g_done;
static int             g_err;
static KlResolveResult g_res;

static void on_done(KlResolveReq *req, const KlResolveResult *result,
                    int error, void *ud) {
    (void)req; (void)ud;
    g_done++;
    g_err = error;
    if (result) g_res = *result;
}

static void reset_dns(void) {
    g_answer_a = g_answer_aaaa = 0; g_rcode = 0; g_silent = 0; g_queries = 0;
    g_done = 0; g_err = 0; memset(&g_res, 0, sizeof(g_res));
}

/* Build a resolver + mock nameserver on one ctx. Caller frees. */
static KlResolver *make_resolver(KlEventCtx *ctx, KlUdpServer *ns, int timeout_ms,
                                 int attempts) {
    KlUdpServerConfig sc = { .bind_addr = "127.0.0.1", .port = 0 };
    if (kl_udp_server_init(ns, ctx, &sc, mock_ns, NULL) != 0)
        return NULL;
    char portbuf[8];
    snprintf(portbuf, sizeof(portbuf), "%u", kl_udp_server_local_port(ns));
    KlDnsResolverConfig dc = {
        .nameserver = "127.0.0.1",
        .port = kl_udp_server_local_port(ns),
        .timeout_ms = timeout_ms,
        .attempts = attempts,
    };
    return kl_dns_resolver_create(ctx, &dc);
}

static void pump(KlEventCtx *ctx, int *flag, int ticks) {
    for (int i = 0; i < ticks && *flag == 0; i++)
        kl_event_ctx_run(ctx, 16, 10);
}

UTEST(dns, resolve_a) {
    reset_dns();
    g_answer_a = 1;
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));
    KlUdpServer ns;
    KlResolver *r = make_resolver(&ctx, &ns, 500, 2);
    ASSERT_TRUE(r != NULL);

    ASSERT_TRUE(r->resolve(r, &ctx, "host.test", 8080, on_done, NULL) != NULL);
    pump(&ctx, &g_done, 200);

    ASSERT_EQ(1, g_done);
    ASSERT_EQ(0, g_err);
    ASSERT_EQ(AF_INET, g_res.ai_family);
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &((struct sockaddr_in *)&g_res.addr)->sin_addr, ip, sizeof(ip));
    ASSERT_STREQ("10.1.2.3", ip);
    ASSERT_EQ(8080, ntohs(((struct sockaddr_in *)&g_res.addr)->sin_port));

    r->destroy(r);
    kl_udp_server_free(&ns);
    kl_event_ctx_free(&ctx);
}

UTEST(dns, fallback_a_to_aaaa) {
    /* A query returns no record (rcode 0, an 0) → resolver retries as AAAA. */
    reset_dns();
    g_answer_a = 0; g_answer_aaaa = 1;
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));
    KlUdpServer ns;
    KlResolver *r = make_resolver(&ctx, &ns, 500, 2);
    ASSERT_TRUE(r != NULL);

    ASSERT_TRUE(r->resolve(r, &ctx, "host.test", 443, on_done, NULL) != NULL);
    pump(&ctx, &g_done, 200);

    ASSERT_EQ(1, g_done);
    ASSERT_EQ(0, g_err);
    ASSERT_EQ(AF_INET6, g_res.ai_family);
    ASSERT_TRUE(g_queries >= 2);        /* A then AAAA */

    r->destroy(r);
    kl_udp_server_free(&ns);
    kl_event_ctx_free(&ctx);
}

UTEST(dns, nxdomain_errors) {
    reset_dns();
    g_rcode = 3;                        /* NXDOMAIN */
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));
    KlUdpServer ns;
    KlResolver *r = make_resolver(&ctx, &ns, 500, 2);
    ASSERT_TRUE(r != NULL);

    ASSERT_TRUE(r->resolve(r, &ctx, "nope.test", 80, on_done, NULL) != NULL);
    pump(&ctx, &g_done, 200);

    ASSERT_EQ(1, g_done);
    ASSERT_EQ(KL_ERR_DNS, g_err);

    r->destroy(r);
    kl_udp_server_free(&ns);
    kl_event_ctx_free(&ctx);
}

UTEST(dns, timeout_errors) {
    reset_dns();
    g_silent = 1;                       /* never reply */
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));
    KlUdpServer ns;
    KlResolver *r = make_resolver(&ctx, &ns, 30, 1);   /* 30ms, 1 try/family */
    ASSERT_TRUE(r != NULL);

    ASSERT_TRUE(r->resolve(r, &ctx, "host.test", 80, on_done, NULL) != NULL);
    pump(&ctx, &g_done, 200);

    ASSERT_EQ(1, g_done);
    ASSERT_EQ(KL_ERR_DNS, g_err);

    r->destroy(r);
    kl_udp_server_free(&ns);
    kl_event_ctx_free(&ctx);
}

UTEST(dns, literal_ip_shortcut) {
    reset_dns();
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));
    KlUdpServer ns;
    KlResolver *r = make_resolver(&ctx, &ns, 500, 2);
    ASSERT_TRUE(r != NULL);

    ASSERT_TRUE(r->resolve(r, &ctx, "192.0.2.10", 1234, on_done, NULL) != NULL);
    pump(&ctx, &g_done, 50);

    ASSERT_EQ(1, g_done);
    ASSERT_EQ(0, g_err);
    ASSERT_EQ(AF_INET, g_res.ai_family);
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &((struct sockaddr_in *)&g_res.addr)->sin_addr, ip, sizeof(ip));
    ASSERT_STREQ("192.0.2.10", ip);
    ASSERT_EQ(1234, ntohs(((struct sockaddr_in *)&g_res.addr)->sin_port));
    ASSERT_EQ(0, g_queries);            /* no packet sent */

    r->destroy(r);
    kl_udp_server_free(&ns);
    kl_event_ctx_free(&ctx);
}

UTEST(dns, destroy_with_inflight) {
    /* Destroy while a query is outstanding — must free cleanly (ASan). */
    reset_dns();
    g_silent = 1;
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));
    KlUdpServer ns;
    KlResolver *r = make_resolver(&ctx, &ns, 5000, 2);
    ASSERT_TRUE(r != NULL);
    ASSERT_TRUE(r->resolve(r, &ctx, "host.test", 80, on_done, NULL) != NULL);
    kl_event_ctx_run(&ctx, 16, 10);     /* let the query go out */

    r->destroy(r);                       /* in-flight req + timer freed here */
    ASSERT_EQ(0, g_done);                /* never completed */

    kl_udp_server_free(&ns);
    kl_event_ctx_free(&ctx);
}

UTEST_MAIN();
