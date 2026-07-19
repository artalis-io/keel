#include <keel/dns_resolver.h>
#include <keel/udp.h>
#include <keel/timer.h>

#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#define DNS_NAME_MAX      256
#define DNS_QUERY_MAX     512   /* query buffer (question + EDNS0 OPT fit easily) */
#define DNS_RND_POOL_SIZE 256   /* entropy bytes buffered per /dev/urandom read */
#define DNS_TYPE_OPT      41    /* EDNS0 OPT pseudo-record type */
#define DNS_EDNS_UDP_SIZE 1232  /* advertised UDP payload size (DNS flag day) */
#define DNS_OPT_RR_LEN    11    /* wire length of a bare OPT RR */
#define DNS_MAX_NS        3     /* nameservers tried (resolv.conf MAXNS) */
#define DNS_PORT          53    /* default nameserver port */
#define DNS_MAX_SEARCH    6     /* search domains (resolv.conf MAXDNSRCH) */
#define DNS_MAX_CAND      8     /* candidate names per resolve (search + bare) */

/* ── Per-request handle ──────────────────────────────────────────────── */

/* One address-family query "leg" (A or AAAA), run concurrently with the other
 * for the current candidate name. Each leg carries its own transaction id,
 * retry/nameserver-rotation state, timeout timer, question bytes, and the
 * addresses it collected. */
typedef struct {
    struct KlDnsReq *req;          /* owning request (timer back-pointer) */
    int              qtype;        /* KL_DNS_TYPE_A or AAAA */
    uint16_t         id;           /* current transaction id */
    int              tries_left;   /* transmits remaining for this leg */
    int              ns_idx;       /* nameserver rotation cursor */
    int64_t          timer_id;     /* per-leg timeout, -1 if none */
    int              done;         /* 1 = settled (answered / empty / exhausted) */
    uint8_t          question[DNS_NAME_MAX + 8]; /* transmitted question (name+type+class) */
    size_t           question_len;
    struct sockaddr_storage addrs[KL_RESOLVE_MAX_ADDRS]; /* collected addresses */
    socklen_t        addrlens[KL_RESOLVE_MAX_ADDRS];
    int              naddrs;
} KlDnsLeg;

