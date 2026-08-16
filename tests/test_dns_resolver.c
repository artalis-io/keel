#include "utest.h"
#include <keel/dns_resolver.h>
#include <keel/resolver.h>
#include <keel/udp_server.h>
#include <keel/event_ctx.h>
#include <keel/allocator.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "net_compat.h"

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
    ASSERT_EQ((int)KL_AF_INET, (int)kl_sockaddr_family(&r.addrs[0]));
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, r.addrs[0].u.ip, ip, sizeof(ip));
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
static int g_truncate;      /* reply with the TC bit set (force TCP fallback) */
/* DNS cookies (RFC 7873) */
static int g_cookie;             /* mock supports cookies: echo client + add server */
static int g_cookie_wrong_client;/* echo a corrupted client cookie (spoof test) */
static int g_cookie_badcookie_once;/* first cookie'd response is BADCOOKIE */
static int g_cookie_bad_sent;    /* internal: a BADCOOKIE reply was already sent */
static const uint8_t g_srv_cookie[8] = {0xA0,0xA1,0xA2,0xA3,0xA4,0xA5,0xA6,0xA7};
static uint8_t g_seen_client[8]; /* client cookie carried by the last query */
static int g_seen_client_ok;     /* 1 = last query carried a client cookie */
static uint8_t g_seen_server[32]; /* server cookie carried by the last query */
static uint8_t g_seen_server_len; /* server-cookie length in last query (0 = none) */
static int g_queries;
static int g_flip_case;     /* echo the question name with each letter case-flipped */
static int g_wrong_question;/* echo a different question entirely (spoof) */
static int g_silent_aaaa;   /* drop only AAAA queries (to exercise resolution delay) */
static uint16_t g_seen_ids[8];   /* transaction ids observed, in order */
static int g_seen_n;
static uint8_t g_last_q[128];    /* last query's question-section bytes */
static size_t g_last_q_len;
static int g_last_arcount;       /* last query's ARCOUNT (1 = EDNS0 OPT present) */
static char g_last_qname[256];   /* last query's decoded name (lowercased) */
/* Step 6.3 conformance knobs (see the section at the bottom): */
static int    g_octet_from_name; /* A record's 4th octet = the query name's first char (lowercased) —
                                  * gives DISTINGUISHABLE answers for the concurrent-resolution demux test */
static int    g_wrong_source;    /* two-phase src-filter proof: send ONLY a POISONED answer from
                                  * g_spoof_sock (a non-nameserver source) + STASH the legit response +
                                  * the resolver's dest for the test to release later. No auto-reply. */
static KlUdp *g_spoof_sock;      /* the wrong-source socket (bound to a different ephemeral port) */
static uint8_t    g_stash_resp[2][512]; /* stashed legit responses (A + AAAA), released in phase 2 */
static size_t     g_stash_len[2];
static int        g_stash_n;
static KlSockAddr g_stash_dest;         /* the resolver's addr:port (reply destination) */
static int        g_poison_sent;        /* poisoned spoofs SUCCESSFULLY submitted (send returned 0) */
static int        g_poison_fail;        /* poisoned spoof submissions that FAILED (send returned != 0) */

/* Parse a DNS query's question: set *qtype, return qend (0 on malformed). */
static size_t dns_q_parse(const uint8_t *q, size_t len, int *qtype) {
    size_t off = 12;
    while (off < len && q[off] != 0) {
        if (q[off] & 0xC0) return 0;             /* no compression in our queries */
        off += 1 + q[off];
    }
    off += 1;                                    /* root label */
    if (off + 4 > len) return 0;
    *qtype = (q[off] << 8) | q[off + 1];
    return off + 4;                              /* end of question */
}

/* Emit a DNS response for query q[0..qend) into resp[]; returns byte count.
 * tc: set the TC (truncation) bit and emit no answer (forces a TCP retry). */
