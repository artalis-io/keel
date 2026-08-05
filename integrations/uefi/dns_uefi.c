/*
 * dns_uefi.c — synchronous DNS-over-EFI_UDP4 resolver (U-5). See dns_uefi.h.
 *
 * Mirrors socket_efi_tcp4.c's EFI child/token/pump lifecycle, but for UDP4 and as a
 * one-shot synchronous resolve: CreateChild → OpenProtocol(EFI_UDP4_PROTOCOL) →
 * Configure (UseDefaultAddress=DHCP station, RemoteAddress=nameserver, RemotePort=53)
 * → Transmit the A-query token + pump → Receive token + pump → parse → SignalEvent
 * (RxData->RecycleSignal) → Configure(NULL) → CloseProtocol → DestroyChild → free.
 *
 * No libc, no errno. Bounds-safe DNS parse (untrusted network input): every read is
 * checked against the response length and the name/pointer walk is capped.
 */

#include "dns_uefi.h"
#include "allocator_uefi.h"                 /* kl_uefi_allocator */
#include "../../spikes/uefi/efi_udp4.h"     /* EFI_UDP4_* types + GUIDs */
#include "../../src/platform.h"             /* kl_plat_random */

#include <keel/sockaddr.h>

#include <stdint.h>
#include <stddef.h>

/* ── DNS wire constants (no magic numbers) ──────────────────────────────────── */
#define DNS_PORT          53
#define DNS_TYPE_A        1
#define DNS_CLASS_IN      1
#define DNS_HDR_LEN       12
#define DNS_FLAG_QR       0x8000   /* response bit (byte 2 high bit) */
#define DNS_FLAG_RD       0x0100   /* recursion desired */
#define DNS_RCODE_MASK    0x000F
#define DNS_MAX_LABELS    128      /* name-walk cap */
#define DNS_MAX_LABEL_LEN 63
#define DNS_QUERY_CAP     512      /* our query buffer (well under a UDP MTU) */
#define DNS_RESP_CAP      1500     /* firmware fragment coalesce target */
#define DNS_ANSWER_FIXED  10       /* TYPE(2)+CLASS(2)+TTL(4)+RDLENGTH(2) */

/* Bounded pump: Poll()+CheckEvent()+Stall(1ms) per spin. A per-op stop so a wedged
 * UDP op fails rather than spins forever (mirrors socket_efi_tcp4.c). */
#ifndef KL_U5_PUMP_SPINS
#define KL_U5_PUMP_SPINS 20000     /* ~20 s worst case at 1 ms/spin */
#endif
#define KL_U5_PUMP_STALL_US 1000

/* Configure()/DHCP settle: retry EFI_NO_MAPPING while the default address binds. */
#ifndef KL_U5_DHCP_RETRIES
#define KL_U5_DHCP_RETRIES 40
#endif
#define KL_U5_DHCP_STALL_US 500000  /* 0.5 s between Configure retries */

/* Token completion events are bare (type 0): CheckEvent-polled, no NotifyFunction. */
#define KL_U5_EVT_TOKEN 0

/* ── file-scope stash (kl_resolve_sync's signature has no bs/image) ─────────── */
static EFI_BOOT_SERVICES *g_bs;
static EFI_HANDLE         g_image;
static uint8_t            g_last_ip[4];
static int                g_have_last;
static uint16_t           g_qid_counter;    /* fallback query-id source */

static EFI_GUID g_udp4_sb_guid = EFI_UDP4_SERVICE_BINDING_PROTOCOL_GUID;
static EFI_GUID g_udp4_guid    = EFI_UDP4_PROTOCOL_GUID;

/* ── small freestanding helpers (no libc) ───────────────────────────────────── */
static void u5_zero(void *p, size_t n) {
    unsigned char *b = (unsigned char *)p;
    for (size_t i = 0; i < n; i++) b[i] = 0;
}

/* Parse "a.b.c.d" into 4 network-order bytes. -1 on any non-numeric input. */
static int u5_parse_ipv4(const char *s, uint8_t out[4]) {
    int oct = 0, val = -1;
    const char *p = s;
    for (;;) {
        if (*p >= '0' && *p <= '9') {
            if (val < 0) val = 0;
            val = val * 10 + (*p - '0');
            if (val > 255) return -1;
        } else if (*p == '.' || *p == '\0') {
            if (val < 0 || oct > 3) return -1;
            out[oct++] = (uint8_t)val;
            val = -1;
            if (*p == '\0') break;
        } else {
            return -1;
        }
        p++;
    }
    return oct == 4 ? 0 : -1;
}

