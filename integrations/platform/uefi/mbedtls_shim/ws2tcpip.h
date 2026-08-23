/*
 * mbedtls_shim/ws2tcpip.h — freestanding <ws2tcpip.h> for the mbedTLS TUs.
 *
 * tls_mbedtls.c includes src/socket.h -> sockcompat.h, whose _WIN32 branch (the
 * x86_64-unknown-windows target takes it) pulls <ws2tcpip.h>. The base freestanding
 * shim (tests/freestanding/shim/ws2tcpip.h) supplies socklen_t + sockaddr_storage;
 * mbedTLS's SAN-formatting path additionally references INET6_ADDRSTRLEN + inet_ntop.
 * Those SAN calls are unreached under a verify-none client, but must still COMPILE
 * and LINK. This local shim (searched via -isystem AHEAD of the base one) adds them.
 *
 * inet_ntop is defined in mbedtls_platform_uefi.c as an inert stub (returns NULL) —
 * it is only reached from mTLS peer-cert SAN extraction, which the client does
 * not perform.
 */
#ifndef KEEL_UEFI_MBEDTLS_SHIM_WS2TCPIP_H
#define KEEL_UEFI_MBEDTLS_SHIM_WS2TCPIP_H

#include <stddef.h>

typedef int socklen_t;
struct sockaddr_storage { unsigned short ss_family; char __ss_pad[126]; };

#ifndef INET6_ADDRSTRLEN
#define INET6_ADDRSTRLEN 46
#endif
#ifndef INET_ADDRSTRLEN
#define INET_ADDRSTRLEN 16
#endif

/* Declared here; inert stub in mbedtls_platform_uefi.c (SAN path unreached). */
const char *inet_ntop(int af, const void *src, char *dst, socklen_t size);

#endif /* KEEL_UEFI_MBEDTLS_SHIM_WS2TCPIP_H */
