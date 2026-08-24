/* freestanding shim <stdio.h>: declarations only (see ../README.md).
 * Only what vendor/llhttp/api.c's never-called debug printer references. */
#ifndef KEEL_FS_SHIM_STDIO_H
#define KEEL_FS_SHIM_STDIO_H
#include <stddef.h>
typedef struct KEEL_FS_FILE FILE;
extern FILE *stderr;
int fprintf(FILE *stream, const char *fmt, ...);
int snprintf(char *buf, size_t n, const char *fmt, ...);
int printf(const char *fmt, ...);
#endif
