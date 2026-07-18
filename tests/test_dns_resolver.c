#include "utest.h"
#include <keel/dns_resolver.h>
#include <keel/resolver.h>
#include <keel/udp_server.h>
#include <keel/event_ctx.h>
#include <keel/allocator.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
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
                                       KL_DNS_TYPE_A, NULL, 0, &r));
    ASSERT_EQ(AF_INET, r.ai_family);
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &((struct sockaddr_in *)&r.addr)->sin_addr, ip, sizeof(ip));
    ASSERT_STREQ("1.2.3.4", ip);
}

UTEST(dns_parse, id_mismatch) {
    KlResolveResult r;
    ASSERT_EQ(-1, kl_dns_parse_response(A_RESP, sizeof(A_RESP), 0x9999,
                                        KL_DNS_TYPE_A, NULL, 0, &r));
}

UTEST(dns_parse, wrong_type_no_record) {
    KlResolveResult r;
    ASSERT_EQ(-1, kl_dns_parse_response(A_RESP, sizeof(A_RESP), 0x1234,
                                        KL_DNS_TYPE_AAAA, NULL, 0, &r));
}

UTEST(dns_parse, truncated_is_safe) {
    KlResolveResult r;
    for (size_t n = 0; n < sizeof(A_RESP); n++)
        ASSERT_EQ(-1, kl_dns_parse_response(A_RESP, n, 0x1234, KL_DNS_TYPE_A, NULL, 0, &r));
}

UTEST(dns_parse, compression_loop_is_safe) {
    /* Question name is a pointer to itself → must fail, not loop/crash. */
    uint8_t pkt[] = {
        0x12,0x34, 0x81,0x80, 0x00,0x01, 0x00,0x00, 0x00,0x00, 0x00,0x00,
        0xc0,0x0c,                          /* name @12 points to @12 */
        0x00,0x01, 0x00,0x01
    };
    KlResolveResult r;
    ASSERT_EQ(-1, kl_dns_parse_response(pkt, sizeof(pkt), 0x1234, KL_DNS_TYPE_A, NULL, 0, &r));
}

/* ── Mock nameserver + resolver integration ──────────────────────────── */

static int g_answer_a;      /* reply with an A record for A queries   */
static int g_answer_aaaa;   /* reply with an AAAA record for AAAA queries */
static int g_rcode;         /* rcode to set (0 = NOERROR, 3 = NXDOMAIN) */
static int g_silent;        /* drop queries (to exercise timeout)      */
static int g_queries;
static int g_flip_case;     /* echo the question name with each letter case-flipped */
static int g_wrong_question;/* echo a different question entirely (spoof) */
static uint16_t g_seen_ids[8];   /* transaction ids observed, in order */
static int g_seen_n;
static uint8_t g_last_q[128];    /* last query's question-section bytes */
static size_t g_last_q_len;
static int g_last_arcount;       /* last query's ARCOUNT (1 = EDNS0 OPT present) */

/* Parse qtype from the question and emit a canned response. */
static void mock_ns(KlUdpServer *s, const void *data, size_t len,
                    const struct sockaddr *src, socklen_t src_len, void *ud) {
    (void)ud;
    if (len < 12) return;
    g_queries++;

    const uint8_t *q = data;
    if (g_seen_n < 8)
        g_seen_ids[g_seen_n++] = (uint16_t)((q[0] << 8) | q[1]);
    g_last_arcount = (q[10] << 8) | q[11];

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

    /* Capture the query's question section for inspection. */
    g_last_q_len = qend - 12;
    if (g_last_q_len <= sizeof(g_last_q))
        memcpy(g_last_q, q + 12, g_last_q_len);

    if (g_silent) return;

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
    if (g_wrong_question) {
        /* Emit a fixed, different question ("evil" A IN) — right id, wrong name. */
        static const uint8_t evilq[] = { 0x04,'e','v','i','l',0x00, 0x00,0x01, 0x00,0x01 };
        memcpy(resp + n, evilq, sizeof(evilq));
        n += sizeof(evilq);
    } else {
        memcpy(resp + n, q + 12, qend - 12); /* echo question */
        if (g_flip_case) {                    /* tamper: flip case of name letters */
            size_t p = n;
            while (resp[p] != 0) {            /* walk labels */
                uint8_t lab = resp[p++];
                for (uint8_t k = 0; k < lab; k++) {
                    uint8_t c = resp[p + k];
                    uint8_t low = (uint8_t)(c | 0x20);
                    if (low >= 'a' && low <= 'z')
                        resp[p + k] = (uint8_t)(c ^ 0x20);
                }
                p += lab;
            }
        }
        n += qend - 12;
    }

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
    g_flip_case = 0; g_wrong_question = 0; g_seen_n = 0; g_last_q_len = 0;
    g_last_arcount = 0;
    g_done = 0; g_err = 0; memset(&g_res, 0, sizeof(g_res));
}

