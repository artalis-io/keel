/* freestanding shim <stdlib.h>: declarations only (see ../README.md). */
#ifndef KEEL_FS_SHIM_STDLIB_H
#define KEEL_FS_SHIM_STDLIB_H
#include <stddef.h>
void          *malloc(size_t n);
void          *calloc(size_t n, size_t sz);
void          *realloc(void *p, size_t n);
void           free(void *p);
void           abort(void);
long           strtol(const char *s, char **end, int base);
unsigned long  strtoul(const char *s, char **end, int base);
#endif
