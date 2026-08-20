/*
 * raw_dns_test.c — LC-3: KEEL's OWN async DNS resolver (src/dns_resolver.c) resolving a name
 * over the lwIP-raw completion backend, then a plaintext KlHttpClient GET / -> 200. In-process over
 * the loopback netif (NO_SYS=1, single-thread).
 *
 * The whole point of LC-3 (docs/phase10_lwip_raw_client_design.md, as corrected on review): the
 * built-in DNS resolver rides KEEL's OWN canonical KlDatagram — the same datagram path that LC-3a
 * (#194) made work over lwip-raw. So there is ONE UDP/DNS path everywhere. kl_dns_resolver_create(ctx,
 * cfg) on a ctx whose ctx.sockets = kl_socket_provider_lwip_raw() resolves names over lwIP with NO
 * changes to src/dns_resolver.c: the resolver's KlDatagram query socket becomes a udp_pcb,
 * kl_datagram_send -> udp_sendto over loopif, the reply lands in the glue's udp recv ring and surfaces
 * as KL_COMP_DGRAM_RECV, and dns_on_recv parses it with KEEL's own kl_dns_parse_response. The TCP
 * fallback (RFC 7766) would ride ctx->sockets' SOCK_STREAM connect (LC-1) too, but a small UDP answer
 * never triggers it here.
 *
 * The in-process DNS responder is a public KlDatagram (not lwIP's dns), riding the SAME canonical
 * datagram-over-raw path the resolver's own query socket uses (M3 migrated the production resolver to
 * KlDatagram) — responder and resolver genuinely share one datagram data-plane. It
 * binds 127.0.0.1:<dns_port>, parses each query's question section, and replies with a hard-coded
 * A record (test.local -> 127.0.0.1); AAAA queries get a NODATA (0-answer) reply so that leg
 * settles promptly (the loopif is IPv4-only). It echoes the question bytes VERBATIM (preserving
 * DNS 0x20 case randomization) and omits the EDNS0 COOKIE option (a cookie-less reply is accepted
 * for backward compat, dns_resolver.c) — so no cookie machinery is needed in the responder.
 *
 * Coverage:
 *   A1  resolve("test.local") over lwip-raw via the KlDatagram responder -> 127.0.0.1 (the A leg).
 *   A2  resolve("127.0.0.1") numeric literal -> the deferred-literal fast path (no query, no
 *         responder); exercises the resolver's non-blocking literal completion.
 *   B1  a full KlHttpClient GET http://test.local:<port>/ with the built-in DNS resolver -> 200 +
 *         byte-exact body: resolve over raw + Happy-Eyeballs connect over raw + request/response.
 *
 * src/dns_resolver.c is UNCHANGED — only the raw provider + glue (from LC-1/LC-3a) supply the
 * transport. Prints "LC-3 PASS". Must be ASan+UBSan+LSan-clean.
 *
 * SPDX-License-Identifier: MIT
 */
#include <keel/keel.h>
#include <keel/dns_resolver.h>
#include <keel/resolver.h>
#include <keel/datagram.h>
#include <keel/datagram_detail.h>   /* complete KlDatagram type — the responder embeds one */
#include <keel/event_ctx.h>
#include <keel/allocator.h>
#include <keel/sockaddr.h>
#include <keel/timer.h>

#include "keel_lwip_raw.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int fail(const char *msg) { printf("LC-3 FAIL: %s\n", msg); return 1; }

#define DNS_PORT 5533   /* responder port (>1024, no privilege needed) */

/* ── in-process DNS responder (a public KlDatagram over the same datagram-over-raw path) ────────
 * Parses each incoming query's question section and replies with a hard-coded A record for
 * test.local -> 127.0.0.1 (or a NODATA reply for AAAA / other names), echoing the question bytes
 * verbatim. State lives in one struct so cleanup is exactly-once. Holds its ctx so the public close
 * lifecycle (close_cancel -> pump -> free) can drive to CLOSED. */
