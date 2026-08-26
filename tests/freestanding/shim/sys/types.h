/* freestanding shim <sys/types.h>: see ../../README.md.
 * The client TUs pull this for ssize_t; sockcompat.h typedefs ssize_t itself
 * (intptr_t) under _WIN32, so this only needs the fixed-width base types. */
#ifndef KEEL_FS_SHIM_SYS_TYPES_H
#define KEEL_FS_SHIM_SYS_TYPES_H
#include <stdint.h>
#include <stddef.h>
#endif
