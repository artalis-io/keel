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
 * P9-3 adds the full-payload send-pump: a per-pcb owned copy of the whole response +
 * written/acked offsets. kl_lwr_send_begin copies the payload and kicks the first
 * tcp_write round; lwr_send_pump writes as much as tcp_sndbuf allows (chunked by
 * tcp_sndbuf and 0xffff), tcp_output, advancing "written"; tcp_sent advances "acked"
 * and resumes the pump (this is the ERR_MEM/full-sndbuf backpressure resume path), and
 * enqueues the SINGLE terminal KL_LWR_WRITE only when acked == total. kl_lwr_sendfile_begin
 * preads the file into the same owned buffer after the head, then reuses the pump. This
 * makes a many-segment (>tcp_sndbuf) response transfer correctly end to end.
 *
 * P9-4 (this stage) hardens close/cancel/error LIFETIME + MEMORY SAFETY:
 *   - tcp_err (RST/OOM/abort): lwIP has ALREADY freed the pcb when this fires, so the
 *     callback must NOT touch it. lwr_srv_err now fully releases the owning slot BY OWNER
 *     (frees the send buffer + staging), marks the slot `dead` (pcb freed by lwIP) so the
 *     backend's later close is a no-op on the dead pointer, and marks it `closed` so the
 *     backend surfaces exactly ONE terminal READ → the driver closes the KlConn once.
 *   - close-with-outstanding-send: kl_lwr_tcp_close frees the owned send buffer first, then
 *     tcp_close; if data is still queued tcp_close returns != ERR_OK and we tcp_abort (lwIP
 *     requires this) — never leaving a half-torn pcb or leaking the send copy.
 *   - kl_comp_cancel (idle timeout): kl_lwr_tcp_abort aborts the live pcb (→ tcp_err path is
 *     suppressed for a self-initiated abort by detaching callbacks first), releases the slot,
 *     and marks it closed so the backend surfaces the terminal READ. Idempotent: a second
 *     abort/close on an already-dead/closed slot does nothing.
 *   - slot recycling: KL_LWR_MAX_CONNS bumped to a bounded cap; accept gracefully REJECTS
 *     (tcp_abort the new pcb) when full rather than overflowing, and every close/err frees
 *     the slot so it is reused for the next accept. A dead pcb pointer can never alias a live
 *     slot because the slot is cleared (pcb=NULL) on release.
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
#include <stdlib.h>   /* malloc/free for the per-pcb send buffer (this TU is a standalone
                       * lwIP-only integration, not part of core libkeel's allocator axis) */
#include <stdint.h>   /* SIZE_MAX (sendfile size overflow guard) */
#include <unistd.h>   /* pread + off_t for the file-send path */

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
 * A fixed table keyed by pcb. Sized for a bounded number of concurrent connections; the
 * many-short-lived-conns P9-4 test drives >8 conns SEQUENTIALLY, so with correct recycling
 * (free on close/err → reuse for the next accept) a small cap suffices. A burst that would
 * exceed the cap is gracefully REJECTED at accept (tcp_abort the new pcb) — an embedded
 * target prefers a bounded table + reject over unbounded growth. Each accepted pcb gets a
 * slot in lwr_srv_accept; the recv callback stages bytes here so the backend can copy them
 * into c->read_buf. */
#define KL_LWR_MAX_CONNS 32
#define KL_LWR_STAGE_CAP 8192   /* matches KEEL's KL_READ_BUF_SIZE headers window */

