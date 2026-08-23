/*
 * mbedtls_shim/stdio.h — freestanding <stdio.h> for the mbedTLS adapter build.
 *
 * The base shim (tests/freestanding/shim/stdio.h) declares FILE/stderr/fprintf/
 * snprintf/printf for the vendored-llhttp residual. tls_mbedtls.c's file-based
 * cert loader (read_file) additionally references fopen/fseek/ftell/fread/fclose +
 * SEEK_SET/SEEK_END. That path is DEAD (we use the *_from_buf ctx creators,
 * never a filesystem path), but it must still COMPILE and LINK. This local shim
 * (searched via -isystem AHEAD of the base) supplies the full set; the file ops are
 * inert stubs in mbedtls_platform_uefi.c that fail-closed (fopen -> NULL).
 *
 * Superset of the base shim, so it fully shadows it for the mbedTLS TUs.
 */
#ifndef KEEL_UEFI_MBEDTLS_SHIM_STDIO_H
#define KEEL_UEFI_MBEDTLS_SHIM_STDIO_H
#include <stddef.h>

typedef struct KEEL_FS_FILE FILE;
extern FILE *stderr;

#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

int   fprintf(FILE *stream, const char *fmt, ...);
int   snprintf(char *buf, size_t n, const char *fmt, ...);
int   printf(const char *fmt, ...);

FILE  *fopen(const char *path, const char *mode);
int    fclose(FILE *f);
int    fseek(FILE *f, long off, int whence);
long   ftell(FILE *f);
size_t fread(void *ptr, size_t sz, size_t n, FILE *f);

#endif /* KEEL_UEFI_MBEDTLS_SHIM_STDIO_H */
