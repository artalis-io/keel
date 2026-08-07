/*
 * link_smoke.c — link/relocation check for the LibreSSL build of the shared
 * OpenSSL adapter. Identical intent to integrations/openssl/link_smoke.c, but
 * links against LibreSSL. A clean exit proves ../openssl/tls_openssl.c resolves
 * against core libkeel.a + LibreSSL through the LIBRESSL_VERSION_NUMBER guards.
 *
 * SPDX-License-Identifier: MIT
 */
#include "keel_tls_libressl.h"   /* re-exports the kl_tls_openssl_* API */
#include <keel/allocator.h>
#include <stdio.h>

int main(void) {
    KlAllocator alloc = kl_allocator_default();
    KlTlsCtx *(*ctor)(const char *, KlAllocator *) = kl_tls_openssl_client_ctx_create;
    KlTlsCtx *ctx = ctor(NULL, &alloc);
    if (ctx) kl_tls_openssl_ctx_destroy(ctx);
    printf("libressl adapter linked (ctx=%s)\n", ctx ? "created" : "null-ok");
    return 0;
}