typedef struct {
    struct tcp_pcb *pcb;      /* NULL = free slot */
    void           *owner;    /* KlConn* the backend associated */
    unsigned char   stage[KL_LWR_STAGE_CAP];
    size_t          stage_len;
    int             closed;   /* peer closed / errored — surface a zero-length READ */
    int             dead;     /* pcb was freed by lwIP (tcp_err) — never deref it again */
    int             terminated;/* a terminal READ was already surfaced — no double close */

    /* ── P9-3 outgoing send state (owned copy of the full response) ──────────────
     * `send_buf`/`send_total` hold the whole payload the driver posted (the backend
     * owns the bytes for the op's lifetime). `send_written` = bytes handed to
     * tcp_write so far; `send_acked` = bytes the peer has acknowledged (tcp_sent).
     * When send_acked == send_total the terminal KL_LWR_WRITE is enqueued and the
     * buffer released. send_buf == NULL means no send in flight. */
    unsigned char  *send_buf;
    size_t          send_total;
    size_t          send_written;
    size_t          send_acked;
} KlLwrConn;

static KlLwrConn g_conns[KL_LWR_MAX_CONNS];

/* The current LISTEN pcb (relocated by tcp_listen); tracked so the backend can adopt it and
 * so close() can distinguish the listener from a connection pcb. */
static struct tcp_pcb *g_listen_pcb = NULL;

/* Find a slot by pcb pointer. NOTE this matches by POINTER VALUE only — a `dead` slot still
 * carries the (freed) pointer for owner/close correlation, but callers must consult ->dead
 * before dereferencing ->pcb. A free slot has pcb==NULL and never matches a live pcb. */
static KlLwrConn *lwr_conn_find(const struct tcp_pcb *pcb) {
    if (pcb == NULL) return NULL;
    for (int i = 0; i < KL_LWR_MAX_CONNS; i++)
        if (g_conns[i].pcb == pcb) return &g_conns[i];
    return NULL;
}
static void lwr_send_reset(KlLwrConn *c) {
    if (c->send_buf) { free(c->send_buf); c->send_buf = NULL; }
    c->send_total = c->send_written = c->send_acked = 0;
}
static KlLwrConn *lwr_conn_alloc(struct tcp_pcb *pcb) {
    for (int i = 0; i < KL_LWR_MAX_CONNS; i++)
        if (g_conns[i].pcb == NULL) {
            g_conns[i].pcb = pcb;
            g_conns[i].owner = NULL;
            g_conns[i].stage_len = 0;
            g_conns[i].closed = 0;
            g_conns[i].dead = 0;
            g_conns[i].terminated = 0;
            g_conns[i].send_buf = NULL;
            g_conns[i].send_total = g_conns[i].send_written = g_conns[i].send_acked = 0;
            return &g_conns[i];
        }
    return NULL;
}
/* Fully clear a slot back to free (pcb=NULL) after releasing owned resources. Used by the
 * close/abort/err paths — recycling: a freed slot is immediately reusable by the next
 * accept, and its NULL pcb can never alias a new pcb's pointer. */