typedef struct KlDnsReq {
    KlResolveReq      base;        /* MUST be first for upcast */
    struct KlDnsReq  *next;        /* in-flight singly-linked list */
    struct KlDnsResolver *r;
    KlResolveDoneFn   done;
    void             *ud;
    char              host[DNS_NAME_MAX];        /* current candidate being queried */
    char              cand[DNS_MAX_CAND][DNS_NAME_MAX]; /* candidate names (search + bare) */
    int               ncand;                     /* number of candidates */
    int               ci;                        /* current candidate index */
    int               port;
    KlDnsLeg          legs[2];     /* [0] = preferred family, [1] = other */
    int64_t           rdelay_timer;/* RFC 8305 §3 resolution-delay timer, -1 if none */
    int               rdelay_armed;/* 1 = resolution delay is running */
    int64_t           timer_id;    /* literal-IP deferred-completion timer, -1 if none */
    int               literal;     /* 1 = literal-IP / hosts deferred completion */
    KlResolveResult   result;      /* literal result / final merged result */
    int               in_done;     /* reentrancy guard */
    int               cancelled;   /* cancel() arrived during done */
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
    int            disable_edns;
    char           hosts_path[DNS_NAME_MAX];
    struct sockaddr_storage ns[DNS_MAX_NS];   /* nameserver addresses */
    socklen_t      ns_len[DNS_MAX_NS];
    int            nns;                        /* number of nameservers */
    char           search[DNS_MAX_SEARCH][DNS_NAME_MAX]; /* search domains */
    int            nsearch;
    int            ndots;                     /* threshold for search expansion */
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

    out->naddrs = 0;
    out->ai_socktype = SOCK_STREAM;
    out->ai_protocol = 0;

    for (uint16_t i = 0; i < an && out->naddrs < KL_RESOLVE_MAX_ADDRS; i++) { /* walk answers */
        if (dns_skip_name(pkt, len, off, &off) != 0)
            return out->naddrs > 0 ? 0 : -1;
        if (off + 10 > len)
            return out->naddrs > 0 ? 0 : -1;
        uint16_t rtype = (uint16_t)((pkt[off] << 8) | pkt[off + 1]);
        uint16_t rdlen = (uint16_t)((pkt[off + 8] << 8) | pkt[off + 9]);
        off += 10;
        if (off + rdlen > len)
            return out->naddrs > 0 ? 0 : -1;

        if (rtype == want_qtype && want_qtype == KL_DNS_TYPE_A && rdlen == 4) {
            struct sockaddr_in *s4 = (struct sockaddr_in *)&out->addrs[out->naddrs];
            memset(s4, 0, sizeof(*s4));
            s4->sin_family = AF_INET;
            memcpy(&s4->sin_addr, pkt + off, 4);
            out->addrlens[out->naddrs] = sizeof(*s4);
            out->naddrs++;
        } else if (rtype == want_qtype && want_qtype == KL_DNS_TYPE_AAAA && rdlen == 16) {
            struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)&out->addrs[out->naddrs];
            memset(s6, 0, sizeof(*s6));
            s6->sin6_family = AF_INET6;
            memcpy(&s6->sin6_addr, pkt + off, 16);
            out->addrlens[out->naddrs] = sizeof(*s6);
            out->naddrs++;
        }
        off += rdlen;
    }
    return out->naddrs > 0 ? 0 : -1;
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
    *q_off = 12;
    *q_len = off - 12;                           /* question section (excludes EDNS0) */

    /* EDNS0: advertise a larger UDP payload via a bare OPT RR in the additional
     * section (arcount = 1). Backward-compatible; a non-EDNS server ignores it. */
    if (!r->disable_edns) {
        if (off + DNS_OPT_RR_LEN > cap)
            return -1;
        buf[11] = 0x01;                          /* arcount = 1 */
        buf[off++] = 0x00;                       /* OPT owner name = root */
        buf[off++] = (uint8_t)(DNS_TYPE_OPT >> 8);
        buf[off++] = (uint8_t)(DNS_TYPE_OPT & 0xFF);
        buf[off++] = (uint8_t)(DNS_EDNS_UDP_SIZE >> 8);
        buf[off++] = (uint8_t)(DNS_EDNS_UDP_SIZE & 0xFF);
        buf[off++] = 0x00; buf[off++] = 0x00;    /* extended rcode + version */
        buf[off++] = 0x00; buf[off++] = 0x00;    /* flags */
        buf[off++] = 0x00; buf[off++] = 0x00;    /* rdlen = 0 */
    }
    *out_len = off;
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

/* Find the in-flight leg carrying transaction id (across all requests). */
static KlDnsLeg *dns_find_leg(KlDnsResolver *r, uint16_t id) {
    for (KlDnsReq *q = r->inflight; q; q = q->next) {
        if (q->literal)
            continue;
        for (int i = 0; i < 2; i++)
            if (!q->legs[i].done && q->legs[i].id == id)
                return &q->legs[i];
    }
    return NULL;
}

/* ── Completion ──────────────────────────────────────────────────────── */

/* Cancel every timer a request may own (literal, both legs, resolution delay). */
static void dns_cancel_timers(KlDnsResolver *r, KlDnsReq *q) {
    if (q->timer_id >= 0)      { kl_timer_cancel(r->ctx, q->timer_id);      q->timer_id = -1; }
    if (q->rdelay_timer >= 0)  { kl_timer_cancel(r->ctx, q->rdelay_timer);  q->rdelay_timer = -1; }
    for (int i = 0; i < 2; i++)
        if (q->legs[i].timer_id >= 0) {
            kl_timer_cancel(r->ctx, q->legs[i].timer_id);
            q->legs[i].timer_id = -1;
        }
}

static void dns_complete(KlDnsResolver *r, KlDnsReq *q,
                         const KlResolveResult *result, int error) {
    dns_cancel_timers(r, q);
    dns_unlink(r, q);

    q->in_done = 1;
    q->done(&q->base, error ? NULL : result, error, q->ud);
    /* The consumer drops its reference inside done(); free unconditionally.
     * (cancelled is honoured only to avoid a double-free if cancel() were
     * called re-entrantly during done — the free still happens exactly here.) */
    kl_free(r->alloc, q, sizeof(*q));
}