/* Start the mock nameserver and create a resolver pointed at it (custom cfg). */
static KlResolver *make_resolver_cfg(KlEventCtx *ctx, KlUdpServer *ns,
                                     KlDnsResolverConfig *dc) {
    KlUdpServerConfig sc = { .bind_addr = "127.0.0.1", .port = 0 };
    if (kl_udp_server_init(ns, ctx, &sc, mock_ns, NULL) != 0)
        return NULL;
    dc->nameserver = "127.0.0.1";
    dc->port = kl_udp_server_local_port(ns);
    return kl_dns_resolver_create(ctx, dc);
}

/* Build a resolver + mock nameserver on one ctx. Caller frees. */
static KlResolver *make_resolver(KlEventCtx *ctx, KlUdpServer *ns, int timeout_ms,
                                 int attempts) {
    KlDnsResolverConfig dc = { .timeout_ms = timeout_ms, .attempts = attempts };
    return make_resolver_cfg(ctx, ns, &dc);
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

UTEST(dns, localhost_shortcut) {
    /* "localhost" resolves to loopback without a DNS query. */
    reset_dns();
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));
    KlUdpServer ns;
    KlResolver *r = make_resolver(&ctx, &ns, 500, 2);
    ASSERT_TRUE(r != NULL);

    ASSERT_TRUE(r->resolve(r, &ctx, "localhost", 8080, on_done, NULL) != NULL);
    pump(&ctx, &g_done, 50);

    ASSERT_EQ(1, g_done);
    ASSERT_EQ(0, g_err);
    ASSERT_EQ(AF_INET, g_res.ai_family);
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &((struct sockaddr_in *)&g_res.addr)->sin_addr, ip, sizeof(ip));
    ASSERT_STREQ("127.0.0.1", ip);
    ASSERT_EQ(8080, ntohs(((struct sockaddr_in *)&g_res.addr)->sin_port));
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

/* ── Hardening (#2) ──────────────────────────────────────────────────── */

UTEST(dns, spoofed_question_rejected) {
    /* Right transaction id, wrong question → response must be ignored (ID-only
     * matching no longer suffices), so the query times out. */
    reset_dns();
    g_answer_a = 1; g_wrong_question = 1;
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));
    KlUdpServer ns;
    KlResolver *r = make_resolver(&ctx, &ns, 30, 1);   /* fast timeout */
    ASSERT_TRUE(r != NULL);

    ASSERT_TRUE(r->resolve(r, &ctx, "host.test", 80, on_done, NULL) != NULL);
    pump(&ctx, &g_done, 200);

    ASSERT_EQ(1, g_done);
    ASSERT_EQ(KL_ERR_DNS, g_err);        /* not accepted despite matching id */
    ASSERT_TRUE(g_queries >= 1);         /* the spoof reply was seen and rejected */

    r->destroy(r);
    kl_udp_server_free(&ns);
    kl_event_ctx_free(&ctx);
}

UTEST(dns, case_tamper_rejected) {
    /* 0x20 on (default): a case-flipped question echo must be rejected. */
    reset_dns();
    g_answer_a = 1; g_flip_case = 1;
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));
    KlUdpServer ns;
    KlResolver *r = make_resolver(&ctx, &ns, 30, 1);
    ASSERT_TRUE(r != NULL);

    ASSERT_TRUE(r->resolve(r, &ctx, "host.test", 80, on_done, NULL) != NULL);
    pump(&ctx, &g_done, 200);

    ASSERT_EQ(1, g_done);
    ASSERT_EQ(KL_ERR_DNS, g_err);
    r->destroy(r);
    kl_udp_server_free(&ns);
    kl_event_ctx_free(&ctx);
}

