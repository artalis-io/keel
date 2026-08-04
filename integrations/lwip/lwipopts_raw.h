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
#define LWIP_LOOPBACK_MAX_PBUFS          64   /* a full send window of segments may queue on
                                              * loopback TX before netif_poll drains it (P9-3) */

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

/* ── Memory / pools ──────────────────────────────────────────────────────────
 * P9-3 pushes a 64 KB response over loopback to force many tcp_sent rounds + hit
 * tcp_sndbuf-full (ERR_MEM) backpressure. The send WINDOW (TCP_SND_BUF = 8*TCP_MSS
 * ≈ 11-12 KB) is deliberately far smaller than the 64 KB body so the send-pump
 * stalls and resumes repeatedly — pbuf/segment demand is bounded by the window, not
 * the whole body. The pools + MEM_SIZE below comfortably cover one window's worth of
 * in-flight segments plus the loopback TX queue. */
#define MEM_LIBC_MALLOC             0
#define MEMP_MEM_MALLOC             0
/* 8-byte alignment: on a 64-bit host lwIP's struct pbuf / struct memp want 8-byte
 * alignment (pointer members). MEM_ALIGNMENT=8 makes lwIP align its pool allocations
 * accordingly — matching the host ABI and keeping the raw backend clean under UBSan's
 * alignment checker (a MEM_ALIGNMENT=4 build trips -fsanitize=alignment on aarch64). */
#define MEM_ALIGNMENT               8
#define MEM_SIZE                    (512 * 1024)

/* MEMP_NUM_TCP_PCB caps how many active TCP pcbs lwIP can hold. The Stage-A concurrency tests
 * (raw_recv_test) open many SIMULTANEOUS connections IN-PROCESS — both the server pcb AND the raw
 * test-client pcb live in the same lwIP stack — so this must cover ~2*N plus the listener and a
 * few pcbs lingering in TIME-WAIT. 40 comfortably covers the tests' bursts with slack; the small
 * per-conn responses keep segment demand low. (P9-1..P9-4 pass at any value >= 8; the exactly-
 * once close discipline that made rapid churn safe is in the glue, not the pool size.) */
#define MEMP_NUM_TCP_PCB            40
#define MEMP_NUM_TCP_PCB_LISTEN     4
#define MEMP_NUM_UDP_PCB            4
#define MEMP_NUM_RAW_PCB            4
#define MEMP_NUM_TCP_SEG            96

#define TCP_MSS                     1460
/* Keep the SEND window well under a big (64 KB) response so the P9-3 send-pump still stalls +
 * resumes on tcp_sent (ERR_MEM backpressure on the SEND side). */
#define TCP_SND_BUF                 (8 * TCP_MSS)
#define TCP_SND_QUEUELEN            (4 * TCP_SND_BUF / TCP_MSS)   /* enough segs for the window */
#define TCP_WND                     (8 * TCP_MSS)
#define PBUF_POOL_SIZE              96

/* Checksums: loopback bypasses the wire, but keep them on to exercise the real
 * TCP path (cheap at this scale). */

#endif /* KEEL_LWIP_LWIPOPTS_RAW_H */
