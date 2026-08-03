/*
 * lwip_raw_glue.c — the lwIP-touching half of the Phase 9 raw completion backend.
 *
 * This TU includes ONLY lwIP's NO_SYS=1 raw headers — never KEEL's socket/net headers —
 * so lwIP's htons/ntohs macros and its ssize_t typedef cannot clash with the host
 * <sys/socket.h>/<netinet/in.h> that event_lwip_raw.c pulls in through the KEEL socket
 * seam. The two halves meet across lwip_raw_glue.h (opaque void* pcb/netif + neutral
 * KlLwrRecord; no lwIP type escapes). See docs/phase9_lwip_raw_design.md.
 *
 * P9-2: wires the raw tcp_* callbacks (tcp_accept/tcp_recv/tcp_sent) into a completion-
 * record queue the backend drains, plus the socket-provider primitives on tcp_pcb
 * (new/bind/listen/close/send) and a raw-API test client. A pcb↔KlConn owner association
 * (tcp_arg) lets recv/sent callbacks tag records with the owning conn. The recv callback
 * copies pbuf bytes into a per-pcb staging buffer and issues tcp_recved immediately (the
 * data is safely copied out), then enqueues a READ record carrying the byte count; the
 * backend copies staging → c->read_buf when it builds the KL_COMP_READ event.
 *
 * Backpressure / partial sends / sendfile / close-with-outstanding are P9-3..P9-5; here
 * kl_lwr_send assumes the (small) response fits tcp_sndbuf.
 *
 * SPDX-License-Identifier: MIT
 */
#include "lwip_raw_glue.h"

#include "lwip/init.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"
#include "lwip/netif.h"
#include "lwip/ip_addr.h"
#include "lwip/sys.h"
#include "lwip/pbuf.h"

#include <string.h>
#include <time.h>

/* ── NO_SYS=1 requires the port to supply sys_now() (u32 ms, monotonic) ──────────
 * Same source as the spike: clock_gettime(CLOCK_MONOTONIC). lwIP calls this from
 * sys_check_timeouts(). Lives here (the lwIP-only TU) so it links with liblwip_raw.a. */
#define KL_LWR_NS_PER_MS 1000000L
u32_t sys_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u32_t)((u32_t)ts.tv_sec * 1000u + (u32_t)(ts.tv_nsec / KL_LWR_NS_PER_MS));
}

/* ── completion-record queue (drained by the backend each tick) ──────────────────
 * NO_SYS=1 is single-threaded — the tcp_* callbacks fire inline on the mainloop tick
 * (kl_lwr_lwip_tick), so a plain ring with no locking is safe. Sized to comfortably
 * hold a P9-2 roundtrip's ACCEPT + READ + WRITE records plus slack. */
#define KL_LWR_REC_CAP 64
static KlLwrRecord g_recs[KL_LWR_REC_CAP];
static int g_rec_head = 0;   /* next slot to read */
static int g_rec_tail = 0;   /* next slot to write */

static void lwr_rec_push(const KlLwrRecord *r) {
    int next = (g_rec_tail + 1) % KL_LWR_REC_CAP;
    if (next == g_rec_head) return;   /* full — drop (P9-2 never fills it) */
    g_recs[g_rec_tail] = *r;
    g_rec_tail = next;
}

int kl_lwr_drain(KlLwrRecord *out, int max) {
    int n = 0;
    while (n < max && g_rec_head != g_rec_tail) {
        out[n++] = g_recs[g_rec_head];
        g_rec_head = (g_rec_head + 1) % KL_LWR_REC_CAP;
    }
    return n;
}

/* ── per-pcb connection state (owner + received-byte staging) ────────────────────
 * A tiny fixed table keyed by pcb. P9-2 serves one connection over loopback at a time;
 * the table is sized for a handful. Each accepted pcb gets a slot at kl_lwr_set_owner;
 * the recv callback stages bytes here so the backend can copy them into c->read_buf. */
#define KL_LWR_MAX_CONNS 8
#define KL_LWR_STAGE_CAP 8192   /* matches KEEL's KL_READ_BUF_SIZE headers window */

typedef struct {
    struct tcp_pcb *pcb;      /* NULL = free slot */
    void           *owner;    /* KlConn* the backend associated */
    unsigned char   stage[KL_LWR_STAGE_CAP];
    size_t          stage_len;
    int             closed;   /* peer closed / errored — surface a zero-length READ */
} KlLwrConn;

