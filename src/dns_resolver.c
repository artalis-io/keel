/*
 * dns_resolver.c — async A+AAAA resolver over KlDatagram (Do53), implementing KlResolver.
 *
 * The datagram transport is the Tier-1 KlDatagram (M3 of the datagram consolidation,
 * docs/datagram_consolidation_design.md §2): a fixed-slot, count-budgeted, confirmed-detachment-close
 * datagram primitive. The socket is prepared provider-neutrally by kl_datagram_open (M0) and adopted by
 * kl_datagram_init. Send backpressure is a datagram COUNT (send_slots): a transient WOULD_BLOCK does NOT
 * fail the leg — the leg is marked send_pending and retried on the writable edge, bounded by a
 * send-admission guard timer so a permanently-full/dead socket can never hang a resolve (D-DNS-3).
 *
 * The resolver's query socket is a Tier-1 KlDatagram (M3); the kl_datagram_open prep input is a
 * KlDatagramSocketConfig (the provider's datagram socket-option config), carrying family only.
 *
 * FREESTANDING (KEEL_FREESTANDING): the UDP query engine — dual-family queries,
 * 0x20 randomization, EDNS0, DNS cookies, the bounds-safe parser, and literal-IP /
 * "localhost" shortcuts — is fully portable and builds without a hosted libc. Three
 * hosted-only surfaces are compiled out, each around a cohesive helper (never a
 * scattered statement):
 *   - RFC 7766 TCP fallback (the whole dns_tcp_* block + its tcp[] state): a
 *     truncated (TC) response fails the leg with a prompt, clear error instead of
 *     retrying over TCP or silently waiting for the leg timeout.
 *   - /etc/hosts lookup (dns_hosts_lookup): returns "not found" — no filesystem.
 *   - resolv.conf discovery (dns_build_ns_list / default hosts path): the caller
 *     MUST supply cfg->nameserver; there is no config discovery.
 * The freestanding build therefore performs UDP-only Do53 against an explicitly
 * configured nameserver — the consumer-visible limitations (explicit nameserver
 * required, no hosts/resolv.conf discovery, no TCP recovery on TC) are documented
 * in docs/datagram_contract.md §11 ("Freestanding (UDP-only) DNS build"), and the
 * TC-settles-clean branch is runtime-proven by tests/freestanding_dns_harness.c.
 */
#include <keel/dns_resolver.h>
#include <keel/datagram.h>       /* KlDatagram public API + KlDatagramSocketConfig (kl_datagram_open prep) */
#include <keel/datagram_detail.h>/* KlDatagram layout — the resolver embeds one by value */
#include <keel/timer.h>
#include <keel/event_ctx.h>
#include <keel/tls.h>

#ifndef KEEL_FREESTANDING
#include <errno.h>       /* hosted TCP fallback only (EINPROGRESS / EAGAIN / EWOULDBLOCK) */
#endif
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "socket.h"      /* kl_sock_* seam + AF_INET/AF_INET6/SOCK_* (via sockcompat / the fs shim) */
#include "datagram_open.h" /* M0 kl_datagram_open — provider-neutral datagram socket prep */
#include "platform.h"
#include "kl_cstr.h"     /* locale-free bounded string primitives (freestanding-safe) */
#include "dns_sys.h"     /* platform config discovery (nameservers/hosts/search) — hosted-only uses */

#define DNS_NAME_MAX      256
_Static_assert(DNS_NAME_MAX == KL_DNS_SYS_NAME_MAX,
               "dns_sys search-domain width must match DNS_NAME_MAX");
#define DNS_QUERY_MAX     512   /* query buffer (question + EDNS0 OPT fit easily) */
#define DNS_RND_POOL_SIZE 256   /* entropy bytes buffered per /dev/urandom read */
#define DNS_TYPE_OPT      41    /* EDNS0 OPT pseudo-record type */
#define DNS_EDNS_UDP_SIZE 1232  /* advertised UDP payload size (DNS flag day) */
#define DNS_OPT_RR_LEN    11    /* wire length of a bare OPT RR */
#define DNS_OPT_COOKIE    10    /* EDNS0 COOKIE option code (RFC 7873) */
#define DNS_COOKIE_CLIENT  8    /* client cookie length (fixed) */
#define DNS_COOKIE_SRV_MAX 32   /* max server cookie length */
#define DNS_RCODE_BADCOOKIE 23  /* full (extended) rcode = BADCOOKIE (RFC 7873 §5.3) */
#define DNS_COOKIE_RETRY_MAX 1  /* BADCOOKIE re-transmits per leg (bounded, no loop) */
#define DNS_MAX_NS        3     /* nameservers tried (resolv.conf MAXNS) */
#define DNS_PORT          53    /* default nameserver port */
#define DNS_TCP_IDLE_MS   10000 /* close an idle persistent TCP connection (RFC 7766) */
#define DNS_TCP_MSG_MAX   65535 /* max DNS-over-TCP message (2-byte length prefix) */
#define DNS_SEND_SLOTS    8     /* default outbound datagram slots (transient-burst tolerance, D-DNS-1) */
#define DNS_RECV_CAP      2048  /* inbound slot payload capacity (parity with the historical UDP recv default) */

/* DNS-over-TCP connection state. */
enum { DNS_TCP_CLOSED = 0, DNS_TCP_CONNECTING, DNS_TCP_READY };
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
    int64_t          timer_id;     /* per-leg timeout OR send-admission guard (mutually exclusive), -1 if none */
    int              done;         /* 1 = settled (answered / empty / exhausted) */
    int              send_pending; /* 1 = a transient WOULD_BLOCK is pending re-send on the writable edge
                                    *     (D-DNS-3); timer_id then holds the send-admission GUARD, not the
                                    *     response timeout. Mutually exclusive with an armed response timer. */
    uint8_t          question[DNS_NAME_MAX + 8]; /* transmitted question (name+type+class) */
    size_t           question_len;
    KlSockAddr       addrs[KL_RESOLVE_MAX_ADDRS]; /* collected addresses */
    int              naddrs;
    int              tcp_pending;  /* 1 = awaiting a DNS-over-TCP response */
    int              tcp_ns;       /* nameserver index of the TCP connection, or -1 */
    int              cookie_retry; /* BADCOOKIE re-transmits already spent on this leg */
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

typedef struct KlDnsResolver KlDnsResolver;

/* Persistent DNS-over-TCP connection to one nameserver (RFC 7766). The TCP
 * fallback path for truncated (TC-bit) responses; reused + pipelined, closed
 * after DNS_TCP_IDLE_MS of inactivity. `tls` is NULL here (plain Do53/TCP) — it
 * is the wrapping point for DNS-over-TLS. All I/O goes through the (fd, tls)
 * helpers so DoT is a drop-in. */
typedef struct {
    KlDnsResolver *r;              /* owner (for callbacks) */
    int            ns_idx;         /* which nameserver this connects to */
    KlSocketHandle fd;             /* -1 = closed */
    int            state;          /* DNS_TCP_CLOSED / CONNECTING / READY */
    KlTls         *tls;            /* NULL = plaintext; set for DoT */
    unsigned char *wbuf;           /* queued outbound framed queries */
    size_t         wlen, wsent, wcap;
    unsigned char *rbuf;           /* accumulated inbound bytes */
    size_t         rlen, rcap;
    size_t         rneed;          /* current message length from prefix (0 = need prefix) */
    int64_t        idle_timer;     /* -1 = none */
    int            watching;       /* 1 = watcher registered on fd */
    KlEventMask    want;           /* current watcher interest */
} KlDnsTcp;

