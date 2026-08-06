# Keel ⇄ OpenSSL / LibreSSL / BoringSSL integration

A first-party adapter implementing Keel's `KlTls` transport vtable
(`include/keel/tls.h`) on top of [OpenSSL](https://www.openssl.org/) 3.x. The
**same** source (`tls_openssl.c`) also builds against
[LibreSSL](https://www.libressl.org/) and
[BoringSSL](https://boringssl.googlesource.com/boringssl/) — the handful of API
divergences are gated with `#if defined(...)` and driven from
`integrations/libressl/` and `integrations/boringssl/` (see their READMEs).
Provides TLS 1.2/1.3 server support (with optional mTLS client-cert verification)
and client support.

Two transport modes over one custom `BIO_METHOD`:

- **Readiness / socket-BIO** (default): ciphertext I/O runs through Keel's
  internal socket seam (`kl_sock_send`/`kl_sock_recv`), so the same source works
  on POSIX and Windows and over non-kernel socket stacks (e.g. lwIP). Would-block
  maps to `BIO_set_retry_*` → `WANT_READ`/`WANT_WRITE`; a 0-byte recv is a clean
  EOF.
- **Completion / memory-BIO**: once `feed_input()` is first called, the session
  reads ciphertext from an internal in-buffer and appends outgoing ciphertext to
  an out-buffer (drained by `drain_output()`), never touching the fd. This is how
  the io_uring / IOCP / pollcomp / lwip-raw / UEFI completion backends drive TLS.

This lives **outside** the dependency-light core: plain `make` / `make test` in
the repo root never touch OpenSSL. You opt in explicitly.

## Bring your own OpenSSL

OpenSSL is **not vendored and never downloaded**. Supply it yourself — e.g.
Homebrew: `OPENSSL_DIR=$(brew --prefix openssl@3)`, or a distro's `libssl-dev`.
If `OPENSSL_DIR` is unset, the compiler's default search paths are used.

### Version matrix

This one adapter source (`tls_openssl.c`) is shared by three integration dirs —
`integrations/openssl/`, `integrations/boringssl/`, `integrations/libressl/` — which
just compile it against their respective library. The public API is the
`kl_tls_openssl_*` names for all three (the name denotes the adapter family).

| Library | Status | Notes |
|---------|--------|-------|
| OpenSSL 3.x | Verified (e2e, both axes) | Primary target; validated on 3.6.x. |
| LibreSSL | Verified (e2e, both axes) | Validated on 4.3.2 (Homebrew). See `integrations/libressl/`. |
| OpenSSL 1.1.x | Supported | The peer-cert accessor guard covers `< 3.0.0` (`SSL_get_peer_certificate`); `ASN1_TIME_to_tm` is 1.1.1+. |
| BoringSSL | Same adapter; build-gated | One `OPENSSL_IS_BORINGSSL` guard; see `integrations/boringssl/`. |

> Uses OpenSSL 3.x non-deprecated APIs (`SSL_get1_peer_certificate`,
> `X509_get0_notBefore`, `SSL_set1_host`, `ASN1_TIME_to_tm`, `EVP_*`).

## Build

**Standalone `libkeel_openssl.a`** (core stays pure; link the adapter alongside):

```sh
cd integrations/openssl && make OPENSSL_DIR=$(brew --prefix openssl@3)
```

## Link

```sh
cc app.c \
   -Iinclude -Iintegrations/openssl \
   -L. -lkeel -Lintegrations/openssl -lkeel_openssl \
   -L$OPENSSL_DIR/lib -lssl -lcrypto
```

`#include <keel_tls_openssl.h>` for the ctx constructors and the
`kl_tls_openssl_create` factory; wire them into `KlTlsConfig`:

```c
KlTlsCtx *ctx = kl_tls_openssl_ctx_create("cert.pem", "key.pem", NULL,
                                          KL_MTLS_NONE, &alloc);
KlConfig config = {
    .tls = &(KlTlsConfig){
        .ctx = ctx,
        .factory = kl_tls_openssl_create,
        .ctx_destroy = kl_tls_openssl_ctx_destroy,
    },
    /* ... */
};
```

For a client that must present a cert to an mTLS server, load it onto the client
ctx: `kl_tls_openssl_client_ctx_set_cert(ctx, cert, clen, key, klen)`.

## Test

```sh
# Link/relocation smoke:
make test OPENSSL_DIR=$(brew --prefix openssl@3)

# End-to-end (BOTH transport axes) under ASan+UBSan:
make e2e  OPENSSL_DIR=$(brew --prefix openssl@3)
```

`make e2e` builds and runs `e2e/tls_e2e.c`, which generates a self-signed CA +
server + client leaf at runtime (OpenSSL library API, no files, no CLI) and
proves:

1. **Axis 1 — socket-BIO**: a `socketpair()` carries ciphertext; both sessions
   handshake to completion on non-blocking fds; a payload round-trips both ways;
   **ALPN** negotiates `h2` (server preference); **mTLS** `peer_cert()` returns
   the verified client identity (CN, issuer, SAN, SHA-256 fingerprint, validity,
   DER).
2. **Axis 2 — memory-BIO**: NO sockets; ciphertext is shuttled purely via
   `feed_input`/`drain_output`; handshake + app-data round-trip both ways; a
   clean `shutdown()` surfaces on the peer as `read()==-1` with `at_eof()==1`.

## Compatibility promise

Source + static-relink only: the adapter tracks the public `KlTls` vtable in
`include/keel/tls.h`. No OpenSSL/BoringSSL type ever appears in a Keel core
public header.