static KlLwrConn g_conns[KL_LWR_MAX_CONNS];

/* The current LISTEN pcb (relocated by tcp_listen); tracked so the backend can adopt it and
 * so close() can distinguish the listener from a connection pcb. */
static struct tcp_pcb *g_listen_pcb = NULL;

static KlLwrConn *lwr_conn_find(const struct tcp_pcb *pcb) {
    for (int i = 0; i < KL_LWR_MAX_CONNS; i++)
        if (g_conns[i].pcb == pcb) return &g_conns[i];
    return NULL;
}
static KlLwrConn *lwr_conn_alloc(struct tcp_pcb *pcb) {
    for (int i = 0; i < KL_LWR_MAX_CONNS; i++)
        if (g_conns[i].pcb == NULL) {
            g_conns[i].pcb = pcb;
            g_conns[i].owner = NULL;
            g_conns[i].stage_len = 0;
            g_conns[i].closed = 0;
            return &g_conns[i];
        }
    return NULL;
}
static void lwr_conn_free(const struct tcp_pcb *pcb) {
    KlLwrConn *c = lwr_conn_find(pcb);
    if (c) { c->pcb = NULL; c->owner = NULL; c->stage_len = 0; c->closed = 0; }
}

/* ── lifecycle / mainloop ──────────────────────────────────────────────────── */

static int g_lwip_inited = 0;   /* lwip_init() runs exactly once (NO_SYS=1, single-thread) */

void *kl_lwr_lwip_up(void) {
    if (!g_lwip_inited) {
        lwip_init();          /* auto-creates the loop netif via LWIP_HAVE_LOOPIF */
        g_lwip_inited = 1;
    }
    /* Find the loopback netif by its 127.0.0.1 address (the loopif symbol is not public),
     * then bring it up + default so 127.0.0.1 routes to it — exactly as the spike does. */
    struct netif *lo = NULL, *nif;
    NETIF_FOREACH(nif) {
        if (ip4_addr_isloopback(netif_ip4_addr(nif))) { lo = nif; break; }
    }
    if (!lo) return NULL;

    ip4_addr_t ip, nm, gw;
    IP4_ADDR(&ip, 127, 0, 0, 1);
    IP4_ADDR(&nm, 255, 0, 0, 0);
    IP4_ADDR(&gw, 127, 0, 0, 1);
    netif_set_addr(lo, &ip, &nm, &gw);
    netif_set_up(lo);
    netif_set_link_up(lo);
    netif_set_default(lo);
    return lo;
}

void kl_lwr_lwip_tick(void *loopif) {
    sys_check_timeouts();                          /* fire lwIP timers (retransmit, etc.) */
    if (loopif) netif_poll((struct netif *)loopif); /* drain loopback TX → RX */
}

/* ── raw tcp_* callbacks → completion records ──────────────────────────────── */

/* recv callback for an accepted (server-side) connection. A NULL pbuf = peer closed. Copies
 * the (possibly chained) pbuf into the per-pcb staging buffer and issues tcp_recved for the
 * bytes accepted (safe once copied out — see the ordering note in the design), then frees the
 * pbuf. NO record is enqueued: the backend pulls a READ from staging only once the owning conn
 * is armed (kl_comp_post_recv), which decouples delivery from this callback's timing (a fast
 * client can deliver data in the same tick as the accept, before any recv was posted). */
static err_t lwr_srv_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    (void)arg;
    KlLwrConn *cs = lwr_conn_find(tpcb);
    if (err != ERR_OK || p == NULL) {
        if (p) pbuf_free(p);
        if (cs) cs->closed = 1;   /* backend surfaces a zero-length READ → driver closes */
        return ERR_OK;
    }
    if (cs) {
        for (struct pbuf *q = p; q != NULL; q = q->next) {
            size_t room = KL_LWR_STAGE_CAP - cs->stage_len;
            size_t take = q->len < room ? q->len : room;   /* P9-2: fits; overflow dropped */
            if (take == 0) break;
            memcpy(cs->stage + cs->stage_len, q->payload, take);
            cs->stage_len += take;
        }
    }
    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

/* sent callback: `len` bytes of a posted send were acknowledged. Enqueue a WRITE record.
 * P9-2 sends the whole (small) response in one tcp_write, so one WRITE record completes it. */