typedef struct {
    KlDatagram  sock;
    KlEventCtx *ctx;
    int    inited;
    int    queries;      /* count of queries answered (diagnostic) */
} DnsResponder;

/* Public close lifecycle for a responder socket: abortive close, pump until CLOSED, free. */
static void dg_close(KlEventCtx *ctx, KlDatagram *dg) {
    if (kl_datagram_close_state(dg) != KL_DGRAM_CLOSE_CLOSED)
        (void)kl_datagram_close_cancel(dg);
    for (int i = 0; i < 300 && kl_datagram_close_state(dg) != KL_DGRAM_CLOSE_CLOSED; i++)
        kl_event_ctx_run(ctx, 16, 5);
    kl_datagram_free(dg);
}

/* Skip a DNS name starting at off (labels until a 0 length byte). No compression is used in a
 * query's QNAME, so a simple label walk suffices. Returns the offset just past the terminating 0,
 * or 0 on malformed input. */
static size_t resp_skip_qname(const uint8_t *pkt, size_t len, size_t off) {
    while (off < len) {
        uint8_t b = pkt[off];
        if (b == 0) return off + 1;
        if ((b & 0xC0) != 0) return 0;   /* a query QNAME has no compression / reserved labels */
        off += 1u + b;
    }
    return 0;
}

/* Does the QNAME at offset 12 (case-insensitively) equal "test.local"? */
static int resp_qname_is_test_local(const uint8_t *pkt, size_t len) {
    static const char *want = "test.local";
    size_t off = 12;
    size_t wi = 0, wlen = strlen(want);
    while (off < len) {
        uint8_t b = pkt[off++];
        if (b == 0) return wi == wlen;           /* consumed the whole expected name */
        if ((b & 0xC0) != 0) return 0;
        if (wi != 0) {                            /* label separator '.' between labels */
            if (wi >= wlen || want[wi] != '.') return 0;
            wi++;
        }
        for (uint8_t i = 0; i < b; i++) {
            if (off >= len || wi >= wlen) return 0;
            char c = (char)pkt[off++];
            char lc = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
            if (lc != want[wi++]) return 0;
        }
    }
    return 0;
}

static void resp_on_query(void *ud, const void *data, size_t len,
                          const KlSockAddr *peer, const KlSockAddr *local, unsigned flags) {
    (void)local; (void)flags;
    DnsResponder *dr = ud;
    const uint8_t *q = data;
    if (!peer || len < 12) return;

    /* Locate the question section: QNAME (from offset 12) + QTYPE + QCLASS. */
    size_t qend = resp_skip_qname(q, len, 12);
    if (qend == 0 || qend + 4 > len) return;
    uint16_t qtype = (uint16_t)((q[qend] << 8) | q[qend + 1]);
    size_t qsec_len = (qend + 4) - 12;            /* question-section bytes to echo verbatim */

    /* Build the response: 12-byte header + the echoed question + (for A) one answer RR. */
    uint8_t buf[600];
    size_t n = 0;
    if (12 + qsec_len > sizeof(buf)) return;
    buf[0] = q[0]; buf[1] = q[1];                 /* echo the transaction ID */
    buf[2] = 0x81;                                /* QR=1, Opcode=0, AA=0, TC=0, RD=1 */
    buf[3] = 0x80;                                /* RA=1, rcode=0 (NOERROR) */
    int answer = (qtype == KL_DNS_TYPE_A && resp_qname_is_test_local(q, len)) ? 1 : 0;
    buf[4] = 0; buf[5] = 1;                        /* QDCOUNT = 1 */
    buf[6] = 0; buf[7] = (uint8_t)answer;         /* ANCOUNT = 1 (A) or 0 (NODATA) */
    buf[8] = 0; buf[9] = 0;                        /* NSCOUNT = 0 */
    buf[10] = 0; buf[11] = 0;                      /* ARCOUNT = 0 (no OPT echoed; cookie-less) */
    n = 12;
    memcpy(buf + n, q + 12, qsec_len);            /* echo the question VERBATIM (0x20 case) */
    n += qsec_len;
    if (answer) {
        buf[n++] = 0xC0; buf[n++] = 0x0C;         /* NAME: pointer to the question name (off 12) */
        buf[n++] = 0x00; buf[n++] = 0x01;         /* TYPE = A */
        buf[n++] = 0x00; buf[n++] = 0x01;         /* CLASS = IN */
        buf[n++] = 0x00; buf[n++] = 0x00;         /* TTL (high) */
        buf[n++] = 0x00; buf[n++] = 0x3C;         /* TTL = 60 */
        buf[n++] = 0x00; buf[n++] = 0x04;         /* RDLENGTH = 4 */
        buf[n++] = 127; buf[n++] = 0; buf[n++] = 0; buf[n++] = 1;   /* RDATA = 127.0.0.1 */
    }
    dr->queries++;
    /* reply to the resolver's ephemeral port */
    (void)kl_datagram_send(&dr->sock, &(KlDatagramMessage){ .data = buf, .len = n, .peer = peer, .tos = -1 });
}