/* Count uppercase ASCII letters in the captured query question. */
static int q_uppercase_count(void) {
    int up = 0;
    for (size_t i = 0; i < g_last_q_len; i++)
        if (g_last_q[i] >= 'A' && g_last_q[i] <= 'Z') up++;
    return up;
}

UTEST(dns, dns_0x20_randomizes_case) {
    /* With 0x20 on, a long letter-rich name gets mixed-case in the wire query. */
    reset_dns();
    g_answer_a = 1;
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));
    KlUdpServer ns;
    KlResolver *r = make_resolver(&ctx, &ns, 500, 2);
    ASSERT_TRUE(r != NULL);

    ASSERT_TRUE(r->resolve(r, &ctx,
        "averylonghostnamewithmanyletters.example.test", 80, on_done, NULL) != NULL);
    pump(&ctx, &g_done, 200);

    ASSERT_EQ(1, g_done);
    ASSERT_TRUE(q_uppercase_count() > 0);   /* ~30 letters → all-lowercase is ~2^-30 */

    r->destroy(r);
    kl_udp_server_free(&ns);
    kl_event_ctx_free(&ctx);
}

UTEST(dns, disable_0x20_lowercase_and_resolves) {
    /* Escape hatch: no case randomization; verbatim echo still resolves. */
    reset_dns();
    g_answer_a = 1;
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));
    KlUdpServer ns;
    KlDnsResolverConfig dc = { .timeout_ms = 500, .attempts = 2, .disable_0x20 = 1 };
    KlResolver *r = make_resolver_cfg(&ctx, &ns, &dc);
    ASSERT_TRUE(r != NULL);

    ASSERT_TRUE(r->resolve(r, &ctx, "host.test", 80, on_done, NULL) != NULL);
    pump(&ctx, &g_done, 200);

    ASSERT_EQ(1, g_done);
    ASSERT_EQ(0, g_err);                    /* resolves */
    ASSERT_EQ(0, q_uppercase_count());      /* lowercase in, lowercase out: no 0x20 */

    r->destroy(r);
    kl_udp_server_free(&ns);
    kl_event_ctx_free(&ctx);
}

UTEST(dns, transaction_ids_not_sequential) {
    /* IDs must not be the old predictable 1,2,3 counter. */
    reset_dns();
    g_answer_a = 1;
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));
    KlUdpServer ns;
    KlResolver *r = make_resolver(&ctx, &ns, 500, 2);
    ASSERT_TRUE(r != NULL);

    for (int i = 0; i < 3; i++) {
        g_done = 0;
        ASSERT_TRUE(r->resolve(r, &ctx, "host.test", 80, on_done, NULL) != NULL);
        pump(&ctx, &g_done, 200);
        ASSERT_EQ(1, g_done);
    }
    ASSERT_TRUE(g_seen_n >= 3);
    /* The old counter would have produced exactly 1,2,3. */
    ASSERT_FALSE(g_seen_ids[0] == 1 && g_seen_ids[1] == 2 && g_seen_ids[2] == 3);

    r->destroy(r);
    kl_udp_server_free(&ns);
    kl_event_ctx_free(&ctx);
}

/* ── Phase 1a: /etc/hosts + EDNS0 ────────────────────────────────────── */

static const char *write_hosts(const char *content) {
    static char path[64];
    snprintf(path, sizeof(path), "/tmp/keel_hosts_%d", (int)getpid());
    FILE *f = fopen(path, "w");
    if (!f) return NULL;
    fputs(content, f);
    fclose(f);
    return path;
}

static const char *write_resolv(const char *content) {
    static char path[64];
    snprintf(path, sizeof(path), "/tmp/keel_resolv_%d", (int)getpid());
    FILE *f = fopen(path, "w");
    if (!f) return NULL;
    fputs(content, f);
    fclose(f);
    return path;
}

