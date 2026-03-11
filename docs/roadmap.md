# KEEL — Roadmap

Future direction and design considerations for keel.

## Near-Term

### ~~100-continue (Expect header)~~ DONE

Auto-detect `Expect: 100-continue` and send `HTTP/1.1 100 Continue\r\n\r\n` before reading the body.

### ~~HEAD method auto-strip~~ DONE

HEAD requests automatically match GET routes; response body suppressed while preserving `Content-Length`.

### ~~Graceful connection drain~~ DONE

`kl_server_stop` enters drain mode: stops accepting, continues in-flight requests, closes at configurable `drain_timeout_ms` deadline.

### ~~Signal handling~~ DONE

Optional SIGTERM/SIGINT handler via `config.install_signal_handlers`. Uses `_Atomic int running` with `sigaction`.

### ~~HTTP/1.0 compatibility~~ DONE

`Connection: keep-alive` header is now conditional on `req.keep_alive`, preventing confusion for HTTP/1.0 clients.

### ~~Static analysis in CI~~ DONE

`scan-build` and `cppcheck` run in CI via `make analyze` and `make cppcheck` targets.

### ~~Fuzz testing~~ DONE

libFuzzer harnesses for HTTP parser and multipart reader with seed corpora (`fuzz/`).

### ~~API reference documentation~~ DONE

Doxyfile + `@brief`/`@param`/`@return` doc comments on all public headers. Generate with `make docs`.

### ~~Chunked request bodies~~ DONE

Parser-agnostic chunked decoder (`KlChunkedDecoder`) sits between the socket and body reader. The parser only sets `req->chunked = 1`; the connection layer routes body data through the decoder. Includes body deadline timer (`body_timeout_ms`) for slow-chunk DoS protection.

### ~~Per-route middleware chain~~ DONE

Pattern-matched middleware via `kl_server_use()` / `kl_router_use()`. Middleware runs after route match, before body reading. Supports prefix (`/api/*`) and exact (`/health`) pattern matching, wildcard method (`*`), and short-circuit (return non-zero to stop chain). Built-in CORS middleware (`kl_cors_middleware`) ships with configurable origins, preflight handling, and credentials support. `req->ctx` enables middleware→handler data passing.

### ~~Security audit hardening~~ DONE

Comprehensive C code audit with 8 fixes: integer overflow guards in `mp_strdup` and `hdr_append`, bounds-safe param capture in router, NULL guards in CORS middleware / server init / response body, multipart `mp_append_data` bounds check, and stored string lengths in multipart parser to eliminate `strlen` on untrusted data during cleanup.

## Medium-Term

### ~~TLS via BearSSL or LibreSSL~~ DONE

Pluggable TLS vtable (`KlTls`) — users bring their own TLS backend (BearSSL, LibreSSL, OpenSSL, rustls-ffi) by implementing a 7-function vtable. No vendored TLS library. TLS wraps the transport layer via `conn_read`/`conn_write` helpers. New `KL_CONN_TLS_HANDSHAKE` state for non-blocking handshake. `pending()` function for edge-triggered event loop drain. Sendfile falls back to `pread` + TLS write. Pre-allocated per-connection TLS sessions (one per pool slot). Keep-alive reuses TLS session (no re-handshake). 20 mock-based unit tests.

### ~~HTTP/2 via nghttp2~~

Planned: see `docs/http2_plan.md` for detailed design.

### ~~WebSocket upgrade~~ DONE

Detect `Upgrade: websocket` in the parser, complete the handshake in the handler, then hand the connection to a WebSocket frame codec. The event loop already handles bidirectional I/O — WebSocket is a protocol change, not an architecture change.

### ~~HTTP Client~~ DONE

Sync (blocking) and async (event-driven) HTTP/1.1 client with TLS support. URL parser with IPv6 and CRLF injection guard. Response parser (llhttp in `HTTP_RESPONSE` mode). `KlEventCtx` composition allows the async client to operate standalone (without `KlServer`). Parser rename: `KlParser` → `KlRequestParser`, new `KlResponseParser` for client-side.

### Response compression

Gzip/deflate for buffer and stream responses:

```c
kl_response_header(res, "Content-Encoding", "gzip");
kl_response_body_compressed(res, data, len);
```

For streaming, integrate with zlib's `deflate` in the chunk write path. For buffer responses, compress before `writev`. File responses are not compressed (use pre-compressed files instead).

### ~~Static file serving~~

Decided: example is sufficient. Static file serving is application logic (MIME types, path traversal, directory listing, etc). The example in `examples/static_files.c` demonstrates the pattern.

## Long-Term / Research

### io_uring native file I/O

Replace `sendfile(2)` with `IORING_OP_READ` for file responses. The current io_uring backend only uses poll-add (readiness notification). Native async file I/O would eliminate the `sendfile` syscall entirely — the kernel reads the file and writes to the socket in a single submission.

### QUIC / HTTP/3

Requires a UDP-based event model and a QUIC library (quiche, ngtcp2). This is a significant architectural change — the connection model shifts from persistent TCP streams to multiplexed UDP datagrams with connection migration. Worth tracking but not near-term.

### Zero-copy receive (MSG_ZEROCOPY)

Linux `MSG_ZEROCOPY` for `send(2)` avoids copying response data from userspace to kernel. Requires notification-based completion and careful buffer lifetime management. Marginal benefit for small responses but significant for large file transfers.

### eBPF request steering

Use eBPF `SO_REUSEPORT` programs to steer connections to specific threads/cores based on request characteristics. Enables CPU affinity without application-level load balancing.

## Design Principles for Future Work

1. **Everything pluggable** — don't force dependencies. TLS, HTTP/2, compression should all be optional, behind vtable interfaces or compile-time flags.

2. **No allocation in the hot path** — new features must not introduce per-request malloc in the event loop or state machine. Pre-allocate, pool, or arena-allocate.

3. **Backwards-compatible API evolution** — new `KlConfig` fields default to zero/NULL (disabled). Existing code recompiles and runs unchanged.

4. **Single-header consumption remains possible** — the library should remain simple enough to vendor as a static archive with a single umbrella header.

5. **Measure before optimizing** — every performance claim should be backed by `bench.sh` numbers. Don't add complexity for theoretical gains.