static void dns_set_port(KlResolveResult *res, int port) {
    for (int i = 0; i < res->naddrs; i++) {
        if (res->addrs[i].ss_family == AF_INET)
            ((struct sockaddr_in *)&res->addrs[i])->sin_port = htons((uint16_t)port);
        else if (res->addrs[i].ss_family == AF_INET6)
            ((struct sockaddr_in6 *)&res->addrs[i])->sin6_port = htons((uint16_t)port);
    }
}

/* ── Two-leg (dual-family) state machine ─────────────────────────────── */

#define DNS_RESOLUTION_DELAY_MS 50   /* RFC 8305 §3: wait for the 2nd family */

static void dns_on_leg_timer(void *ud);
static void dns_on_rdelay(void *ud);
static void dns_on_literal(void *ud);
static void dns_advance_candidate(KlDnsResolver *r, KlDnsReq *q);

/* 1 if some other in-flight leg already carries this id. */
static int dns_id_in_use(const KlDnsResolver *r, uint16_t id, const KlDnsLeg *self) {
    for (const KlDnsReq *q = r->inflight; q; q = q->next) {
        if (q->literal)
            continue;
        for (int i = 0; i < 2; i++)
            if (&q->legs[i] != self && !q->legs[i].done && q->legs[i].id == id)
                return 1;
    }
    return 0;
}

/* Transmit one leg's query (rotating nameserver); schedules its timeout. */
static int dns_transmit_leg(KlDnsResolver *r, const KlDnsReq *q, KlDnsLeg *leg) {
    if (leg->tries_left <= 0)
        return -1;
    leg->tries_left--;

    for (int tries = 0; tries < 8; tries++) {
        leg->id = dns_rand_u16(r);
        if (!dns_id_in_use(r, leg->id, leg))
            break;
    }

    uint8_t buf[DNS_QUERY_MAX];
    size_t qlen = 0, q_off = 0, q_len = 0;
    if (dns_build_query(r, buf, sizeof(buf), leg->id, q->host, leg->qtype,
                        &qlen, &q_off, &q_len) != 0)
        return -1;
    if (q_len > sizeof(leg->question))
        return -1;
    memcpy(leg->question, buf + q_off, q_len);
    leg->question_len = q_len;

    const struct sockaddr *nsa = (const struct sockaddr *)&r->ns[leg->ns_idx];
    socklen_t nsl = r->ns_len[leg->ns_idx];
    leg->ns_idx = (leg->ns_idx + 1) % r->nns;
    if (kl_udp_send_to(&r->sock, buf, qlen, nsa, nsl) != 0)
        return -1;

    if (leg->timer_id >= 0)
        kl_timer_cancel(r->ctx, leg->timer_id);
    leg->timer_id = kl_timer_add(r->ctx, (uint64_t)r->timeout_ms, dns_on_leg_timer, leg);
    return (leg->timer_id >= 0) ? 0 : -1;
}

/* Merge both legs' addresses, interleaved preferred-first (RFC 8305 §4). */
static void dns_finalize(KlDnsResolver *r, KlDnsReq *q) {
    KlResolveResult *res = &q->result;
    memset(res, 0, sizeof(*res));
    res->ai_socktype = SOCK_STREAM;
    res->ai_protocol = 0;
    int i0 = 0, i1 = 0;                          /* legs[0] = preferred */
    while ((i0 < q->legs[0].naddrs || i1 < q->legs[1].naddrs) &&
           res->naddrs < KL_RESOLVE_MAX_ADDRS) {
        if (i0 < q->legs[0].naddrs) {
            res->addrs[res->naddrs]   = q->legs[0].addrs[i0];
            res->addrlens[res->naddrs] = q->legs[0].addrlens[i0];
            res->naddrs++; i0++;
        }
        if (res->naddrs < KL_RESOLVE_MAX_ADDRS && i1 < q->legs[1].naddrs) {
            res->addrs[res->naddrs]   = q->legs[1].addrs[i1];
            res->addrlens[res->naddrs] = q->legs[1].addrlens[i1];
            res->naddrs++; i1++;
        }
    }
    dns_set_port(res, q->port);
    dns_complete(r, q, res, 0);
}

