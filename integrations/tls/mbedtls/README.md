# Keel ⇄ mbedTLS integration

A first-party adapter implementing Keel's `KlTls` transport vtable
(`include/keel/tls.h`) on top of [mbedTLS](https://github.com/Mbed-TLS/mbedtls)
3.x. Provides TLS 1.2/1.3 server support (with optional mTLS client-cert
verification) and client support. All I/O is non-blocking and routed through
Keel's internal socket seam, so the same source builds on POSIX and Windows.

This lives **outside** the dependency-light core: plain `make` / `make test` in
the repo root never touch mbedTLS. You opt in explicitly.

## Bring your own mbedTLS

mbedTLS is **not vendored and never downloaded**. Supply it yourself:

- a **system prefix** (`include/` + `lib/`), e.g. Homebrew:
  `MBEDTLS_DIR=$(brew --prefix mbedtls)`, or a Linux distro's `libmbedtls-dev`; or
- a **source tree** (`include/` + `library/`) you built yourself.

If `MBEDTLS_DIR` is unset, the compiler's default search paths are used
(e.g. MSYS2 `/mingw64`).

### Version matrix

| mbedTLS | Status | Notes |
|---------|--------|-------|
| 3.6.x (LTS) | Supported | Primary target; validated via `smoke-tls`. |
| 3.5.x | Expected to work | Same 3.x `mbedtls_pk_parse_key` signature. |
| 2.x | Unsupported | Pre-3.0 API differs (`mbedtls_pk_parse_key` arg count, PSA). |

> The mbedTLS 3.x → 2.x break in `mbedtls_pk_parse_key` is exactly why the
> adapter is BYO-version and kept out of CI: distros ship incompatible majors.

## Build

Two equivalent paths:

**Folded into `libkeel.a`** (simplest for the repo's own smokes/examples):

```sh
make KEEL_TLS=mbedtls MBEDTLS_DIR=$(brew --prefix mbedtls)          # from repo root
make KEEL_TLS=mbedtls MBEDTLS_DIR=$(brew --prefix mbedtls) smoke-tls
```

**Standalone `libkeel_mbedtls.a`** (core stays pure; link the adapter alongside):

```sh
make integration-mbedtls MBEDTLS_DIR=$(brew --prefix mbedtls)      # from repo root
# or directly:
cd integrations/tls/mbedtls && make MBEDTLS_DIR=$(brew --prefix mbedtls)
```

## Link

```sh
cc app.c \
   -Iinclude -Iintegrations/tls/mbedtls \
   -L. -lkeel -Lintegrations/tls/mbedtls -lkeel_mbedtls \
   -L$MBEDTLS_DIR/lib -lmbedtls -lmbedx509 -lmbedcrypto
```

`#include <keel_tls_mbedtls.h>` for the ctx constructors and the
`kl_tls_mbedtls_create` factory; wire them into `KlTlsConfig`:

```c
KlTlsCtx *ctx = kl_tls_mbedtls_ctx_create("cert.pem", "key.pem", NULL, 0, &alloc);
KlHttpServerConfig config = {
    .tls = &(KlTlsConfig){
        .ctx = ctx,
        .factory = kl_tls_mbedtls_create,
        .ctx_destroy = kl_tls_mbedtls_ctx_destroy,
    },
    /* ... */
};
```

## Test

```sh
make integration-mbedtls MBEDTLS_DIR=... && \
  (cd integrations/tls/mbedtls && make test MBEDTLS_DIR=...)   # link/relocation smoke
make KEEL_TLS=mbedtls MBEDTLS_DIR=... smoke-tls            # real loopback handshake
```

## Compatibility promise

Source + static-relink only: the adapter tracks the public `KlTls` vtable in
`include/keel/tls.h`. No mbedTLS type ever appears in a Keel core public header.
