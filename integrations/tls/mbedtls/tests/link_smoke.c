/*
 * link_smoke.c: minimal link/relocation check for the mbedTLS integration.
 *
 * Does NOT perform a handshake (that is covered by the repo's smoke-tls target,
 * which exercises a real loopback handshake). This only proves the adapter
 * archive resolves against core libkeel.a + mbedTLS: it takes the address of a
 * client-context ctor and creates/destroys a client ctx (which links the whole
 * adapter TU + its mbedTLS symbols). A clean exit means the relocation is sound.
 */
#include <keel_tls_mbedtls.h>
#include <keel/allocator.h>
#include <stdio.h>

int main(void) {
    KlAllocator alloc = kl_allocator_default();
    /* Reference a ctor symbol so the linker must pull the adapter object in. */
    KlTlsCtx *(*ctor)(KlAllocator *) = kl_tls_mbedtls_client_ctx_create_insecure;
    KlTlsCtx *ctx = ctor(&alloc);                 /* no CA needed: nothing connects */
    if (ctx) kl_tls_mbedtls_ctx_destroy(ctx);
    printf("mbedtls adapter linked (ctx=%s)\n", ctx ? "created" : "null-ok");
    return 0;
}