/* Called after a leg settles or the resolution delay fires: decide whether to
 * complete, wait for the other leg, or advance to the next candidate. */
static void dns_check_complete(KlDnsResolver *r, KlDnsReq *q) {
    int both_done = q->legs[0].done && q->legs[1].done;
    int have_addr = (q->legs[0].naddrs + q->legs[1].naddrs) > 0;

    if (both_done) {
        if (have_addr)
            dns_finalize(r, q);
        else
            dns_advance_candidate(r, q);           /* both families empty → next name */
        return;
    }
    /* One leg still outstanding. If we already have addresses, run the
     * resolution-delay timer so a slow/absent family can't stall us. */
    if (have_addr && !q->rdelay_armed) {
        q->rdelay_armed = 1;
        q->rdelay_timer = kl_timer_add(r->ctx, DNS_RESOLUTION_DELAY_MS,
                                       dns_on_rdelay, q);
    }
}

/* Mark a leg settled (cancel its timeout) and re-evaluate completion. */
static void dns_leg_settle(KlDnsResolver *r, KlDnsReq *q, KlDnsLeg *leg) {
    if (leg->timer_id >= 0) {
        kl_timer_cancel(r->ctx, leg->timer_id);
        leg->timer_id = -1;
    }
    leg->done = 1;
    dns_check_complete(r, q);
}

/* Fire both legs (A + AAAA) for the current candidate name. */
static int dns_start_candidate(KlDnsResolver *r, KlDnsReq *q) {
    if (q->rdelay_timer >= 0) {
        kl_timer_cancel(r->ctx, q->rdelay_timer);
        q->rdelay_timer = -1;
    }
    q->rdelay_armed = 0;

    q->legs[0].qtype = r->prefer_ipv6 ? KL_DNS_TYPE_AAAA : KL_DNS_TYPE_A;
    q->legs[1].qtype = r->prefer_ipv6 ? KL_DNS_TYPE_A : KL_DNS_TYPE_AAAA;
    int any = 0;
    for (int i = 0; i < 2; i++) {
        KlDnsLeg *leg = &q->legs[i];
        leg->req = q;
        leg->tries_left = r->attempts * r->nns;
        leg->ns_idx = 0;
        leg->timer_id = -1;
        leg->done = 0;
        leg->naddrs = 0;
        if (dns_transmit_leg(r, q, leg) == 0)
            any = 1;
        else
            leg->done = 1;                          /* couldn't send → treat as empty */
    }
    return any ? 0 : -1;                            /* -1 = neither leg could transmit */
}

/* Move to the next candidate name (search expansion) or fail. */
static void dns_advance_candidate(KlDnsResolver *r, KlDnsReq *q) {
    q->ci++;
    if (q->ci < q->ncand) {
        memcpy(q->host, q->cand[q->ci], DNS_NAME_MAX);
        if (dns_start_candidate(r, q) == 0)
            return;
    }
    dns_complete(r, q, NULL, KL_ERR_DNS);
}

static void dns_on_leg_timer(void *ud) {
    KlDnsLeg *leg = ud;
    KlDnsReq *q = leg->req;
    KlDnsResolver *r = q->r;
    leg->timer_id = -1;

    if (leg->tries_left > 0) {                      /* retransmit (rotates nameserver) */
        if (dns_transmit_leg(r, q, leg) == 0)
            return;
    }
    dns_leg_settle(r, q, leg);                       /* exhausted → empty */
}

static void dns_on_rdelay(void *ud) {
    KlDnsReq *q = ud;
    q->rdelay_timer = -1;
    /* Armed only after ≥1 address was collected, so finalize with what we have. */
    dns_finalize(q->r, q);
}