static err_t lwr_srv_sent(void *arg, struct tcp_pcb *tpcb, u16_t len) {
    (void)arg;
    KlLwrConn *cs = lwr_conn_find(tpcb);
    KlLwrRecord r = { .kind = KL_LWR_WRITE, .pcb = tpcb,
                      .owner = cs ? cs->owner : NULL, .nbytes = len, .ok = 1 };
    lwr_rec_push(&r);
    return ERR_OK;
}

/* err callback: the pcb was aborted by the stack (RST, OOM, …). lwIP has already freed the
 * pcb by the time this fires, so we cannot key by pcb. arg is the KlConn* owner we set via
 * tcp_arg; find its slot, invalidate the dangling pcb pointer, and mark it closed so the
 * backend surfaces a zero-length READ (the driver then releases the conn). */
static void lwr_srv_err(void *arg, err_t err) {
    (void)err;
    /* Mark the owning slot closed (keyed by owner, not pcb — the pcb is being freed). The pcb
     * pointer value is retained only for status matching by value (never dereferenced after
     * this); the backend's close path frees the slot. P9-2's clean loopback never hits this. */
    for (int i = 0; i < KL_LWR_MAX_CONNS; i++)
        if (g_conns[i].owner == arg && g_conns[i].pcb != NULL) {
            g_conns[i].closed = 1;
            break;
        }
}

static err_t lwr_srv_accept(void *arg, struct tcp_pcb *newpcb, err_t err) {
    (void)arg;
    if (err != ERR_OK || newpcb == NULL) return ERR_VAL;
    /* Reserve a slot so the recv/sent callbacks can stage against this pcb. */
    if (!lwr_conn_alloc(newpcb)) { tcp_abort(newpcb); return ERR_ABRT; }
    tcp_recv(newpcb, lwr_srv_recv);
    tcp_sent(newpcb, lwr_srv_sent);
    tcp_err(newpcb, lwr_srv_err);

    KlLwrRecord r = { .kind = KL_LWR_ACCEPT, .pcb = NULL, .accepted = newpcb, .ok = 1 };
    /* Peer address (loopback for P9-2, but marshal it properly anyway). */
    memcpy(r.peer_ip, &newpcb->remote_ip.addr, 4);   /* network order (ip4 addr) */
    r.peer_port = newpcb->remote_port;                /* host order in lwIP */
    lwr_rec_push(&r);
    return ERR_OK;
}

/* ── socket-provider primitives on tcp_pcb ─────────────────────────────────── */

void *kl_lwr_tcp_new(void) {
    return tcp_new();
}

int kl_lwr_tcp_bind(void *pcb, const uint8_t ip4[4], uint16_t port) {
    ip_addr_t addr;
    if (!ip4 || (ip4[0] | ip4[1] | ip4[2] | ip4[3]) == 0) {
        addr = *IP_ADDR_ANY;
    } else {
        IP_ADDR4(&addr, ip4[0], ip4[1], ip4[2], ip4[3]);
    }
    return tcp_bind((struct tcp_pcb *)pcb, &addr, port) == ERR_OK ? 0 : -1;
}

void *kl_lwr_tcp_listen(void *pcb) {
    struct tcp_pcb *lp = tcp_listen((struct tcp_pcb *)pcb);
    if (!lp) return NULL;
    tcp_accept(lp, lwr_srv_accept);
    g_listen_pcb = lp;   /* the original `pcb` is now freed by lwIP */
    return lp;
}

void *kl_lwr_listen_pcb(void) { return g_listen_pcb; }

uint16_t kl_lwr_tcp_local_port(void *pcb) {
    return ((struct tcp_pcb *)pcb)->local_port;
}

void kl_lwr_tcp_close(void *pcb) {
    struct tcp_pcb *p = (struct tcp_pcb *)pcb;
    if (p == g_listen_pcb) g_listen_pcb = NULL;
    lwr_conn_free(p);
    tcp_arg(p, NULL);
    /* tcp_recv/tcp_sent assert against a LISTEN pcb — only a connection pcb has these
     * callbacks. A LISTEN pcb (the listener being torn down) just needs tcp_close, which
     * calls tcp_accept(NULL) internally. */
    if (p->state != LISTEN) {
        tcp_recv(p, NULL);
        tcp_sent(p, NULL);
        tcp_err(p, NULL);
    }
    if (tcp_close(p) != ERR_OK)
        tcp_abort(p);
}

