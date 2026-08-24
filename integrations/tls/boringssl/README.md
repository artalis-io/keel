# Keel ⇄ BoringSSL integration

BoringSSL is API-compatible enough with OpenSSL that the **same** Keel adapter
source serves both. This directory contains **no adapter source**; it compiles
the shared adapter, `integrations/tls/openssl/tls_openssl.c`, against a BoringSSL
prefix. That is the whole point: it demonstrates the identical `KlTls`
implementation works against BoringSSL, with only a few call sites gated by
`#if defined(OPENSSL_IS_BORINGSSL)` (a macro BoringSSL's `<openssl/base.h>`
defines).

The public API is the OpenSSL adapter's `kl_tls_openssl_*` names (the name
denotes the adapter family, not the linked library). `keel_tls_boringssl.h` just
re-exports `../openssl/keel_tls_openssl.h`.

## What differs from OpenSSL (the guarded sites)

The shared adapter (`../openssl/tls_openssl.c`) needs exactly **two**
`OPENSSL_IS_BORINGSSL` guards:

1. **Peer-certificate accessor.** OpenSSL 3.0 renamed the ref-taking accessor to
   `SSL_get1_peer_certificate`; BoringSSL keeps `SSL_get_peer_certificate`
   (ref-taking, `X509_free`d after use). The guard selects the right name.
2. **ASN1 time → Unix.** BoringSSL does not provide `ASN1_TIME_to_tm`; it offers
   `ASN1_TIME_to_time_t` (a direct Unix-seconds conversion), which the guard uses
   instead of the OpenSSL/LibreSSL `ASN1_TIME_to_tm` + civil-date path.

(The SAN dNSName accessor needed no guard: all three expose
`ASN1_STRING_get0_data` / `ASN1_STRING_length` identically.)

Everything else (the custom `BIO_METHOD`, `SSL_CTX`/`SSL` lifecycle, ALPN
(`SSL_CTX_set_alpn_protos` / `SSL_CTX_set_alpn_select_cb`), `SSL_set1_host`, the
memory-BIO feed/drain rings) is shared verbatim.

**Link note:** BoringSSL is written in C++, so its static libs pull in the C++
runtime. Add it to the link line (`-lstdc++`, or `-lc++` with libc++); the
Makefile does this via `BORINGSSL_EXTRA_LIBS`. OpenSSL/LibreSSL (C) need nothing
extra.

## Bring your own BoringSSL

BoringSSL is **not on Homebrew** and is **not vendored/downloaded** here. Build
it from source (it needs **Go**, plus **cmake** + **ninja**):

```sh
git clone https://boringssl.googlesource.com/boringssl
cd boringssl
cmake -GNinja -B build -DCMAKE_BUILD_TYPE=Release
ninja -C build ssl crypto

# Point BORINGSSL_DIR at a prefix with include/ + the built libs. The in-tree
# layout is include/ + build/ssl/libssl.a + build/crypto/libcrypto.a; either
# install to a prefix, or set BORINGSSL_LIBDIR to the build dirs.
```

Then, from this directory:

```sh
make BORINGSSL_DIR=/path/to/boringssl                 # if include/ + lib/ layout
# or, in-tree:
make BORINGSSL_DIR=/path/to/boringssl \
     BORINGSSL_LIBDIR=/path/to/boringssl/build/ssl    # (also needs crypto on the link line)

make test BORINGSSL_DIR=...                            # link smoke
```

## Verified

BoringSSL was built from source (`master`, `-O1`) in a Linux container and the
shared adapter was exercised end to end against it. `../openssl/tests/tls_e2e.c`
(the same unmodified test) passes **both** `KlTls` transport axes:

- **Readiness / socket-BIO**: handshake over a `socketpair`, ALPN (h2), app-data
  round-trip both directions, and mTLS `peer_cert` returning the verified client
  identity.
- **Completion / memory-BIO**: full handshake + round-trip with **no sockets** (
  ciphertext shuttled only via `feed_input`/`drain_output`) plus a clean
  `shutdown` surfacing `read()==-1` with `at_eof()==1`.

Both pass under ASan+UBSan. The e2e links with `-lstdc++` (BoringSSL is C++).

Building BoringSSL is heavy (needs Go + cmake + ninja, and its FIPS module
compile is memory-hungry; use `-O1` / plenty of RAM). That is a property of
BoringSSL's build, not this adapter: the adapter is the same source that the
OpenSSL and LibreSSL integrations build and test.

## Compatibility promise

Same as the OpenSSL adapter: source + static-relink only, tracking the public
`KlTls` vtable. No BoringSSL type appears in a Keel core public header.
