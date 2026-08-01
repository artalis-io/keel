# Keel integrations

First-party, **optional** adapters that connect Keel's pluggable vtables to
specific third-party libraries. They live here — outside `src/` — so the core
stays dependency-light: plain `make` and `make test` in the repo root build and
pass with **no** external library present.

| Directory | Implements | Backing library | BYO var |
|-----------|-----------|-----------------|---------|
| [`mbedtls/`](mbedtls/) | `KlTls` (`include/keel/tls.h`) | mbedTLS 3.x | `MBEDTLS_DIR` |
| [`nghttp2/`](nghttp2/) | `KlH2ClientSession` + `KlH2ServerSession` (`include/keel/h2_client.h`, `h2_server.h`) | nghttp2 1.x | `NGHTTP2_DIR` / pkg-config |

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
