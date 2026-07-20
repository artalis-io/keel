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
 * docs/phase6_winsock_design.md §B.0.
 */

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <stdint.h>
  #include <sys/types.h>   /* off_t (MinGW) */
  #include <errno.h>

  /* MinGW has ssize_t via <sys/types.h>, but include-order can leave it
   * undefined here; define it pointer-width to match POSIX. */
  #ifndef _SSIZE_T_DEFINED
    #define _SSIZE_T_DEFINED
    typedef intptr_t ssize_t;
  #endif

  /* Windows has no <sys/uio.h>; provide a POSIX-layout struct iovec so the seam
   * and response.c keep using it unchanged. The Winsock writev provider converts
   * struct iovec[] -> WSABUF[] internally. */
  #ifndef KL_HAVE_STRUCT_IOVEC
    #define KL_HAVE_STRUCT_IOVEC
    struct iovec { void *iov_base; size_t iov_len; };
  #endif
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <sys/uio.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <errno.h>
#endif

#endif /* KEEL_SRC_SOCKCOMPAT_H */
