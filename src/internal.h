#ifndef KEEL_INTERNAL_H
#define KEEL_INTERNAL_H

#include <unistd.h>

/* Suppress warn_unused_result on best-effort error writes */
static inline void best_effort_write(int fd, const void *buf, size_t len) {
    ssize_t r = write(fd, buf, len);
    (void)r;
}

#endif