/* A nameserver that receives but never replies (to exercise failover). */
static int g_silent_hits;
static void silent_ns(KlUdpServer *s, const void *data, size_t len,
                      const struct sockaddr *src, socklen_t src_len, void *ud) {
    (void)s; (void)data; (void)len; (void)src; (void)src_len; (void)ud;
    g_silent_hits++;
}

UTEST(dns, hosts_file_lookup) {
    reset_dns();
    const char *hp = write_hosts("# a comment\n10.9.8.7  myhost.test alias.test\n");
    ASSERT_TRUE(hp != NULL);
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));
    KlUdpServer ns;
    KlDnsResolverConfig dc = { .timeout_ms = 500, .attempts = 2, .hosts_path = hp };
    KlResolver *r = make_resolver_cfg(&ctx, &ns, &dc);
    ASSERT_TRUE(r != NULL);

    /* Resolve via the alias name — must hit the hosts file, no DNS query. */
    ASSERT_TRUE(r->resolve(r, &ctx, "alias.test", 80, on_done, NULL) != NULL);
    pump(&ctx, &g_done, 50);

    ASSERT_EQ(1, g_done);
    ASSERT_EQ(0, g_err);
    ASSERT_EQ(AF_INET, g_res.ai_family);
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &((struct sockaddr_in *)&g_res.addr)->sin_addr, ip, sizeof(ip));
    ASSERT_STREQ("10.9.8.7", ip);
    ASSERT_EQ(80, ntohs(((struct sockaddr_in *)&g_res.addr)->sin_port));
    ASSERT_EQ(0, g_queries);            /* no packet sent */

    r->destroy(r);
    kl_udp_server_free(&ns);
    kl_event_ctx_free(&ctx);
    unlink(hp);
}

UTEST(dns, hosts_prefer_ipv6) {
    reset_dns();
    const char *hp = write_hosts("10.0.0.1 dual.test\n2001:db8::1 dual.test\n");
    ASSERT_TRUE(hp != NULL);
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));
    KlUdpServer ns;
    KlDnsResolverConfig dc = { .prefer_ipv6 = 1, .hosts_path = hp };
    KlResolver *r = make_resolver_cfg(&ctx, &ns, &dc);
    ASSERT_TRUE(r != NULL);

    ASSERT_TRUE(r->resolve(r, &ctx, "dual.test", 443, on_done, NULL) != NULL);
    pump(&ctx, &g_done, 50);

    ASSERT_EQ(1, g_done);
    ASSERT_EQ(AF_INET6, g_res.ai_family);   /* prefer_ipv6 picks the AAAA line */
    char ip[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&g_res.addr)->sin6_addr, ip, sizeof(ip));
    ASSERT_STREQ("2001:db8::1", ip);

    r->destroy(r);
    kl_udp_server_free(&ns);
    kl_event_ctx_free(&ctx);
    unlink(hp);
}

UTEST(dns, edns0_opt_advertised) {
    reset_dns();
    g_answer_a = 1;
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));
    KlUdpServer ns;
    KlResolver *r = make_resolver(&ctx, &ns, 500, 2);   /* EDNS0 on by default */
    ASSERT_TRUE(r != NULL);

    ASSERT_TRUE(r->resolve(r, &ctx, "host.test", 80, on_done, NULL) != NULL);
    pump(&ctx, &g_done, 200);

    ASSERT_EQ(1, g_done);
    ASSERT_EQ(0, g_err);
    ASSERT_EQ(1, g_last_arcount);           /* OPT record in the additional section */

    r->destroy(r);
    kl_udp_server_free(&ns);
    kl_event_ctx_free(&ctx);
}

UTEST(dns, edns0_disabled) {
    reset_dns();
    g_answer_a = 1;
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));
    KlUdpServer ns;
    KlDnsResolverConfig dc = { .timeout_ms = 500, .attempts = 2, .disable_edns = 1 };
    KlResolver *r = make_resolver_cfg(&ctx, &ns, &dc);
    ASSERT_TRUE(r != NULL);

    ASSERT_TRUE(r->resolve(r, &ctx, "host.test", 80, on_done, NULL) != NULL);
    pump(&ctx, &g_done, 200);

    ASSERT_EQ(1, g_done);
    ASSERT_EQ(0, g_last_arcount);           /* no OPT record */

    r->destroy(r);
    kl_udp_server_free(&ns);
    kl_event_ctx_free(&ctx);
}