void kl_uefi_dns_init(EFI_BOOT_SERVICES *bs, EFI_HANDLE image) {
    g_bs = bs;
    g_image = image;
    g_have_last = 0;
    /* Seed the fallback query-id counter from platform entropy if available. */
    uint16_t seed = 0;
    kl_plat_random(&seed, sizeof(seed));
    g_qid_counter = seed;
}

int kl_uefi_dns_last_ip(uint8_t out[4]) {
    if (!g_have_last) return 0;
    for (int i = 0; i < 4; i++) out[i] = g_last_ip[i];
    return 1;
}

/* Draw a 16-bit transaction id from platform entropy; fall back to a counter if the
 * platform is fail-closed (kl_plat_random zeroes — spike-acceptable). */
static uint16_t u5_query_id(void) {
    uint16_t id = 0;
    kl_plat_random(&id, sizeof(id));
    if (id == 0) id = ++g_qid_counter;
    return id;
}

/* ── DNS query builder (A record, RD=1, single question, no EDNS) ───────────────
 * Encodes host as length-prefixed labels + 0 terminator, then QTYPE=A, QCLASS=IN.
 * Returns the total query length, or -1 on a malformed host / capacity overflow.
 * Also returns the question span [DNS_HDR_LEN, *q_len) for response verification. */
static int u5_build_query(uint8_t *buf, size_t cap, uint16_t id, const char *host,
                          size_t *q_len) {
    if (cap < DNS_HDR_LEN) return -1;
    u5_zero(buf, DNS_HDR_LEN);
    buf[0] = (uint8_t)(id >> 8);
    buf[1] = (uint8_t)(id & 0xFF);
    buf[2] = (uint8_t)(DNS_FLAG_RD >> 8);   /* RD = recursion desired */
    buf[5] = 0x01;                          /* QDCOUNT = 1 */

    size_t off = DNS_HDR_LEN;
    const char *p = host;
    while (*p) {
        /* find the next label boundary ('.') */
        const char *q = p;
        size_t lab = 0;
        while (*q && *q != '.') { q++; lab++; }
        if (lab == 0 || lab > DNS_MAX_LABEL_LEN) return -1;
        if (off + 1 + lab > cap) return -1;
        buf[off++] = (uint8_t)lab;
        for (size_t k = 0; k < lab; k++) buf[off + k] = (uint8_t)p[k];
        off += lab;
        if (*q == '.') p = q + 1; else break;
    }
    if (off + 5 > cap) return -1;           /* root + qtype + qclass */
    buf[off++] = 0x00;                       /* root label */
    buf[off++] = (uint8_t)(DNS_TYPE_A >> 8);
    buf[off++] = (uint8_t)(DNS_TYPE_A & 0xFF);
    buf[off++] = (uint8_t)(DNS_CLASS_IN >> 8);
    buf[off++] = (uint8_t)(DNS_CLASS_IN & 0xFF);
    *q_len = off;
    return (int)off;
}

/* Advance past a DNS name at @off. Handles a 0xC0 compression pointer (terminates
 * the name for skipping — we do NOT follow it into the packet, so no OOB / loop).
 * Returns 0 with the post-name offset in *out, or -1 on OOB / malformed. */
static int u5_skip_name(const uint8_t *pkt, size_t len, size_t off, size_t *out) {
    int labels = 0;
    for (;;) {
        if (off >= len) return -1;
        uint8_t b = pkt[off];
        if ((b & 0xC0) == 0xC0) {            /* compression pointer: 2 bytes */
            if (off + 2 > len) return -1;
            *out = off + 2;
            return 0;
        }
        if ((b & 0xC0) != 0) return -1;      /* reserved label type */
        if (b == 0) { *out = off + 1; return 0; }   /* root — name ends */
        off += (size_t)1 + b;
        if (++labels > DNS_MAX_LABELS) return -1;
    }
}

/* Parse a DNS response: verify id + QR + RCODE==0, skip the question, walk the
 * answers for the FIRST A/IN/RDLENGTH==4 record → its 4 IPv4 bytes into out_ip.
 * Bounds-safe: every offset checked against @len. Returns 0 / -1. */
