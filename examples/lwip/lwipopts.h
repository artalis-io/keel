/*
 * lwipopts.h — SAMPLE lwIP config for the Keel lwIP reference (examples/lwip).
 *
 * This is a minimal host/loopback-oriented config, NOT a blessed production one:
 * a real target supplies its own lwipopts.h. Key points for the Keel provider:
 *   - LWIP_SOCKET      : the BSD socket API socket_lwip.c maps onto
 *   - LWIP_SOCKET_POLL : lwip_poll, used by the reference event backend
 *   - LWIP_COMPAT_SOCKETS 0 : keep only the lwip_* names (no `socket`/`recv`
 *                             macro hijack that would clash with Keel/host)
 *   - LWIP_NETIF_LOOPBACK : 127.0.0.1 works with no real netif (host demo)
 */
#ifndef KEEL_LWIP_LWIPOPTS_H
#define KEEL_LWIP_LWIPOPTS_H

#define NO_SYS                      0
#define LWIP_IPV4                   1
#define LWIP_IPV6                   0
#define SYS_LIGHTWEIGHT_PROT        1

#define LWIP_NETCONN                1
#define LWIP_SOCKET                 1
#define LWIP_SOCKET_POLL            1
#define LWIP_COMPAT_SOCKETS         0   /* only lwip_* names — no macro hijack */
#define LWIP_POSIX_SOCKETS_IO_NAMES 0
#define LWIP_TCP_KEEPALIVE          1
#define SO_REUSE                    1

#define LWIP_DNS                    1
#define LWIP_NETIF_LOOPBACK         1   /* 127.0.0.1 without a real netif */
#define LWIP_HAVE_LOOPIF            1

#define MEM_ALIGNMENT               4
#define MEM_SIZE                    (256 * 1024)
#define MEMP_NUM_NETCONN            16
#define MEMP_NUM_TCP_PCB            16
#define PBUF_POOL_SIZE              64
#define TCP_MSS                     1460
#define TCP_SND_BUF                 (8 * TCP_MSS)
#define TCP_WND                     (8 * TCP_MSS)

#define LWIP_STATS                  0
#define LWIP_NETIF_API              1

#endif /* KEEL_LWIP_LWIPOPTS_H */
