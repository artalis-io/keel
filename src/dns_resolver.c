#include <keel/dns_resolver.h>
#include <keel/udp.h>
#include <keel/timer.h>

#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#define DNS_NAME_MAX      256
#define DNS_QUERY_MAX     512   /* classic DNS message cap (no EDNS0) */
#define DNS_RND_POOL_SIZE 256   /* entropy bytes buffered per /dev/urandom read */

/* ── Per-request handle ──────────────────────────────────────────────── */

typedef struct KlDnsReq {
    KlResolveReq      base;        /* MUST be first for upcast */
    struct KlDnsReq  *next;        /* in-flight singly-linked list */
    struct KlDnsResolver *r;
    KlResolveDoneFn   done;
    void             *ud;
    char              host[DNS_NAME_MAX];
    int               port;
    uint16_t          id;          /* current transaction id */
    int               qtype;       /* current query type (A/AAAA) */
    int               tries_left;  /* transmits remaining for this family */
    int               tried_secondary;
    int64_t           timer_id;    /* timeout/deferred timer, -1 if none */
    int               literal;     /* 1 = literal-IP deferred completion */
    KlResolveResult   result;      /* literal result, pending deferral */
    int               in_done;     /* reentrancy guard */
    int               cancelled;   /* cancel() arrived during done */
    uint8_t           question[DNS_NAME_MAX + 8]; /* transmitted question section (qname+qtype+qclass) */
    size_t            question_len;/* length of the stored question */
} KlDnsReq;

/* ── Resolver ────────────────────────────────────────────────────────── */

typedef struct KlDnsResolver {
    KlResolver     base;           /* MUST be first for upcast */
    KlEventCtx    *ctx;
    KlAllocator   *alloc;
    KlUdp          sock;
    int            timeout_ms;
    int            attempts;
    int            prefer_ipv6;
    int            disable_0x20;
    KlDnsReq      *inflight;
    unsigned char  rnd_pool[DNS_RND_POOL_SIZE]; /* pooled OS entropy for IDs + 0x20 */
    size_t         rnd_off;       /* next unused byte in rnd_pool */
} KlDnsResolver;

/* ── Entropy (pooled /dev/urandom — portable across glibc/musl/macOS/cosmo) ── */

static void dns_rand_refill(KlDnsResolver *r) {
    FILE *f = fopen("/dev/urandom", "rb");
    size_t got = 0;
    if (f) {
        got = fread(r->rnd_pool, 1, sizeof(r->rnd_pool), f);
        fclose(f);
    }
    /* Fallback if /dev/urandom is unavailable (effectively never on a real
     * system): the low byte of each element's own address still varies per
     * slot, so we produce *a* non-crashing value without a magic mixer. */
    for (size_t i = got; i < sizeof(r->rnd_pool); i++)
        r->rnd_pool[i] = (unsigned char)(uintptr_t)&r->rnd_pool[i];
    r->rnd_off = 0;
}

static unsigned char dns_rand_byte(KlDnsResolver *r) {
    if (r->rnd_off >= sizeof(r->rnd_pool))
        dns_rand_refill(r);
    return r->rnd_pool[r->rnd_off++];
}

static uint16_t dns_rand_u16(KlDnsResolver *r) {
    uint16_t hi = dns_rand_byte(r);
    return (uint16_t)((hi << 8) | dns_rand_byte(r));
}

/* ── Response parser (public, bounds-safe — fuzzed) ───────────────────── */

/* Advance past a DNS name starting at `off`. Handles compression pointers
 * (a pointer terminates the name in-place for skipping). Returns 0 with the
 * post-name offset in *out, or -1 on any out-of-bounds / malformed input. */
static int dns_skip_name(const uint8_t *pkt, size_t len, size_t off, size_t *out) {
    int labels = 0;
    for (;;) {
        if (off >= len)
            return -1;
        uint8_t b = pkt[off];
        if ((b & 0xC0) == 0xC0) {              /* compression pointer: 2 bytes */
            if (off + 2 > len)
                return -1;
            *out = off + 2;
            return 0;
        }
        if ((b & 0xC0) != 0)                    /* reserved label type */
            return -1;
        if (b == 0) {                           /* root label — name ends */
            *out = off + 1;
            return 0;
        }
        off += (size_t)1 + b;                    /* skip length byte + label */
        if (++labels > 128)                      /* sanity cap */
            return -1;
    }
}