static int u5_parse_response(const uint8_t *pkt, size_t len, uint16_t expect_id,
                             uint8_t out_ip[4]) {
    if (!pkt || len < DNS_HDR_LEN) return -1;

    uint16_t id = (uint16_t)((pkt[0] << 8) | pkt[1]);
    if (id != expect_id) return -1;
    if (!(pkt[2] & 0x80)) return -1;                 /* QR must be set */
    if ((pkt[3] & DNS_RCODE_MASK) != 0) return -1;   /* RCODE != NOERROR */

    uint16_t qd = (uint16_t)((pkt[4] << 8) | pkt[5]);
    uint16_t an = (uint16_t)((pkt[6] << 8) | pkt[7]);

    size_t off = DNS_HDR_LEN;
    for (uint16_t i = 0; i < qd; i++) {              /* skip question section */
        if (u5_skip_name(pkt, len, off, &off) != 0) return -1;
        if (off + 4 > len) return -1;
        off += 4;                                    /* qtype + qclass */
    }

    for (uint16_t i = 0; i < an; i++) {              /* walk answers */
        if (u5_skip_name(pkt, len, off, &off) != 0) return -1;
        if (off + DNS_ANSWER_FIXED > len) return -1;
        uint16_t rtype = (uint16_t)((pkt[off] << 8) | pkt[off + 1]);
        uint16_t rclass = (uint16_t)((pkt[off + 2] << 8) | pkt[off + 3]);
        uint16_t rdlen  = (uint16_t)((pkt[off + 8] << 8) | pkt[off + 9]);
        off += DNS_ANSWER_FIXED;
        if (off + rdlen > len) return -1;
        if (rtype == DNS_TYPE_A && rclass == DNS_CLASS_IN && rdlen == 4) {
            out_ip[0] = pkt[off];
            out_ip[1] = pkt[off + 1];
            out_ip[2] = pkt[off + 2];
            out_ip[3] = pkt[off + 3];
            return 0;
        }
        off += rdlen;
    }
    return -1;                                       /* no A record */
}

/* Pump the UDP4 stack until @ev fires or the bound elapses. 1 if signaled, 0 else. */
static int u5_pump_until(EFI_BOOT_SERVICES *bs, EFI_UDP4_PROTOCOL *udp, EFI_EVENT ev) {
    for (int spins = 0; spins < KL_U5_PUMP_SPINS; spins++) {
        udp->Poll(udp);
        if (bs->CheckEvent(ev) == EFI_SUCCESS) return 1;
        bs->Stall(KL_U5_PUMP_STALL_US);
    }
    return 0;
}