static size_t dns_write_response(const uint8_t *q, size_t qend, int qtype,
                                 uint8_t *resp, int tc) {
    size_t n = 0;
    resp[0] = q[0]; resp[1] = q[1];              /* echo id */
    resp[2] = (uint8_t)(tc ? 0x83 : 0x81);       /* QR + RD (+ TC when truncated) */
    resp[3] = (uint8_t)(0x80 | (g_rcode & 0x0F));/* RA + rcode */
    resp[4] = 0; resp[5] = 1;                    /* qdcount */
    int answer = !tc && ((qtype == KL_DNS_TYPE_A && g_answer_a) ||
                         (qtype == KL_DNS_TYPE_AAAA && g_answer_aaaa));
    resp[6] = 0; resp[7] = (uint8_t)(answer ? 1 : 0);   /* ancount */
    resp[8] = 0; resp[9] = 0; resp[10] = 0; resp[11] = 0;
    n = 12;
    if (g_wrong_question) {
        /* Emit a fixed, different question ("evil" A IN) — right id, wrong name. */
        static const uint8_t evilq[] = { 0x04,'e','v','i','l',0x00, 0x00,0x01, 0x00,0x01 };
        memcpy(resp + n, evilq, sizeof(evilq));
        n += sizeof(evilq);
    } else {
        memcpy(resp + n, q + 12, qend - 12);     /* echo question */
        if (g_flip_case) {                        /* tamper: flip case of name letters */
            size_t p = n;
            while (resp[p] != 0) {               /* walk labels */
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
        resp[n++] = 0xc0; resp[n++] = 0x0c;      /* name → question */
        resp[n++] = (uint8_t)(qtype >> 8); resp[n++] = (uint8_t)(qtype & 0xFF);
        resp[n++] = 0x00; resp[n++] = 0x01;      /* IN */
        resp[n++] = 0; resp[n++] = 0; resp[n++] = 0x01; resp[n++] = 0x00; /* ttl */
        if (qtype == KL_DNS_TYPE_A) {
            /* Default answer 10.1.2.3; with g_octet_from_name the 4th octet = the query name's first
             * character lowercased (q[13] is the first label's first byte — case-insensitive so it is
             * stable under 0x20 randomization), giving each distinct name a distinct answer. */
            uint8_t last = g_octet_from_name ? (uint8_t)(q[13] | 0x20) : (uint8_t)3;
            resp[n++] = 0; resp[n++] = 4;
            resp[n++] = 10; resp[n++] = 1; resp[n++] = 2; resp[n++] = last;
        } else {
            resp[n++] = 0; resp[n++] = 16;
            memset(resp + n, 0, 16);
            resp[n + 15] = 1;                    /* ::1 */
            n += 16;
        }
    }
    return n;
}

/* Parse qtype from the question and emit a canned response. */
static void mock_ns(KlUdpServer *s, const void *data, size_t len,
                    const KlSockAddr *src, void *ud) {
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

    /* Decode the queried name (lowercased, dot-joined) for search/ndots tests. */
    {
        size_t o = 12, w = 0;
        while (o < len && q[o] != 0 && w < sizeof(g_last_qname) - 1) {
            uint8_t ll = q[o++];
            if (ll & 0xC0) break;
            for (uint8_t k = 0; k < ll && o < len && w < sizeof(g_last_qname) - 1; k++, o++) {
                char c = (char)q[o];
                if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
                g_last_qname[w++] = c;
            }
            if (w < sizeof(g_last_qname) - 1) g_last_qname[w++] = '.';
        }
        if (w > 0 && g_last_qname[w - 1] == '.') w--;
        g_last_qname[w] = '\0';
    }

    /* Capture the COOKIE option the query carried. The query is
     * [header][question][OPT RR], so the OPT RR begins right at qend. */
    g_seen_client_ok = 0; g_seen_server_len = 0;
    if (qend + 11 <= len && q[qend] == 0x00 &&
        ((q[qend + 1] << 8) | q[qend + 2]) == 41 /* OPT */) {
        uint16_t rdlen = (uint16_t)((q[qend + 9] << 8) | q[qend + 10]);
        size_t o = qend + 11, end = o + rdlen;
        if (end <= len) {
            while (o + 4 <= end) {
                uint16_t oc = (uint16_t)((q[o] << 8) | q[o + 1]);
                uint16_t ol = (uint16_t)((q[o + 2] << 8) | q[o + 3]);
                o += 4;
                if (o + ol > end) break;
                if (oc == 10 && ol >= 8) {        /* COOKIE */
                    memcpy(g_seen_client, q + o, 8);
                    g_seen_client_ok = 1;
                    uint8_t sl = (uint8_t)(ol - 8);
                    if (sl && sl <= 32) { memcpy(g_seen_server, q + o + 8, sl); g_seen_server_len = sl; }
                }
                o += ol;
            }
        }
    }

    if (g_silent) return;
    if (g_silent_aaaa && qtype == KL_DNS_TYPE_AAAA) return;   /* drop AAAA only */

    uint8_t resp[512];
    int badcookie = g_cookie && g_cookie_badcookie_once && !g_cookie_bad_sent;
    size_t n = dns_write_response(q, qend, qtype, resp, g_truncate);
    if (g_cookie) {
        if (badcookie) {                          /* no answer + BADCOOKIE rcode */
            resp[3] = (uint8_t)(0x80 | 7);        /* RA + header rcode 7 */
            resp[7] = 0;                          /* ancount = 0 */
            n = 12 + (qend - 12);                 /* drop any answer: header + question */
            g_cookie_bad_sent = 1;
        }
        resp[10] = 0; resp[11] = 1;               /* arcount = 1 (OPT) */
        resp[n++] = 0x00;                         /* OPT owner = root */
        resp[n++] = 0x00; resp[n++] = 41;         /* type OPT */
        resp[n++] = 0x04; resp[n++] = 0xD0;       /* UDP size 1232 */
        resp[n++] = badcookie ? 0x01 : 0x00;      /* TTL b0 = ext rcode (1 => BADCOOKIE) */
        resp[n++] = 0x00; resp[n++] = 0x00; resp[n++] = 0x00; /* version + flags */
        resp[n++] = 0x00; resp[n++] = 4 + 16;     /* rdlen = opt-hdr(4) + client(8)+server(8) */
        resp[n++] = 0x00; resp[n++] = 10;         /* option-code COOKIE */
        resp[n++] = 0x00; resp[n++] = 16;         /* option-length */
        uint8_t cli[8];
        if (g_seen_client_ok) memcpy(cli, g_seen_client, 8); else memset(cli, 0, 8);
        if (g_cookie_wrong_client) cli[0] = (uint8_t)(cli[0] ^ 0xFF);
        memcpy(resp + n, cli, 8); n += 8;         /* echo client cookie */
        memcpy(resp + n, g_srv_cookie, 8); n += 8;/* add server cookie */
    }
    /* Two-phase wrong-source proof: send ONLY a POISONED copy (corrupt the A answer's last octet) FROM
     * a socket that is NOT the configured nameserver, and STASH the legit response + the resolver's
     * dest so the test can release it from the real nameserver socket in phase 2. No legit reply now —
     * so if the resolver's src (address+port) gate were broken, the poison alone would (wrongly)
     * complete the resolution; a working gate leaves it PENDING until the stashed reply is released. */
    if (g_wrong_source && g_spoof_sock && n <= 512) {
        uint8_t poison[512];
        memcpy(poison, resp, n);
        if (poison[7] >= 1)                     /* an A answer present (no cookie in this test) */
            poison[n - 1] = 0xEE;               /* 10.1.2.3 -> 10.1.2.238 (distinguishable) */
        if (kl_udp_send_to(g_spoof_sock, poison, n, src) == 0)   /* count only submitted spoofs */
            g_poison_sent++;
        else
            g_poison_fail++;                    /* a send failure would falsely leave the resolver pending */
        if (g_stash_n < 2) {                    /* stash the legit A + AAAA responses for phase 2 */
            memcpy(g_stash_resp[g_stash_n], resp, n);
            g_stash_len[g_stash_n] = n;
            g_stash_n++;
        }
        g_stash_dest = *src;
        return;                                 /* withhold the legit reply until phase 2 */
    }
    kl_udp_server_reply(s, resp, n, src);
}

/* ── Mock DNS-over-TCP server (event-loop driven, RFC 7766 framing) ─────── */

#define MOCK_TCP_MAX 8
typedef struct MockTcp MockTcp;
typedef struct {
    MockTcp      *m;
    int           fd;
    size_t        len;
    unsigned char buf[4096];
} MockTcpConn;

struct MockTcp {
    KlEventCtx  *ctx;
    int          listen_fd;
    int          accepts;        /* accepted connections (persistent-reuse assertion) */
    int          drop_on_accept; /* close each connection immediately (simulate drop) */
    MockTcpConn *conns[MOCK_TCP_MAX];
    int          nconns;
};

static void mock_tcp_on_client(KlSocketHandle fd, KlEventMask ready, void *ud) {
    (void)ready;
    MockTcpConn *c = ud;
    ssize_t r = recv(fd, c->buf + c->len, sizeof(c->buf) - c->len, 0);
    if (r <= 0)
        return;                                  /* EOF / error — test tears down */
    c->len += (size_t)r;

    size_t off = 0;
    while (c->len - off >= 2) {                   /* frame by the 2-byte length prefix */
        size_t mlen = ((size_t)c->buf[off] << 8) | c->buf[off + 1];
        if (c->len - off - 2 < mlen)
            break;                               /* incomplete frame — wait for more */
        const uint8_t *q = c->buf + off + 2;
        int qtype = 0;
        size_t qend = dns_q_parse(q, mlen, &qtype);
        if (qend) {
            uint8_t resp[512];
            size_t n = dns_write_response(q, qend, qtype, resp, 0);
            unsigned char framed[2 + 512];
            framed[0] = (uint8_t)(n >> 8); framed[1] = (uint8_t)(n & 0xFF);
            memcpy(framed + 2, resp, n);
            ssize_t w = send(fd, framed, n + 2, 0);
            (void)w;
        }
        off += 2 + mlen;
    }
    if (off > 0) { memmove(c->buf, c->buf + off, c->len - off); c->len -= off; }
}

static void mock_tcp_on_listen(KlSocketHandle fd, KlEventMask ready, void *ud) {
    (void)ready;
    MockTcp *m = ud;
    int cfd = accept(fd, NULL, NULL);
    if (cfd < 0)
        return;
    m->accepts++;
    kl_test_set_nonblock(cfd);
    if (m->drop_on_accept || m->nconns >= MOCK_TCP_MAX) { kl_test_closesock(cfd); return; }
    MockTcpConn *c = calloc(1, sizeof(*c));
    if (!c) { kl_test_closesock(cfd); return; }
    c->m = m; c->fd = cfd;
    m->conns[m->nconns++] = c;
    kl_watcher_add(m->ctx, cfd, KL_EVENT_READ, mock_tcp_on_client, c);
}

/* Bind a TCP DNS listener on 127.0.0.1:port (the same port the UDP mock uses). */
static int mock_tcp_start(MockTcp *m, KlEventCtx *ctx, int port) {
    memset(m, 0, sizeof(*m));
    m->ctx = ctx;
    m->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (m->listen_fd < 0) return -1;
    int one = 1;
    setsockopt(m->listen_fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));
    struct sockaddr_in a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(m->listen_fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        kl_test_closesock(m->listen_fd); return -1;
    }
    if (listen(m->listen_fd, 8) != 0) { kl_test_closesock(m->listen_fd); return -1; }
    kl_test_set_nonblock(m->listen_fd);
    return kl_watcher_add(ctx, m->listen_fd, KL_EVENT_READ, mock_tcp_on_listen, m);
}

static void mock_tcp_stop(MockTcp *m) {
    for (int i = 0; i < m->nconns; i++) {
        kl_watcher_del(m->ctx, m->conns[i]->fd);
        kl_test_closesock(m->conns[i]->fd);
        free(m->conns[i]);
    }
    kl_watcher_del(m->ctx, m->listen_fd);
    kl_test_closesock(m->listen_fd);
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
    g_last_arcount = 0; g_last_qname[0] = '\0'; g_silent_aaaa = 0;
    g_truncate = 0;
    g_cookie = g_cookie_wrong_client = g_cookie_badcookie_once = g_cookie_bad_sent = 0;
    g_seen_client_ok = 0; g_seen_server_len = 0;
    g_done = 0; g_err = 0; memset(&g_res, 0, sizeof(g_res));
    g_octet_from_name = 0; g_wrong_source = 0; g_spoof_sock = NULL; g_stash_n = 0;
    g_poison_sent = 0; g_poison_fail = 0;
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
    ASSERT_EQ((int)KL_AF_INET, (int)kl_sockaddr_family(&g_res.addrs[0]));
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, g_res.addrs[0].u.ip, ip, sizeof(ip));
    ASSERT_STREQ("10.1.2.3", ip);
    ASSERT_EQ(8080, kl_sockaddr_port(&g_res.addrs[0]));
    ASSERT_EQ(1, g_res.naddrs);            /* resolver propagates the address list */

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
    ASSERT_EQ((int)KL_AF_INET6, (int)kl_sockaddr_family(&g_res.addrs[0]));
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
    ASSERT_EQ((int)KL_AF_INET, (int)kl_sockaddr_family(&g_res.addrs[0]));
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, g_res.addrs[0].u.ip, ip, sizeof(ip));
    ASSERT_STREQ("192.0.2.10", ip);
    ASSERT_EQ(1234, kl_sockaddr_port(&g_res.addrs[0]));
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
    ASSERT_EQ((int)KL_AF_INET, (int)kl_sockaddr_family(&g_res.addrs[0]));
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, g_res.addrs[0].u.ip, ip, sizeof(ip));
    ASSERT_STREQ("127.0.0.1", ip);
    ASSERT_EQ(8080, kl_sockaddr_port(&g_res.addrs[0]));
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
                      const KlSockAddr *src, void *ud) {
    (void)s; (void)data; (void)len; (void)src; (void)ud;
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
    ASSERT_EQ((int)KL_AF_INET, (int)kl_sockaddr_family(&g_res.addrs[0]));
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, g_res.addrs[0].u.ip, ip, sizeof(ip));
    ASSERT_STREQ("10.9.8.7", ip);
    ASSERT_EQ(80, kl_sockaddr_port(&g_res.addrs[0]));
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
    ASSERT_EQ((int)KL_AF_INET6, (int)kl_sockaddr_family(&g_res.addrs[0]));   /* prefer_ipv6 picks the AAAA line */
    char ip[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, g_res.addrs[0].u.ip, ip, sizeof(ip));
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
    ASSERT_EQ((int)KL_AF_INET, (int)kl_sockaddr_family(&g_res.addrs[0]));
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