/* Public API (declared in dns_resolver.h): exercised by the unit tests and the
 * fuzz target, though not called elsewhere inside the library. */
int kl_dns_parse_response(const uint8_t *pkt, size_t len, uint16_t expect_id,
                          int want_qtype, const uint8_t *expect_q, size_t expect_q_len,
                          KlResolveResult *out) {
    if (!pkt || !out || len < 12)
        return -1;

    uint16_t id = (uint16_t)((pkt[0] << 8) | pkt[1]);
    if (id != expect_id)
        return -1;
    if (!(pkt[2] & 0x80))                        /* QR must be set (response) */
        return -1;
    if ((pkt[3] & 0x0F) != 0)                    /* rcode != NOERROR */
        return -1;

    uint16_t qd = (uint16_t)((pkt[4] << 8) | pkt[5]);
    uint16_t an = (uint16_t)((pkt[6] << 8) | pkt[7]);

    size_t off = 12;
    for (uint16_t i = 0; i < qd; i++) {          /* skip question section */
        if (dns_skip_name(pkt, len, off, &off) != 0)
            return -1;
        if (off + 4 > len)
            return -1;
        off += 4;                                /* qtype + qclass */
    }

    /* Bind the answer to our query: the question section must echo exactly the
     * bytes we sent (name + type + class, including 0x20 case). */
    if (expect_q) {
        if (off - 12 != expect_q_len || memcmp(pkt + 12, expect_q, expect_q_len) != 0)
            return -1;
    }

    for (uint16_t i = 0; i < an; i++) {          /* walk answers */
        if (dns_skip_name(pkt, len, off, &off) != 0)
            return -1;
        if (off + 10 > len)
            return -1;
        uint16_t rtype = (uint16_t)((pkt[off] << 8) | pkt[off + 1]);
        uint16_t rdlen = (uint16_t)((pkt[off + 8] << 8) | pkt[off + 9]);
        off += 10;
        if (off + rdlen > len)
            return -1;

        if (rtype == want_qtype && want_qtype == KL_DNS_TYPE_A && rdlen == 4) {
            struct sockaddr_in *s4 = (struct sockaddr_in *)&out->addr;
            memset(s4, 0, sizeof(*s4));
            s4->sin_family = AF_INET;
            memcpy(&s4->sin_addr, pkt + off, 4);
            out->addrlen = sizeof(*s4);
            out->ai_family = AF_INET;
            out->ai_socktype = SOCK_STREAM;
            out->ai_protocol = 0;
            return 0;
        }
        if (rtype == want_qtype && want_qtype == KL_DNS_TYPE_AAAA && rdlen == 16) {
            struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)&out->addr;
            memset(s6, 0, sizeof(*s6));
            s6->sin6_family = AF_INET6;
            memcpy(&s6->sin6_addr, pkt + off, 16);
            out->addrlen = sizeof(*s6);
            out->ai_family = AF_INET6;
            out->ai_socktype = SOCK_STREAM;
            out->ai_protocol = 0;
            return 0;
        }
        off += rdlen;
    }
    return -1;
}

/* ── Query builder ────────────────────────────────────────────────────── */

/* Build a query with transaction id, applying DNS 0x20 case randomization to
 * the QNAME letters (unless disabled). Also returns the question-section span
 * [*q_off, off) so the caller can store it for response verification. */