static int responder_init(DnsResponder *dr, KlEventCtx *ctx) {
    memset(dr, 0, sizeof(*dr));
    dr->ctx = ctx;
    KlDatagramSocketConfig uc = { .ctx = ctx, .bind_addr = "127.0.0.1", .bind_port = DNS_PORT };
    if (kl_datagram_socket_init(&dr->sock, &uc) != 0) return -1;
    if (kl_datagram_recv_start(&dr->sock, resp_on_query, dr) != 0) { dg_close(ctx, &dr->sock); return -1; }
    dr->inited = 1;
    return 0;
}
static void responder_free(DnsResponder *dr) {
    if (dr->inited) { dg_close(dr->ctx, &dr->sock); dr->inited = 0; }
}

/* ── A1/A2: standalone resolver captures ──────────────────────────────────────── */
static atomic_int g_done;
static atomic_int g_error;
static char       g_ip[KL_SOCKADDR_STRLEN];
static uint16_t   g_port;

static void on_resolved(KlResolveReq *req, const KlResolveResult *res, int error, void *ud) {
    (void)req; (void)ud;
    g_ip[0] = '\0';
    g_port = 0;
    if (error == 0 && res && res->naddrs > 0) {
        kl_sockaddr_format_ip(&res->addrs[0], g_ip, sizeof(g_ip));
        g_port = kl_sockaddr_port(&res->addrs[0]);
    }
    atomic_store(&g_error, error);
    atomic_store(&g_done, 1);
}

static void pump_until_done(KlEventCtx *ctx, int ticks) {
    for (int i = 0; i < ticks && !atomic_load(&g_done); i++)
        kl_event_ctx_run(ctx, 16, 5);
}

/* A1 — resolve "test.local" over the raw UDP path via the KlDatagram responder. */
static int a1_resolve_name(KlEventCtx *ctx) {
    atomic_store(&g_done, 0);
    atomic_store(&g_error, -1);

    DnsResponder dr;
    if (responder_init(&dr, ctx) != 0) return fail("A1: responder init");

    KlDnsResolverConfig cfg = { .nameserver = "127.0.0.1", .port = DNS_PORT,
                                .timeout_ms = 1000, .attempts = 1 };
    KlResolver *r = kl_dns_resolver_create(ctx, &cfg);
    if (!r) { responder_free(&dr); return fail("A1: resolver create"); }

    const KlResolveReq *req = r->resolve(r, ctx, "test.local", 8080, on_resolved, NULL);
    if (!req) { r->destroy(r); responder_free(&dr); return fail("A1: resolve start"); }

    pump_until_done(ctx, 800);

    int rc = 0;
    if (!atomic_load(&g_done)) rc = fail("A1: resolve did not complete (no responder reply?)");
    else if (atomic_load(&g_error) != 0) rc = fail("A1: resolve returned error");
    else if (strcmp(g_ip, "127.0.0.1") != 0) { printf("   [diag] got ip=%s port=%u\n", g_ip, g_port); rc = fail("A1: wrong resolved address"); }
    else if (g_port != 8080) { printf("   [diag] port=%u\n", g_port); rc = fail("A1: wrong resolved port"); }

    r->destroy(r);
    responder_free(&dr);
    if (rc) return rc;
    printf("PASS A1 (resolve test.local -> 127.0.0.1:8080 over lwip-raw KlDatagram)\n");
    return 0;
}