/* ── Phase 1b-ii: search domains + ndots ─────────────────────────────── */

/* Build a resolver from a resolv.conf fixture with one mock nameserver. */
static KlResolver *resolver_with_search(KlEventCtx *ctx, KlUdpServer *ns,
                                        const char *search_line, int timeout_ms) {
    KlUdpServerConfig sc = { .bind_addr = "127.0.0.1", .port = 0 };
    if (kl_udp_server_init(ns, ctx, &sc, mock_ns, NULL) != 0)
        return NULL;
    static char rcbuf[256];
    snprintf(rcbuf, sizeof(rcbuf), "nameserver 127.0.0.1#%u\n%s\n",
             kl_udp_server_local_port(ns), search_line);
    const char *rcpath = write_resolv(rcbuf);
    if (!rcpath) return NULL;
    KlDnsResolverConfig dc = { .resolv_conf_path = rcpath, .timeout_ms = timeout_ms,
                               .attempts = 1 };
    return kl_dns_resolver_create(ctx, &dc);
}

UTEST(dns, search_appended_for_unqualified) {
    /* 0 dots < ndots(1): the search domain is appended and tried first. */
    reset_dns();
    g_answer_a = 1;
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));
    KlUdpServer ns;
    KlResolver *r = resolver_with_search(&ctx, &ns, "search corp.example", 500);
    ASSERT_TRUE(r != NULL);

    ASSERT_TRUE(r->resolve(r, &ctx, "myhost", 80, on_done, NULL) != NULL);
    pump(&ctx, &g_done, 200);

    ASSERT_EQ(1, g_done);
    ASSERT_EQ(0, g_err);
    ASSERT_STREQ("myhost.corp.example", g_last_qname);   /* search-appended */

    r->destroy(r);
    kl_udp_server_free(&ns);
    kl_event_ctx_free(&ctx);
}