static void lwr_slot_clear(KlLwrConn *c) {
    lwr_send_reset(c);
    c->pcb = NULL; c->owner = NULL; c->stage_len = 0;
    c->closed = 0; c->dead = 0; c->terminated = 0;
}
static void lwr_conn_free(const struct tcp_pcb *pcb) {
    KlLwrConn *c = lwr_conn_find(pcb);
    if (c) lwr_slot_clear(c);
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

static void lwr_push_terminal(KlLwrConn *slot);   /* fwd: single terminal completion record */

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
        if (cs) {
            cs->closed = 1;
            /* P9-4: if a send is in flight when the peer closes (close-with-outstanding: the
             * client read part of a large body then FIN'd), the conn is NOT recv-armed, so the
             * armed-READ close gate would never fire. Push a terminal completion record + drop
             * the owned send buffer so the driver tears the conn down promptly (no leak, no
             * further writes to a closing pcb). A plain idle keep-alive close (no send in
             * flight) keeps the original armed-READ path (a fresh recv IS posted post-response). */
            if (cs->send_buf) {
                lwr_send_reset(cs);
                lwr_push_terminal(cs);
            }
        }
        return ERR_OK;   /* keep the pcb; the driver's close (tcp_close/abort) tears it down */
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

/* ── P9-3 send-pump ──────────────────────────────────────────────────────────
 * Push as much of the owned send buffer's unwritten tail as tcp_sndbuf allows, in
 * chunks bounded by both tcp_sndbuf(pcb) and 0xffff (tcp_write's u16 length limit),
 * then tcp_output(). Advances send_written. Stops on ERR_MEM (send queue full —
 * backpressure): the next tcp_sent callback (an ack frees queue space) resumes the
 * round. Any other tcp_write error aborts the connection. NOT called once fully
 * written; the terminal WRITE record is enqueued from tcp_sent when acked catches up. */
#define KL_LWR_TCP_WRITE_MAX 0xffffu   /* tcp_write's u16_t len ceiling */
static void lwr_send_pump(struct tcp_pcb *pcb, KlLwrConn *cs) {
    if (!cs || !cs->send_buf) return;
    int wrote_any = 0;
    while (cs->send_written < cs->send_total) {
        u16_t sndbuf = tcp_sndbuf(pcb);
        if (sndbuf == 0) break;                       /* no headroom — wait for tcp_sent */
        size_t remain = cs->send_total - cs->send_written;
        size_t chunk = remain;
        if (chunk > sndbuf) chunk = sndbuf;
        if (chunk > KL_LWR_TCP_WRITE_MAX) chunk = KL_LWR_TCP_WRITE_MAX;
        err_t w = tcp_write(pcb, cs->send_buf + cs->send_written, (u16_t)chunk,
                            TCP_WRITE_FLAG_COPY);
        if (w == ERR_MEM) break;                      /* queue full — backpressure, resume later */
        if (w != ERR_OK) { tcp_abort(pcb); return; }  /* hard error — abort the conn */
        cs->send_written += chunk;
        wrote_any = 1;
    }
    if (wrote_any) tcp_output(pcb);                   /* flush the freshly-queued segments */
}

/* sent callback: `len` bytes of a posted send were acknowledged. Advance the acked
 * offset, pump more of the tail (the ack freed send-queue space — this is where the
 * ERR_MEM backpressure path resumes), and only when the WHOLE payload is acknowledged
 * enqueue the single terminal KL_LWR_WRITE record and release the send buffer. This is
 * the completion contract: the driver sees ONE fully-completed WRITE, never a partial. */
static err_t lwr_srv_sent(void *arg, struct tcp_pcb *tpcb, u16_t len) {
    (void)arg;
    KlLwrConn *cs = lwr_conn_find(tpcb);
    if (!cs || !cs->send_buf) return ERR_OK;          /* stray ack (no send in flight) */
    cs->send_acked += len;
    if (cs->send_acked >= cs->send_total) {
        KlLwrRecord r = { .kind = KL_LWR_WRITE, .pcb = tpcb,
                          .owner = cs->owner, .nbytes = cs->send_total, .ok = 1 };
        lwr_rec_push(&r);
        lwr_send_reset(cs);                           /* release the owned copy */
        return ERR_OK;
    }
    lwr_send_pump(tpcb, cs);                          /* push more of the tail */
    return ERR_OK;
}

/* P9-4: push a SINGLE terminal "connection dead" record for a conn, keyed by owner. Surfaced
 * as a failed WRITE (ok=0) — the driver's completion path turns any ok=0 TCP completion into
 * comp_close, so this releases the KlConn exactly once regardless of whether it was awaiting a
 * READ or a WRITE. Sets `terminated` so the armed-READ gate cannot ALSO surface a terminal
 * event for the same conn (exactly-one-close). Only pushes if not already terminated and an
 * owner is known (the backend needs a target). */
static void lwr_push_terminal(KlLwrConn *slot) {
    if (!slot || slot->terminated || slot->owner == NULL) return;
    KlLwrRecord r = { .kind = KL_LWR_WRITE, .pcb = slot->pcb,
                      .owner = slot->owner, .nbytes = 0, .ok = 0 };
    lwr_rec_push(&r);
    slot->terminated = 1;
}

/* err callback (P9-4): the pcb was aborted by the stack (RST, OOM, self-initiated tcp_abort).
 * lwIP has ALREADY FREED the pcb by the time this fires — we MUST NOT dereference it. Key the
 * slot by owner (arg == the KlConn* set via tcp_arg), free its owned send buffer + staging
 * BY OWNER (the pcb can't reach them), mark it `dead` (so the backend's later close is a no-op
 * on the freed pointer) and `closed` (so the backend surfaces exactly one terminal zero-length
 * READ → the driver releases the KlConn exactly once). The freed pcb pointer value is retained
 * ONLY so the backend's close path can correlate c->fd → slot; it is never dereferenced. */
static void lwr_srv_err(void *arg, err_t err) {
    (void)err;
    for (int i = 0; i < KL_LWR_MAX_CONNS; i++)
        if (g_conns[i].owner == arg && g_conns[i].pcb != NULL) {
            lwr_send_reset(&g_conns[i]);   /* free-by-owner: pcb is dead, can't reach the buf */
            g_conns[i].stage_len = 0;
            g_conns[i].dead = 1;           /* never deref g_conns[i].pcb again */
            g_conns[i].closed = 1;
            /* Surface the single terminal completion via a record (owner-keyed), so the driver
             * closes the KlConn exactly once whether it was awaiting a READ or a WRITE (a
             * mid-send RST leaves the conn NOT recv-armed — the armed-READ gate alone would
             * never fire). lwr_push_terminal sets `terminated`, suppressing the armed path. */
            lwr_push_terminal(&g_conns[i]);
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
    if (p == NULL) return;

    /* P9-4: if this pcb was already freed by lwIP (tcp_err marked its slot `dead`), the
     * driver is closing the KlConn whose c->fd still holds the dead pointer. Free ONLY the
     * slot — never touch the freed pcb. This is what makes close-after-err safe (no UAF). */
    KlLwrConn *slot = lwr_conn_find(p);
    if (slot && slot->dead) {
        lwr_slot_clear(slot);
        return;
    }

    if (p == g_listen_pcb) g_listen_pcb = NULL;
    lwr_conn_free(p);              /* frees the owned send buffer (close-with-outstanding) */
    tcp_arg(p, NULL);
    /* tcp_recv/tcp_sent/tcp_err assert against a LISTEN pcb — only a connection pcb has these
     * callbacks. A LISTEN pcb (the listener being torn down) just needs tcp_close, which
     * calls tcp_accept(NULL) internally. Detaching the callbacks first also means a
     * self-initiated abort below won't re-enter lwr_srv_err for an already-freed slot. */
    if (p->state != LISTEN) {
        tcp_recv(p, NULL);
        tcp_sent(p, NULL);
        tcp_err(p, NULL);
    }
    /* tcp_close may fail (return != ERR_OK) when data is still queued on the pcb — e.g. a
     * close mid-send with unacked/unsent segments. lwIP REQUIRES a tcp_abort fallback in
     * that case (tcp_close left the pcb intact). tcp_abort frees the pcb + sends a RST. */
    if (tcp_close(p) != ERR_OK)
        tcp_abort(p);
}

/* P9-4: forcibly abort a LIVE connection pcb (idle-timeout cancel path). Detach the callbacks
 * FIRST so lwIP's tcp_abort (which invokes tcp_err internally) does not re-enter lwr_srv_err
 * against a slot we are about to clear, then free the slot by owner + send buffer, then abort
 * (frees the pcb + RSTs the peer). Idempotent: a NULL/dead/free slot is a no-op — safe to call
 * on a conn that already closed. Marks the slot `closed` so the backend still surfaces the
 * single terminal READ that drives the driver's exactly-one release of the KlConn. */
void kl_lwr_tcp_abort(void *pcb) {
    struct tcp_pcb *p = (struct tcp_pcb *)pcb;
    if (p == NULL || p == g_listen_pcb) return;
    KlLwrConn *slot = lwr_conn_find(p);
    if (!slot || slot->dead) return;   /* already gone — idempotent no-op */

    lwr_send_reset(slot);              /* release the in-flight send copy (no leak) */
    slot->stage_len = 0;
    slot->closed = 1;
    slot->dead = 1;                    /* pcb about to be freed — never deref again */
    /* Surface the single terminal completion via a record (owner-keyed) so the driver closes
     * the KlConn once whether it was awaiting a READ (idle) or a WRITE (mid-send cancel). */
    lwr_push_terminal(slot);

    tcp_arg(p, NULL);
    if (p->state != LISTEN) {
        tcp_recv(p, NULL);
        tcp_sent(p, NULL);
        tcp_err(p, NULL);
    }
    tcp_abort(p);                      /* frees the pcb + RST */
}

void kl_lwr_set_owner(void *pcb, void *owner) {
    struct tcp_pcb *p = (struct tcp_pcb *)pcb;
    KlLwrConn *cs = lwr_conn_find(p);
    if (cs) cs->owner = owner;
    tcp_arg(p, owner);   /* the err callback receives this as arg */
}

void kl_lwr_conn_status(void *pcb, int *has_data, int *closed) {
    KlLwrConn *cs = lwr_conn_find((struct tcp_pcb *)pcb);
    /* A `dead` (tcp_err-freed) slot never has usable staged data — only report the terminal
     * close. `terminated` suppresses re-reporting once the backend consumed the terminal READ
     * (defence-in-depth against a double terminal → double comp_close). */
    if (has_data) *has_data = (cs && !cs->dead && !cs->terminated && cs->stage_len > 0) ? 1 : 0;
    if (closed)   *closed   = (cs && !cs->terminated && cs->closed) ? 1 : 0;
}

/* P9-4: the backend calls this right after it surfaces a terminal (zero-length) READ for a
 * closed conn, so a re-check (e.g. if the conn is somehow re-armed) will not surface a second
 * terminal event. Idempotent; a NULL/unknown pcb is ignored. */
void kl_lwr_mark_terminated(void *pcb) {
    KlLwrConn *cs = lwr_conn_find((struct tcp_pcb *)pcb);
    if (cs) cs->terminated = 1;
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

/* ── P9-3: full-payload send + file send (owned copy + pump) ─────────────────── */

/* Install `total` bytes into the pcb's owned send buffer and kick the first pump round.
 * Caller (kl_lwr_send_begin / kl_lwr_sendfile_begin) has already allocated + filled
 * `buf` (which becomes the owned buffer — ownership transfers here). */
static int lwr_send_install(struct tcp_pcb *p, unsigned char *buf, size_t total) {
    KlLwrConn *cs = lwr_conn_find(p);
    if (!cs) { free(buf); return -1; }
    lwr_send_reset(cs);              /* one send in flight per conn (completion contract) */
    cs->send_buf     = buf;
    cs->send_total   = total;
    cs->send_written = 0;
    cs->send_acked   = 0;
    if (total == 0) {               /* nothing to send — synthesize an immediate completion */
        KlLwrRecord r = { .kind = KL_LWR_WRITE, .pcb = p, .owner = cs->owner,
                          .nbytes = 0, .ok = 1 };
        lwr_rec_push(&r);
        lwr_send_reset(cs);
        return 0;
    }
    lwr_send_pump(p, cs);
    return 0;
}

int kl_lwr_send_begin(void *pcb, const void *buf, size_t len) {
    struct tcp_pcb *p = (struct tcp_pcb *)pcb;
    unsigned char *copy = malloc(len ? len : 1);
    if (!copy) return -1;
    if (len) memcpy(copy, buf, len);
    return lwr_send_install(p, copy, len);
}

int kl_lwr_sendfile_begin(void *pcb, const void *head, size_t head_len,
                          int file_fd, uint64_t count) {
    struct tcp_pcb *p = (struct tcp_pcb *)pcb;
    /* P9-3 approach (a): buffer head + the whole file into one owned copy, then reuse the
     * send-pump. Simplest, bounded by head_len+count (fine for the test's temp file). The
     * glue reads file_fd but does NOT close it — the response layer owns res->file_fd. */
    if (count > (uint64_t)(SIZE_MAX - head_len)) return -1;   /* overflow guard */
    size_t total = head_len + (size_t)count;
    unsigned char *copy = malloc(total ? total : 1);
    if (!copy) return -1;
    if (head_len) memcpy(copy, head, head_len);
    /* pread the file bytes after the head (offset 0, looping for short reads). */
    size_t off = 0;
    while (off < (size_t)count) {
        ssize_t nr = pread(file_fd, copy + head_len + off, (size_t)count - off,
                           (off_t)off);
        if (nr < 0) { free(copy); return -1; }
        if (nr == 0) break;                       /* unexpected EOF — send what we have */
        off += (size_t)nr;
    }
    if (off != (size_t)count) { free(copy); return -1; }   /* file shorter than declared */
    return lwr_send_install(p, copy, total);
}

void kl_lwr_send_release(void *pcb) {
    KlLwrConn *cs = lwr_conn_find((struct tcp_pcb *)pcb);
    if (cs) lwr_send_reset(cs);
}

/* ── raw-API test client (mirrors raw_loopback_spike.c) ──────────────────────
 * P9-3 responses can be large (a 64 KB body + headers), so the client accumulates
 * into a heap buffer sized on start (owned here; freed on the next start / release).
 * All client calls run on the single lwIP tick thread (marshalled via KEEL timers in
 * the test), so plain non-atomic state is safe. */
static unsigned char *g_cli_buf;    /* heap accumulator (NUL-terminated for strstr) */
static size_t g_cli_cap;
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
        if (!g_cli_buf) break;
        size_t room = (g_cli_cap > g_cli_len + 1) ? g_cli_cap - 1 - g_cli_len : 0;
        size_t take = q->len < room ? q->len : room;
        if (take > 0) { memcpy(g_cli_buf + g_cli_len, q->payload, take); g_cli_len += take; }
    }
    if (g_cli_buf) g_cli_buf[g_cli_len] = '\0';
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

/* Start a fresh request; `cap` sizes the response accumulator (large enough for the
 * expected body + headers). Returns 0 on the connect being issued, -1 on failure. */
int kl_lwr_client_start_cap(const uint8_t ip4[4], uint16_t port,
                            const void *req, size_t req_len, size_t cap) {
    free(g_cli_buf);
    g_cli_buf = malloc(cap ? cap : 1);
    if (!g_cli_buf) return -1;
    g_cli_cap = cap ? cap : 1;
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

/* P9-2 convenience: small default accumulator (1 KB). */
int kl_lwr_client_start(const uint8_t ip4[4], uint16_t port,
                        const void *req, size_t req_len) {
    return kl_lwr_client_start_cap(ip4, port, req, req_len, 1024);
}

size_t kl_lwr_client_response(char *dst, size_t cap) {
    if (cap == 0) return 0;
    size_t n = g_cli_len < cap - 1 ? g_cli_len : cap - 1;
    if (g_cli_buf) memcpy(dst, g_cli_buf, n);
    dst[n] = '\0';
    return n;
}

size_t kl_lwr_client_len(void) { return g_cli_len; }

/* Locate the response body (after the CRLFCRLF header terminator) and return its length
 * + a simple additive checksum over the body bytes (0 if no terminator yet). Used by the
 * P9-3 tests to verify a large/file body arrived intact without copying it all out. */
size_t kl_lwr_client_body(size_t *out_checksum, unsigned char *first, unsigned char *last) {
    if (out_checksum) *out_checksum = 0;
    if (first) *first = 0;
    if (last) *last = 0;
    if (!g_cli_buf || g_cli_len < 4) return 0;
    unsigned char *hdr_end = NULL;
    for (size_t i = 0; i + 3 < g_cli_len; i++) {
        if (g_cli_buf[i] == '\r' && g_cli_buf[i+1] == '\n' &&
            g_cli_buf[i+2] == '\r' && g_cli_buf[i+3] == '\n') {
            hdr_end = g_cli_buf + i + 4;
            break;
        }
    }
    if (!hdr_end) return 0;
    size_t body_len = g_cli_len - (size_t)(hdr_end - g_cli_buf);
    if (body_len == 0) return 0;
    size_t sum = 0;
    for (size_t i = 0; i < body_len; i++) sum += hdr_end[i];
    if (out_checksum) *out_checksum = sum;
    if (first) *first = hdr_end[0];
    if (last) *last = hdr_end[body_len - 1];
    return body_len;
}

void kl_lwr_client_release(void) {
    free(g_cli_buf);
    g_cli_buf = NULL;
    g_cli_cap = g_cli_len = 0;
}

/* DEBUG: copy up to `cap` body bytes (after CRLFCRLF) starting at body offset `off`. */
size_t kl_lwr_client_body_peek(size_t off, unsigned char *dst, size_t cap) {
    if (!g_cli_buf) return 0;
    unsigned char *hdr_end = NULL;
    for (size_t i = 0; i + 3 < g_cli_len; i++)
        if (g_cli_buf[i]=='\r'&&g_cli_buf[i+1]=='\n'&&g_cli_buf[i+2]=='\r'&&g_cli_buf[i+3]=='\n')
            { hdr_end = g_cli_buf + i + 4; break; }
    if (!hdr_end) return 0;
    size_t body_len = g_cli_len - (size_t)(hdr_end - g_cli_buf);
    if (off >= body_len) return 0;
    size_t n = body_len - off; if (n > cap) n = cap;
    memcpy(dst, hdr_end + off, n);
    return n;
}

/* ── P9-4 lifetime test client ────────────────────────────────────────────────
 * A separate, minimal raw-API client used ONLY by the P9-4 close/cancel/err cases. It is
 * deliberately independent of the P9-2/P9-3 accumulator so those cases stay untouched. It
 * tracks its own pcb (so a case can RST or FIN mid-read), counts received bytes + whether it
 * saw an HTTP "200", and increments a completed-roundtrip counter. All calls run on the single
 * lwIP tick thread (marshalled via KEEL timers), so plain non-atomic state is safe. */
enum { KLW_MODE_FULL = 0, KLW_MODE_PARTIAL_ABORT = 1, KLW_MODE_PARTIAL_CLOSE = 2 };

static struct tcp_pcb *g_lc_pcb;          /* current test client pcb (NULL = none live) */
static const void     *g_lc_req;
static size_t          g_lc_req_len;
static int             g_lc_mode;
static size_t          g_lc_abort_after;   /* bytes to receive before RST/FIN (partial modes) */
static size_t          g_lc_recv;          /* bytes received so far this roundtrip */
static int             g_lc_saw_200;       /* status line contained "200" */
static int             g_lc_done;          /* this roundtrip resolved (200 seen or torn down) */
static int             g_lc_completed;      /* count of resolved roundtrips (many-conns test) */
static char            g_lc_head[64];      /* first bytes, to scan the status line for "200" */
static size_t          g_lc_head_len;

static void lc_teardown(struct tcp_pcb *tpcb, int abort_it) {
    tcp_recv(tpcb, NULL);
    tcp_err(tpcb, NULL);
    tcp_arg(tpcb, NULL);
    if (abort_it) {
        tcp_abort(tpcb);                   /* RST — exercises the server's tcp_err path */
    } else {
        if (tcp_close(tpcb) != ERR_OK) tcp_abort(tpcb);   /* FIN (fallback to abort) */
    }
    if (g_lc_pcb == tpcb) g_lc_pcb = NULL;
}

static void lc_err(void *arg, err_t err) {
    (void)arg; (void)err;
    /* The server RST'd us (or the stack aborted the pcb): pcb is freed — just resolve. */
    g_lc_pcb = NULL;
    if (!g_lc_done) { g_lc_done = 1; g_lc_completed++; }
}

static err_t lc_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    (void)arg;
    if (err != ERR_OK || p == NULL) {      /* server closed / error */
        if (p) pbuf_free(p);
        lc_teardown(tpcb, 0);
        if (!g_lc_done) { g_lc_done = 1; g_lc_completed++; }
        return ERR_OK;
    }
    /* Capture the head for status scanning. */
    for (struct pbuf *q = p; q != NULL; q = q->next) {
        size_t room = sizeof(g_lc_head) - 1 - g_lc_head_len;
        size_t take = q->len < room ? q->len : room;
        if (take) { memcpy(g_lc_head + g_lc_head_len, q->payload, take); g_lc_head_len += take; }
    }
    g_lc_head[g_lc_head_len] = '\0';
    if (!g_lc_saw_200 && strstr(g_lc_head, " 200 ")) g_lc_saw_200 = 1;

    g_lc_recv += p->tot_len;
    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);

    /* Partial modes: once we've read enough, RST or FIN mid-stream (the server still has a
     * large send in flight → exercises close-with-outstanding / reset-mid-send). */
    if ((g_lc_mode == KLW_MODE_PARTIAL_ABORT || g_lc_mode == KLW_MODE_PARTIAL_CLOSE) &&
        g_lc_recv >= g_lc_abort_after) {
        lc_teardown(tpcb, g_lc_mode == KLW_MODE_PARTIAL_ABORT);
        if (!g_lc_done) { g_lc_done = 1; g_lc_completed++; }
        return ERR_OK;
    }
    /* Full mode: a small response completing counts as a resolved roundtrip. */
    if (g_lc_mode == KLW_MODE_FULL && g_lc_saw_200) {
        lc_teardown(tpcb, 0);
        if (!g_lc_done) { g_lc_done = 1; g_lc_completed++; }
    }
    return ERR_OK;
}

static err_t lc_connected(void *arg, struct tcp_pcb *tpcb, err_t err) {
    (void)arg;
    if (err != ERR_OK) return err;
    tcp_recv(tpcb, lc_recv);
    tcp_err(tpcb, lc_err);
    err_t w = tcp_write(tpcb, g_lc_req, (u16_t)g_lc_req_len, TCP_WRITE_FLAG_COPY);
    if (w != ERR_OK) return w;
    tcp_output(tpcb);
    return ERR_OK;
}

/* Start ONE P9-4 lifetime roundtrip. mode: 0 full-read, 1 partial-then-RST, 2 partial-then-FIN.
 * abort_after = bytes to receive before the RST/FIN (partial modes). Resets per-roundtrip state
 * but NOT g_lc_completed (the many-conns test polls the running total). Returns 0/-1. */
int kl_lwr_lc_start(const uint8_t ip4[4], uint16_t port, const void *req, size_t req_len,
                    int mode, size_t abort_after) {
    g_lc_req = req; g_lc_req_len = req_len;
    g_lc_mode = mode; g_lc_abort_after = abort_after;
    g_lc_recv = 0; g_lc_saw_200 = 0; g_lc_done = 0;
    g_lc_head_len = 0; g_lc_head[0] = '\0';
    struct tcp_pcb *cli = tcp_new();
    if (!cli) return -1;
    g_lc_pcb = cli;
    ip_addr_t dst;
    IP_ADDR4(&dst, ip4[0], ip4[1], ip4[2], ip4[3]);
    if (tcp_connect(cli, &dst, port, lc_connected) != ERR_OK) {
        tcp_abort(cli);
        g_lc_pcb = NULL;
        return -1;
    }
    return 0;
}

int    kl_lwr_lc_done(void)      { return g_lc_done; }
int    kl_lwr_lc_saw_200(void)   { return g_lc_saw_200; }
int    kl_lwr_lc_completed(void) { return g_lc_completed; }
size_t kl_lwr_lc_recv(void)      { return g_lc_recv; }
void   kl_lwr_lc_reset_counter(void) { g_lc_completed = 0; }