static int dns_build_query(KlDnsResolver *r, uint8_t *buf, size_t cap, uint16_t id,
                           const char *host, int qtype,
                           size_t *out_len, size_t *q_off, size_t *q_len) {
    if (cap < 12)
        return -1;
    memset(buf, 0, 12);
    buf[0] = (uint8_t)(id >> 8);
    buf[1] = (uint8_t)(id & 0xFF);
    buf[2] = 0x01;                 /* RD = recursion desired */
    buf[5] = 0x01;                 /* qdcount = 1 */

    int use_0x20 = !r->disable_0x20;
    size_t off = 12;
    const char *p = host;
    while (*p) {
        const char *dot = strchr(p, '.');
        size_t lab = dot ? (size_t)(dot - p) : strlen(p);
        if (lab == 0 || lab > 63)
            return -1;             /* empty or over-long label */
        if (off + 1 + lab > cap)
            return -1;
        buf[off++] = (uint8_t)lab;
        for (size_t k = 0; k < lab; k++) {
            unsigned char c = (unsigned char)p[k];
            unsigned char lower = (unsigned char)(c | 0x20);
            if (use_0x20 && lower >= 'a' && lower <= 'z')
                c = (dns_rand_byte(r) & 1) ? lower : (unsigned char)(c & (unsigned char)~0x20);
            buf[off + k] = c;
        }
        off += lab;
        if (!dot)
            break;
        p = dot + 1;
    }
    if (off + 5 > cap)             /* root label + qtype + qclass */
        return -1;
    buf[off++] = 0;                              /* root label */
    buf[off++] = (uint8_t)(qtype >> 8);
    buf[off++] = (uint8_t)(qtype & 0xFF);
    buf[off++] = 0x00;
    buf[off++] = 0x01;                           /* QCLASS = IN */
    *out_len = off;
    *q_off = 12;
    *q_len = off - 12;
    return 0;
}

/* ── In-flight list helpers ──────────────────────────────────────────── */

static void dns_unlink(KlDnsResolver *r, KlDnsReq *q) {
    KlDnsReq **pp = &r->inflight;
    while (*pp) {
        if (*pp == q) { *pp = q->next; q->next = NULL; return; }
        pp = &(*pp)->next;
    }
}

static KlDnsReq *dns_find_by_id(KlDnsResolver *r, uint16_t id) {
    for (KlDnsReq *q = r->inflight; q; q = q->next)
        if (!q->literal && q->id == id)
            return q;
    return NULL;
}

/* ── Completion ──────────────────────────────────────────────────────── */

static void dns_complete(KlDnsResolver *r, KlDnsReq *q,
                         const KlResolveResult *result, int error) {
    if (q->timer_id >= 0) {
        kl_timer_cancel(r->ctx, q->timer_id);
        q->timer_id = -1;
    }
    dns_unlink(r, q);

    q->in_done = 1;
    q->done(&q->base, error ? NULL : result, error, q->ud);
    /* The consumer drops its reference inside done(); free unconditionally.
     * (cancelled is honoured only to avoid a double-free if cancel() were
     * called re-entrantly during done — the free still happens exactly here.) */
    kl_free(r->alloc, q, sizeof(*q));
}

static void dns_set_port(KlResolveResult *res, int port) {
    if (res->ai_family == AF_INET)
        ((struct sockaddr_in *)&res->addr)->sin_port = htons((uint16_t)port);
    else if (res->ai_family == AF_INET6)
        ((struct sockaddr_in6 *)&res->addr)->sin6_port = htons((uint16_t)port);
}

/* ── Transmit + timeout ──────────────────────────────────────────────── */

static void dns_on_timer(void *ud);

/* 1 if some other in-flight request already carries this id. */
static int dns_id_in_use(const KlDnsResolver *r, uint16_t id, const KlDnsReq *self) {
    for (const KlDnsReq *q = r->inflight; q; q = q->next)
        if (q != self && !q->literal && q->id == id)
            return 1;
    return 0;
}

static int dns_transmit(KlDnsResolver *r, KlDnsReq *q) {
    if (q->tries_left <= 0)
        return -1;
    q->tries_left--;

    /* Randomized transaction id (redraw on the rare in-flight collision). */
    for (int tries = 0; tries < 8; tries++) {
        q->id = dns_rand_u16(r);
        if (!dns_id_in_use(r, q->id, q))
            break;
    }

    uint8_t buf[DNS_QUERY_MAX];
    size_t qlen = 0, q_off = 0, q_len = 0;
    if (dns_build_query(r, buf, sizeof(buf), q->id, q->host, q->qtype,
                        &qlen, &q_off, &q_len) != 0)
        return -1;
    /* Stash the exact question bytes to verify the response echoes them. */
    if (q_len > sizeof(q->question))
        return -1;
    memcpy(q->question, buf + q_off, q_len);
    q->question_len = q_len;
    if (kl_udp_send(&r->sock, buf, qlen) != 0)
        return -1;

    if (q->timer_id >= 0)
        kl_timer_cancel(r->ctx, q->timer_id);
    q->timer_id = kl_timer_add(r->ctx, (uint64_t)r->timeout_ms, dns_on_timer, q);
    return (q->timer_id >= 0) ? 0 : -1;
}