UTEST(dns, absolute_tried_first_when_ndots_met) {
    /* 1 dot >= ndots(1): the name is tried as-is first (no search prefix). */
    reset_dns();
    g_answer_a = 1;
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));
    KlUdpServer ns;
    KlResolver *r = resolver_with_search(&ctx, &ns, "search corp.example", 500);
    ASSERT_TRUE(r != NULL);

    ASSERT_TRUE(r->resolve(r, &ctx, "my.host", 80, on_done, NULL) != NULL);
    pump(&ctx, &g_done, 200);

    ASSERT_EQ(1, g_done);
    ASSERT_STREQ("my.host", g_last_qname);   /* queried absolute, not my.host.corp.example */

    r->destroy(r);
    kl_udp_server_free(&ns);
    kl_event_ctx_free(&ctx);
}

UTEST(dns, search_falls_back_to_bare) {
    /* NXDOMAIN on every candidate: search variant first, then the bare name,
     * then fail. The last name queried is the bare one. */
    reset_dns();
    g_rcode = 3;   /* NXDOMAIN */
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));
    KlUdpServer ns;
    KlResolver *r = resolver_with_search(&ctx, &ns, "search corp.example", 500);
    ASSERT_TRUE(r != NULL);

    ASSERT_TRUE(r->resolve(r, &ctx, "myhost", 80, on_done, NULL) != NULL);
    pump(&ctx, &g_done, 300);

    ASSERT_EQ(1, g_done);
    ASSERT_EQ(KL_ERR_DNS, g_err);
    ASSERT_STREQ("myhost", g_last_qname);    /* bare name tried after the search variant */
    ASSERT_TRUE(g_queries >= 2);             /* both candidates were queried */

    r->destroy(r);
    kl_udp_server_free(&ns);
    kl_event_ctx_free(&ctx);
}

/* ── Real-response corpus (hermetic): wire formats the mock doesn't emit ── */

/* CNAME chain: a.test CNAME b.test, then A 5.6.7.8 for b.test. Owner names use
 * compression pointers, as real recursive resolvers emit. */
static const uint8_t RESP_CNAME[] = {
    0xAB,0xCD, 0x81,0x80, 0x00,0x01, 0x00,0x02, 0x00,0x00, 0x00,0x00,
    0x01,'a', 0x04,'t','e','s','t', 0x00, 0x00,0x01, 0x00,0x01,        /* Q a.test A IN */
    0xC0,0x0C, 0x00,0x05, 0x00,0x01, 0x00,0x00,0x01,0x2C, 0x00,0x08,   /* AN1 CNAME */
    0x01,'b', 0x04,'t','e','s','t', 0x00,                             /* rdata b.test @36 */
    0xC0,0x24, 0x00,0x01, 0x00,0x01, 0x00,0x00,0x01,0x2C, 0x00,0x04,   /* AN2 A for @36 */
    0x05,0x06,0x07,0x08
};

/* Three A records for m.test (10.0.0.1/.2/.3). */
static const uint8_t RESP_MULTI_A[] = {
    0x11,0x11, 0x81,0x80, 0x00,0x01, 0x00,0x03, 0x00,0x00, 0x00,0x00,
    0x01,'m', 0x04,'t','e','s','t', 0x00, 0x00,0x01, 0x00,0x01,
    0xC0,0x0C, 0x00,0x01, 0x00,0x01, 0x00,0x00,0x00,0x3C, 0x00,0x04, 10,0,0,1,
    0xC0,0x0C, 0x00,0x01, 0x00,0x01, 0x00,0x00,0x00,0x3C, 0x00,0x04, 10,0,0,2,
    0xC0,0x0C, 0x00,0x01, 0x00,0x01, 0x00,0x00,0x00,0x3C, 0x00,0x04, 10,0,0,3
};

