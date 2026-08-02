/*
 * lwipopts.h — a production-oriented lwIP config baseline for the Keel integration.
 *
 * This is a *blessed baseline*, not a drop-in for every target: it is dependency-
 * light, hardened, and documented, and it is what the Keel lwIP loopback +
 * HTTPS-over-lwIP tests run against — so the settings the Keel providers depend on
 * are exercised in CI. A real deployment copies this and TUNES the sizing block
 * (search "TUNE") to its own memory budget + concurrency, and MUST replace the
 * placeholder RNG (see LWIP_RAND) with a cryptographic source.
 *
 * Three layers below:
 *   1. Keel provider requirements — do NOT change; the socket/event/udp/TLS
 *      providers rely on these exact features.
 *   2. Security hardening — sane, protocol-level defaults independent of scale.
 *   3. Scale / memory — the per-deployment tuning surface.
 */
#ifndef KEEL_LWIP_LWIPOPTS_H
#define KEEL_LWIP_LWIPOPTS_H

#include <stdlib.h>   /* rand() for the placeholder LWIP_RAND — see the warning */

/* ── 1. Keel provider requirements (do NOT change) ───────────────────────────
 * The socket provider (socket_lwip.c) maps onto the BSD socket API; the event
 * backend (event_lwip.c) polls via lwip_poll; udp_io_lwip.c + the resolve_sync
 * client seam need UDP + DNS; TLS-over-lwIP rides the same socket provider. */
#define NO_SYS                      0   /* OS mode: tcpip_thread + sockets/netconn */
#define SYS_LIGHTWEIGHT_PROT        1
#define LWIP_NETCONN                1
#define LWIP_SOCKET                 1   /* BSD socket API socket_lwip.c maps onto */
#define LWIP_SOCKET_POLL            1   /* lwip_poll — the event_lwip.c backend */
#define LWIP_COMPAT_SOCKETS         0   /* only lwip_* names — no socket()/recv() hijack */
#define LWIP_POSIX_SOCKETS_IO_NAMES 0
#define LWIP_NETIF_API              1
#define LWIP_TCP                    1
#define LWIP_UDP                    1   /* KlUdp / udp_server / DNS transport */
#define LWIP_DNS                    1   /* resolve_sync_lwip.c (lwip_getaddrinfo) */
#define LWIP_IGMP                   1   /* IPv4 multicast join/leave (udp_io_lwip) */
#define SO_REUSE                    1   /* SO_REUSEADDR (bind reuse) */
#define LWIP_SO_RCVBUF              1   /* SO_RCVBUF (udp_io_lwip best-effort sizing) */

/* ── 2. Security + robustness hardening (scale-independent) ───────────────────
 *
 * WARNING — RNG: lwIP draws its ephemeral-port randomization and (with a hook)
 * TCP ISNs from LWIP_RAND(). The placeholder below uses the C library rand(),
 * which is NOT cryptographically secure. A production build MUST define LWIP_RAND
 * to a CSPRNG (e.g. mbedtls_ctr_drbg_random via a small shim) and SHOULD install
 * a keyed initial-sequence-number hook (LWIP_HOOK_TCP_ISN, see lwIP contrib
 * ports) — the built-in ISN is predictable and enables off-path TCP injection. */
#define LWIP_RAND()                 ((u32_t)rand())
#define LWIP_RANDOMIZE_INITIAL_LOCAL_PORTS 1   /* don't hand out predictable source ports */

#define LWIP_TCP_KEEPALIVE          1   /* detect dead peers on idle keep-alive conns */
#define LWIP_SO_RCVTIMEO            1   /* per-socket receive timeout (SO_RCVTIMEO) */
#define LWIP_SO_SNDTIMEO            1   /* per-socket send timeout (SO_SNDTIMEO) */
#define TCP_QUEUE_OOSEQ            1   /* reassemble out-of-order segments (lwIP bounds the queue) */
#define TCP_LISTEN_BACKLOG         1   /* honour listen() backlog — bound half-open accept storms */
#define IP_REASS_MAX_PBUFS         10   /* cap IP-reassembly buffers (fragmentation DoS guard) */

/* Diagnostics OFF in production (flip on only for a debug build): the overflow /
 * sanity canaries and stats cost memory + cycles and are a debugging aid, not a
 * runtime safety net. */
#define MEMP_OVERFLOW_CHECK         0
#define MEM_SANITY_CHECK            0
#define LWIP_STATS                  0
#define LWIP_STATS_DISPLAY          0

/* ── 3. Scale / memory — TUNE per deployment ─────────────────────────────────
 * These bound concurrency and buffering. Sized here for a small server (~64
 * concurrent TCP connections). Raise for more load, lower for a constrained MCU;
 * the heap (MEM_SIZE) and the pbuf/PCB pools are the dominant RAM consumers.
 * lwIP sanity-checks these against each other at init — keep the relationships
 * (e.g. MEMP_NUM_TCP_SEG >= TCP_SND_QUEUELEN, MEMP_NUM_NETCONN >= the PCB pools). */
#define MEM_ALIGNMENT               4
#define MEM_SIZE                    (512 * 1024)   /* TUNE: main heap */

#define MEMP_NUM_TCP_PCB            64             /* TUNE: max concurrent established conns */
#define MEMP_NUM_TCP_PCB_LISTEN     8              /* listening sockets */
#define MEMP_NUM_UDP_PCB            16             /* UDP sockets (KlUdp + DNS) */
#define MEMP_NUM_NETCONN            (MEMP_NUM_TCP_PCB + MEMP_NUM_TCP_PCB_LISTEN + MEMP_NUM_UDP_PCB)

#define TCP_MSS                     1460
#define TCP_SND_BUF                 (8 * TCP_MSS)  /* per-conn send buffer */
#define TCP_WND                     (8 * TCP_MSS)  /* per-conn receive window */
/* TCP_SND_QUEUELEN derives from TCP_SND_BUF (~4*SND_BUF/MSS = 32); the segment
 * pool must cover it with headroom for concurrent senders. */
#define MEMP_NUM_TCP_SEG            128
#define PBUF_POOL_SIZE              128            /* TUNE: RX/TX pbuf pool */

/* Mailboxes for the sequential (tcpip_thread) API the sockets layer rides. */
#define TCPIP_MBOX_SIZE             32
#define DEFAULT_ACCEPTMBOX_SIZE     32
#define DEFAULT_TCP_RECVMBOX_SIZE   32
#define DEFAULT_UDP_RECVMBOX_SIZE   32

/* ── Protocol family ─────────────────────────────────────────────────────────
 * IPv4 only by default. To run dual-stack, set LWIP_IPV6 1 (and, for IPv6
 * multicast, LWIP_IPV6_MLD 1): socket_lwip.c and udp_io_lwip.c already carry the
 * LWIP_IPV6 marshalling/branches, so no Keel change is needed — only more RAM. */
#define LWIP_IPV4                   1
#define LWIP_IPV6                   0

/* Loopback (127.0.0.1) without a real netif — needed by the loopback tests and
 * harmless in production (a same-host client/health-check can use it). */
#define LWIP_NETIF_LOOPBACK         1
#define LWIP_HAVE_LOOPIF            1

#endif /* KEEL_LWIP_LWIPOPTS_H */
