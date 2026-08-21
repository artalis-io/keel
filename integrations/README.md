# Keel integrations

First-party, **optional** adapters that connect Keel's pluggable vtables to
specific third-party libraries. They live here — outside `src/` — so the core
stays dependency-light: plain `make` and `make test` in the repo root build and
pass with **no** external library present.

| Directory | Implements | Backing library | BYO var |
|-----------|-----------|-----------------|---------|
| [`mbedtls/`](mbedtls/) | `KlTls` (`include/keel/tls.h`) | mbedTLS 3.x | `MBEDTLS_DIR` |
| [`nghttp2/`](nghttp2/) | `KlHttp2ClientSession` + `KlHttp2ServerSession` (`include/keel/http2_client.h`, `http2_server.h`) | nghttp2 1.x | `NGHTTP2_DIR` / pkg-config |

## Ground rules (all integrations)

- **Bring your own library.** Nothing here is vendored, and nothing is
  downloaded at build time. You point the build at a library you supply.
- **Core stays pure.** No integration is required by `make` / `make test`. No
  third-party type ever appears in a Keel *core* public header — an integration
  only ever implements a vtable already defined in `include/keel/`.
- **No dynamic loading, no plugin registry, no executable memory.** Adapters are
  ordinary static objects you link. This preserves Keel's W^X and static-linking
  goals.
- **Compatibility:** source + static-relink. Adapters track the public vtables
  they implement; a vtable change is a source change you recompile against.

## Build (from the repo root)

```sh
make integration-mbedtls MBEDTLS_DIR=$(brew --prefix mbedtls)
make integration-nghttp2 NGHTTP2_DIR=$(brew --prefix nghttp2)
make integrations        MBEDTLS_DIR=... NGHTTP2_DIR=...   # both (skips absent libs)
make integration-test    MBEDTLS_DIR=... NGHTTP2_DIR=...   # per-adapter smokes
```

Each `integration-*` target **skips with a notice** (rather than failing) when
its BYO library isn't provided, so `make integrations` is safe to run with only
some libraries available. See each subdirectory's README for specifics.

## Continuous integration

The ordinary `make` / `make test` CI jobs stay dependency-light and never build
these. A dedicated **Integrations (mbedTLS + nghttp2)** job (`.github/workflows/ci.yml`)
exercises them on `ubuntu-latest` so they cannot silently rot, running:

- nghttp2 adapter: roundtrip + real-socket e2e, `h2spec` conformance, `h2load`,
  and curl / nghttpd interop (system `libnghttp2` via pkg-config);
- mbedTLS adapter: a real TLS handshake smoke;
- **ALPN e2e**: `openssl s_client -alpn`, `curl --http2` / `--http1.1`, and the
  Keel HTTP/2 client over TLS — all on one shared handler.

**Versions exercised in CI:** nghttp2 = the Ubuntu system package (currently
1.59). mbedTLS is **built from source at v3.6.2**, because Ubuntu ships the 2.28
LTS which the 3.x adapter does not support (see `mbedtls/README.md`). To
reproduce locally, point `MBEDTLS_DIR` at a 3.x tree and run the same targets
(`make -C integrations/nghttp2 alpn-interop MBEDTLS_DIR=… `, etc.).
