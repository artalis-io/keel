# KEEL — Roadmap

Future direction and design considerations for keel.

## Near-Term

### Chunked request bodies

Support `Transfer-Encoding: chunked` for requests. The parser already detects the `chunked` flag — the connection layer needs to de-chunk incoming data before feeding it to the body reader. This is straightforward: a small state machine that strips chunk headers and feeds raw data through the existing body reader interface.

### Graceful shutdown

Drain active connections before exiting:
1. Stop accepting new connections (remove listen fd from event loop)
2. Set a deadline (e.g. 5 seconds)
3. Continue processing in-flight requests
4. Close remaining connections at deadline

Currently `kl_server_stop` sets `running = 0` and the loop exits after the current tick.

### Per-route middleware chain

```c
kl_server_use(&s, "GET", "/api/*", auth_middleware, NULL);
kl_server_use(&s, "*", "/*", cors_middleware, NULL);
```

Middleware runs before the handler and can short-circuit (return error response) or modify request state. Implemented as a secondary route table checked before the primary handler.

## Medium-Term

### TLS via BearSSL or LibreSSL

Pluggable TLS, not forced:

```c
KlTlsConfig tls = {
    .cert_file = "cert.pem",
    .key_file = "key.pem",
};
KlConfig cfg = {
    .port = 8443,
    .tls = &tls,
};
```

TLS wraps the socket read/write at the connection layer. The rest of the stack (parser, router, handler, response) is unchanged. BearSSL is preferred for its small footprint and no-allocation design; LibreSSL for broader cipher support.

### Worker thread pool

For CPU-bound handlers (image processing, compression, templating):

```c
KlConfig cfg = {
    .port = 8080,
    .worker_threads = 4,
};
```

The event loop remains single-threaded. When a handler is registered as "blocking", the connection is handed to a worker thread for processing. The response is sent back through the event loop. This preserves the single-threaded event loop model while supporting compute-heavy handlers.

### HTTP/2 via nghttp2

The pluggable parser makes this feasible — HTTP/2 framing is a different parser backend, not a different architecture. The connection state machine needs multiplexing awareness (multiple concurrent streams per connection), and the response builder needs frame formatting, but the router, handlers, and body readers are unchanged.

### WebSocket upgrade

Detect `Upgrade: websocket` in the parser, complete the handshake in the handler, then hand the connection to a WebSocket frame codec. The event loop already handles bidirectional I/O — WebSocket is a protocol change, not an architecture change.

### Response compression

Gzip/deflate for buffer and stream responses:

```c
kl_response_header(res, "Content-Encoding", "gzip");
kl_response_body_compressed(res, data, len);
```

For streaming, integrate with zlib's `deflate` in the chunk write path. For buffer responses, compress before `writev`. File responses are not compressed (use pre-compressed files instead).

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
