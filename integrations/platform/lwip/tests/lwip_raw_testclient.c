/*
 * lwip_raw_testclient.c: TEST-ONLY raw-API TCP client for the lwIP-raw completion tests.
 *
 * Lives outside the production glue (lwip_raw_glue.c) so no test-client state lives
 * in the shipping backend. Compiled ONLY into the test binaries. Includes lwIP's NO_SYS=1 raw
 * headers directly (like the glue): a separate TU from the KEEL-header-only backend, so the
 * lwIP/host header seam is preserved. It creates its own client PCBs (tcp_connect) and never
 * touches the server's per-conn slots.
 *
 * malloc/free is used here (this is a test peer, not production code: the KlAllocator
 * discipline governs production glue). All calls run on the single lwIP tick thread (marshalled
 * via KEEL timers in the tests), so plain non-atomic state is safe.
 *
 * SPDX-License-Identifier: MIT
 */
#include "lwip_raw_testclient.h"

#include "lwip/tcp.h"
#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>   /* intptr_t (slot index carried in tcp_arg) */

/* ── accumulating client (server roundtrips + byte-exact body checks) ────────── */
static unsigned char *g_cli_buf;    /* heap accumulator (NUL-terminated for strstr) */
static size_t g_cli_cap;
static size_t g_cli_len;
static const void *g_cli_req;
static size_t g_cli_req_len;
static size_t g_cli_req_sent;       /* bytes of the request handed to tcp_write so far */
static struct tcp_pcb *g_cli_pcb;   /* the live accumulating-client pcb (NULL = none live) */
static int g_cli_closed;            /* the current roundtrip's connection fully closed */

/* Send the request in tcp_sndbuf-sized chunks (a large POST body exceeds a single tcp_write's
 * capacity / the client's TCP_SND_BUF). Resumed from tcp_sent as the window opens. */
static void cli_req_pump(struct tcp_pcb *pcb) {
    int wrote = 0;
    while (g_cli_req_sent < g_cli_req_len) {
        u16_t sndbuf = tcp_sndbuf(pcb);
        if (sndbuf == 0) break;
        size_t remain = g_cli_req_len - g_cli_req_sent;
        size_t chunk = remain < sndbuf ? remain : sndbuf;
        if (chunk > 0xffffu) chunk = 0xffffu;
        err_t w = tcp_write(pcb, (const char *)g_cli_req + g_cli_req_sent, (u16_t)chunk,
                            TCP_WRITE_FLAG_COPY);
        if (w == ERR_MEM) break;             /* queue full: resume on tcp_sent */
        if (w != ERR_OK) return;
        g_cli_req_sent += chunk;
        wrote = 1;
    }
    if (wrote) tcp_output(pcb);
}

static err_t cli_sent(void *arg, struct tcp_pcb *tpcb, u16_t len) {
    (void)arg; (void)len;
    cli_req_pump(tpcb);                      /* window opened: push more of the request */
    return ERR_OK;
}

/* Detach + gracefully close the current accumulating-client pcb (if still live). Called before a
 * fresh start so a previous connection can never keep writing into the (about-to-be-freed)
 * g_cli_buf: the accumulating client is a single-slot peer, so overlapping two connections would
 * be a use-after-free on the shared buffer. Uses tcp_close (graceful FIN) so the previous conn
 * settles into TIME_WAIT rather than being RST/freed-and-immediately-reused. */
static void lwr_cli_teardown(void) {
    if (!g_cli_pcb) return;
    struct tcp_pcb *p = g_cli_pcb;
    g_cli_pcb = NULL;
    tcp_recv(p, NULL);
    tcp_arg(p, NULL);
    if (tcp_close(p) != ERR_OK) tcp_abort(p);
}

static err_t lwr_cli_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    (void)arg;
    if (err != ERR_OK || p == NULL) {
        if (p) pbuf_free(p);
        tcp_recv(tpcb, NULL);
        if (g_cli_pcb == tpcb) g_cli_pcb = NULL;
        g_cli_closed = 1;            /* server FIN'd: this roundtrip's conn is fully done */
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
    tcp_sent(tpcb, cli_sent);          /* resume the request-send pump as the window opens */
    g_cli_req_sent = 0;
    cli_req_pump(tpcb);                 /* send the request in tcp_sndbuf-sized chunks */
    return ERR_OK;
}