/* A record + an EDNS0 OPT in the additional section (arcount=1). */
static const uint8_t RESP_EDNS_OPT[] = {
    0x22,0x22, 0x81,0x80, 0x00,0x01, 0x00,0x01, 0x00,0x00, 0x00,0x01,
    0x01,'e', 0x04,'t','e','s','t', 0x00, 0x00,0x01, 0x00,0x01,
    0xC0,0x0C, 0x00,0x01, 0x00,0x01, 0x00,0x00,0x00,0x3C, 0x00,0x04, 9,9,9,9, /* AN A */
    0x00, 0x00,0x29, 0x10,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00               /* AR OPT */
};

UTEST(dns_parse, real_cname_chain) {
    KlResolveResult r;
    ASSERT_EQ(0, kl_dns_parse_response(RESP_CNAME, sizeof(RESP_CNAME), 0xABCD,
                                       KL_DNS_TYPE_A, NULL, 0, &r));
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, r.addrs[0].u.ip, ip, sizeof(ip));
    ASSERT_STREQ("5.6.7.8", ip);           /* A after the CNAME is found */
}

UTEST(dns_parse, real_multi_a_returns_all) {
    KlResolveResult r;
    memset(&r, 0, sizeof(r));
    ASSERT_EQ(0, kl_dns_parse_response(RESP_MULTI_A, sizeof(RESP_MULTI_A), 0x1111,
                                       KL_DNS_TYPE_A, NULL, 0, &r));
    ASSERT_EQ(3, r.naddrs);                 /* all three A records collected */
    const char *want[] = { "10.0.0.1", "10.0.0.2", "10.0.0.3" };
    for (int i = 0; i < 3; i++) {
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, r.addrs[i].u.ip, ip, sizeof(ip));
        ASSERT_STREQ(want[i], ip);
    }
}

UTEST(dns_parse, real_edns_opt_ignored) {
    KlResolveResult r;
    ASSERT_EQ(0, kl_dns_parse_response(RESP_EDNS_OPT, sizeof(RESP_EDNS_OPT), 0x2222,
                                       KL_DNS_TYPE_A, NULL, 0, &r));
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, r.addrs[0].u.ip, ip, sizeof(ip));
    ASSERT_STREQ("9.9.9.9", ip);           /* OPT in additional doesn't derail the walk */
}

/* ── Opt-in live comparison vs the system resolver (KEEL_DNS_E2E=1) ─────── */

UTEST(dns, e2e_vs_getaddrinfo) {
    if (!getenv("KEEL_DNS_E2E"))
        UTEST_SKIP("set KEEL_DNS_E2E=1 to run the live system-DNS comparison");

    static const char *names[] = { "example.com", "dns.google" };
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));
    KlResolver *r = kl_dns_resolver_create(&ctx, NULL);   /* real /etc/resolv.conf */
    ASSERT_TRUE(r != NULL);

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        struct addrinfo hints, *gai = NULL;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(names[i], NULL, &hints, &gai) != 0 || !gai) {
            if (gai) freeaddrinfo(gai);
            continue;                        /* unresolvable here — skip this name */
        }

        reset_dns();
        ASSERT_TRUE(r->resolve(r, &ctx, names[i], 80, on_done, NULL) != NULL);
        for (int t = 0; t < 100 && g_done == 0; t++)
            kl_event_ctx_run(&ctx, 16, 50);   /* up to ~5s */

        ASSERT_EQ(1, g_done);
        ASSERT_EQ(0, g_err);
        ASSERT_EQ((int)KL_AF_INET, (int)kl_sockaddr_family(&g_res.addrs[0]));

        /* Our single answer must be a member of getaddrinfo's address set. */
        uint32_t ours; memcpy(&ours, g_res.addrs[0].u.ip, 4);
        int found = 0;
        for (struct addrinfo *a = gai; a; a = a->ai_next)
            if (a->ai_family == AF_INET &&
                ((struct sockaddr_in *)a->ai_addr)->sin_addr.s_addr == ours) {
                found = 1;
                break;
            }
        freeaddrinfo(gai);
        ASSERT_TRUE(found);
    }

    r->destroy(r);
    kl_event_ctx_free(&ctx);
}

/* ── Phase 2b: dual-family (A + AAAA concurrent) + resolution delay ────── */

UTEST(dns, dual_family_returns_both) {
    /* Both families answer → the result carries both, interleaved (A first by
     * default), from concurrent A and AAAA queries. */
    reset_dns();
    g_answer_a = 1; g_answer_aaaa = 1;
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));
    KlUdpServer ns;
    KlResolver *r = make_resolver(&ctx, &ns, 500, 2);
    ASSERT_TRUE(r != NULL);

    ASSERT_TRUE(r->resolve(r, &ctx, "host.test", 80, on_done, NULL) != NULL);
    pump(&ctx, &g_done, 200);

    ASSERT_EQ(1, g_done);
    ASSERT_EQ(0, g_err);
    ASSERT_EQ(2, g_res.naddrs);
    ASSERT_EQ((int)KL_AF_INET, (int)kl_sockaddr_family(&g_res.addrs[0]));    /* A first (default preference) */
    ASSERT_EQ((int)KL_AF_INET6, (int)kl_sockaddr_family(&g_res.addrs[1]));
    char ip[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET, g_res.addrs[0].u.ip, ip, sizeof(ip));
    ASSERT_STREQ("10.1.2.3", ip);
    inet_ntop(AF_INET6, g_res.addrs[1].u.ip, ip, sizeof(ip));
    ASSERT_STREQ("::1", ip);
    ASSERT_EQ(80, (int)kl_sockaddr_port(&g_res.addrs[1]));

    r->destroy(r);
    kl_udp_server_free(&ns);
    kl_event_ctx_free(&ctx);
}

