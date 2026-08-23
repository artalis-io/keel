# Keel ALPN policy

How Keel negotiates the application protocol (HTTP/2 vs HTTP/1.1) over TLS, for
both the server and the client, and how the two protocols converge on one shared
REST layer. Authoritative; paired with `docs/operations/capability_matrix.md`.

## Architecture

```text
TCP → TLS handshake → ALPN result
        h2        → HTTP/2 adapter (src/protocols/http2/http2_server.c + a KlHttp2ServerSession, e.g. nghttp2)
        http/1.1  → HTTP/1.1 adapter (src/protocols/http/http_connection.c + the llhttp parser)
        (none)    → HTTP/1.1 adapter
   → one KlHttpRequest / KlHttpResponse
   → one KlHttpRouter (kl_http_router_match)
   → one middleware chain (kl_http_router_run_middleware)
   → one KlHttpHandler
```

Protocol selection happens **after** the TLS handshake completes and **before**
any HTTP parser or protocol engine sees application bytes
(`kl_http_conn_on_handshake`, `src/protocols/http/http_connection.c`). The selected protocol is immutable
for the life of the connection; there is no post-negotiation protocol switch.

## Server policy

- **Advertise, in preference order: `h2`, then `http/1.1`.** Configure the TLS
  context with `kl_tls_mbedtls_ctx_set_alpn(ctx, (const char*[]){"h2","http/1.1",NULL})`.
  mbedTLS selects `h2` when the peer offers it, else `http/1.1`.
- After the handshake, `kl_http_conn_on_handshake` reads the negotiated protocol
  (`KlTls::alpn_protocol`):
  - `h2` **and** the server has an HTTP/2 config (`KlHttpServerConfig.h2`) → the HTTP/2
    adapter owns the connection (`KL_HTTP_CONN_HTTP2`).
  - `http/1.1`, **no ALPN** (peer didn't use it), an **unsupported/unexpected**
    value, or `h2` without an HTTP/2 config → the HTTP/1.1 adapter
    (`KL_HTTP_CONN_READING`).
- **No-ALPN policy: accept as HTTP/1.1** (compatibility). A TLS client that does
  not use ALPN gets HTTP/1.1. This is deliberate, not accidental.
- An unsupported ALPN value never engages HTTP/2 — it falls back to HTTP/1.1
  rather than failing, matching the no-ALPN policy.
- If the server does **not** advertise `h2` (or has no `KlHttpServerConfig.h2`), it serves
  HTTP/1.1 only; a client that somehow selected `h2` would not be misrouted
  because the h2 path is gated on `KlHttpServerConfig.h2`.

The h2 server accepts HTTP/2 via three entry paths, all converging on the same
adapter: ALPN-negotiated `h2` over TLS, the `Upgrade: h2c` mechanism, and h2c
prior-knowledge (the `PRI * HTTP/2.0` preface). In every case the HTTP/2 engine
consumes the client connection preface itself.

## Client policy

- **Offer, in preference order: `h2`, then `http/1.1`** — set on the client TLS
  context with `kl_tls_mbedtls_ctx_set_alpn`.
- The HTTP/2 client (`kl_http2_client_connect`) verifies the negotiated protocol
  after the handshake:
  - selected `h2` → proceed with HTTP/2.
  - selected a **non-`h2`** value → **fail** the connection (no silent switch to
    a protocol the client's engine can't speak).
  - **no ALPN** selected (server didn't use it) → proceed as **prior-knowledge
    HTTP/2** (documented compatibility policy). Never guess HTTP/2 from received
    bytes.
- Once ALPN selects a protocol, the client never opportunistically switches on
  the same connection. Connection pooling keys on `(host, port, is_tls)` and does
  not reuse a connection under a different protocol assumption.

## Shared REST layer invariant

HTTP/1.1 and HTTP/2 are transport adapters over one application model. Both build
the same `KlHttpRequest`/`KlHttpResponse`, match on the same `KlHttpRouter`, run the same
middleware, and call the same `KlHttpHandler`. Handlers see no ALPN result, HTTP/2
frames, stream IDs, HPACK state, or HTTP/1.1 parser internals — only
`KlHttpRequest`/`KlHttpResponse`.

**Authority convergence:** HTTP/2's `:authority` pseudo-header is injected as a
synthetic `host` header, so `kl_http_request_header(req, "host")` returns the
authority identically over both protocols (HTTP/1.1 `Host` and HTTP/2
`:authority` converge on one field).

## Tests (regression guards)

- **`tests/protocols/http/test_alpn.c`** (core `make test`, deterministic, no crypto): drives
  `kl_http_conn_on_handshake` with a mock TLS whose negotiated ALPN is configurable and
  asserts the connection enters exactly the right adapter for `h2` / `http/1.1` /
  no-ALPN / unsupported / `h2`-without-h2-config; and drives an HTTP/2 request
  through the real h2 path to assert it uses the **same** router + middleware +
  handler an HTTP/1.1 request would, with `:authority` converged onto `host`.
- **`make -C integrations/http2/nghttp2 alpn-interop MBEDTLS_DIR=... NGHTTP2_DIR=...`**
  (BYO, real TLS): a Keel server advertising `{h2, http/1.1}` verified with
  `openssl s_client -alpn …`, `curl --http2`, `curl --http1.1`, **and** the Keel
  HTTP/2 client over TLS — all four negotiation directions land on one shared
  handler. This is the end-to-end ALPN regression guard.

Matrix proven:

```text
external client --ALPN h2--------> Keel server → HTTP/2 adapter   (curl --http2, openssl)
external client --ALPN http/1.1--> Keel server → HTTP/1.1 adapter (curl --http1.1, openssl)
Keel client     --ALPN h2 (TLS)--> Keel server → HTTP/2 adapter   (alpn_client)
Keel client     --h2c-----------> Keel server → HTTP/2 adapter    (e2e_socket)
Keel client     --h2c-----------> nghttpd (external)              (client_probe)
```