/* A2 — resolve a numeric literal: the resolver's deferred-literal fast path (no query, no
 * responder). dns_resolver.c completes a literal via a 0ms timer, so it fires on the next tick —
 * a non-blocking, no-network completion. Exercises the literal/no-query path over the raw loop. */
static int a2_resolve_literal(KlEventCtx *ctx) {
    atomic_store(&g_done, 0);
    atomic_store(&g_error, -1);

    KlDnsResolverConfig cfg = { .nameserver = "127.0.0.1", .port = DNS_PORT,
                                .timeout_ms = 1000, .attempts = 1 };
    KlResolver *r = kl_dns_resolver_create(ctx, &cfg);
    if (!r) return fail("A2: resolver create");

    const KlResolveReq *req = r->resolve(r, ctx, "127.0.0.1", 9090, on_resolved, NULL);
    if (!req) { r->destroy(r); return fail("A2: resolve start"); }

    pump_until_done(ctx, 200);

    int rc = 0;
    if (!atomic_load(&g_done)) rc = fail("A2: literal did not complete");
    else if (atomic_load(&g_error) != 0) rc = fail("A2: literal returned error");
    else if (strcmp(g_ip, "127.0.0.1") != 0 || g_port != 9090) rc = fail("A2: wrong literal result");

    r->destroy(r);
    if (rc) return rc;
    printf("PASS A2 (numeric literal 127.0.0.1:9090 via deferred-literal fast path)\n");
    return 0;
}

/* ── B1: full client GET over raw, resolving via the built-in DNS resolver ──────── */
#define WANT_BODY "{\"lc3\":true,\"raw\":\"dns\",\"n\":424242}"

static void handle_root(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)req; (void)ud;
    kl_http_response_status(res, 200);
    kl_http_response_header(res, "Content-Type", "application/json");
    kl_http_response_body_copy(res, WANT_BODY, sizeof(WANT_BODY) - 1);
}

typedef struct {
    KlHttpServer     *srv;
    DnsResponder *dr;
    KlResolver   *resolver;
    const char   *url;
    KlHttpClient     *client;
    atomic_int    done;
    atomic_int    pass;
} ClientCase;

static void b1_on_done(KlHttpClient *client, void *ud) {
    ClientCase *cc = ud;
    int err = kl_http_client_error(client);
    int ok = 0;
    if (err == 0) {
        const KlHttpClientResponse *r = kl_http_client_response(client);
        ok = (r && r->status == 200 && r->body_len == sizeof(WANT_BODY) - 1 &&
              r->body && memcmp(r->body, WANT_BODY, sizeof(WANT_BODY) - 1) == 0);
        if (!ok && r) printf("   [diag] status=%d body_len=%zu\n", r->status, r->body_len);
    } else {
        printf("   [diag] client error=%d last_error=%d\n", err, (int)kl_http_client_last_error(client));
    }
    atomic_store(&cc->pass, ok);
    atomic_store(&cc->done, 1);
    kl_http_server_stop(cc->srv);
}

/* Fired on the loop thread: stand up the responder + the built-in DNS resolver, then start the
 * async client at a hostname URL. Everything rides the server's shared ctx (the one lwIP loop). */