UTEST(dns, dual_family_prefer_ipv6_orders_first) {
    /* prefer_ipv6 → AAAA leads the interleaved list. */
    reset_dns();
    g_answer_a = 1; g_answer_aaaa = 1;
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));
    KlUdpServer ns;
    KlDnsResolverConfig dc = { .prefer_ipv6 = 1, .timeout_ms = 500, .attempts = 2 };
    KlResolver *r = make_resolver_cfg(&ctx, &ns, &dc);
    ASSERT_TRUE(r != NULL);

    ASSERT_TRUE(r->resolve(r, &ctx, "host.test", 80, on_done, NULL) != NULL);
    pump(&ctx, &g_done, 200);

    ASSERT_EQ(1, g_done);
    ASSERT_EQ(2, g_res.naddrs);
    ASSERT_EQ((int)KL_AF_INET6, (int)kl_sockaddr_family(&g_res.addrs[0]));   /* AAAA first when preferred */
    ASSERT_EQ((int)KL_AF_INET, (int)kl_sockaddr_family(&g_res.addrs[1]));

    r->destroy(r);
    kl_udp_server_free(&ns);
    kl_event_ctx_free(&ctx);
}

UTEST(dns, resolution_delay_does_not_stall_on_slow_family) {
    /* A answers, AAAA never does. The resolve must complete via the ~50ms
     * resolution delay, NOT wait for the (2s) AAAA timeout. */
    reset_dns();
    g_answer_a = 1; g_silent_aaaa = 1;
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));
    KlUdpServer ns;
    KlDnsResolverConfig dc = { .timeout_ms = 2000, .attempts = 1 };
    KlResolver *r = make_resolver_cfg(&ctx, &ns, &dc);
    ASSERT_TRUE(r != NULL);

    ASSERT_TRUE(r->resolve(r, &ctx, "host.test", 80, on_done, NULL) != NULL);
    pump(&ctx, &g_done, 40);              /* ~400ms: enough for the 50ms delay, not 2s */

    ASSERT_EQ(1, g_done);                 /* completed via resolution delay */
    ASSERT_EQ(0, g_err);
    ASSERT_EQ(1, g_res.naddrs);           /* A only — AAAA never arrived */
    ASSERT_EQ((int)KL_AF_INET, (int)kl_sockaddr_family(&g_res.addrs[0]));

    r->destroy(r);
    kl_udp_server_free(&ns);
    kl_event_ctx_free(&ctx);
}

/* ── DNS-over-TCP fallback (RFC 7766) ─────────────────────────────────── */

/* A truncated (TC) UDP response makes the resolver retry over TCP and recover
 * the answer from the mock TCP DNS server. */
UTEST(dns, tcp_fallback) {
    reset_dns();
    g_answer_a = 1;                       /* TCP server answers A; AAAA has none */
    g_truncate = 1;                       /* every UDP reply sets TC → force TCP */
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));
    KlUdpServer ns;
    KlResolver *r = make_resolver(&ctx, &ns, 2000, 1);
    ASSERT_TRUE(r != NULL);
    MockTcp tcp;
    ASSERT_EQ(0, mock_tcp_start(&tcp, &ctx, kl_udp_server_local_port(&ns)));

    ASSERT_TRUE(r->resolve(r, &ctx, "host.test", 8080, on_done, NULL) != NULL);
    pump(&ctx, &g_done, 200);

    ASSERT_EQ(1, g_done);
    ASSERT_EQ(0, g_err);
    ASSERT_EQ(1, g_res.naddrs);           /* A recovered over TCP; AAAA empty */
    ASSERT_EQ((int)KL_AF_INET, (int)kl_sockaddr_family(&g_res.addrs[0]));
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, g_res.addrs[0].u.ip, ip, sizeof(ip));
    ASSERT_STREQ("10.1.2.3", ip);
    ASSERT_TRUE(tcp.accepts >= 1);        /* the fallback actually opened a TCP conn */

    r->destroy(r);
    mock_tcp_stop(&tcp);
    kl_udp_server_free(&ns);
    kl_event_ctx_free(&ctx);
}

/* The per-nameserver TCP connection is persistent: a second resolve reuses the
 * connection cached from the first (single accept), and both A+AAAA legs of a
 * single resolve pipeline over it. */
UTEST(dns, tcp_persistent_reuse) {
    reset_dns();
    g_answer_a = 1; g_answer_aaaa = 1;    /* TCP answers both families */
    g_truncate = 1;                       /* force both legs to TCP */
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));
    KlUdpServer ns;
    KlResolver *r = make_resolver(&ctx, &ns, 2000, 1);
    ASSERT_TRUE(r != NULL);
    MockTcp tcp;
    ASSERT_EQ(0, mock_tcp_start(&tcp, &ctx, kl_udp_server_local_port(&ns)));

    /* First resolve — both families pipeline over one connection. */
    ASSERT_TRUE(r->resolve(r, &ctx, "host.test", 80, on_done, NULL) != NULL);
    pump(&ctx, &g_done, 200);
    ASSERT_EQ(1, g_done);
    ASSERT_EQ(0, g_err);
    ASSERT_EQ(2, g_res.naddrs);           /* A + AAAA both recovered over TCP */
    ASSERT_EQ(1, tcp.accepts);            /* single connection for two pipelined legs */

    /* Second resolve — reuses the idle-cached connection (still one accept). */
    g_done = 0;
    ASSERT_TRUE(r->resolve(r, &ctx, "host.test", 80, on_done, NULL) != NULL);
    pump(&ctx, &g_done, 200);
    ASSERT_EQ(1, g_done);
    ASSERT_EQ(2, g_res.naddrs);
    ASSERT_EQ(1, tcp.accepts);            /* persistent: no new connection */

    r->destroy(r);
    mock_tcp_stop(&tcp);
    kl_udp_server_free(&ns);
    kl_event_ctx_free(&ctx);
}