int kl_lwr_client_start_cap(const uint8_t ip4[4], uint16_t port,
                            const void *req, size_t req_len, size_t cap) {
    lwr_cli_teardown();          /* close any previous connection BEFORE freeing its buffer */
    free(g_cli_buf);
    g_cli_buf = malloc(cap ? cap : 1);
    if (!g_cli_buf) return -1;
    g_cli_cap = cap ? cap : 1;
    g_cli_len = 0;
    g_cli_closed = 0;
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
    g_cli_pcb = cli;
    return 0;
}

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

int kl_lwr_client_closed(void) { return g_cli_closed; }

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
    lwr_cli_teardown();          /* close the live pcb before freeing its buffer (no UAF) */
    free(g_cli_buf);
    g_cli_buf = NULL;
    g_cli_cap = g_cli_len = 0;
}

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

/* ── lifetime client (close-cancel token lifetime) ──────────────────────────── */
enum { KLW_MODE_FULL = 0, KLW_MODE_PARTIAL_ABORT = 1, KLW_MODE_PARTIAL_CLOSE = 2 };

static struct tcp_pcb *g_lc_pcb;
static const void     *g_lc_req;
static size_t          g_lc_req_len;
static int             g_lc_mode;
static size_t          g_lc_abort_after;
static size_t          g_lc_recv;
static int             g_lc_saw_200;
static int             g_lc_done;
static int             g_lc_completed;
static char            g_lc_head[64];
static size_t          g_lc_head_len;

static void lc_teardown(struct tcp_pcb *tpcb, int abort_it) {
    tcp_recv(tpcb, NULL);
    tcp_err(tpcb, NULL);
    tcp_arg(tpcb, NULL);
    if (abort_it) {
        tcp_abort(tpcb);
    } else {
        if (tcp_close(tpcb) != ERR_OK) tcp_abort(tpcb);
    }
    if (g_lc_pcb == tpcb) g_lc_pcb = NULL;
}

static void lc_err(void *arg, err_t err) {
    (void)arg; (void)err;
    g_lc_pcb = NULL;
    if (!g_lc_done) { g_lc_done = 1; g_lc_completed++; }
}

static err_t lc_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    (void)arg;
    if (err != ERR_OK || p == NULL) {
        if (p) pbuf_free(p);
        lc_teardown(tpcb, 0);
        if (!g_lc_done) { g_lc_done = 1; g_lc_completed++; }
        return ERR_OK;
    }
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

    if ((g_lc_mode == KLW_MODE_PARTIAL_ABORT || g_lc_mode == KLW_MODE_PARTIAL_CLOSE) &&
        g_lc_recv >= g_lc_abort_after) {
        lc_teardown(tpcb, g_lc_mode == KLW_MODE_PARTIAL_ABORT);
        if (!g_lc_done) { g_lc_done = 1; g_lc_completed++; }
        return ERR_OK;
    }
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

/* ── multi-connection client (concurrency tests) ──────────────────────────────
 * Each slot owns an independent client pcb + response accumulator. The pcb's tcp_arg carries
 * the slot index so the shared callbacks route to the right slot. */
typedef struct {
    struct tcp_pcb *pcb;
    unsigned char  *buf;
    size_t          cap;
    size_t          len;      /* bytes accumulated (headers + body) */
    const void     *req;
    size_t          req_len;
    int             saw_200;
    int             done;     /* full response arrived, or torn down */
    int             refused;  /* connect refused / error before any response */
} McSlot;

static McSlot g_mc[KL_LWR_MC_MAX];

static size_t mc_body_len(const McSlot *s) {
    if (!s->buf || s->len < 4) return 0;
    for (size_t i = 0; i + 3 < s->len; i++)
        if (s->buf[i]=='\r'&&s->buf[i+1]=='\n'&&s->buf[i+2]=='\r'&&s->buf[i+3]=='\n')
            return s->len - (i + 4);
    return 0;
}

/* A response is "complete" once we have a Content-Length header's worth of body OR the peer
 * closed. To keep the test simple + robust the tests use Connection: close, so the server FINs
 * after the full body: lc-style. We resolve `done` on peer close (recv NULL) which arrives
 * after the whole body, and also opportunistically when a 200 + non-empty body is present. */
static void mc_resolve(McSlot *s) {
    if (!s->done) s->done = 1;
}

