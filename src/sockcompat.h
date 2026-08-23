#ifndef KEEL_SRC_SOCKCOMPAT_H
#define KEEL_SRC_SOCKCOMPAT_H

/*
 * sockcompat.h — the ONE contained platform-include boundary for socket types.
 *
 * This is the single place Keel resolves "where do struct sockaddr / socklen_t /
 * struct iovec / ssize_t come from" across platforms. It contains *no logic* —
 * only the system-header selection that is genuinely unavoidable for any file
 * referencing socket types on both POSIX and Windows. socket.h (and the per-
 * platform provider TUs) include this so they can stay logic-neutral. See
 * docs/archive/phases/phase6_winsock_design.md §B.0.
 */

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>    /* getaddrinfo / IPPROTO_* / TCP_NODELAY / IPV6_V6ONLY */
  #include <stdint.h>
  #include <sys/types.h>   /* off_t (MinGW) */
  #include <errno.h>
  /* AF_UNIX on Windows lives in <afunix.h> (Win10+); optional so a toolchain
   * without it still compiles the IP-only TUs. */
  #if defined(__has_include)
    #if __has_include(<afunix.h>)
      #include <afunix.h>
    #endif
  #endif

  /* MinGW has ssize_t via <sys/types.h>, but include-order can leave it
   * undefined here; define it pointer-width to match POSIX. */
  #ifndef _SSIZE_T_DEFINED
    #define _SSIZE_T_DEFINED
    typedef intptr_t ssize_t;
  #endif

  /* Windows has no <sys/uio.h>; provide a POSIX-layout struct iovec so the seam
   * and http_response.c keep using it unchanged. The Winsock writev provider converts
   * struct iovec[] -> WSABUF[] internally. */
  #ifndef KL_HAVE_STRUCT_IOVEC
    #define KL_HAVE_STRUCT_IOVEC
    struct iovec { void *iov_base; size_t iov_len; };
  #endif

  /* Translate WSAGetLastError() into the CRT errno space (defined in
   * socket_winsock.c). Shared so the Winsock event backend and kl_plat_poll1
   * signal would-block/EINTR the same way the socket seam ops do — callers that
   * branch on errno after a -1 return depend on it. */
  void kl_wsa_set_errno(void);
#elif defined(KEEL_PLATFORM_LWIP)
  /* lwIP target: the stack provides its own BSD socket types (struct sockaddr{,_
   * storage}, socklen_t, struct iovec) via lwip/sockets.h — do NOT pull the host
   * <sys/socket.h>, which would clash. The internal counterpart of net.h's lwIP
   * branch, so an internal TU built with -DKEEL_PLATFORM_LWIP (e.g. the -Isrc lwIP
   * seam overrides resolve_sync_lwip.c / platform_wakeup_lwip.c) resolves socket
   * types to lwIP's, not the host's. Inert unless a provider defines
   * KEEL_PLATFORM_LWIP. ssize_t comes
   * from the host libc <sys/types.h> (no sockaddr), which the lwIP unix port
   * already builds against. */
  #include <sys/types.h>
  #include <errno.h>
  #include "lwip/sockets.h"
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <sys/uio.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>   /* TCP_NODELAY */
  #include <arpa/inet.h>
  #include <netdb.h>         /* getaddrinfo / struct addrinfo */
  #include <sys/un.h>        /* struct sockaddr_un (AF_UNIX) */
  #include <unistd.h>
  #include <errno.h>
#endif

#endif /* KEEL_SRC_SOCKCOMPAT_H */