int kl_uefi_dns_resolve(const char *host, uint16_t port, KlSockAddr *out) {
    if (!g_bs || !host || !out) return -1;
    EFI_BOOT_SERVICES *bs = g_bs;

    /* nameserver from -DKL_U5_NAMESERVER="a.b.c.d" (dotted quad). */
#ifndef KL_U5_NAMESERVER
#define KL_U5_NAMESERVER "10.0.2.2"
#endif
    uint8_t ns[4];
    if (u5_parse_ipv4(KL_U5_NAMESERVER, ns) != 0) return -1;

    /* Locate the UDP4 ServiceBinding. */
    UINTN nh = 0;
    EFI_HANDLE *handles = NULL;
    EFI_STATUS st = bs->LocateHandleBuffer(ByProtocol, &g_udp4_sb_guid, NULL, &nh, &handles);
    if (EFI_ERROR(st) || nh == 0 || !handles) {
        if (handles) bs->FreePool(handles);
        return -1;
    }
    EFI_SERVICE_BINDING_PROTOCOL *sb = NULL;
    st = bs->HandleProtocol(handles[0], &g_udp4_sb_guid, (VOID **)&sb);
    bs->FreePool(handles);
    if (EFI_ERROR(st) || !sb) return -1;

    /* CreateChild + OpenProtocol(EFI_UDP4_PROTOCOL). */
    EFI_HANDLE child = NULL;
    st = sb->CreateChild(sb, &child);
    if (EFI_ERROR(st) || !child) return -1;

    EFI_UDP4_PROTOCOL *udp = NULL;
    st = bs->OpenProtocol(child, &g_udp4_guid, (VOID **)&udp,
                          g_image, child, EFI_OPEN_PROTOCOL_BY_DRIVER);
    if (EFI_ERROR(st) || !udp) {
        st = bs->OpenProtocol(child, &g_udp4_guid, (VOID **)&udp,
                              g_image, child, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
    }
    if (EFI_ERROR(st) || !udp) {
        sb->DestroyChild(sb, child);
        return -1;
    }

    int rc = -1;   /* pessimistic; set to 0 on a parsed A record */

    /* Configure: DHCP station, remote = nameserver:53. Tolerate EFI_NO_MAPPING. */
    EFI_UDP4_CONFIG_DATA cfg;
    u5_zero(&cfg, sizeof(cfg));
    cfg.AcceptAnyPort      = FALSE;
    cfg.AllowDuplicatePort = TRUE;
    cfg.TimeToLive         = 64;
    cfg.ReceiveTimeout     = 0;    /* 0 = no driver-side timeout (we pump-bound) */
    cfg.TransmitTimeout    = 0;
    cfg.UseDefaultAddress  = TRUE; /* DHCP-assigned station address */
    cfg.StationPort        = 0;    /* pick an ephemeral source port */
    for (int i = 0; i < 4; i++) cfg.RemoteAddress.Addr[i] = ns[i];
    cfg.RemotePort         = DNS_PORT;

    int configured = 0;
    for (int i = 0; i < KL_U5_DHCP_RETRIES; i++) {
        st = udp->Configure(udp, &cfg);
        if (!EFI_ERROR(st)) { configured = 1; break; }
        if (st == EFI_NO_MAPPING) {          /* DHCP not settled — pump + retry */
            udp->Poll(udp);
            bs->Stall(KL_U5_DHCP_STALL_US);
            continue;
        }
        break;                               /* a real Configure error */
    }
    if (!configured) goto teardown;

    /* Build the A-query. */
    uint8_t qbuf[DNS_QUERY_CAP];
    uint16_t qid = u5_query_id();
    size_t qlen = 0;
    if (u5_build_query(qbuf, sizeof(qbuf), qid, host, &qlen) < 0) goto teardown;

    /* Create the two token events. */
    EFI_EVENT tx_ev = NULL, rx_ev = NULL;
    st = bs->CreateEvent(KL_U5_EVT_TOKEN, TPL_CALLBACK, NULL, NULL, &tx_ev);
    if (EFI_ERROR(st)) goto teardown;
    st = bs->CreateEvent(KL_U5_EVT_TOKEN, TPL_CALLBACK, NULL, NULL, &rx_ev);
    if (EFI_ERROR(st)) { bs->CloseEvent(tx_ev); goto teardown; }

    /* Transmit the query. */
    EFI_UDP4_TRANSMIT_DATA tx;
    u5_zero(&tx, sizeof(tx));
    tx.UdpSessionData = NULL;             /* use Configure()d remote (ns:53) */
    tx.GatewayAddress = NULL;
    tx.DataLength = (UINT32)qlen;
    tx.FragmentCount = 1;
    tx.FragmentTable[0].FragmentLength = (UINT32)qlen;
    tx.FragmentTable[0].FragmentBuffer = qbuf;

    EFI_UDP4_COMPLETION_TOKEN tx_tok;
    u5_zero(&tx_tok, sizeof(tx_tok));
    tx_tok.Event = tx_ev;
    tx_tok.Status = EFI_NOT_READY;
    tx_tok.Packet.TxData = &tx;

    st = udp->Transmit(udp, &tx_tok);
    if (EFI_ERROR(st)) goto close_events;
    if (!u5_pump_until(bs, udp, tx_ev)) goto close_events;
    if (EFI_ERROR(tx_tok.Status)) goto close_events;

    /* Receive the response. The firmware allocates RxData; we SignalEvent its
     * RecycleSignal when done. */
    EFI_UDP4_COMPLETION_TOKEN rx_tok;
    u5_zero(&rx_tok, sizeof(rx_tok));
    rx_tok.Event = rx_ev;
    rx_tok.Status = EFI_NOT_READY;
    rx_tok.Packet.RxData = NULL;

    st = udp->Receive(udp, &rx_tok);
    if (EFI_ERROR(st)) goto close_events;
    if (!u5_pump_until(bs, udp, rx_ev)) goto close_events;
    if (EFI_ERROR(rx_tok.Status)) goto close_events;

    EFI_UDP4_RECEIVE_DATA *rx = rx_tok.Packet.RxData;
    if (rx) {
        /* Coalesce the fragment(s) into a flat buffer (usually 1 fragment). */
        uint8_t resp[DNS_RESP_CAP];
        size_t rlen = 0;
        UINT32 fc = rx->FragmentCount;
        for (UINT32 f = 0; f < fc; f++) {
            const uint8_t *fb = (const uint8_t *)rx->FragmentTable[f].FragmentBuffer;
            UINT32 fl = rx->FragmentTable[f].FragmentLength;
            if (!fb) continue;
            for (UINT32 k = 0; k < fl && rlen < sizeof(resp); k++)
                resp[rlen++] = fb[k];
        }
        uint8_t ip[4];
        if (u5_parse_response(resp, rlen, qid, ip) == 0) {
            if (kl_sockaddr_from_ipv4(out, ip, port) == 0) {
                for (int i = 0; i < 4; i++) g_last_ip[i] = ip[i];
                g_have_last = 1;
                rc = 0;
            }
        }
        /* Return the firmware-owned buffer. */
        if (rx->RecycleSignal) bs->SignalEvent(rx->RecycleSignal);
    }

close_events:
    if (rx_ev) bs->CloseEvent(rx_ev);
    if (tx_ev) bs->CloseEvent(tx_ev);

teardown:
    udp->Configure(udp, NULL);                       /* reset config */
    bs->CloseProtocol(child, &g_udp4_guid, g_image, child);
    sb->DestroyChild(sb, child);
    return rc;
}