static err_t mc_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    int idx = (int)(intptr_t)arg;
    if (idx < 0 || idx >= KL_LWR_MC_MAX) { if (p) pbuf_free(p); tcp_close(tpcb); return ERR_OK; }
    McSlot *s = &g_mc[idx];
    if (err != ERR_OK || p == NULL) {          /* peer closed: full response delivered */
        if (p) pbuf_free(p);
        tcp_recv(tpcb, NULL);
        tcp_err(tpcb, NULL);
        tcp_arg(tpcb, NULL);
        if (tcp_close(tpcb) != ERR_OK) tcp_abort(tpcb);
        if (s->pcb == tpcb) s->pcb = NULL;
        mc_resolve(s);
        return ERR_OK;
    }
    for (struct pbuf *q = p; q != NULL; q = q->next) {
        if (!s->buf) break;
        size_t room = (s->cap > s->len + 1) ? s->cap - 1 - s->len : 0;
        size_t take = q->len < room ? q->len : room;
        if (take) { memcpy(s->buf + s->len, q->payload, take); s->len += take; }
    }
    if (s->buf) s->buf[s->len] = '\0';
    if (!s->saw_200 && s->buf && strstr((char *)s->buf, " 200 ")) s->saw_200 = 1;
    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static void mc_err(void *arg, err_t err) {
    (void)err;
    int idx = (int)(intptr_t)arg;
    if (idx < 0 || idx >= KL_LWR_MC_MAX) return;
    McSlot *s = &g_mc[idx];
    s->pcb = NULL;
    if (!s->saw_200 && s->len == 0) s->refused = 1;   /* aborted before any response */
    mc_resolve(s);
}

static err_t mc_connected(void *arg, struct tcp_pcb *tpcb, err_t err) {
    int idx = (int)(intptr_t)arg;
    if (err != ERR_OK) return err;
    if (idx < 0 || idx >= KL_LWR_MC_MAX) return ERR_ABRT;
    McSlot *s = &g_mc[idx];
    tcp_recv(tpcb, mc_recv);
    tcp_err(tpcb, mc_err);
    err_t w = tcp_write(tpcb, s->req, (u16_t)s->req_len, TCP_WRITE_FLAG_COPY);
    if (w != ERR_OK) return w;
    tcp_output(tpcb);
    return ERR_OK;
}

void kl_lwr_mc_reset(void) {
    for (int i = 0; i < KL_LWR_MC_MAX; i++) {
        if (g_mc[i].pcb) {
            tcp_recv(g_mc[i].pcb, NULL);
            tcp_err(g_mc[i].pcb, NULL);
            tcp_arg(g_mc[i].pcb, NULL);
            tcp_abort(g_mc[i].pcb);
        }
        free(g_mc[i].buf);
        memset(&g_mc[i], 0, sizeof(g_mc[i]));
    }
}

int kl_lwr_mc_start(int idx, const uint8_t ip4[4], uint16_t port,
                    const void *req, size_t req_len, size_t cap) {
    if (idx < 0 || idx >= KL_LWR_MC_MAX) return -1;
    McSlot *s = &g_mc[idx];
    free(s->buf);
    memset(s, 0, sizeof(*s));
    s->buf = malloc(cap ? cap : 1);
    if (!s->buf) return -1;
    s->cap = cap ? cap : 1;
    s->buf[0] = '\0';
    s->req = req;
    s->req_len = req_len;
    struct tcp_pcb *cli = tcp_new();
    if (!cli) { free(s->buf); s->buf = NULL; return -1; }
    s->pcb = cli;
    tcp_arg(cli, (void *)(intptr_t)idx);
    ip_addr_t dst;
    IP_ADDR4(&dst, ip4[0], ip4[1], ip4[2], ip4[3]);
    if (tcp_connect(cli, &dst, port, mc_connected) != ERR_OK) {
        tcp_abort(cli);
        s->pcb = NULL;
        s->refused = 1;
        s->done = 1;
        return -1;
    }
    return 0;
}

int kl_lwr_mc_done(int idx) {
    if (idx < 0 || idx >= KL_LWR_MC_MAX) return 0;
    return g_mc[idx].done;
}
int kl_lwr_mc_ok(int idx) {
    if (idx < 0 || idx >= KL_LWR_MC_MAX) return 0;
    return g_mc[idx].saw_200;
}
int kl_lwr_mc_refused(int idx) {
    if (idx < 0 || idx >= KL_LWR_MC_MAX) return 0;
    return g_mc[idx].refused;
}
size_t kl_lwr_mc_body(int idx, size_t *out_checksum) {
    if (out_checksum) *out_checksum = 0;
    if (idx < 0 || idx >= KL_LWR_MC_MAX) return 0;
    McSlot *s = &g_mc[idx];
    size_t blen = mc_body_len(s);
    if (blen == 0) return 0;
    unsigned char *body = s->buf + (s->len - blen);
    if (out_checksum) {
        size_t sum = 0;
        for (size_t i = 0; i < blen; i++) sum += body[i];
        *out_checksum = sum;
    }
    return blen;
}