/* Advance to the other address family, or fail if both are exhausted. */
static void dns_advance_or_fail(KlDnsResolver *r, KlDnsReq *q) {
    if (!q->tried_secondary) {
        q->tried_secondary = 1;
        q->qtype = (q->qtype == KL_DNS_TYPE_A) ? KL_DNS_TYPE_AAAA : KL_DNS_TYPE_A;
        q->tries_left = r->attempts;
        if (dns_transmit(r, q) == 0)
            return;
    }
    dns_complete(r, q, NULL, KL_ERR_DNS);
}

static void dns_on_timer(void *ud) {
    KlDnsReq *q = ud;
    KlDnsResolver *r = q->r;
    q->timer_id = -1;

    if (q->literal) {                            /* deferred literal-IP success */
        dns_complete(r, q, &q->result, 0);
        return;
    }
    if (q->tries_left > 0) {                      /* retransmit same family */
        if (dns_transmit(r, q) != 0)
            dns_complete(r, q, NULL, KL_ERR_DNS);
        return;
    }
    dns_advance_or_fail(r, q);                     /* try the other family / fail */
}

/* ── Receive ─────────────────────────────────────────────────────────── */

/* Does the response's first question echo exactly the bytes we sent
 * (name + type + class, including 0x20 case)? Bounds-safe. */
static int dns_question_matches(const uint8_t *pkt, size_t len,
                                const uint8_t *eq, size_t eq_len) {
    if (len < 12)
        return 0;
    uint16_t qd = (uint16_t)((pkt[4] << 8) | pkt[5]);
    if (qd < 1)
        return 0;
    size_t off = 12;
    if (dns_skip_name(pkt, len, off, &off) != 0)
        return 0;
    if (off + 4 > len)
        return 0;
    off += 4;                                    /* qtype + qclass */
    return (off - 12 == eq_len) && (memcmp(pkt + 12, eq, eq_len) == 0);
}

static void dns_on_recv(KlUdp *u, const void *data, size_t len,
                        const struct sockaddr *src, socklen_t src_len,
                        const struct sockaddr *local, socklen_t local_len, void *ud) {
    (void)u; (void)src; (void)src_len; (void)local; (void)local_len;
    KlDnsResolver *r = ud;
    if (len < 12)
        return;
    const uint8_t *pkt = data;
    uint16_t id = (uint16_t)((pkt[0] << 8) | pkt[1]);
    KlDnsReq *q = dns_find_by_id(r, id);
    if (!q)
        return;                                  /* stale or unknown id */

    KlResolveResult res;
    memset(&res, 0, sizeof(res));
    if (kl_dns_parse_response(pkt, len, id, q->qtype,
                              q->question, q->question_len, &res) == 0) {
        dns_set_port(&res, q->port);
        dns_complete(r, q, &res, 0);
        return;
    }
    /* Parse failed: only trust rcode / advance if the question is genuinely
     * ours — otherwise ignore (spoofed or unrelated) and keep waiting. */
    if (!dns_question_matches(pkt, len, q->question, q->question_len))
        return;
    if ((pkt[3] & 0x0F) == 3) {                   /* NXDOMAIN — definitive */
        dns_complete(r, q, NULL, KL_ERR_DNS);
        return;
    }
    dns_advance_or_fail(r, q);                     /* no record of this family */
}

/* ── Literal-IP shortcut ─────────────────────────────────────────────── */

