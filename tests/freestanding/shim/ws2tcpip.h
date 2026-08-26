/* freestanding shim <ws2tcpip.h>: see ../README.md. */
#ifndef KEEL_FS_SHIM_WS2TCPIP_H
#define KEEL_FS_SHIM_WS2TCPIP_H
typedef int socklen_t;
struct sockaddr_storage { unsigned short ss_family; char __ss_pad[126]; };
#endif
