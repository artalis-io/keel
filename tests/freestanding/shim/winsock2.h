/* freestanding shim <winsock2.h> — types + AF_/SOCK_ constants only.
 * See ../README.md. sockcompat.h's _WIN32 branch pulls this; the freestanding
 * client passes addresses as the neutral KlSockAddr and never touches a native
 * struct sockaddr, so only the family/type constants client_async.c maps to are
 * needed. A real UEFI build supplies these from the EFI socket protocols. */
#ifndef KEEL_FS_SHIM_WINSOCK2_H
#define KEEL_FS_SHIM_WINSOCK2_H
#include <stdint.h>
#include <stddef.h>

typedef uintptr_t SOCKET;
struct sockaddr { unsigned short sa_family; char sa_data[14]; };
typedef struct sockaddr SOCKADDR;

#define AF_UNIX      1
#define AF_INET      2
#define AF_INET6    23
#define SOCK_STREAM  1
#define SOCK_DGRAM   2

#endif