/* Per-nameserver DNS cookie state (RFC 7873). The client cookie is generated
 * once per nameserver; the server cookie is learned from responses and echoed
 * back on subsequent queries. */
typedef struct {
    uint8_t client[DNS_COOKIE_CLIENT];   /* our client cookie (random, per NS) */
    uint8_t server[DNS_COOKIE_SRV_MAX];  /* server cookie learned from responses */
    uint8_t server_len;                  /* 0 = none learned yet (8..32 once known) */
    uint8_t have_client;                 /* 1 = client cookie generated */
} KlDnsCookie;

struct KlDnsResolver {
    KlResolver     base;           /* MUST be first for upcast */
    KlEventCtx    *ctx;
    KlAllocator   *alloc;
    KlDatagram     sock;          /* Tier-1 datagram transport (M3); prepared via kl_datagram_open */
    int            send_slots;    /* outbound slot count (D-DNS-1 burst sizing) */
    int            in_done;       /* >0 while a user done() callback is on the stack (dns_complete depth) */
    int            destroy_requested; /* destroy() called from within done() → run at dns_complete's tail */
    int            timeout_ms;
    int            attempts;
    int            prefer_ipv6;
    int            disable_0x20;
    int            disable_edns;
    int            disable_cookies;           /* 1 = don't send/verify DNS cookies */
    char           hosts_path[DNS_NAME_MAX];
    KlSockAddr     ns[DNS_MAX_NS];            /* nameserver addresses (neutral currency) */
    int            nns;                        /* number of nameservers */
    char           search[DNS_MAX_SEARCH][DNS_NAME_MAX]; /* search domains */
    int            nsearch;
    int            ndots;                     /* threshold for search expansion */
    KlDnsReq      *inflight;
#ifndef KEEL_FREESTANDING
    KlDnsTcp       tcp[DNS_MAX_NS]; /* per-nameserver persistent TCP fallback conns (hosted only) */
#endif
    KlDnsCookie    cookie[DNS_MAX_NS]; /* per-nameserver DNS cookie state (RFC 7873) */
    unsigned char  rnd_pool[DNS_RND_POOL_SIZE]; /* pooled OS entropy for IDs + 0x20 */
    size_t         rnd_off;       /* next unused byte in rnd_pool */
};

/* ── Entropy (pooled OS RNG via the platform layer) ─────────────────────── */

static void dns_rand_refill(KlDnsResolver *r) {
    kl_plat_random(r->rnd_pool, sizeof(r->rnd_pool));
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
            /* port 0 here; dns_set_port() stamps the real port after parsing */
            kl_sockaddr_from_ipv4(&out->addrs[out->naddrs], pkt + off, 0);
            out->naddrs++;
        } else if (rtype == want_qtype && want_qtype == KL_DNS_TYPE_AAAA && rdlen == 16) {
            kl_sockaddr_from_ipv6(&out->addrs[out->naddrs], pkt + off, 0, 0);
            out->naddrs++;
        }
        off += rdlen;
    }
    return out->naddrs > 0 ? 0 : -1;
}

/* ── Query builder ────────────────────────────────────────────────────── */

/* Return the client cookie for a nameserver, generating it once on first use. */
static const uint8_t *dns_cookie_client(KlDnsResolver *r, int ns_idx) {
    KlDnsCookie *c = &r->cookie[ns_idx];
    if (!c->have_client) {
        for (int i = 0; i < DNS_COOKIE_CLIENT; i++)
            c->client[i] = dns_rand_byte(r);
        c->have_client = 1;
    }
    return c->client;
}

/* Build a query with transaction id, applying DNS 0x20 case randomization to
 * the QNAME letters (unless disabled). Also returns the question-section span
 * [*q_off, off) so the caller can store it for response verification. When
 * cookies are enabled, a COOKIE option (client [+ learned server]) is added to
 * the EDNS0 OPT rdata for nameserver @p ns_idx. */
