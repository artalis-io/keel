/*
 * link_smoke.c: minimal link/relocation check for the OpenSSL integration.
 *
 * Does NOT perform a handshake (that is covered by tests/tls_e2e.c). This only
 * proves the adapter archive resolves against core libkeel.a + OpenSSL: it takes
 * the address of a client-context ctor and creates/destroys a client ctx (which
 * links the whole adapter TU + its OpenSSL symbols). A clean exit means the
 * relocation is sound.
 *
 * SPDX-License-Identifier: MIT
 */
#include <keel_tls_openssl.h>
#include <keel/allocator.h>
#include <stdio.h>

int main(void) {
    KlAllocator alloc = kl_allocator_default();
    /* Reference a ctor symbol so the linker must pull the adapter object in. */
    KlTlsCtx *(*ctor)(const char *, KlAllocator *) = kl_tls_openssl_client_ctx_create;
    KlTlsCtx *ctx = ctor(NULL, &alloc);           /* NULL CA → verify-none */
    if (ctx) kl_tls_openssl_ctx_destroy(ctx);
    printf("openssl adapter linked (ctx=%s)\n", ctx ? "created" : "null-ok");
    return 0;
}