/* ── Phase 1b-i: multiple nameservers + failover ─────────────────────── */

UTEST(dns, nameserver_failover) {
    /* Two nameservers via a resolv.conf fixture: the first is silent, so the
     * resolver must fail over to the second (which answers). */
    reset_dns();
    g_answer_a = 1;
    g_silent_hits = 0;
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));

    KlUdpServer ns1, ns2;
    KlUdpServerConfig sc = { .bind_addr = "127.0.0.1", .port = 0 };
    ASSERT_EQ(0, kl_udp_server_init(&ns1, &ctx, &sc, silent_ns, NULL));
    ASSERT_EQ(0, kl_udp_server_init(&ns2, &ctx, &sc, mock_ns, NULL));
    uint16_t p1 = kl_udp_server_local_port(&ns1);
    uint16_t p2 = kl_udp_server_local_port(&ns2);

    char rc[160];
    snprintf(rc, sizeof(rc),
             "# fixture\nnameserver 127.0.0.1#%u\nnameserver 127.0.0.1#%u\n", p1, p2);
    const char *rcpath = write_resolv(rc);
    ASSERT_TRUE(rcpath != NULL);

    KlDnsResolverConfig dc = { .resolv_conf_path = rcpath, .timeout_ms = 40, .attempts = 2 };
    KlResolver *r = kl_dns_resolver_create(&ctx, &dc);
    ASSERT_TRUE(r != NULL);

    ASSERT_TRUE(r->resolve(r, &ctx, "host.test", 80, on_done, NULL) != NULL);
    pump(&ctx, &g_done, 400);

    ASSERT_EQ(1, g_done);
    ASSERT_EQ(0, g_err);                 /* resolved via the second nameserver */
    ASSERT_EQ(AF_INET, g_res.ai_family);
    ASSERT_TRUE(g_silent_hits >= 1);     /* the first (silent) nameserver was tried */

    r->destroy(r);
    kl_udp_server_free(&ns1);
    kl_udp_server_free(&ns2);
    kl_event_ctx_free(&ctx);
    unlink(rcpath);
}

UTEST(dns, all_nameservers_silent_times_out) {
    /* Both nameservers silent → the resolve fails after exhausting them. */
    reset_dns();
    g_silent_hits = 0;
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));

    KlUdpServer ns1, ns2;
    KlUdpServerConfig sc = { .bind_addr = "127.0.0.1", .port = 0 };
    ASSERT_EQ(0, kl_udp_server_init(&ns1, &ctx, &sc, silent_ns, NULL));
    ASSERT_EQ(0, kl_udp_server_init(&ns2, &ctx, &sc, silent_ns, NULL));
    char rc[160];
    snprintf(rc, sizeof(rc), "nameserver 127.0.0.1#%u\nnameserver 127.0.0.1#%u\n",
             kl_udp_server_local_port(&ns1), kl_udp_server_local_port(&ns2));
    const char *rcpath = write_resolv(rc);

    KlDnsResolverConfig dc = { .resolv_conf_path = rcpath, .timeout_ms = 20, .attempts = 1 };
    KlResolver *r = kl_dns_resolver_create(&ctx, &dc);
    ASSERT_TRUE(r != NULL);

    ASSERT_TRUE(r->resolve(r, &ctx, "host.test", 80, on_done, NULL) != NULL);
    pump(&ctx, &g_done, 400);

    ASSERT_EQ(1, g_done);
    ASSERT_EQ(KL_ERR_DNS, g_err);
    ASSERT_TRUE(g_silent_hits >= 2);     /* both nameservers were tried */

    r->destroy(r);
    kl_udp_server_free(&ns1);
    kl_udp_server_free(&ns2);
    kl_event_ctx_free(&ctx);
    unlink(rcpath);
}

UTEST_MAIN();