static void dns_on_literal(void *ud) {
    KlDnsReq *q = ud;
    q->timer_id = -1;
    dns_complete(q->r, q, &q->result, 0);            /* deferred literal / hosts success */
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

/* Is the response from one of our configured nameservers? (The socket is
 * unconnected for multi-NS, so this replaces the kernel peer filter.) */
static int dns_src_is_ns(const KlDnsResolver *r, const struct sockaddr *src) {
    for (int i = 0; i < r->nns; i++) {
        const struct sockaddr *ns = (const struct sockaddr *)&r->ns[i];
        if (src->sa_family != ns->sa_family)
            continue;
        if (src->sa_family == AF_INET) {
            const struct sockaddr_in *a = (const struct sockaddr_in *)src;
            const struct sockaddr_in *b = (const struct sockaddr_in *)ns;
            if (a->sin_port == b->sin_port &&
                a->sin_addr.s_addr == b->sin_addr.s_addr)
                return 1;
        } else if (src->sa_family == AF_INET6) {
            const struct sockaddr_in6 *a = (const struct sockaddr_in6 *)src;
            const struct sockaddr_in6 *b = (const struct sockaddr_in6 *)ns;
            if (a->sin6_port == b->sin6_port &&
                memcmp(&a->sin6_addr, &b->sin6_addr, sizeof(a->sin6_addr)) == 0)
                return 1;
        }
    }
    return 0;
}

static void dns_on_recv(KlUdp *u, const void *data, size_t len,
                        const struct sockaddr *src, socklen_t src_len,
                        const struct sockaddr *local, socklen_t local_len, void *ud) {
    (void)u; (void)src_len; (void)local; (void)local_len;
    KlDnsResolver *r = ud;
    if (len < 12)
        return;
    if (!src || !dns_src_is_ns(r, src))
        return;                                  /* not from a nameserver — ignore (anti-spoof) */
    const uint8_t *pkt = data;
    uint16_t id = (uint16_t)((pkt[0] << 8) | pkt[1]);
    KlDnsLeg *leg = dns_find_leg(r, id);
    if (!leg)
        return;                                  /* stale or unknown id */
    KlDnsReq *q = leg->req;

    KlResolveResult scratch;
    memset(&scratch, 0, sizeof(scratch));
    if (kl_dns_parse_response(pkt, len, id, leg->qtype,
                              leg->question, leg->question_len, &scratch) == 0) {
        /* Collect this leg's addresses; arm the resolution delay on first data. */
        leg->naddrs = scratch.naddrs;
        for (int i = 0; i < scratch.naddrs; i++) {
            leg->addrs[i] = scratch.addrs[i];
            leg->addrlens[i] = scratch.addrlens[i];
        }
        dns_leg_settle(r, q, leg);
        return;
    }
    /* Parse failed: only act if the question is genuinely ours (else ignore the
     * spoof and keep waiting). NXDOMAIN and no-record both settle the leg empty
     * — the other family runs concurrently, so there's no in-family fallback. */
    if (!dns_question_matches(pkt, len, leg->question, leg->question_len))
        return;
    leg->naddrs = 0;
    dns_leg_settle(r, q, leg);
}

/* ── Literal-IP shortcut ─────────────────────────────────────────────── */

static int dns_is_literal(const char *host, int port, KlResolveResult *out) {
    memset(out, 0, sizeof(*out));
    struct in_addr a4;
    struct in6_addr a6;
    if (inet_pton(AF_INET, host, &a4) == 1) {
        struct sockaddr_in *s4 = (struct sockaddr_in *)&out->addrs[0];
        s4->sin_family = AF_INET;
        s4->sin_addr = a4;
        s4->sin_port = htons((uint16_t)port);
        out->addrlens[0] = sizeof(*s4);
        out->naddrs = 1;
        out->ai_socktype = SOCK_STREAM;
        return 1;
    }
    if (inet_pton(AF_INET6, host, &a6) == 1) {
        struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)&out->addrs[0];
        s6->sin6_family = AF_INET6;
        s6->sin6_addr = a6;
        s6->sin6_port = htons((uint16_t)port);
        out->addrlens[0] = sizeof(*s6);
        out->naddrs = 1;
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

/* Look up `host` in a hosts file (default /etc/hosts), honoring prefer_ipv6.
 * Fills a v4 and/or v6 address from matching lines and returns 1 if found. */
static int dns_hosts_lookup(const char *path, const char *host, int port,
                            int prefer_ipv6, KlResolveResult *out) {
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;

    char line[512];
    int have_v4 = 0, have_v6 = 0;
    struct in_addr a4;
    struct in6_addr a6;
    while (fgets(line, sizeof(line), f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';                 /* strip comment */

        char *save = NULL;
        const char *addr = strtok_r(line, " \t\r\n", &save);
        if (!addr)
            continue;
        int match = 0;
        const char *name;
        while ((name = strtok_r(NULL, " \t\r\n", &save)) != NULL) {
            if (dns_ci_eq(name, host)) { match = 1; break; }
        }
        if (!match)
            continue;

        if (!have_v4 && inet_pton(AF_INET, addr, &a4) == 1)
            have_v4 = 1;
        else if (!have_v6 && inet_pton(AF_INET6, addr, &a6) == 1)
            have_v6 = 1;
        if (have_v4 && have_v6)
            break;
    }
    fclose(f);

    if (!have_v4 && !have_v6)
        return 0;

    memset(out, 0, sizeof(*out));
    if ((prefer_ipv6 && have_v6) || (!have_v4 && have_v6)) {
        struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)&out->addrs[0];
        s6->sin6_family = AF_INET6;
        s6->sin6_addr = a6;
        s6->sin6_port = htons((uint16_t)port);
        out->addrlens[0] = sizeof(*s6);
    } else {
        struct sockaddr_in *s4 = (struct sockaddr_in *)&out->addrs[0];
        s4->sin_family = AF_INET;
        s4->sin_addr = a4;
        s4->sin_port = htons((uint16_t)port);
        out->addrlens[0] = sizeof(*s4);
    }
    out->naddrs = 1;
    out->ai_socktype = SOCK_STREAM;
    return 1;
}

/* ── Candidate names (search domains + ndots) ────────────────────────── */

/* Copy `a` into out (length-checked; avoids snprintf %s format-truncation). */
static int dns_copy(char *out, size_t cap, const char *a) {
    size_t la = strlen(a);
    if (la + 1 > cap)
        return -1;
    memcpy(out, a, la + 1);
    return 0;
}

/* out = "a.b" (length-checked). */
static int dns_join(char *out, size_t cap, const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    if (la + 1 + lb + 1 > cap)
        return -1;
    memcpy(out, a, la);
    out[la] = '.';
    memcpy(out + la + 1, b, lb);
    out[la + 1 + lb] = '\0';
    return 0;
}

static void dns_add_cand(KlDnsReq *q, const char *name) {
    if (q->ncand < DNS_MAX_CAND &&
        dns_copy(q->cand[q->ncand], DNS_NAME_MAX, name) == 0)
        q->ncand++;
}

/* Build the candidate-name list for `host` from search domains + ndots.
 * Absolute (trailing-dot) or dots >= ndots: bare name first, then search
 * variants. Otherwise: search variants first, then the bare name. */
static void dns_build_candidates(const KlDnsResolver *r, KlDnsReq *q, const char *host) {
    q->ncand = 0;
    q->ci = 0;

    char base[DNS_NAME_MAX];
    if (dns_copy(base, sizeof(base), host) != 0)
        return;                                  /* too long — no candidates */
    size_t bl = strlen(base);
    int absolute = (bl > 0 && base[bl - 1] == '.');
    if (absolute)
        base[bl - 1] = '\0';                     /* drop the trailing dot */

    int dots = 0;
    for (const char *p = base; *p; p++)
        if (*p == '.') dots++;

    if (absolute || r->nsearch == 0) {
        dns_add_cand(q, base);
        return;
    }

    char joined[DNS_NAME_MAX];
    if (dots >= r->ndots) {
        dns_add_cand(q, base);
        for (int i = 0; i < r->nsearch; i++)
            if (dns_join(joined, sizeof(joined), base, r->search[i]) == 0)
                dns_add_cand(q, joined);
    } else {
        for (int i = 0; i < r->nsearch; i++)
            if (dns_join(joined, sizeof(joined), base, r->search[i]) == 0)
                dns_add_cand(q, joined);
        dns_add_cand(q, base);
    }
}

/* Parse `search`/`domain` and `options ndots:` from a resolv.conf file. */
static void dns_parse_resolv_options(const char *path,
                                     char search[][DNS_NAME_MAX], int max_s,
                                     int *nsearch, int *ndots) {
    *nsearch = 0;
    *ndots = 1;
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';
        char *save = NULL;
        const char *kw = strtok_r(line, " \t\r\n", &save);
        if (!kw)
            continue;
        if (strcmp(kw, "search") == 0 || strcmp(kw, "domain") == 0) {
            *nsearch = 0;                        /* last search/domain line wins */
            const char *tok;
            while ((tok = strtok_r(NULL, " \t\r\n", &save)) != NULL && *nsearch < max_s) {
                if (dns_copy(search[*nsearch], DNS_NAME_MAX, tok) == 0)
                    (*nsearch)++;
            }
        } else if (strcmp(kw, "options") == 0) {
            const char *tok;
            while ((tok = strtok_r(NULL, " \t\r\n", &save)) != NULL) {
                if (strncmp(tok, "ndots:", 6) == 0) {
                    char *end;
                    long n = strtol(tok + 6, &end, 10);
                    if (end != tok + 6 && *end == '\0' && n >= 0 && n <= 15)
                        *ndots = (int)n;
                }
            }
        }
    }
    fclose(f);
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
    q->rdelay_timer = -1;
    q->legs[0].timer_id = -1;
    q->legs[1].timer_id = -1;
    snprintf(q->host, sizeof(q->host), "%s", host);

    /* Link before scheduling so a synchronous timer/send callback can find it. */
    q->next = r->inflight;
    r->inflight = q;

    /* Resolve without a query where possible: literal IP → hosts file →
     * localhost fallback (for hosts files that omit it). */
    if (dns_is_literal(host, port, &q->result) ||
        dns_hosts_lookup(r->hosts_path, host, port, r->prefer_ipv6, &q->result) ||
        dns_is_localhost(host, port, r->prefer_ipv6, &q->result)) {
        q->literal = 1;
        q->timer_id = kl_timer_add(r->ctx, 0, dns_on_literal, q);
        if (q->timer_id < 0) {
            dns_unlink(r, q);
            kl_free(r->alloc, q, sizeof(*q));
            return NULL;
        }
        return &q->base;
    }

    /* Expand into candidate names (search domains + ndots), query the first
     * with both address families concurrently. */
    dns_build_candidates(r, q, host);
    if (q->ncand == 0) {
        dns_unlink(r, q);
        kl_free(r->alloc, q, sizeof(*q));
        return NULL;
    }
    memcpy(q->host, q->cand[0], DNS_NAME_MAX);
    q->ci = 0;
    if (dns_start_candidate(r, q) != 0) {
        dns_cancel_timers(r, q);
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
    dns_cancel_timers(r, q);
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
        dns_cancel_timers(r, q);
        kl_free(r->alloc, q, sizeof(*q));
        q = next;
    }
    r->inflight = NULL;
    kl_udp_free(&r->sock);
    kl_free(r->alloc, r, sizeof(*r));
}

/* ── Nameserver selection ────────────────────────────────────────────── */

/* Parse "IP" or "IP#port" into a sockaddr (the #port form is a testing/advanced
 * extension; standard resolv.conf entries have no port and default to 53). */
static int dns_parse_ns(const char *s, uint16_t defport,
                        struct sockaddr_storage *out, socklen_t *out_len, int *family) {
    char buf[80];
    size_t sl = strlen(s);
    if (sl >= sizeof(buf))
        return -1;                /* nameserver token too long */
    memcpy(buf, s, sl + 1);
    uint16_t port = defport;
    char *hash = strchr(buf, '#');
    if (hash) {
        *hash = '\0';
        char *end;
        long p = strtol(hash + 1, &end, 10);
        if (end != hash + 1 && *end == '\0' && p > 0 && p <= 65535)
            port = (uint16_t)p;
    }

    memset(out, 0, sizeof(*out));
    struct in_addr a4;
    struct in6_addr a6;
    if (inet_pton(AF_INET, buf, &a4) == 1) {
        struct sockaddr_in *s4 = (struct sockaddr_in *)out;
        s4->sin_family = AF_INET;
        s4->sin_addr = a4;
        s4->sin_port = htons(port);
        *out_len = sizeof(*s4);
        *family = AF_INET;
        return 0;
    }
    if (inet_pton(AF_INET6, buf, &a6) == 1) {
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

/* Collect up to `max` `nameserver` entries from a resolv.conf file. */
static int resolv_conf_nameservers(const char *path, char out[][80], int max) {
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    char line[256];
    int n = 0;
    while (n < max && fgets(line, sizeof(line), f)) {
        if (strncmp(line, "nameserver", 10) != 0)
            continue;
        const char *p = line + 10;
        if (*p != ' ' && *p != '\t')
            continue;
        while (*p == ' ' || *p == '\t')
            p++;
        size_t i = 0;
        while (p[i] && p[i] != ' ' && p[i] != '\t' && p[i] != '\n' &&
               p[i] != '\r' && i + 1 < 80) {
            out[n][i] = p[i];
            i++;
        }
        out[n][i] = '\0';
        if (i > 0)
            n++;
    }
    fclose(f);
    return n;
}

/* Fill the resolver's nameserver list; returns the (single) socket family. */
static int dns_build_ns_list(KlDnsResolver *r, const KlDnsResolverConfig *cfg, int *family) {
    char nsbuf[DNS_MAX_NS][80];
    int count = 0;
    uint16_t defport = (cfg && cfg->port) ? cfg->port : DNS_PORT;

    r->ndots = 1;
    if (cfg && cfg->nameserver) {
        /* Explicit nameserver overrides resolv.conf entirely (no search domains). */
        snprintf(nsbuf[0], sizeof(nsbuf[0]), "%s", cfg->nameserver);
        count = 1;
    } else {
        const char *rc = (cfg && cfg->resolv_conf_path) ? cfg->resolv_conf_path
                                                        : "/etc/resolv.conf";
        count = resolv_conf_nameservers(rc, nsbuf, DNS_MAX_NS);
        dns_parse_resolv_options(rc, r->search, DNS_MAX_SEARCH, &r->nsearch, &r->ndots);
        if (count == 0) {
            snprintf(nsbuf[0], sizeof(nsbuf[0]), "127.0.0.1");
            count = 1;
        }
    }

    /* Parse; keep only nameservers of the first one's family (one socket). */
    int fam = -1, nns = 0;
    for (int i = 0; i < count && nns < DNS_MAX_NS; i++) {
        struct sockaddr_storage ss;
        socklen_t sl = 0;
        int f;
        if (dns_parse_ns(nsbuf[i], defport, &ss, &sl, &f) != 0)
            continue;
        if (fam == -1)
            fam = f;
        else if (f != fam)
            continue;
        r->ns[nns] = ss;
        r->ns_len[nns] = sl;
        nns++;
    }
    if (nns == 0)
        return -1;
    r->nns = nns;
    *family = fam;
    return 0;
}

/* ── Constructor ─────────────────────────────────────────────────────── */

KlResolver *kl_dns_resolver_create(KlEventCtx *ctx, const KlDnsResolverConfig *cfg) {
    if (!ctx)
        return NULL;
    KlAllocator *alloc = (cfg && cfg->alloc) ? cfg->alloc : ctx->alloc;
    if (!alloc)
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
    r->disable_edns = cfg ? cfg->disable_edns : 0;
    snprintf(r->hosts_path, sizeof(r->hosts_path), "%s",
             (cfg && cfg->hosts_path) ? cfg->hosts_path : "/etc/hosts");
    r->rnd_off = sizeof(r->rnd_pool);   /* pool empty → refills on first draw */

    int family = AF_INET;
    if (dns_build_ns_list(r, cfg, &family) != 0) {
        kl_free(alloc, r, sizeof(*r));
        return NULL;
    }

    /* Unconnected socket — sends target each nameserver by address (multi-NS);
     * dns_on_recv verifies the source is a configured nameserver. */
    KlUdpConfig uc = { .ctx = ctx, .family = family, .alloc = alloc };
    if (kl_udp_init(&r->sock, &uc) != 0) {
        kl_free(alloc, r, sizeof(*r));
        return NULL;
    }
    if (kl_udp_recv_start(&r->sock, dns_on_recv, r) != 0) {
        kl_udp_free(&r->sock);
        kl_free(alloc, r, sizeof(*r));
        return NULL;
    }
    return &r->base;
}
