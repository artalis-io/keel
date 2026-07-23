#ifndef KEEL_NET_H
#define KEEL_NET_H

/*
 * net.h — the public socket-types compatibility boundary.
 *
 * The single place Keel's *installed* headers resolve where struct sockaddr /
 * sockaddr_storage / socklen_t come from across platforms. Public headers that
 * expose socket addresses in their API (connection.h, resolver.h,
 * proxy_protocol.h, udp.h, udp_server.h) include this instead of <sys/socket.h>,
 * so the platform selection lives in one file rather than scattered #ifdefs.
 *
 * This is the public counterpart of the internal src/sockcompat.h (which adds
 * implementation-only shims: struct iovec, ssize_t, afunix, etc.).
 *
 * On Windows, winsock2.h must precede windows.h — including this header first
 * keeps that ordering correct for consumers.
 */

#if defined(_WIN32)
  #include <winsock2.h>   /* SOCKET, struct sockaddr, fd_set */
  #include <ws2tcpip.h>   /* struct sockaddr_storage, socklen_t, getaddrinfo */
#else
  #include <sys/socket.h>
#endif

#endif /* KEEL_NET_H */