static int dns_build_query(KlDnsResolver *r, uint8_t *buf, size_t cap, uint16_t id,
                           const char *host, int qtype, int ns_idx,
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
        const char *dot = kl_strchr(p, '.');
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
        int cookie = !r->disable_cookies && ns_idx >= 0 && ns_idx < r->nns;
        uint8_t srv_len = cookie ? r->cookie[ns_idx].server_len : 0;
        size_t optlen = cookie ? (size_t)DNS_COOKIE_CLIENT + srv_len : 0;
        size_t rdlen  = cookie ? 4 + optlen : 0;   /* option-code+len (4) + data */
        if (off + DNS_OPT_RR_LEN + rdlen > cap)
            return -1;
        buf[11] = 0x01;                          /* arcount = 1 */
        buf[off++] = 0x00;                       /* OPT owner name = root */
        buf[off++] = (uint8_t)(DNS_TYPE_OPT >> 8);
        buf[off++] = (uint8_t)(DNS_TYPE_OPT & 0xFF);
        buf[off++] = (uint8_t)(DNS_EDNS_UDP_SIZE >> 8);
        buf[off++] = (uint8_t)(DNS_EDNS_UDP_SIZE & 0xFF);
        buf[off++] = 0x00; buf[off++] = 0x00;    /* extended rcode + version */
        buf[off++] = 0x00; buf[off++] = 0x00;    /* flags */
        buf[off++] = (uint8_t)(rdlen >> 8);      /* OPT rdlen */
        buf[off++] = (uint8_t)(rdlen & 0xFF);
        if (cookie) {                            /* COOKIE option (RFC 7873) */
            const uint8_t *cc = dns_cookie_client(r, ns_idx);
            buf[off++] = (uint8_t)(DNS_OPT_COOKIE >> 8);
            buf[off++] = (uint8_t)(DNS_OPT_COOKIE & 0xFF);
            buf[off++] = (uint8_t)(optlen >> 8);
            buf[off++] = (uint8_t)(optlen & 0xFF);
            memcpy(buf + off, cc, DNS_COOKIE_CLIENT);
            off += DNS_COOKIE_CLIENT;
            if (srv_len) {
                memcpy(buf + off, r->cookie[ns_idx].server, srv_len);
                off += srv_len;
            }
        }
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

#ifndef KEEL_FREESTANDING
static void dns_tcp_touch_idle_all(KlDnsResolver *r);   /* RFC 7766 TCP fallback (defined below) */
#else
/* UDP-only freestanding build: no persistent TCP fallback, so idle-touch is a no-op. */
static void dns_tcp_touch_idle_all(KlDnsResolver *r) { (void)r; }
#endif

static void dns_teardown(KlDnsResolver *r);   /* the real destroy body (below) */

static void dns_complete(KlDnsResolver *r, KlDnsReq *q,
                         const KlResolveResult *result, int error) {
    dns_cancel_timers(r, q);
    dns_unlink(r, q);

    q->in_done = 1;
    /* Resolver-level deferred-destroy sentinel (reentrancy across ANY completion source, incl.
     * timer-driven done — the guard/timeout/literal timers have no datagram busy frame to defer the
     * teardown). If the user's done() calls r->destroy(r), it only SETS destroy_requested and returns; the
     * real teardown runs at the tail below, AFTER dns_complete is finished touching `r`. */
    r->in_done++;
    q->done(&q->base, error ? NULL : result, error, q->ud);
    /* The consumer drops its reference inside done(); free unconditionally.
     * (cancelled is honoured only to avoid a double-free if cancel() were
     * called re-entrantly during done — the free still happens exactly here.) */
    kl_free(r->alloc, q, sizeof(*q));
    dns_tcp_touch_idle_all(r);   /* a TCP-pending leg may have just vanished */
    if (--r->in_done == 0 && r->destroy_requested)
        dns_teardown(r);         /* destructive tail — no `r` access after this */
}

static void dns_set_port(KlResolveResult *res, int port) {
    for (int i = 0; i < res->naddrs; i++)
        kl_sockaddr_set_port(&res->addrs[i], (uint16_t)port);
}

/* ── Two-leg (dual-family) state machine ─────────────────────────────── */

#define DNS_RESOLUTION_DELAY_MS 50   /* RFC 8305 §3: wait for the 2nd family */

static void dns_on_leg_timer(void *ud);
static void dns_on_rdelay(void *ud);
static void dns_on_literal(void *ud);
static void dns_advance_candidate(KlDnsResolver *r, KlDnsReq *q);
static void dns_leg_settle(KlDnsResolver *r, KlDnsReq *q, KlDnsLeg *leg);

/* Outcome of one transmit attempt (D-DNS-3). A network attempt (tries_left) and the response timer are
 * consumed ONLY on DNS_TX_SENT; DNS_TX_WOULDBLOCK consumes nothing (the leg is retried on the writable
 * edge); DNS_TX_FAILED is a permanent send failure (build error / TOO_LARGE / UNSUPPORTED / CLOSED /
 * transport ERROR) handled like today (settle the leg). */
typedef enum { DNS_TX_SENT, DNS_TX_WOULDBLOCK, DNS_TX_FAILED } DnsTxResult;

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

/* Transmit one leg's query (rotating nameserver). A try/timer is consumed ONLY on successful admission
 * (KL_DATAGRAM_ACCEPTED); on transient WOULD_BLOCK nothing is consumed and the leg stays on the same
 * nameserver/id-space for a writable-edge retry (D-DNS-3). Returns a 3-way DnsTxResult. */
static DnsTxResult dns_transmit_leg(KlDnsResolver *r, const KlDnsReq *q, KlDnsLeg *leg) {
    if (leg->tries_left <= 0)
        return DNS_TX_FAILED;

    for (int tries = 0; tries < 8; tries++) {
        leg->id = dns_rand_u16(r);
        if (!dns_id_in_use(r, leg->id, leg))
            break;
    }

    uint8_t buf[DNS_QUERY_MAX];
    size_t qlen = 0, q_off = 0, q_len = 0;
    if (dns_build_query(r, buf, sizeof(buf), leg->id, q->host, leg->qtype,
                        leg->ns_idx, &qlen, &q_off, &q_len) != 0)
        return DNS_TX_FAILED;
    if (q_len > sizeof(leg->question))
        return DNS_TX_FAILED;

    const KlSockAddr *ns_ksa = &r->ns[leg->ns_idx];
    KlDatagramMessage msg = { .data = buf, .len = qlen, .peer = ns_ksa,
                              .local = NULL, .tos = -1, .flags = 0 };
    KlDatagramSendStatus st = kl_datagram_send(&r->sock, &msg);
    if (st == KL_DATAGRAM_WOULD_BLOCK)
        return DNS_TX_WOULDBLOCK;          /* transient: nothing consumed; retry on the writable edge */
    if (st != KL_DATAGRAM_ACCEPTED)
        return DNS_TX_FAILED;              /* TOO_LARGE / UNSUPPORTED / CLOSED / ERROR — permanent */

    /* ACCEPTED — commit: record the sent question, rotate the nameserver, consume a try, arm the
     * response timeout. (question is recorded only now, so a would-blocked build isn't tracked.) */
    memcpy(leg->question, buf + q_off, q_len);
    leg->question_len = q_len;
    leg->ns_idx = (leg->ns_idx + 1) % r->nns;
    leg->tries_left--;
    if (leg->timer_id >= 0)
        kl_timer_cancel(r->ctx, leg->timer_id);
    leg->timer_id = kl_timer_add(r->ctx, (uint64_t)r->timeout_ms, dns_on_leg_timer, leg);
    return (leg->timer_id >= 0) ? DNS_TX_SENT : DNS_TX_FAILED;
}

/* Enter the send_pending state on the FIRST transient WOULD_BLOCK: arm the send-admission guard timer
 * (reusing leg->timer_id, free while pending) for r->timeout_ms so a leg that never sees a writable edge
 * (permanently-full/dead socket) settles instead of hanging (D-DNS-3). Preserved across writable retries
 * — armed once on entry, not re-armed per attempt. Returns 0 (pending, guard running) or -1 (the guard
 * timer could not be armed — the CALLER settles the leg; this function never settles, so it is safe to
 * call mid-candidate-start before both legs are initialized). */
static int dns_leg_mark_pending(KlDnsResolver *r, KlDnsLeg *leg) {
    if (leg->send_pending)
        return 0;                          /* guard already running — do not re-arm */
    if (leg->timer_id >= 0) { kl_timer_cancel(r->ctx, leg->timer_id); leg->timer_id = -1; }
    int64_t t = kl_timer_add(r->ctx, (uint64_t)r->timeout_ms, dns_on_leg_timer, leg);
    if (t < 0)
        return -1;                         /* guard couldn't arm */
    leg->timer_id = t;
    leg->send_pending = 1;
    return 0;
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
            res->addrs[res->naddrs] = q->legs[0].addrs[i0];
            res->naddrs++; i0++;
        }
        if (res->naddrs < KL_RESOLVE_MAX_ADDRS && i1 < q->legs[1].naddrs) {
            res->addrs[res->naddrs] = q->legs[1].addrs[i1];
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

/* Mark a leg settled (cancel its timeout OR send-admission guard) and re-evaluate completion. Clearing
 * send_pending here is what makes the writable scan skip a settled leg (D-DNS-3 reentrancy). */
static void dns_leg_settle(KlDnsResolver *r, KlDnsReq *q, KlDnsLeg *leg) {
    if (leg->timer_id >= 0) {
        kl_timer_cancel(r->ctx, leg->timer_id);
        leg->timer_id = -1;
    }
    leg->send_pending = 0;
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
        leg->tcp_pending = 0;
        leg->tcp_ns = -1;
        leg->cookie_retry = 0;
        leg->send_pending = 0;
        DnsTxResult tr = dns_transmit_leg(r, q, leg);
        if (tr == DNS_TX_SENT) {
            any = 1;
        } else if (tr == DNS_TX_WOULDBLOCK && dns_leg_mark_pending(r, leg) == 0) {
            any = 1;                                /* transient backpressure → retry on writable edge
                                                     * (pending IS progress, guard-bounded) */
        } else {
            leg->done = 1;                          /* permanent failure / guard-arm failure → empty.
                                                     * NB: no dns_check_complete here — the other leg may
                                                     * still be mid-init; completion is decided by the
                                                     * caller via this function's any?0:-1 return. */
        }
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

    if (leg->send_pending) {                         /* SEND-ADMISSION GUARD fired (D-DNS-3): the leg was
                                                      * never transmitted → settle. No attempt is consumed
                                                      * (no packet went out); retrying a congested/dead
                                                      * socket is futile, so settle rather than retransmit. */
        leg->send_pending = 0;
        dns_leg_settle(r, q, leg);
        return;
    }
    if (leg->tcp_pending) {                          /* DNS-over-TCP exchange timed out */
        leg->tcp_pending = 0;
        leg->tcp_ns = -1;
        dns_leg_settle(r, q, leg);
        return;
    }
    if (leg->tries_left > 0) {                       /* response timeout → retransmit (rotates NS) */
        DnsTxResult tr = dns_transmit_leg(r, q, leg);
        if (tr == DNS_TX_SENT)
            return;
        if (tr == DNS_TX_WOULDBLOCK && dns_leg_mark_pending(r, leg) == 0)
            return;                                  /* transient again → wait for the writable edge */
    }
    dns_leg_settle(r, q, leg);                       /* exhausted / permanent failure → empty */
}

/* Send-queue full→non-full edge (D-DNS-3): re-attempt every send_pending leg. Registered once at resolver
 * init. Non-destructive in the common case (SENT/WOULD_BLOCK); a permanent failure settles the leg, which
 * may complete + free its request, so the scan RESTARTS after any settle (mirrors the TCP writable sweep). */
static void dns_on_writable(void *ud) {
    KlDnsResolver *r = ud;
restart:
    for (KlDnsReq *q = r->inflight; q; q = q->next) {
        for (int i = 0; i < 2; i++) {
            KlDnsLeg *leg = &q->legs[i];
            if (!leg->send_pending)
                continue;
            DnsTxResult tr = dns_transmit_leg(r, q, leg);
            if (tr == DNS_TX_WOULDBLOCK)
                return;                              /* queue re-filled → leave the rest pending (guard
                                                      * still running) for the next writable edge */
            leg->send_pending = 0;                   /* SENT (guard cancelled + response timer armed by
                                                      * dns_transmit_leg) or permanent failure */
            if (tr == DNS_TX_SENT)
                continue;                            /* try the next pending leg */
            dns_leg_settle(r, q, leg);               /* FAILED → settle; may free q → restart the scan */
            goto restart;
        }
    }
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

/* Walk to the OPT RR in the additional section. On finding one, sets *ext_rcode
 * (the OPT TTL's high byte — the EDNS extended-rcode bits) and, if a COOKIE
 * option is present with a valid length, copies the echoed client cookie (8) +
 * server cookie (0..32) and sets *have_cookie. Returns 0 if an OPT RR was found,
 * -1 otherwise. Fully bounds-checked against hostile packets. */
static int dns_extract_opt(const uint8_t *pkt, size_t len, uint8_t *ext_rcode,
                           uint8_t *client_out, uint8_t *server_out,
                           uint8_t *server_len_out, int *have_cookie) {
    *ext_rcode = 0;
    *server_len_out = 0;
    *have_cookie = 0;
    if (len < 12)
        return -1;
    uint16_t qd = (uint16_t)((pkt[4] << 8) | pkt[5]);
    uint16_t an = (uint16_t)((pkt[6] << 8) | pkt[7]);
    uint16_t nscount = (uint16_t)((pkt[8] << 8) | pkt[9]);
    uint16_t ar = (uint16_t)((pkt[10] << 8) | pkt[11]);

    size_t off = 12;
    for (uint16_t i = 0; i < qd; i++) {          /* skip questions */
        if (dns_skip_name(pkt, len, off, &off) != 0)
            return -1;
        if (off + 4 > len)
            return -1;
        off += 4;
    }
    uint32_t pre = (uint32_t)an + nscount;       /* skip answer + authority RRs */
    for (uint32_t i = 0; i < pre; i++) {
        if (dns_skip_name(pkt, len, off, &off) != 0)
            return -1;
        if (off + 10 > len)
            return -1;
        uint16_t rdlen = (uint16_t)((pkt[off + 8] << 8) | pkt[off + 9]);
        off += 10;
        if (off + rdlen > len)
            return -1;
        off += rdlen;
    }
    for (uint16_t i = 0; i < ar; i++) {          /* scan additional for OPT */
        if (dns_skip_name(pkt, len, off, &off) != 0)
            return -1;
        if (off + 10 > len)
            return -1;
        uint16_t rtype = (uint16_t)((pkt[off] << 8) | pkt[off + 1]);
        uint16_t rdlen = (uint16_t)((pkt[off + 8] << 8) | pkt[off + 9]);
        if (rtype == DNS_TYPE_OPT) {
            *ext_rcode = pkt[off + 4];           /* TTL byte 0 = extended rcode hi */
            size_t rd = off + 10, end = rd + rdlen;
            if (end > len)
                return -1;
            for (size_t o = rd; o + 4 <= end; ) { /* walk EDNS options */
                uint16_t oc = (uint16_t)((pkt[o] << 8) | pkt[o + 1]);
                uint16_t ol = (uint16_t)((pkt[o + 2] << 8) | pkt[o + 3]);
                o += 4;
                if (o + ol > end)
                    break;
                if (oc == DNS_OPT_COOKIE &&
                    (ol == DNS_COOKIE_CLIENT ||
                     (ol >= DNS_COOKIE_CLIENT + 8 &&
                      ol <= DNS_COOKIE_CLIENT + DNS_COOKIE_SRV_MAX))) {
                    memcpy(client_out, pkt + o, DNS_COOKIE_CLIENT);
                    uint8_t sl = (uint8_t)(ol - DNS_COOKIE_CLIENT);
                    if (sl)
                        memcpy(server_out, pkt + o + DNS_COOKIE_CLIENT, sl);
                    *server_len_out = sl;
                    *have_cookie = 1;
                }
                o += ol;
            }
            return 0;
        }
        if (off + 10 + rdlen > len)
            return -1;
        off += 10 + rdlen;
    }
    return -1;                                    /* no OPT RR */
}

/* The index of the configured nameserver `src` matches, or -1. (The socket is
 * unconnected for multi-NS, so this replaces the kernel peer filter.) */
static int dns_ns_index(const KlDnsResolver *r, const KlSockAddr *src) {
    for (int i = 0; i < r->nns; i++) {
        if (kl_sockaddr_equal(src, &r->ns[i]))   /* family + address + port */
            return i;
    }
    return -1;
}

/* ── DNS-over-TCP fallback (RFC 7766): persistent, pipelined per-NS ─────── */
/* Hosted-only: the freestanding (UDP-only) build compiles this whole block out —
 * truncated responses fail the leg clearly rather than recovering over TCP. */
#ifndef KEEL_FREESTANDING

static void dns_tcp_on_event(KlSocketHandle fd, KlEventMask mask, void *ud);

/* Transport read/write — plaintext today; the tls branch is the DoT hook. */
static ssize_t dns_tcp_write(KlDnsTcp *t, const void *b, size_t n) {
    if (t->tls)
        return t->tls->write(t->tls, t->fd, b, n);
    return kl_sock_send(t->r->ctx->sockets, t->fd, b, n);
}
static ssize_t dns_tcp_read(KlDnsTcp *t, void *b, size_t n) {
    if (t->tls)
        return t->tls->read(t->tls, t->fd, b, n);
    return kl_sock_recv(t->r->ctx->sockets, t->fd, b, n);
}

/* Number of legs still awaiting a response on this nameserver's connection. */
static int dns_tcp_pending_on(const KlDnsResolver *r, int ns_idx) {
    int n = 0;
    for (const KlDnsReq *q = r->inflight; q; q = q->next)
        for (int i = 0; i < 2; i++)
            if (q->legs[i].tcp_pending && q->legs[i].tcp_ns == ns_idx)
                n++;
    return n;
}

static void dns_tcp_close(KlDnsResolver *r, KlDnsTcp *t) {
    if (t->idle_timer >= 0) { kl_timer_cancel(r->ctx, t->idle_timer); t->idle_timer = -1; }
    if (t->watching)        { kl_watcher_del(r->ctx, t->fd); t->watching = 0; }
    if (t->tls)             { t->tls->destroy(t->tls); t->tls = NULL; }
    if (kl_handle_valid(t->fd)) { kl_sock_close(r->ctx->sockets, t->fd); t->fd = KL_INVALID_SOCKET; }
    if (t->wbuf) { kl_free(r->alloc, t->wbuf, t->wcap); t->wbuf = NULL; t->wcap = 0; }
    if (t->rbuf) { kl_free(r->alloc, t->rbuf, t->rcap); t->rbuf = NULL; t->rcap = 0; }
    t->wlen = t->wsent = t->rlen = t->rneed = 0;
    t->state = DNS_TCP_CLOSED;
    t->want = 0;
}

/* Reconcile watcher interest: READ always; WRITE while connecting or unsent. */
static void dns_tcp_update_interest(KlDnsResolver *r, KlDnsTcp *t) {
    if (!t->watching) return;
    KlEventMask want = KL_EVENT_READ;
    if (t->state == DNS_TCP_CONNECTING || t->wsent < t->wlen)
        want |= KL_EVENT_WRITE;
    if (want != t->want && kl_watcher_mod(r->ctx, t->fd, want) == 0)
        t->want = want;
}

static void dns_tcp_on_idle(void *ud) {
    KlDnsTcp *t = ud;
    t->idle_timer = -1;
    if (dns_tcp_pending_on(t->r, t->ns_idx) == 0 && t->wlen == t->wsent)
        dns_tcp_close(t->r, t);
}

/* Arm the idle-close timer when the connection has no pending work; cancel it
 * while busy. */
static void dns_tcp_arm_idle(KlDnsResolver *r, KlDnsTcp *t) {
    if (!kl_handle_valid(t->fd)) return;
    int busy = dns_tcp_pending_on(r, t->ns_idx) > 0 || t->wlen > t->wsent;
    if (busy) {
        if (t->idle_timer >= 0) { kl_timer_cancel(r->ctx, t->idle_timer); t->idle_timer = -1; }
    } else if (t->idle_timer < 0) {
        t->idle_timer = kl_timer_add(r->ctx, DNS_TCP_IDLE_MS, dns_tcp_on_idle, t);
    }
}

/* Re-evaluate idle state of every open connection (after a request completes and
 * a pending leg may have vanished). */
static void dns_tcp_touch_idle_all(KlDnsResolver *r) {
    for (int i = 0; i < r->nns; i++)
        if (kl_handle_valid(r->tcp[i].fd))
            dns_tcp_arm_idle(r, &r->tcp[i]);
}

/* Close the connection and settle every leg that was waiting on it (empty). The
 * scan restarts after each settle since dns_leg_settle may complete + free a
 * request. */
static void dns_tcp_fail(KlDnsResolver *r, KlDnsTcp *t) {
    int ns = t->ns_idx;
    dns_tcp_close(r, t);
    for (int again = 1; again; ) {
        again = 0;
        for (KlDnsReq *q = r->inflight; q; q = q->next) {
            int settled = 0;
            for (int i = 0; i < 2; i++)
                if (q->legs[i].tcp_pending && q->legs[i].tcp_ns == ns) {
                    q->legs[i].tcp_pending = 0;
                    q->legs[i].tcp_ns = -1;
                    q->legs[i].naddrs = 0;
                    dns_leg_settle(r, q, &q->legs[i]);   /* may free q → restart */
                    settled = 1;
                    break;
                }
            /* dns_leg_settle may have freed q — break BEFORE q = q->next runs on
             * freed memory, then rescan from the (updated) inflight head. */
            if (settled) { again = 1; break; }
        }
    }
}

static int dns_tcp_connect(KlDnsResolver *r, KlDnsTcp *t, int ns_idx) {
    const KlSockAddr *ns_ksa = &r->ns[ns_idx];
    int fam = (kl_sockaddr_family(ns_ksa) == KL_AF_INET6) ? AF_INET6 : AF_INET;
    KlSocketHandle fd = kl_sock_socket(r->ctx->sockets, fam, SOCK_STREAM, 0);
    if (!kl_handle_valid(fd))
        return -1;
    if (kl_sock_set_nonblocking(r->ctx->sockets, fd) < 0) {
        kl_sock_close(r->ctx->sockets, fd); return -1;
    }
    kl_sock_set_cloexec(r->ctx->sockets, fd);

    int rc = kl_sock_connect(r->ctx->sockets, fd, ns_ksa);
    if (rc < 0 && errno != EINPROGRESS) {
        kl_sock_close(r->ctx->sockets, fd); return -1;
    }

    t->r = r; t->ns_idx = ns_idx; t->fd = fd;
    t->state = (rc == 0) ? DNS_TCP_READY : DNS_TCP_CONNECTING;
    t->want = KL_EVENT_READ | KL_EVENT_WRITE;
    if (kl_watcher_add(r->ctx, fd, t->want, dns_tcp_on_event, t) != 0) {
        kl_sock_close(r->ctx->sockets, fd);
        t->fd = KL_INVALID_SOCKET; t->state = DNS_TCP_CLOSED; return -1;
    }
    t->watching = 1;
    return 0;
}

/* Frame + parse each complete inbound message; route to its leg by txn-id. */
static void dns_tcp_deliver(KlDnsResolver *r, KlDnsTcp *t) {
    for (;;) {
        if (t->rneed == 0) {
            if (t->rlen < 2)
                break;
            t->rneed = ((size_t)t->rbuf[0] << 8) | t->rbuf[1];
        }
        if (t->rlen < 2 + t->rneed)
            break;                                   /* need more bytes */
        const uint8_t *msg = t->rbuf + 2;
        size_t mlen = t->rneed;
        uint16_t id = (mlen >= 2) ? (uint16_t)((msg[0] << 8) | msg[1]) : 0;
        KlDnsLeg *leg = (mlen > 0) ? dns_find_leg(r, id) : NULL;
        KlDnsReq *q = NULL;

        if (leg && leg->tcp_pending && leg->tcp_ns == t->ns_idx) {
            q = leg->req;
            leg->tcp_pending = 0; leg->tcp_ns = -1;
            KlResolveResult scr;
            memset(&scr, 0, sizeof(scr));
            if (kl_dns_parse_response(msg, mlen, id, leg->qtype,
                                      leg->question, leg->question_len, &scr) == 0) {
                leg->naddrs = scr.naddrs;
                for (int i = 0; i < scr.naddrs; i++)
                    leg->addrs[i] = scr.addrs[i];
            } else {
                leg->naddrs = 0;
            }
        }
        /* Consume the framed message BEFORE settling (settle may re-enter/free). */
        memmove(t->rbuf, t->rbuf + 2 + mlen, t->rlen - (2 + mlen));
        t->rlen -= 2 + mlen;
        t->rneed = 0;

        if (q)
            dns_leg_settle(r, q, leg);
    }
}

static void dns_tcp_flush(KlDnsResolver *r, KlDnsTcp *t) {
    while (t->wsent < t->wlen) {
        ssize_t n = dns_tcp_write(t, t->wbuf + t->wsent, t->wlen - t->wsent);
        if (n > 0) { t->wsent += (size_t)n; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;                                  /* keep WRITE interest */
        dns_tcp_fail(r, t);                          /* peer closed / hard error */
        return;
    }
    t->wlen = t->wsent = 0;                           /* fully flushed */
    dns_tcp_update_interest(r, t);
}

static void dns_tcp_on_event(KlSocketHandle fd, KlEventMask mask, void *ud) {
    (void)fd;
    KlDnsTcp *t = ud;
    KlDnsResolver *r = t->r;

    if (t->state == DNS_TCP_CONNECTING) {
        int err = 0;
        if (kl_sock_get_so_error(r->ctx->sockets, t->fd, &err) != 0 || err != 0) {
            dns_tcp_fail(r, t); return;
        }
        t->state = DNS_TCP_READY;
        dns_tcp_update_interest(r, t);
    }
    if (mask & KL_EVENT_WRITE)
        dns_tcp_flush(r, t);
    if (!kl_handle_valid(t->fd))
        return;                                      /* flush failed → closed */

    if (mask & KL_EVENT_READ) {
        for (;;) {
            if (t->rlen == t->rcap) {
                size_t ncap = t->rcap ? t->rcap * 2 : 4096;
                if (ncap > 2 + DNS_TCP_MSG_MAX) ncap = 2 + DNS_TCP_MSG_MAX;
                if (ncap == t->rcap) { dns_tcp_fail(r, t); return; }  /* oversize */
                unsigned char *nb = kl_realloc(r->alloc, t->rbuf, t->rcap, ncap);
                if (!nb) { dns_tcp_fail(r, t); return; }
                t->rbuf = nb; t->rcap = ncap;
            }
            ssize_t n = dns_tcp_read(t, t->rbuf + t->rlen, t->rcap - t->rlen);
            if (n > 0) {
                t->rlen += (size_t)n;
                dns_tcp_deliver(r, t);
                if (!kl_handle_valid(t->fd)) return;
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                break;
            dns_tcp_fail(r, t);                       /* EOF / error */
            return;
        }
    }
    dns_tcp_arm_idle(r, t);
}

/* Re-issue a truncated leg's query over the nameserver's TCP connection. */
static void dns_tcp_send_leg(KlDnsResolver *r, KlDnsLeg *leg, int ns_idx) {
    KlDnsReq *q = leg->req;
    KlDnsTcp *t = &r->tcp[ns_idx];

    if (!kl_handle_valid(t->fd) && dns_tcp_connect(r, t, ns_idx) != 0) {
        leg->naddrs = 0; dns_leg_settle(r, q, leg); return;
    }

    uint8_t qb[DNS_QUERY_MAX];
    size_t qlen = 0, q_off = 0, q_len = 0;
    if (dns_build_query(r, qb, sizeof(qb), leg->id, q->host, leg->qtype,
                        ns_idx, &qlen, &q_off, &q_len) != 0 ||
        q_len > sizeof(leg->question) || qlen > DNS_TCP_MSG_MAX) {
        leg->naddrs = 0; dns_leg_settle(r, q, leg); return;
    }
    memcpy(leg->question, qb + q_off, q_len);
    leg->question_len = q_len;

    size_t need = t->wlen + 2 + qlen;
    if (need > t->wcap) {
        size_t ncap = t->wcap ? t->wcap : 2048;
        while (ncap < need) {
            if (ncap > SIZE_MAX / 2) { ncap = need; break; }   /* doubling-overflow guard (uniform w/ the rest) */
            ncap *= 2;
        }
        unsigned char *nb = kl_realloc(r->alloc, t->wbuf, t->wcap, ncap);
        if (!nb) { leg->naddrs = 0; dns_leg_settle(r, q, leg); return; }
        t->wbuf = nb; t->wcap = ncap;
    }
    t->wbuf[t->wlen++] = (uint8_t)(qlen >> 8);
    t->wbuf[t->wlen++] = (uint8_t)(qlen & 0xFF);
    memcpy(t->wbuf + t->wlen, qb, qlen);
    t->wlen += qlen;

    leg->tcp_pending = 1;
    leg->tcp_ns = ns_idx;
    if (leg->timer_id >= 0)
        kl_timer_cancel(r->ctx, leg->timer_id);
    leg->timer_id = kl_timer_add(r->ctx, (uint64_t)r->timeout_ms, dns_on_leg_timer, leg);
    if (t->idle_timer >= 0) { kl_timer_cancel(r->ctx, t->idle_timer); t->idle_timer = -1; }

    if (t->state == DNS_TCP_READY)
        dns_tcp_flush(r, t);
    else
        dns_tcp_update_interest(r, t);
}

#endif /* !KEEL_FREESTANDING — DNS-over-TCP fallback */

/* KlDatagramRecvFn: one datagram from the resolver's KlDatagram socket. `peer` is the source (always
 * non-NULL); `local`/`flags` are unused (DNS uses the payload's TC bit, not the UDP MSG_TRUNC flag). */
static void dns_on_recv(void *ud, const void *data, size_t len,
                        const KlSockAddr *peer, const KlSockAddr *local, unsigned flags) {
    (void)local; (void)flags;
    const KlSockAddr *src = peer;
    KlDnsResolver *r = ud;
    if (len < 12)
        return;
    int ns_idx = src ? dns_ns_index(r, src) : -1;
    if (ns_idx < 0)
        return;                                  /* not from a nameserver — ignore (anti-spoof) */
    const uint8_t *pkt = data;
    uint16_t id = (uint16_t)((pkt[0] << 8) | pkt[1]);
    KlDnsLeg *leg = dns_find_leg(r, id);
    if (!leg)
        return;                                  /* stale or unknown id */
    KlDnsReq *q = leg->req;

    /* Truncation (TC bit): recover the answer over TCP (RFC 7766) — but only when
     * the response genuinely echoes our question, so a spoofed TC can't force TCP. */
    if ((pkt[2] & 0x02) && !leg->tcp_pending) {
        if (dns_question_matches(pkt, len, leg->question, leg->question_len)) {
#ifndef KEEL_FREESTANDING
            dns_tcp_send_leg(r, leg, ns_idx);    /* recover over TCP (RFC 7766) */
#else
            /* No TCP fallback in the UDP-only freestanding build: settle the leg
             * empty so the resolution fails promptly and clearly, rather than
             * silently waiting for the per-leg timeout. */
            leg->naddrs = 0;
            dns_leg_settle(r, q, leg);
#endif
        }
        return;
    }

    /* DNS cookies (RFC 7873): verify the echoed client cookie (a strong off-path
     * anti-spoof layered on txn-id/0x20/source), learn the server cookie, and on
     * BADCOOKIE re-issue once carrying it. A response without a cookie option
     * (server doesn't support cookies) is accepted for backward compatibility. */
    if (!r->disable_cookies && !r->disable_edns) {
        /* Zero-init ck_client: dns_extract_opt fills it before setting have=1,
         * but that cross-function contract is opaque to gcc's -O2
         * maybe-uninitialized analysis (newer MinGW gcc flags it under -Werror). */
        uint8_t ext = 0, ck_client[DNS_COOKIE_CLIENT] = {0};
        uint8_t ck_server[DNS_COOKIE_SRV_MAX], ck_slen = 0;
        int have = 0;
        if (dns_extract_opt(pkt, len, &ext, ck_client, ck_server, &ck_slen, &have) == 0 &&
            have) {
            KlDnsCookie *c = &r->cookie[ns_idx];
            if (c->have_client &&
                memcmp(ck_client, c->client, DNS_COOKIE_CLIENT) != 0)
                return;                          /* client-cookie mismatch → spoof, ignore */
            if (ck_slen) {
                memcpy(c->server, ck_server, ck_slen);
                c->server_len = ck_slen;
            }
            if ((((int)ext << 4) | (pkt[3] & 0x0F)) == DNS_RCODE_BADCOOKIE) {
                if (leg->cookie_retry < DNS_COOKIE_RETRY_MAX && leg->tries_left > 0) {
                    leg->cookie_retry++;
                    leg->ns_idx = ns_idx;        /* retry the same NS with its cookie */
                    if (leg->timer_id >= 0) {
                        kl_timer_cancel(r->ctx, leg->timer_id);
                        leg->timer_id = -1;
                    }
                    DnsTxResult tr = dns_transmit_leg(r, q, leg);
                    if (tr == DNS_TX_SENT)
                        return;                  /* awaiting the cookie'd retry */
                    if (tr == DNS_TX_WOULDBLOCK && dns_leg_mark_pending(r, leg) == 0)
                        return;                  /* transient backpressure → retry on the writable edge */
                }
                leg->naddrs = 0;                 /* retries exhausted / permanent failure → settle empty */
                dns_leg_settle(r, q, leg);
                return;
            }
        }
    }

    KlResolveResult scratch;
    memset(&scratch, 0, sizeof(scratch));
    if (kl_dns_parse_response(pkt, len, id, leg->qtype,
                              leg->question, leg->question_len, &scratch) == 0) {
        /* Collect this leg's addresses; arm the resolution delay on first data. */
        leg->naddrs = scratch.naddrs;
        for (int i = 0; i < scratch.naddrs; i++)
            leg->addrs[i] = scratch.addrs[i];
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
    /* Numeric IPv4/IPv6 literal → neutral KlSockAddr (kl_sockaddr_parse does NO name resolution, so
     * both families collapse into one freestanding-clean call). */
    if (kl_sockaddr_parse(&out->addrs[0], host, (uint16_t)port) == 0) {
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
#ifdef KEEL_FREESTANDING
    /* No hosts-file lookup in the freestanding (no-filesystem) build. */
    (void)path; (void)host; (void)port; (void)prefer_ipv6; (void)out;
    return 0;
#else
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
    if ((prefer_ipv6 && have_v6) || (!have_v4 && have_v6))
        kl_sockaddr_from_ipv6(&out->addrs[0], (const uint8_t *)&a6, (uint16_t)port, 0);
    else
        kl_sockaddr_from_ipv4(&out->addrs[0], (const uint8_t *)&a4, (uint16_t)port);
    out->naddrs = 1;
    out->ai_socktype = SOCK_STREAM;
    return 1;
#endif /* !KEEL_FREESTANDING */
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
    if (dns_copy(q->host, sizeof(q->host), host) != 0) {   /* reject an over-long name (can't be a DNS name) */
        kl_free(r->alloc, q, sizeof(*q));
        return NULL;
    }

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
    dns_tcp_touch_idle_all(r);   /* a TCP-pending leg may have just vanished */
}

/* Owner reclaim for kl_datagram_teardown (§4a destructive tail): free the resolver itself. Runs once the
 * datagram has reclaimed its own state — synchronously on readiness / when no callback frame is active,
 * or at the outermost frame leave if destroy was reached from within a datagram callback. Either way it
 * is the last action of the teardown, so `r` (which embeds the now-detached facade) is safe to free. */
static void dns_free_self(void *ud) {
    KlDnsResolver *r = ud;
    kl_free(r->alloc, r, sizeof(*r));
}

/* The real teardown body. Frees the TCP conns + in-flight requests, then reclaims the datagram + the
 * resolver via kl_datagram_teardown (dns_free_self is its destructive-tail owner reclaim). Reached either
 * directly from dns_destroy (no done callback active) or, when destroy was requested from within a done
 * callback, from dns_complete's tail once it is finished touching `r` (in_done back to 0). DO NOT touch
 * `r` after the kl_datagram_teardown call — it may free `r` synchronously (no datagram frame) or defer to
 * the outermost datagram frame leave (recv delivery). */
static void dns_teardown(KlDnsResolver *r) {
#ifndef KEEL_FREESTANDING
    for (int i = 0; i < r->nns; i++)
        if (kl_handle_valid(r->tcp[i].fd))
            dns_tcp_close(r, &r->tcp[i]);          /* close persistent TCP conns */
#endif
    KlDnsReq *q = r->inflight;
    while (q) {
        KlDnsReq *next = q->next;
        dns_cancel_timers(r, q);
        kl_free(r->alloc, q, sizeof(*q));
        q = next;
    }
    r->inflight = NULL;
    kl_datagram_teardown(&r->sock, dns_free_self, r);
}

static void dns_destroy(KlResolver *self) {
    KlDnsResolver *r = (KlDnsResolver *)self;
    if (!r)
        return;
    /* Reentrant destruction from ANY completion source: if a user done() callback is on the stack (recv
     * OR timer driven), DEFER the teardown — just mark it. dns_complete runs it at its destructive tail,
     * after it is done touching `r`. This complements the datagram-level busy-frame deferral (which only
     * covers recv-delivery-driven destruction). */
    if (r->in_done) {
        r->destroy_requested = 1;
        return;
    }
    dns_teardown(r);
}

/* ── Nameserver selection ────────────────────────────────────────────── */

/* Parse "IP" or "IP#port" into a sockaddr (the #port form is a testing/advanced
 * extension; standard resolv.conf entries have no port and default to 53). */
static int dns_parse_ns(const char *s, uint16_t defport, KlSockAddr *out) {
    char buf[80];
    size_t sl = strlen(s);
    if (sl >= sizeof(buf))
        return -1;                /* nameserver token too long */
    memcpy(buf, s, sl + 1);
    uint16_t port = defport;
    char *hash = (char *)kl_strchr(buf, '#');   /* resolv.conf "ip#port" extension */
    if (hash) {
        *hash = '\0';
        const char *ps = hash + 1;
        uint16_t p;
        /* Full-token decimal, 1..65535 (kl_parse_u16_decimal rejects empty / non-digit / >65535). */
        if (kl_parse_u16_decimal(ps, strlen(ps), &p) == 0 && p > 0)
            port = p;
    }
    /* Numeric IPv4/IPv6 literal → neutral KlSockAddr (a nameserver is always a literal, never a
     * hostname — kl_sockaddr_parse does NO name resolution, so this stays freestanding-clean). */
    return kl_sockaddr_parse(out, buf, port);
}

/* Fill the resolver's nameserver list; returns the (single) socket family. */
static int dns_build_ns_list(KlDnsResolver *r, const KlDnsResolverConfig *cfg, int *family) {
    char nsbuf[DNS_MAX_NS][KL_DNS_SYS_NS_STRMAX];
    int count = 0;
    uint16_t defport = (cfg && cfg->port) ? cfg->port : DNS_PORT;

    r->ndots = 1;
    if (cfg && cfg->nameserver) {
        /* Explicit nameserver overrides system discovery (no search domains). */
        if (dns_copy(nsbuf[0], sizeof(nsbuf[0]), cfg->nameserver) != 0)
            return -1;                          /* nameserver token too long */
        count = 1;
    } else {
#ifdef KEEL_FREESTANDING
        /* Freestanding (UDP-only) DNS has no resolv.conf discovery — the caller MUST supply a
         * nameserver via cfg->nameserver (see the file-header note). */
        return -1;
#else
        const char *rc = (cfg && cfg->resolv_conf_path) ? cfg->resolv_conf_path : NULL;
        count = kl_dns_sys_nameservers(r->alloc, rc, nsbuf, DNS_MAX_NS);
        kl_dns_sys_resolv_options(r->alloc, rc, r->search, DNS_MAX_SEARCH,
                                  &r->nsearch, &r->ndots);
        if (count == 0) {
            snprintf(nsbuf[0], sizeof(nsbuf[0]), "127.0.0.1");
            count = 1;
        }
#endif
    }

    /* Parse; keep only nameservers of the first one's family (one socket). */
    int nns = 0;
    KlAddrFamily fam = KL_AF_UNSPEC;
    for (int i = 0; i < count && nns < DNS_MAX_NS; i++) {
        KlSockAddr ns;
        if (dns_parse_ns(nsbuf[i], defport, &ns) != 0)
            continue;
        KlAddrFamily f = kl_sockaddr_family(&ns);
        if (fam == KL_AF_UNSPEC)
            fam = f;
        else if (f != fam)
            continue;
        r->ns[nns] = ns;
        nns++;
    }
    if (nns == 0)
        return -1;
    r->nns = nns;
    /* KlDatagramSocketConfig.family is a host domain (AF_INET/AF_INET6); map from the neutral family,
     * exactly as the freestanding client (http_client_async.c) does when opening a socket. */
    *family = (fam == KL_AF_INET6) ? AF_INET6 : AF_INET;
    return 0;
}

/* ── Constructor ─────────────────────────────────────────────────────── */

/* Internal creator with an explicit outbound-slot count (NOT in the public header). send_slots 0 = the
 * DNS_SEND_SLOTS default; a positive value is a TEST hook to force KL_DATAGRAM_WOULD_BLOCK deterministically
 * (the transient-burst size is an internal transport detail, not a public config knob). The public
 * kl_dns_resolver_create below delegates with 0. */
KlResolver *kl_dns_resolver_create_slots(KlEventCtx *ctx, const KlDnsResolverConfig *cfg, int send_slots) {
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
    r->send_slots = (send_slots > 0) ? send_slots : DNS_SEND_SLOTS;
    r->prefer_ipv6 = cfg ? cfg->prefer_ipv6 : 0;
    r->disable_0x20 = cfg ? cfg->disable_0x20 : 0;
    r->disable_edns = cfg ? cfg->disable_edns : 0;
    r->disable_cookies = cfg ? cfg->disable_cookies : 0;
#ifndef KEEL_FREESTANDING
    snprintf(r->hosts_path, sizeof(r->hosts_path), "%s",
             (cfg && cfg->hosts_path) ? cfg->hosts_path
                                      : kl_dns_sys_default_hosts_path());
#else
    r->hosts_path[0] = '\0';            /* no hosts file in the freestanding build */
#endif
    r->rnd_off = sizeof(r->rnd_pool);   /* pool empty → refills on first draw */
#ifndef KEEL_FREESTANDING
    for (int i = 0; i < DNS_MAX_NS; i++) {
        r->tcp[i].fd = KL_INVALID_SOCKET;
        r->tcp[i].idle_timer = -1;
    }
#endif

    int family = AF_INET;
    if (dns_build_ns_list(r, cfg, &family) != 0) {
        kl_free(alloc, r, sizeof(*r));
        return NULL;
    }

    /* Unconnected socket — sends target each nameserver by address (multi-NS); dns_on_recv verifies the
     * source is a configured nameserver. Prepare the fd provider-neutrally (M0), then adopt it into a
     * fixed-slot KlDatagram (M3). The prep socket-option config carries family only — no bind (ephemeral
     * source port), no extensions (want_caps 0; the resolver reads neither `local` nor recv-TOS). */
    KlDatagramSocketConfig uc = { .family = family };
    KlDatagramPrep prep;
    if (kl_datagram_open(ctx->sockets, &uc, &prep) != 0) {
        kl_free(alloc, r, sizeof(*r));
        return NULL;
    }
    KlDatagramConfig dc = {
        .ctx = ctx, .alloc = alloc, .sockets = ctx->sockets, .fd = prep.fd,
        .send_slots = (size_t)r->send_slots, .send_slot_cap = DNS_QUERY_MAX,
        .recv_cap = DNS_RECV_CAP, .want_caps = 0,
    };
    if (kl_datagram_init(&r->sock, &dc) != 0) {
        kl_sock_close(ctx->sockets, prep.fd);   /* init failed → fd stays with the caller (M0/init contract) */
        kl_free(alloc, r, sizeof(*r));
        return NULL;
    }
    /* Register the writable-edge handler ONCE (D-DNS-3): it re-sends every send_pending leg. */
    kl_datagram_on_writable(&r->sock, dns_on_writable, r);
    if (kl_datagram_recv_start(&r->sock, dns_on_recv, r) != 0) {
        kl_datagram_teardown(&r->sock, NULL, NULL);   /* synchronous abandon (no owner reclaim: r freed below) */
        kl_free(alloc, r, sizeof(*r));
        return NULL;
    }
    return &r->base;
}

KlResolver *kl_dns_resolver_create(KlEventCtx *ctx, const KlDnsResolverConfig *cfg) {
    return kl_dns_resolver_create_slots(ctx, cfg, 0);   /* 0 = default outbound-slot count */
}
