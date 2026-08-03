/*
 * lwipopts_raw.h — NO_SYS=1 (bare mainloop) lwIP config for the Phase 9 spike.
 *
 * This is the config for a KEEL-DRIVEN raw (tcp_*) callback-API provider: lwIP runs
 * with NO tcpip thread, NO sockets, NO netconn/api layer. KEEL's single event loop
 * drives lwIP by calling sys_check_timeouts() + draining the loopback netif each
 * tick. The raw tcp_* callbacks fire inline on that loop thread — no locking needed.
 *
 * Contrast with lwipopts.h (NO_SYS=0): that one is the sockets/netconn/tcpip_thread
 * baseline the existing loopback test rides. This file is a SEPARATE config used
 * only for the raw NO_SYS=1 build (liblwip_raw.a).
 */
#ifndef KEEL_LWIP_LWIPOPTS_RAW_H
#define KEEL_LWIP_LWIPOPTS_RAW_H

#include <stdlib.h>   /* rand() for the placeholder LWIP_RAND */

/* ── Bare mode: no OS, no threads, no sockets/netconn ─────────────────────────
 * NO_SYS=1 is the crux: lwIP raw tcp_* callbacks are not thread-safe and must run
 * on a single mainloop. KEEL owns that loop. */
#define NO_SYS                      1
#define SYS_LIGHTWEIGHT_PROT        0
#define LWIP_NETCONN                0   /* no sequential API */
#define LWIP_SOCKET                 0   /* no BSD socket layer */
#define LWIP_NETIF_API              0   /* netifapi needs tcpip thread */

/* ── Protocols: raw TCP over IPv4 loopback ───────────────────────────────────*/
#define LWIP_IPV4                   1
#define LWIP_IPV6                   0
#define LWIP_TCP                    1
#define LWIP_RAW                    1   /* raw PCB API (spike scope names it) */
#define LWIP_UDP                    1   /* cheap; keeps ip layer complete */
#define LWIP_DNS                    0
#define LWIP_ICMP                   1
#define LWIP_IGMP                   0
#define LWIP_ARP                    0   /* loopback needs no ARP */
#define LWIP_ETHERNET               0

/* ── Timers: NO_SYS=1 needs sys_check_timeouts() driven from the mainloop ─────*/
#define LWIP_TIMERS                 1
#define NO_SYS_NO_TIMERS            0   /* keep the sys_timeout machinery */

/* ── Loopback netif (127.0.0.1) with no real hardware ────────────────────────
 * LWIP_HAVE_LOOPIF auto-creates loop_netif at lwip_init(). LOOPBACK_MULTITHREADING
 * MUST be 0 in NO_SYS mode: loop TX is queued and drained by netif_poll() from the
 * mainloop (there is no tcpip thread to hand it to). */
#define LWIP_NETIF_LOOPBACK               1
#define LWIP_HAVE_LOOPIF                  1
#define LWIP_NETIF_LOOPBACK_MULTITHREADING 0
#define LWIP_LOOPBACK_MAX_PBUFS          16

/* Loopback netif does not need link-layer callbacks / status callbacks. */
#define LWIP_NETIF_STATUS_CALLBACK  0
#define LWIP_NETIF_LINK_CALLBACK    0

/* ── RNG (placeholder — see lwipopts.h warning; fine for a loopback spike) ────*/
#define LWIP_RAND()                 ((u32_t)rand())
#define LWIP_RANDOMIZE_INITIAL_LOCAL_PORTS 1

/* ── Diagnostics off ─────────────────────────────────────────────────────────*/
#define MEMP_OVERFLOW_CHECK         0
#define MEM_SANITY_CHECK            0
#define LWIP_STATS                  0
#define LWIP_STATS_DISPLAY          0

/* ── Memory / pools: small — this is loopback echo, a handful of PCBs ─────────*/
#define MEM_LIBC_MALLOC             0
#define MEMP_MEM_MALLOC             0
#define MEM_ALIGNMENT               4
#define MEM_SIZE                    (64 * 1024)

#define MEMP_NUM_TCP_PCB            8
#define MEMP_NUM_TCP_PCB_LISTEN     4
#define MEMP_NUM_UDP_PCB            4
#define MEMP_NUM_RAW_PCB            4
#define MEMP_NUM_TCP_SEG            32

#define TCP_MSS                     1460
#define TCP_SND_BUF                 (4 * TCP_MSS)
#define TCP_WND                     (4 * TCP_MSS)
#define PBUF_POOL_SIZE              32

/* Checksums: loopback bypasses the wire, but keep them on to exercise the real
 * TCP path (cheap at this scale). */

#endif /* KEEL_LWIP_LWIPOPTS_RAW_H */