/* If the TCP connection drops before answering, the pending legs settle empty
 * and the resolve completes (best effort) rather than hanging. */
UTEST(dns, tcp_drop) {
    reset_dns();
    g_answer_a = 1;
    g_truncate = 1;                       /* force TCP */
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));
    KlUdpServer ns;
    KlResolver *r = make_resolver(&ctx, &ns, 2000, 1);
    ASSERT_TRUE(r != NULL);
    MockTcp tcp;
    ASSERT_EQ(0, mock_tcp_start(&tcp, &ctx, kl_udp_server_local_port(&ns)));
    tcp.drop_on_accept = 1;               /* accept then immediately close */

    ASSERT_TRUE(r->resolve(r, &ctx, "host.test", 80, on_done, NULL) != NULL);
    pump(&ctx, &g_done, 200);

    ASSERT_EQ(1, g_done);                 /* completed, not hung */
    ASSERT_EQ(0, g_res.naddrs);           /* no addresses recovered */
    ASSERT_TRUE(tcp.accepts >= 1);

    r->destroy(r);
    mock_tcp_stop(&tcp);
    kl_udp_server_free(&ns);
    kl_event_ctx_free(&ctx);
}

/* ── DNS cookies (RFC 7873) ───────────────────────────────────────────── */

/* The resolver learns the server cookie from a response and echoes it on the
 * next query to the same nameserver. */
UTEST(dns, cookie_learned_and_echoed) {
    reset_dns();
    g_answer_a = 1;
    g_cookie = 1;                         /* mock echoes client + adds server cookie */
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));
    KlUdpServer ns;
    KlResolver *r = make_resolver(&ctx, &ns, 500, 2);
    ASSERT_TRUE(r != NULL);

    /* First resolve — the query carries only a client cookie; resolver learns
     * the server cookie from the reply. */
    ASSERT_TRUE(r->resolve(r, &ctx, "host.test", 80, on_done, NULL) != NULL);
    pump(&ctx, &g_done, 200);
    ASSERT_EQ(1, g_done);
    ASSERT_EQ(1, g_seen_client_ok);       /* the query carried a client cookie */

    /* Second resolve — the query must now echo the learned server cookie. */
    g_done = 0; g_seen_server_len = 0;
    ASSERT_TRUE(r->resolve(r, &ctx, "host.test", 80, on_done, NULL) != NULL);
    pump(&ctx, &g_done, 200);
    ASSERT_EQ(1, g_done);
    ASSERT_EQ(8, g_seen_server_len);      /* server cookie sent back */
    ASSERT_EQ(0, memcmp(g_seen_server, g_srv_cookie, 8));

    r->destroy(r);
    kl_udp_server_free(&ns);
    kl_event_ctx_free(&ctx);
}

/* A BADCOOKIE response carries a fresh server cookie; the resolver stores it and
 * re-issues the query once, which then resolves. */
UTEST(dns, cookie_badcookie_retry) {
    reset_dns();
    g_answer_a = 1;                       /* only A answers */
    g_cookie = 1;
    g_cookie_badcookie_once = 1;          /* first cookie'd reply is BADCOOKIE */
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));
    KlUdpServer ns;
    KlResolver *r = make_resolver(&ctx, &ns, 1000, 2);
    ASSERT_TRUE(r != NULL);

    /* A leg is transmitted before AAAA, so A receives the BADCOOKIE and must
     * retry to recover its address. */
    ASSERT_TRUE(r->resolve(r, &ctx, "host.test", 8080, on_done, NULL) != NULL);
    pump(&ctx, &g_done, 200);

    ASSERT_EQ(1, g_done);
    ASSERT_EQ(1, g_res.naddrs);           /* A recovered via the cookie'd retry */
    ASSERT_EQ((int)KL_AF_INET, (int)kl_sockaddr_family(&g_res.addrs[0]));
    ASSERT_TRUE(g_queries >= 3);          /* BADCOOKIE + retry + the other family */
    ASSERT_EQ(8, g_seen_server_len);      /* the retry carried the server cookie */

    r->destroy(r);
    kl_udp_server_free(&ns);
    kl_event_ctx_free(&ctx);
}

/* A response echoing the wrong client cookie is treated as a spoof and ignored,
 * even though it carries a valid-looking answer. */
UTEST(dns, cookie_client_mismatch_ignored) {
    reset_dns();
    g_answer_a = 1;                       /* the spoof would resolve if not rejected */
    g_cookie = 1;
    g_cookie_wrong_client = 1;            /* echo a corrupted client cookie */
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));
    KlUdpServer ns;
    KlResolver *r = make_resolver(&ctx, &ns, 300, 1);  /* short timeout, single try */
    ASSERT_TRUE(r != NULL);

    ASSERT_TRUE(r->resolve(r, &ctx, "host.test", 80, on_done, NULL) != NULL);
    pump(&ctx, &g_done, 200);             /* wait past the per-leg timeout */

    ASSERT_EQ(1, g_done);                 /* completes (via timeout), not from the spoof */
    ASSERT_EQ(0, g_res.naddrs);           /* the mismatched-cookie answer was rejected */

    r->destroy(r);
    kl_udp_server_free(&ns);
    kl_event_ctx_free(&ctx);
}

/* ── Step 6.3: DNS-over-KlUdp receive-machine conformance ─────────────────────────────────────────
 * The built-in resolver transitively rides the shared serial-receive machine (KlDgramRecv over the
 * dedicated inbound slot) through kl_udp_recv_start(dns_on_recv) — exactly like KlUdpServer; no
 * DNS-specific receive seam exists. Its UDP SENDS (kl_udp_send_to) and TEARDOWN (kl_udp_free) keep the
 * existing KlUdp compatibility semantics until the public KlUdpTransport path (Step 7), and the TCP
 * fallback (mock above) is an independent byte-stream path, NOT the datagram machine. These two cases
 * cover the couplings dns_on_recv leans on THROUGH the machine — src on every recv (anti-spoof) and
 * txid demux across serial re-arms — and run on both readiness and completion backends. */