static void b1_start(void *ud) {
    ClientCase *cc = ud;
    KlEventCtx *ctx = &cc->srv->ev;

    if (responder_init(cc->dr, ctx) != 0) {
        atomic_store(&cc->pass, 0); atomic_store(&cc->done, 1); kl_http_server_stop(cc->srv); return;
    }
    KlDnsResolverConfig dcfg = { .nameserver = "127.0.0.1", .port = DNS_PORT,
                                 .timeout_ms = 2000, .attempts = 1 };
    cc->resolver = kl_dns_resolver_create(ctx, &dcfg);
    if (!cc->resolver) {
        atomic_store(&cc->pass, 0); atomic_store(&cc->done, 1); kl_http_server_stop(cc->srv); return;
    }

    static KlAllocator alloc;   /* stable storage: KlHttpClientResponse stores the allocator by value */
    alloc = kl_allocator_default();
    KlHttpClientConfig ccfg = { .timeout_ms = 5000, .resolver = cc->resolver };
    cc->client = kl_http_client_start(ctx, &alloc, &ccfg, "GET", cc->url, NULL, 0, NULL, 0,
                                 b1_on_done, cc);
    if (!cc->client) {
        atomic_store(&cc->pass, 0); atomic_store(&cc->done, 1); kl_http_server_stop(cc->srv);
    }
}

static void *server_thread(void *a) { kl_http_server_run((KlHttpServer *)a); return NULL; }

static int b1_client_get(void) {
    const int port = 7860;
    KlHttpServerConfig cfg = { .port = port, .bind_addr = "127.0.0.1",
                     .event_provider = kl_event_provider_lwip_raw() };
    KlHttpServer srv;
    if (kl_http_server_init(&srv, &cfg) != 0) return fail("B1: server_init");
    kl_http_server_route(&srv, "GET", "/", handle_root, NULL, NULL);

    DnsResponder dr;
    memset(&dr, 0, sizeof(dr));
    ClientCase cc;
    memset(&cc, 0, sizeof(cc));
    cc.srv = &srv;
    cc.dr  = &dr;
    cc.url = "http://test.local:7860/";
    atomic_store(&cc.done, 0);
    atomic_store(&cc.pass, 0);

    /* Marshal setup onto the loop thread (single-thread NO_SYS=1 discipline). */
    kl_timer_add(&srv.ev, 20, b1_start, &cc);

    pthread_t th;
    if (pthread_create(&th, NULL, server_thread, &srv) != 0) {
        kl_http_server_free(&srv); return fail("B1: pthread_create");
    }
    for (int i = 0; i < 800 && !atomic_load(&cc.done); i++) {
        struct timespec sl = { 0, 10 * 1000000L };
        nanosleep(&sl, NULL);
    }
    if (!atomic_load(&cc.done)) kl_http_server_stop(&srv);   /* hang guard */
    pthread_join(th, NULL);

    int rc = 0;
    if (!atomic_load(&cc.done)) rc = fail("B1: HANG");
    else if (!atomic_load(&cc.pass)) rc = fail("B1: client GET failed");

    if (cc.client)   kl_http_client_free(cc.client);
    if (cc.resolver) cc.resolver->destroy(cc.resolver);
    responder_free(&dr);
    kl_http_server_free(&srv);
    if (rc) return rc;
    printf("PASS B1 (GET http://test.local/ -> 200, resolved over built-in DNS on lwip-raw)\n");
    return 0;
}

int main(void) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    if (kl_event_ctx_init_ex(&ctx, &alloc, kl_event_provider_lwip_raw()) != 0)
        return fail("ctx init (bring up lwIP raw)");
    /* A standalone KlEventCtx leaves ctx.sockets NULL — a DNS/UDP consumer over raw MUST set it
     * to the raw provider explicitly (the same auto-wire a KlHttpServer does via native_provider). */
    ctx.sockets = kl_socket_provider_lwip_raw();

    int rc = 0;
    if (rc == 0) rc = a1_resolve_name(&ctx);
    if (rc == 0) rc = a2_resolve_literal(&ctx);

    kl_event_ctx_free(&ctx);

    /* B1 uses its own KlHttpServer (which owns its ctx) so the server run loop drives everything. */
    if (rc == 0) rc = b1_client_get();

    if (rc != 0) return 1;
    printf("LC-3 PASS\n");
    return 0;
}
