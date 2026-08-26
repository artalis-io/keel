/*
 * lwipopts_raw.h: NO_SYS=1 (bare mainloop) lwIP config for the raw completion backend.
 *
 * This is the config for a KEEL-DRIVEN raw (tcp_*) callback-API provider: lwIP runs
 * with NO tcpip thread, NO sockets, NO netconn/api layer. KEEL's single event loop
 * drives lwIP by calling sys_check_timeouts() + draining the loopback netif each
 * tick. The raw tcp_* callbacks fire inline on that loop thread; no locking needed.
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
#define LWIP_RAW                    1   /* raw PCB API */
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
                                              * loopback TX before netif_poll drains it */

/* Loopback netif does not need link-layer callbacks / status callbacks. */
#define LWIP_NETIF_STATUS_CALLBACK  0
#define LWIP_NETIF_LINK_CALLBACK    0

/* ── RNG (placeholder, see lwipopts.h warning; fine for a loopback spike) ────*/
#define LWIP_RAND()                 ((u32_t)rand())
#define LWIP_RANDOMIZE_INITIAL_LOCAL_PORTS 1

/* ── Diagnostics off ─────────────────────────────────────────────────────────*/
#define MEMP_OVERFLOW_CHECK         0
#define MEM_SANITY_CHECK            0
#define LWIP_STATS                  0
#define LWIP_STATS_DISPLAY          0

/* ── Memory / pools ──────────────────────────────────────────────────────────
 * A large (64 KB) response over loopback forces many tcp_sent rounds + hits
 * tcp_sndbuf-full (ERR_MEM) backpressure. The send WINDOW (TCP_SND_BUF = 8*TCP_MSS
 * ≈ 11-12 KB) is deliberately far smaller than the 64 KB body so the send-pump
 * stalls and resumes repeatedly; pbuf/segment demand is bounded by the window, not
 * the whole body. The pools + MEM_SIZE below comfortably cover one window's worth of
 * in-flight segments plus the loopback TX queue. */
#define MEM_LIBC_MALLOC             0
#define MEMP_MEM_MALLOC             0
/* 8-byte alignment: on a 64-bit host lwIP's struct pbuf / struct memp want 8-byte
 * alignment (pointer members). MEM_ALIGNMENT=8 makes lwIP align its pool allocations
 * accordingly, matching the host ABI and keeping the raw backend clean under UBSan's
 * alignment checker (a MEM_ALIGNMENT=4 build trips -fsanitize=alignment on aarch64). */
#define MEM_ALIGNMENT               8
/* TLS over raw: a TLS record is up to 16 KiB, and in the loopback test BOTH ends live in the
 * SAME lwIP stack, so a handshake round has ciphertext in flight on the client pcb AND the server
 * pcb (plus each side's retained rx) concurrently. 1 MiB gives comfortable slack for both
 * directions' segments + the loopback TX queue during the handshake burst. */
#define MEM_SIZE                    (1024 * 1024)

/* MEMP_NUM_TCP_PCB caps how many active TCP pcbs lwIP can hold. The Stage-A concurrency tests
 * (raw_recv_test) open many SIMULTANEOUS connections IN-PROCESS, both the server pcb AND the raw
 * test-client pcb live in the same lwIP stack, so this must cover ~2*N plus the listener and a
 * few pcbs lingering in TIME-WAIT. 40 comfortably covers the tests' bursts with slack; the small
 * per-conn responses keep segment demand low. The exactly-once close discipline that makes rapid
 * churn safe is in the glue, not the pool size. */
#define MEMP_NUM_TCP_PCB            40
#define MEMP_NUM_TCP_PCB_LISTEN     4
#define MEMP_NUM_UDP_PCB            4
#define MEMP_NUM_RAW_PCB            4
/* A 16 KiB TLS record is ~12 segments at TCP_MSS=1460; with both ends in one stack and
 * multiple handshake records + the request/response in flight, segment demand rises. Raise the
 * segment pool so a full TLS record can be queued in both directions without ERR_MEM stalling the
 * handshake. The 64 KiB SEND path is still window-bounded, not seg-pool-bounded. */
#define MEMP_NUM_TCP_SEG            192

#define TCP_MSS                     1460
/* TLS over raw: the RECEIVE window (TCP_WND) must admit a full 16 KiB TLS record in one
 * window so the peer can push a whole handshake/application record without the window closing
 * mid-record and deadlocking (the receiver only advances the window as the completion driver
 * feed_input's + drains). 12*TCP_MSS ≈ 17.5 KiB > 16 KiB covers the largest record with slack.
 *
 * The SEND buffer (TCP_SND_BUF) is DELIBERATELY kept smaller than a big (64 KB) plaintext response
 * so the send-pump still stalls + resumes on tcp_sent (ERR_MEM backpressure on the SEND side)
 * but it must still admit a full 16 KiB TLS record so a single tls->write of one record does not
 * itself deadlock. 12*TCP_MSS satisfies both (>16 KiB record, <64 KiB body). */
#define TCP_SND_BUF                 (12 * TCP_MSS)
#define TCP_SND_QUEUELEN            (4 * TCP_SND_BUF / TCP_MSS)   /* enough segs for the window */
#define TCP_WND                     (12 * TCP_MSS)
/* More pbufs for concurrent in-flight TLS records on both pcbs + retained rx. */
#define PBUF_POOL_SIZE              128

/* Checksums: loopback bypasses the wire, but keep them on to exercise the real
 * TCP path (cheap at this scale). */

#endif /* KEEL_LWIP_LWIPOPTS_RAW_H */
