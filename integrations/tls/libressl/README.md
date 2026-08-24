# Keel ⇄ LibreSSL integration

LibreSSL is API-compatible enough with OpenSSL that the **same** Keel adapter
source serves it. This directory contains **no adapter source**; it compiles the
shared adapter, `integrations/tls/openssl/tls_openssl.c`, against a LibreSSL prefix.
That is the point: it demonstrates the identical `KlTls` implementation works
against LibreSSL, with only the peer-cert accessor gated by
`#if defined(LIBRESSL_VERSION_NUMBER)`.

The public API is the OpenSSL adapter's `kl_tls_openssl_*` names (the name
denotes the adapter family, not the linked library). `keel_tls_libressl.h` just
re-exports `../openssl/keel_tls_openssl.h`.

## What differs from OpenSSL (the guarded sites)

Exactly **one** site: OpenSSL 3.0 renamed the ref-taking peer-cert accessor to
`SSL_get1_peer_certificate`; LibreSSL (like BoringSSL) keeps
`SSL_get_peer_certificate` (ref-taking, `X509_free`d after use). Everything else,
the custom `BIO_METHOD`, `SSL_CTX`/`SSL` lifecycle, ALPN, `SSL_set1_host`,
`ASN1_TIME_to_tm`, the memory-BIO feed/drain rings, is shared verbatim.

## Bring your own LibreSSL

Homebrew ships LibreSSL (keg-only, so pass the prefix explicitly):

```sh
brew install libressl
make      LIBRESSL_DIR=$(brew --prefix libressl)   # build libkeel_libressl.a
make test LIBRESSL_DIR=$(brew --prefix libressl)   # link smoke
make e2e  LIBRESSL_DIR=$(brew --prefix libressl)   # both-axes handshake (ASan+UBSan)
```

Link into a consumer (adapter before core, LibreSSL last):

```sh
cc app.c -L<keel> -lkeel -L. -lkeel_libressl \
         -L$(brew --prefix libressl)/lib -lssl -lcrypto
```

## Verified

Built and end-to-end tested against **LibreSSL 4.3.2** (Homebrew). `make e2e`
exercises **both** `KlTls` transport axes with a self-signed CA + server/client
leaf generated at runtime:

- **Readiness / socket-BIO**: handshake over a `socketpair`, ALPN negotiation
  (h2), app-data round-trip both directions, and mTLS `peer_cert` returning the
  verified client identity.
- **Completion / memory-BIO**: a full handshake and round-trip with **no sockets
  at all**, ciphertext shuttled only via `feed_input`/`drain_output`, plus a
  clean `shutdown` surfacing `read()==-1` with `at_eof()==1`.

Both pass under ASan+UBSan.

## Compatibility promise

Same as the OpenSSL adapter: source + static-relink only, tracking the public
`KlTls` vtable. No LibreSSL type appears in a Keel core public header.

SPDX-License-Identifier: MIT