/* Two-phase proof that a valid-content response from a NON-nameserver socket is rejected at the src
 * (address+port) gate — independent of cross-socket scheduling. Phase 1: only the poisoned answer is
 * ever sent (from the spoofer's port); pump and assert the resolution stays PENDING (a broken gate
 * would complete it here with 10.1.2.238). Phase 2: release the withheld legit responses from the real
 * nameserver socket; pump and assert completion with 10.1.2.3. (A single-phase send of both would not
 * prove the gate: if the legit arrived first it would retire the txn and the poison would be dropped
 * as an unknown id regardless of the src check.) */
UTEST(dns, wrong_source_response_ignored) {
    reset_dns();
    g_answer_a = 1;
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));
    KlUdpServer ns;
    KlResolver *r = make_resolver(&ctx, &ns, 3000, 1);   /* long timeout, 1 attempt: no retransmit in phase 1 */
    ASSERT_TRUE(r != NULL);

    KlUdp spoof;
    KlUdpConfig sp = { .ctx = &ctx, .bind_addr = "127.0.0.1", .bind_port = 0 };
    ASSERT_EQ(0, kl_udp_init(&spoof, &sp));
    g_wrong_source = 1; g_spoof_sock = &spoof;
    /* Same address (127.0.0.1), DIFFERENT port — the port is what makes the spoofer's source not match
     * the configured nameserver, so the mismatch is purely the discriminator dns_ns_index rejects on. */
    ASSERT_TRUE(kl_udp_local_port(&spoof) != kl_udp_server_local_port(&ns));

    ASSERT_TRUE(r->resolve(r, &ctx, "host.test", 8080, on_done, NULL) != NULL);

    /* Phase 1: only the wrong-source poison is ever in flight. Pump until BOTH queries (A+AAAA) have
     * been seen (both legit responses stashed) AND both poisons were SUCCESSFULLY submitted, then a
     * margin so each poison round-trips to the resolver and is dropped. Track submissions separately
     * from stashing: a poison SEND failure (not a source rejection) would otherwise leave the resolver
     * pending and falsely pass. All well under the 3s timeout, so a still-pending resolution here is a
     * source rejection, not a timeout. */
    for (int i = 0; i < 200 && g_poison_fail == 0 && (g_stash_n < 2 || g_poison_sent < 2); i++)
        kl_event_ctx_run(&ctx, 16, 2);
    ASSERT_EQ(0, g_poison_fail);             /* every spoof was actually submitted (no send failure) */
    ASSERT_EQ(2, g_stash_n);                 /* both legit responses (A + AAAA) captured */
    ASSERT_EQ(2, g_poison_sent);             /* both poisons in flight from the wrong source */
    for (int i = 0; i < 20; i++) kl_event_ctx_run(&ctx, 16, 2);   /* poison round-trip + drop */
    ASSERT_EQ(0, g_done);                    /* poison rejected at the src gate — still pending */

    /* Phase 2: release the legit responses from the configured nameserver socket (correct src). */
    for (int i = 0; i < g_stash_n; i++)
        ASSERT_EQ(0, kl_udp_server_reply(&ns, g_stash_resp[i], g_stash_len[i], &g_stash_dest));
    pump(&ctx, &g_done, 200);

    ASSERT_EQ(1, g_done);
    ASSERT_EQ(0, g_err);
    ASSERT_TRUE(g_res.naddrs >= 1);
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, g_res.addrs[0].u.ip, ip, sizeof(ip));
    ASSERT_STREQ("10.1.2.3", ip);            /* the LEGIT answer, released only in phase 2 */

    g_spoof_sock = NULL;
    kl_udp_free(&spoof);
    r->destroy(r);
    kl_udp_server_free(&ns);
    kl_event_ctx_free(&ctx);
}

typedef struct { int done; int err; KlResolveResult res; } DnsCap;
static void on_done_ctx(KlResolveReq *req, const KlResolveResult *result, int error, void *ud) {
    (void)req;
    DnsCap *c = ud;
    c->done++;
    c->err = error;
    if (result) c->res = *result;
}

/* Two DISTINCT names resolved concurrently over the ONE shared resolver socket. Each name's answer is
 * distinguishable (A octet = the name's first char via g_octet_from_name), so verifying each callback
 * gets ONLY its own name's address proves transaction-id demultiplexing across the machine's serial
 * receive re-arms — no cross-matching between the interleaved in-flight legs. */
UTEST(dns, concurrent_distinct_resolutions) {
    reset_dns();
    g_answer_a = 1; g_octet_from_name = 1;
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));
    KlUdpServer ns;
    KlResolver *r = make_resolver(&ctx, &ns, 500, 2);
    ASSERT_TRUE(r != NULL);

    DnsCap ca = {0}, cb = {0};
    ASSERT_TRUE(r->resolve(r, &ctx, "aa.test", 111, on_done_ctx, &ca) != NULL);
    ASSERT_TRUE(r->resolve(r, &ctx, "bb.test", 222, on_done_ctx, &cb) != NULL);

    for (int i = 0; i < 300 && (ca.done == 0 || cb.done == 0); i++)
        kl_event_ctx_run(&ctx, 16, 10);

    ASSERT_EQ(1, ca.done); ASSERT_EQ(0, ca.err);
    ASSERT_EQ(1, cb.done); ASSERT_EQ(0, cb.err);
    ASSERT_TRUE(ca.res.naddrs >= 1); ASSERT_TRUE(cb.res.naddrs >= 1);
    /* Each callback received ONLY its own name's answer (4th octet = first char) and its own port. */
    ASSERT_EQ((int)'a', (int)ca.res.addrs[0].u.ip[3]);
    ASSERT_EQ((int)'b', (int)cb.res.addrs[0].u.ip[3]);
    ASSERT_EQ(111, kl_sockaddr_port(&ca.res.addrs[0]));
    ASSERT_EQ(222, kl_sockaddr_port(&cb.res.addrs[0]));

    r->destroy(r);
    kl_udp_server_free(&ns);
    kl_event_ctx_free(&ctx);
}

UTEST_MAIN();
