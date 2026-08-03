/*
 * raw_loopback_spike.c — Phase 9 feasibility spike.
 *
 * Question: can we drive lwIP's raw (tcp_*) callback API in NO_SYS=1 mode over a
 * loopback netif, entirely in-process with no privileges, and prove a TCP
 * request/response roundtrip?
 *
 * This program:
 *   - lwip_init()  (auto-creates loop_netif via LWIP_HAVE_LOOPIF)
 *   - brings loop_netif up + default so 127.0.0.1 routes to it
 *   - a raw-API TCP echo server (tcp_new/bind/listen/accept -> recv echoes bytes)
 *   - a raw-API TCP client (tcp_new/connect -> write payload -> recv captures echo)
 *   - a bare mainloop: sys_check_timeouts() + netif_poll(loopif) each tick
 *   - prints PASS if the echoed payload matches, FAIL/timeout otherwise.
 *
 * NO tcpip thread, NO sockets, NO netconn. All tcp_* callbacks run inline on this
 * single loop — exactly the model a KEEL completion/raw provider would use.
 *
 * NOTE: This spike does NOT touch KlEventCtx / KEEL core. It only proves the
 * raw+loopback+NO_SYS mainloop is real and testable.
 */
#include "lwip/init.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"
#include "lwip/netif.h"
#include "lwip/ip_addr.h"
#include "lwip/sys.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define SPIKE_PORT      7777
#define SPIKE_PAYLOAD   "hello-lwip-raw-noSys"
#define SPIKE_MAX_TICKS 5000     /* ~5s at 1ms/tick */

/* ── NO_SYS=1 requires we provide sys_now() (u32 milliseconds, monotonic) ─────*/
u32_t sys_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

/* ── shared state ─────────────────────────────────────────────────────────────*/
static char g_echo_buf[256];       /* what the client received back */
static int  g_echo_len;
static int  g_client_done;         /* set when client has its echo */
static int  g_client_failed;

/* ── raw TCP echo server ──────────────────────────────────────────────────────*/
static err_t srv_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    (void)arg;
    if (err != ERR_OK || p == NULL) {
        /* remote closed */
        if (p) pbuf_free(p);
        tcp_close(tpcb);
        return ERR_OK;
    }
    /* Echo every byte in the (possibly chained) pbuf straight back. */
    struct pbuf *q;
    for (q = p; q != NULL; q = q->next) {
        tcp_write(tpcb, q->payload, q->len, TCP_WRITE_FLAG_COPY);
    }
    tcp_output(tpcb);
    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static err_t srv_accept(void *arg, struct tcp_pcb *newpcb, err_t err) {
    (void)arg;
    if (err != ERR_OK || newpcb == NULL) return ERR_VAL;
    tcp_recv(newpcb, srv_recv);
    return ERR_OK;
}

/* ── raw TCP client ───────────────────────────────────────────────────────────*/
static err_t cli_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    (void)arg;
    if (err != ERR_OK || p == NULL) {
        if (p) pbuf_free(p);
        tcp_close(tpcb);
        if (!g_client_done) g_client_failed = 1;
        return ERR_OK;
    }
    struct pbuf *q;
    for (q = p; q != NULL; q = q->next) {
        int n = q->len;
        if (g_echo_len + n > (int)sizeof(g_echo_buf) - 1) n = (int)sizeof(g_echo_buf) - 1 - g_echo_len;
        if (n > 0) { memcpy(g_echo_buf + g_echo_len, q->payload, n); g_echo_len += n; }
    }
    g_echo_buf[g_echo_len] = 0;
    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);
    if (g_echo_len >= (int)strlen(SPIKE_PAYLOAD)) {
        g_client_done = 1;
        tcp_close(tpcb);
    }
    return ERR_OK;
}

static err_t cli_connected(void *arg, struct tcp_pcb *tpcb, err_t err) {
    (void)arg;
    if (err != ERR_OK) { g_client_failed = 1; return err; }
    tcp_recv(tpcb, cli_recv);
    err_t w = tcp_write(tpcb, SPIKE_PAYLOAD, (u16_t)strlen(SPIKE_PAYLOAD), TCP_WRITE_FLAG_COPY);
    if (w != ERR_OK) { g_client_failed = 1; return w; }
    tcp_output(tpcb);
    return ERR_OK;
}

int main(void) {
    lwip_init();

    /* Find the loopback netif LWIP_HAVE_LOOPIF created and bring it up/default.
     * In STABLE-2_2_0 the loopif is a global `loop_netif` (netif/loopif). We locate
     * it by matching the 127.0.0.1 address rather than depending on the symbol. */
    ip4_addr_t loop_ip, loop_nm, loop_gw;
    IP4_ADDR(&loop_ip, 127, 0, 0, 1);
    IP4_ADDR(&loop_nm, 255, 0, 0, 0);
    IP4_ADDR(&loop_gw, 127, 0, 0, 1);

    struct netif *lo = NULL;
    struct netif *nif;
    NETIF_FOREACH(nif) {
        if (ip4_addr_isloopback(netif_ip4_addr(nif))) { lo = nif; break; }
    }
    if (!lo) {
        /* No auto loopif present (build didn't create one) — hard blocker for CI. */
        printf("SPIKE FAIL: no loopback netif after lwip_init (LWIP_HAVE_LOOPIF)\n");
        return 3;
    }
    netif_set_addr(lo, &loop_ip, &loop_nm, &loop_gw);
    netif_set_up(lo);
    netif_set_link_up(lo);
    netif_set_default(lo);

    /* ── server ── */
    struct tcp_pcb *srv = tcp_new();
    if (!srv) { printf("SPIKE FAIL: tcp_new(server)\n"); return 3; }
    if (tcp_bind(srv, IP_ADDR_ANY, SPIKE_PORT) != ERR_OK) {
        printf("SPIKE FAIL: tcp_bind\n"); return 3;
    }
    srv = tcp_listen(srv);
    if (!srv) { printf("SPIKE FAIL: tcp_listen\n"); return 3; }
    tcp_accept(srv, srv_accept);

    /* ── client ── */
    ip_addr_t dst;
    IP_ADDR4(&dst, 127, 0, 0, 1);
    struct tcp_pcb *cli = tcp_new();
    if (!cli) { printf("SPIKE FAIL: tcp_new(client)\n"); return 3; }
    if (tcp_connect(cli, &dst, SPIKE_PORT, cli_connected) != ERR_OK) {
        printf("SPIKE FAIL: tcp_connect\n"); return 3;
    }

    /* ── bare mainloop: drive lwIP entirely from here ── */
    int ticks = 0;
    for (; ticks < SPIKE_MAX_TICKS && !g_client_done && !g_client_failed; ticks++) {
        sys_check_timeouts();       /* fire lwIP's timers (retransmit, TCP fast/slow) */
        netif_poll(lo);             /* drain the loopback TX queue into RX */
        struct timespec sl = { 0, 1000000 };   /* 1ms */
        nanosleep(&sl, NULL);
    }

    if (g_client_done && strcmp(g_echo_buf, SPIKE_PAYLOAD) == 0) {
        printf("SPIKE PASS: raw+loopback+NO_SYS roundtrip after %d ticks — sent=\"%s\" echo=\"%s\"\n",
               ticks, SPIKE_PAYLOAD, g_echo_buf);
        return 0;
    }
    printf("SPIKE FAIL: no matching echo (done=%d failed=%d ticks=%d echo_len=%d echo=\"%s\")\n",
           g_client_done, g_client_failed, ticks, g_echo_len, g_echo_buf);
    return 1;
}