static int dns_is_literal(const char *host, int port, KlResolveResult *out) {
    memset(out, 0, sizeof(*out));
    struct in_addr a4;
    struct in6_addr a6;
    if (inet_pton(AF_INET, host, &a4) == 1) {
        struct sockaddr_in *s4 = (struct sockaddr_in *)&out->addr;
        s4->sin_family = AF_INET;
        s4->sin_addr = a4;
        s4->sin_port = htons((uint16_t)port);
        out->addrlen = sizeof(*s4);
        out->ai_family = AF_INET;
        out->ai_socktype = SOCK_STREAM;
        return 1;
    }
    if (inet_pton(AF_INET6, host, &a6) == 1) {
        struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)&out->addr;
        s6->sin6_family = AF_INET6;
        s6->sin6_addr = a6;
        s6->sin6_port = htons((uint16_t)port);
        out->addrlen = sizeof(*s6);
        out->ai_family = AF_INET6;
        out->ai_socktype = SOCK_STREAM;
        return 1;
    }
    return 0;
}

/* ASCII case-insensitive equality (letters only; targets are all-lowercase). */
static int dns_ci_eq(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (((unsigned char)*a | 0x20) != ((unsigned char)*b | 0x20))
            return 0;
    return *a == *b;
}

/* "localhost" → loopback, since it's usually served by /etc/hosts, not DNS.
 * A minimal safety net for the opt-out built-in-DNS default (full /etc/hosts
 * is a roadmap item). */
static int dns_is_localhost(const char *host, int port, int prefer_ipv6,
                            KlResolveResult *out) {
    if (!dns_ci_eq(host, "localhost"))
        return 0;
    return dns_is_literal(prefer_ipv6 ? "::1" : "127.0.0.1", port, out);
}

/* ── Vtable ──────────────────────────────────────────────────────────── */

static KlResolveReq *dns_resolve(KlResolver *self, KlEventCtx *ctx,
                                 const char *host, int port,
                                 KlResolveDoneFn done, void *user_data) {
    KlDnsResolver *r = (KlDnsResolver *)self;
    (void)ctx;
    if (!host || !done)
        return NULL;

    KlDnsReq *q = kl_malloc(r->alloc, sizeof(*q));
    if (!q)
        return NULL;
    memset(q, 0, sizeof(*q));
    q->base.resolver = self;
    q->r = r;
    q->done = done;
    q->ud = user_data;
    q->port = port;
    q->timer_id = -1;
    snprintf(q->host, sizeof(q->host), "%s", host);

    /* Link before scheduling so a synchronous timer/send callback can find it. */
    q->next = r->inflight;
    r->inflight = q;

    if (dns_is_literal(host, port, &q->result) ||
        dns_is_localhost(host, port, r->prefer_ipv6, &q->result)) {
        q->literal = 1;
        q->timer_id = kl_timer_add(r->ctx, 0, dns_on_timer, q);
        if (q->timer_id < 0) {
            dns_unlink(r, q);
            kl_free(r->alloc, q, sizeof(*q));
            return NULL;
        }
        return &q->base;
    }

    q->qtype = r->prefer_ipv6 ? KL_DNS_TYPE_AAAA : KL_DNS_TYPE_A;
    q->tries_left = r->attempts;
    if (dns_transmit(r, q) != 0) {
        if (q->timer_id >= 0)
            kl_timer_cancel(r->ctx, q->timer_id);
        dns_unlink(r, q);
        kl_free(r->alloc, q, sizeof(*q));
        return NULL;
    }
    return &q->base;
}

static void dns_cancel(KlResolveReq *req) {
    KlDnsReq *q = (KlDnsReq *)req;
    if (!q)
        return;
    KlDnsResolver *r = q->r;
    if (q->in_done) {           /* re-entrant cancel during done() — defer free */
        q->cancelled = 1;
        return;
    }
    if (q->timer_id >= 0) {
        kl_timer_cancel(r->ctx, q->timer_id);
        q->timer_id = -1;
    }
    dns_unlink(r, q);
    kl_free(r->alloc, q, sizeof(*q));
}

static void dns_destroy(KlResolver *self) {
    KlDnsResolver *r = (KlDnsResolver *)self;
    if (!r)
        return;
    KlDnsReq *q = r->inflight;
    while (q) {
        KlDnsReq *next = q->next;
        if (q->timer_id >= 0)
            kl_timer_cancel(r->ctx, q->timer_id);
        kl_free(r->alloc, q, sizeof(*q));
        q = next;
    }
    r->inflight = NULL;
    kl_udp_free(&r->sock);
    kl_free(r->alloc, r, sizeof(*r));
}

