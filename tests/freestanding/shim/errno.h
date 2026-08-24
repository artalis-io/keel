/* freestanding shim <errno.h>: see ../README.md.
 * sockcompat.h's _WIN32 branch pulls <errno.h>; the freestanding client seam is
 * errno-free (classification rides KlIoStatus), so this only needs the symbol +
 * the would-block/EINTR/EINPROGRESS macros any include-order might reference. */
#ifndef KEEL_FS_SHIM_ERRNO_H
#define KEEL_FS_SHIM_ERRNO_H
extern int errno;
#define EAGAIN       11
#define EWOULDBLOCK  11
#define EINTR         4
#define EINPROGRESS 115
#endif