void kl_lwr_set_owner(void *pcb, void *owner) {
    struct tcp_pcb *p = (struct tcp_pcb *)pcb;
    KlLwrConn *cs = lwr_conn_find(p);
    if (cs) cs->owner = owner;
    tcp_arg(p, owner);   /* the err callback receives this as arg */
}

void kl_lwr_conn_status(void *pcb, int *has_data, int *closed) {
    KlLwrConn *cs = lwr_conn_find((struct tcp_pcb *)pcb);
    if (has_data) *has_data = (cs && cs->stage_len > 0) ? 1 : 0;
    if (closed)   *closed   = (cs && cs->closed) ? 1 : 0;
}

size_t kl_lwr_take_staged(void *pcb, void *dst, size_t cap) {
    KlLwrConn *cs = lwr_conn_find((struct tcp_pcb *)pcb);
    if (!cs || cs->stage_len == 0) return 0;
    size_t n = cs->stage_len < cap ? cs->stage_len : cap;
    memcpy(dst, cs->stage, n);
    /* Shift any remainder down (P9-2 consumes everything, but stay correct). */
    if (n < cs->stage_len)
        memmove(cs->stage, cs->stage + n, cs->stage_len - n);
    cs->stage_len -= n;
    return n;
}

long kl_lwr_send(void *pcb, const void *buf, size_t len) {
    struct tcp_pcb *p = (struct tcp_pcb *)pcb;
    u16_t sndbuf = tcp_sndbuf(p);
    /* P9-2: small single-segment response — send what fits (bounded by tcp_sndbuf).
     * P9-3 will loop on tcp_sent for partial sends + honour backpressure. */
    size_t want = len;
    if (want > sndbuf) want = sndbuf;
    if (want == 0) return 0;
    err_t w = tcp_write(p, buf, (u16_t)want, TCP_WRITE_FLAG_COPY);
    if (w != ERR_OK) return -1;
    if (tcp_output(p) != ERR_OK) return -1;
    return (long)want;
}

/* ── raw-API test client (mirrors raw_loopback_spike.c) ────────────────────── */

static char g_cli_buf[1024];
static size_t g_cli_len;
static const void *g_cli_req;
static size_t g_cli_req_len;

static err_t lwr_cli_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    (void)arg;
    if (err != ERR_OK || p == NULL) {
        if (p) pbuf_free(p);
        tcp_close(tpcb);
        return ERR_OK;
    }
    for (struct pbuf *q = p; q != NULL; q = q->next) {
        size_t room = sizeof(g_cli_buf) - 1 - g_cli_len;
        size_t take = q->len < room ? q->len : room;
        if (take > 0) { memcpy(g_cli_buf + g_cli_len, q->payload, take); g_cli_len += take; }
    }
    g_cli_buf[g_cli_len] = '\0';
    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static err_t lwr_cli_connected(void *arg, struct tcp_pcb *tpcb, err_t err) {
    (void)arg;
    if (err != ERR_OK) return err;
    tcp_recv(tpcb, lwr_cli_recv);
    err_t w = tcp_write(tpcb, g_cli_req, (u16_t)g_cli_req_len, TCP_WRITE_FLAG_COPY);
    if (w != ERR_OK) return w;
    tcp_output(tpcb);
    return ERR_OK;
}

int kl_lwr_client_start(const uint8_t ip4[4], uint16_t port,
                        const void *req, size_t req_len) {
    g_cli_len = 0;
    g_cli_buf[0] = '\0';
    g_cli_req = req;
    g_cli_req_len = req_len;
    struct tcp_pcb *cli = tcp_new();
    if (!cli) return -1;
    ip_addr_t dst;
    IP_ADDR4(&dst, ip4[0], ip4[1], ip4[2], ip4[3]);
    err_t rc = tcp_connect(cli, &dst, port, lwr_cli_connected);
    if (rc != ERR_OK) {
        tcp_abort(cli);
        return -1;
    }
    return 0;
}

size_t kl_lwr_client_response(char *dst, size_t cap) {
    size_t n = g_cli_len < cap - 1 ? g_cli_len : cap - 1;
    memcpy(dst, g_cli_buf, n);
    dst[n] = '\0';
    return n;
}