/* ── Nameserver selection ────────────────────────────────────────────── */

static int resolv_conf_nameserver(char *out, size_t cap) {
    FILE *f = fopen("/etc/resolv.conf", "r");
    if (!f)
        return -1;
    char line[256];
    int found = -1;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "nameserver", 10) != 0)
            continue;
        const char *p = line + 10;
        if (*p != ' ' && *p != '\t')
            continue;
        while (*p == ' ' || *p == '\t')
            p++;
        size_t i = 0;
        while (p[i] && p[i] != ' ' && p[i] != '\t' && p[i] != '\n' &&
               p[i] != '\r' && i + 1 < cap) {
            out[i] = p[i];
            i++;
        }
        out[i] = '\0';
        if (i > 0) { found = 0; break; }
    }
    fclose(f);
    return found;
}

static int dns_nameserver_addr(const KlDnsResolverConfig *cfg,
                               struct sockaddr_storage *out, socklen_t *out_len,
                               int *family) {
    char nsbuf[64];
    const char *ns = cfg ? cfg->nameserver : NULL;
    if (!ns) {
        if (resolv_conf_nameserver(nsbuf, sizeof(nsbuf)) == 0)
            ns = nsbuf;
        else
            ns = "127.0.0.1";
    }
    uint16_t port = (cfg && cfg->port) ? cfg->port : 53;

    memset(out, 0, sizeof(*out));
    struct in_addr a4;
    struct in6_addr a6;
    if (inet_pton(AF_INET, ns, &a4) == 1) {
        struct sockaddr_in *s4 = (struct sockaddr_in *)out;
        s4->sin_family = AF_INET;
        s4->sin_addr = a4;
        s4->sin_port = htons(port);
        *out_len = sizeof(*s4);
        *family = AF_INET;
        return 0;
    }
    if (inet_pton(AF_INET6, ns, &a6) == 1) {
        struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)out;
        s6->sin6_family = AF_INET6;
        s6->sin6_addr = a6;
        s6->sin6_port = htons(port);
        *out_len = sizeof(*s6);
        *family = AF_INET6;
        return 0;
    }
    return -1;
}

/* ── Constructor ─────────────────────────────────────────────────────── */

KlResolver *kl_dns_resolver_create(KlEventCtx *ctx, const KlDnsResolverConfig *cfg) {
    if (!ctx)
        return NULL;
    KlAllocator *alloc = (cfg && cfg->alloc) ? cfg->alloc : ctx->alloc;
    if (!alloc)
        return NULL;

    struct sockaddr_storage ns;
    socklen_t ns_len = 0;
    int family = AF_INET;
    if (dns_nameserver_addr(cfg, &ns, &ns_len, &family) != 0)
        return NULL;

    KlDnsResolver *r = kl_malloc(alloc, sizeof(*r));
    if (!r)
        return NULL;
    memset(r, 0, sizeof(*r));
    r->base.resolve = dns_resolve;
    r->base.cancel  = dns_cancel;
    r->base.destroy = dns_destroy;
    r->ctx = ctx;
    r->alloc = alloc;
    r->timeout_ms = (cfg && cfg->timeout_ms) ? cfg->timeout_ms : 5000;
    r->attempts = (cfg && cfg->attempts) ? cfg->attempts : 2;
    r->prefer_ipv6 = cfg ? cfg->prefer_ipv6 : 0;
    r->disable_0x20 = cfg ? cfg->disable_0x20 : 0;
    r->rnd_off = sizeof(r->rnd_pool);   /* pool empty → refills on first draw */

    KlUdpConfig uc = { .ctx = ctx, .family = family, .alloc = alloc };
    if (kl_udp_init(&r->sock, &uc) != 0) {
        kl_free(alloc, r, sizeof(*r));
        return NULL;
    }
    if (kl_udp_connect(&r->sock, (struct sockaddr *)&ns, ns_len) != 0 ||
        kl_udp_recv_start(&r->sock, dns_on_recv, r) != 0) {
        kl_udp_free(&r->sock);
        kl_free(alloc, r, sizeof(*r));
        return NULL;
    }
    return &r->base;
}
