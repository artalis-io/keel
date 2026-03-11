# KEEL — Roadmap

Future direction and design considerations for keel.

## Completed

### 100-continue (Expect header)

Auto-detect `Expect: 100-continue` and send `HTTP/1.1 100 Continue\r\n\r\n` before reading the body.

### HEAD method auto-strip

HEAD requests automatically match GET routes; response body suppressed while preserving `Content-Length`.

### Graceful connection drain

`kl_server_stop` enters drain mode: stops accepting, continues in-flight requests, closes at configurable `drain_timeout_ms` deadline.

### Signal handling

Optional SIGTERM/SIGINT handler via `config.install_signal_handlers`. Uses `_Atomic int running` with `sigaction`.

### HTTP/1.0 compatibility

`Connection: keep-alive` header is now conditional on `req.keep_alive`, preventing confusion for HTTP/1.0 clients.

### Static analysis in CI

`scan-build` and `cppcheck` run in CI via `make analyze` and `make cppcheck` targets.

### Fuzz testing

libFuzzer harnesses for HTTP parser, multipart reader, WebSocket frame parser, and response parser with seed corpora (`fuzz/`).

### API reference documentation

Doxyfile + `@brief`/`@param`/`@return` doc comments on all public headers. Generate with `make docs`.

### Chunked request bodies

Parser-agnostic chunked decoder (`KlChunkedDecoder`) sits between the socket and body reader. The parser only sets `req->chunked = 1`; the connection layer routes body data through the decoder. Includes body deadline timer (`body_timeout_ms`) for slow-chunk DoS protection.

### Per-route middleware chain

Pattern-matched middleware via `kl_server_use()` / `kl_router_use()`. Middleware runs after route match, before body reading. Supports prefix (`/api/*`) and exact (`/health`) pattern matching, wildcard method (`*`), and short-circuit (return non-zero to stop chain). Built-in CORS middleware (`kl_cors_middleware`) ships with configurable origins, preflight handling, and credentials support. `req->ctx` enables middleware-to-handler data passing.

### Security audit hardening

Comprehensive C code audit with 8 fixes: integer overflow guards in `mp_strdup` and `hdr_append`, bounds-safe param capture in router, NULL guards in CORS middleware / server init / response body, multipart `mp_append_data` bounds check, and stored string lengths in multipart parser to eliminate `strlen` on untrusted data during cleanup.

### TLS via pluggable vtable

Pluggable TLS vtable (`KlTls`) — users bring their own TLS backend (BearSSL, LibreSSL, OpenSSL, rustls-ffi) by implementing a 7-function vtable. No vendored TLS library. TLS wraps the transport layer via `conn_read`/`conn_write` helpers.

### HTTP/2 server

Pluggable session vtable (`KlH2ServerSession`). Server-side HTTP/2 with multiplexed streams, upgrade from HTTP/1.1 or direct. Shared protocol constants in `h2.h`, server API in `h2_server.h`.

### WebSocket server

RFC 6455 WebSocket with frame encoding/decoding, fragmentation, close handshake. Shared frame parser in `websocket.h`, server API in `websocket_server.h`.

### WebSocket client

Async WebSocket client (`websocket_client.h`) with masked frames, handshake validation, TLS support. Symmetric API with server side. 28 tests.

### HTTP/2 client

Async HTTP/2 client (`h2_client.h`) with pluggable session vtable, multiplexed streams, per-stream response accumulation. Mock-testable without nghttp2. 18 tests.

### HTTP/1.1 client

Sync (blocking) and async (event-driven) HTTP/1.1 client with TLS support. URL parser with http/https/ws/wss, IPv6, and CRLF injection guard. Response parser (llhttp in `HTTP_RESPONSE` mode). `KlEventCtx` composition allows standalone operation without `KlServer`.

### Async primitives

`KlWatcher` for generic FD callbacks, `KlAsyncOp` for connection suspension, `KlThreadPool` for blocking work offload with pipe-based event loop wakeup. All operate on `KlEventCtx`, independent of `KlServer`.

---

## Near-Term

### Client streaming responses

**Priority: High** | **Effort: Moderate**

The async HTTP/1.1 client buffers the entire response body before invoking `on_done`. For SSE, streaming APIs, line-delimited JSON, and large downloads, this is a non-starter.

Add an optional `KlClientChunkFn on_chunk` callback to `kl_client_start()`. When set, the client enters a `RECEIVING_STREAM` state that delivers body chunks to the callback as they arrive instead of accumulating into `KlClientResponse.body`. The response parser already handles chunked TE — the change is routing decoded chunks to the callback instead of `accum_append`.

The same gap exists in the H2 client: `h2c_on_data` accumulates into `KlH2ClientResponse.body`. Adding a per-stream `on_chunk` callback mirrors the HTTP/1.1 fix.

```c
typedef void (*KlClientChunkFn)(const char *data, size_t len, void *user_data);

/* Async client with streaming */
kl_client_start(&ev, &alloc, &cfg, "GET", url,
                NULL, 0, NULL, 0,
                on_done, on_chunk, user_data);
```

### Client request streaming

**Priority: High** | **Effort: Moderate**

Both sync and async clients take `const char *body, size_t body_len` — no way to stream a request body. For large file uploads or generated payloads, the entire body must be buffered in memory first.

Add a pluggable body writer callback (mirrors the server-side `KlBodyReader` pattern):

```c
typedef int (*KlClientBodyFn)(void *ctx, char *buf, size_t buf_size);
/* Returns bytes written to buf, 0 on EOF, -1 on error */
```

The client sends `Transfer-Encoding: chunked` when using a body callback (Content-Length unknown). For known-length streams, accept an optional `content_length` parameter.

### Timer API

**Priority: High** | **Effort: Moderate**

No general-purpose timer on `KlEventCtx`. The timeout sweep and `KlAsyncOp` deadlines prove the infrastructure works, but there's no public timer primitive for retry backoff, scheduled cleanup, or periodic tasks.

```c
typedef void (*KlTimerFn)(void *user_data);

int  kl_timer_add(KlEventCtx *ctx, uint64_t delay_ms, KlTimerFn cb, void *ud);
int  kl_timer_cancel(KlEventCtx *ctx, int timer_id);
```

Implementation: min-heap on `KlEventCtx`, checked after each `kl_event_wait` return. ~200 lines. Unlocks WebSocket keep-alive pings and retry patterns.

### SSE helper

**Priority: Low** | **Effort: Trivial**

`kl_response_begin_stream()` already provides chunked transfer encoding. SSE is just `data:` line framing on top. A thin helper would handle the boilerplate:

```c
int kl_sse_begin(KlResponse *res, KlWriteFn *write_fn, void **write_ctx);
int kl_sse_event(KlWriteFn write_fn, void *ctx,
                 const char *event, const char *data, size_t data_len,
                 const char *id);
```

~50 lines of convenience code. No new architecture.

---

## Medium-Term

### Client connection pooling

**Priority: High** | **Effort: Significant**

Each `kl_client_start()` opens a fresh TCP + TLS handshake, even for repeated requests to the same host. For microservice patterns with many requests to the same backend, this is the biggest latency hit.

Needs a per-host persistent connection cache with:
- Idle timeout and eviction
- Max connections per host
- Keep-alive management (HTTP/1.1 `Connection: keep-alive` tracking)
- TLS session reuse

The server-side connection pool is a different shape (pre-allocated, fixed-size). This needs a new per-host LRU cache. Significant effort but highest payoff for repeated-request workloads.

### Redirect following

**Priority: Medium** | **Effort: Moderate**

Client returns 3xx status; caller must manually parse `Location` and re-issue. Add optional auto-redirect:

```c
typedef struct {
    int   timeout_ms;
    int   max_redirects;      /* 0 = no following (default), 5 = typical */
    /* ... */
} KlClientConfig;
```

Handles 301/302/303/307/308 with correct method preservation (307/308 preserve method; 301/302/303 switch POST to GET per RFC 7231). Loop detection via visited-URL set. Chains `kl_client_start()` calls internally.

### WebSocket auto-ping keep-alive

**Priority: Medium** | **Effort: Moderate**

Neither WS client nor server sends proactive keep-alive pings. Long-lived connections through proxies/load balancers with idle timeouts silently disconnect.

Add configurable automatic pings:

```c
typedef struct {
    /* ... */
    int ping_interval_ms;   /* 0 = disabled (default) */
} KlWsClientConfig;
```

Depends on the timer API (above). Server-side equivalent via `KlWsServerConfig`.

### Response compression

**Priority: Medium** | **Effort: Moderate**

No automatic response compression. For bandwidth-sensitive deployments, gzip/deflate on buffer and stream responses:

```c
kl_response_header(res, "Content-Encoding", "gzip");
kl_response_body_compressed(res, data, len);
```

For streaming, integrate with zlib's `deflate` in the chunk write path. For buffer responses, compress before `writev`. File responses are not compressed (use pre-compressed files).

Requires zlib dependency or a pluggable compression vtable to avoid forced dependencies.

### Response decompression (client)

**Priority: Medium** | **Effort: Moderate**

Client response parser passes raw body; `Content-Encoding` header is visible but no decompression happens. Many APIs return gzip by default. Users must decompress themselves.

Optional decompression in the response parser or via a post-receive callback. Same zlib dependency concern as response compression. Could share a pluggable compression vtable.

### Backpressure callback

**Priority: Low** | **Effort: Low**

The streaming write path already returns a positive value on partial send, but there's no explicit "socket full, pause writes" callback. For high-throughput streaming (large file downloads, SSE fan-out), an explicit backpressure signal would be cleaner than checking return values:

```c
typedef struct {
    void (*on_writable)(KlWsClientConn *ws, void *user_data);
} KlWsClientCallbacks;
```

Same pattern applies to server-side streaming responses and HTTP client request streaming.

---

## Long-Term / Research

### io_uring native file I/O

Replace `sendfile(2)` with `IORING_OP_READ` for file responses. The current io_uring backend only uses poll-add (readiness notification). Native async file I/O would eliminate the `sendfile` syscall entirely — the kernel reads the file and writes to the socket in a single submission.

### QUIC / HTTP/3

Requires a UDP-based event model and a QUIC library (quiche, ngtcp2). This is a significant architectural change — the connection model shifts from persistent TCP streams to multiplexed UDP datagrams with connection migration. Worth tracking but not near-term.

### Zero-copy receive (MSG_ZEROCOPY)

Linux `MSG_ZEROCOPY` for `send(2)` avoids copying response data from userspace to kernel. Requires notification-based completion and careful buffer lifetime management. Marginal benefit for small responses but significant for large file transfers.

### eBPF request steering

Use eBPF `SO_REUSEPORT` programs to steer connections to specific threads/cores based on request characteristics. Enables CPU affinity without application-level load balancing.

### Proxy support

HTTP CONNECT tunneling for HTTPS through proxies. Configuration for proxy URL + optional auth. Non-trivial integration point but important in corporate/containerized environments.

### DNS caching

`getaddrinfo()` is called per-request in the client. The OS usually caches, but an explicit cache with TTL would help high-frequency client workloads. Could also support pluggable resolver vtable for custom DNS.

### WebSocket compression (RFC 7692)

`permessage-deflate` compression negotiation and per-message compression. Rarely needed in practice due to CPU overhead vs. bandwidth savings. Significant effort (new handshake negotiation + compression integration).

---

## Considered and Rejected

These belong in application code or middleware, not in the transport library:

- **Authentication / authorization** — policy decisions vary per application; middleware interface supports it
- **Rate limiting** — depends on auth layer, billing tiers, abuse patterns; implement as middleware
- **Request validation / JSON parsing** — schema-specific; use a JSON library
- **ETag / 304 / conditional responses** — application-specific (Keel doesn't know when data changes)
- **Metrics / Prometheus export** — observability is application-level; `access_log` callback provides building blocks
- **Request IDs / tracing** — middleware can generate and propagate; not a transport concern
- **Custom error response templates** — middleware can intercept and rewrite error responses

---

## Design Principles

1. **Everything pluggable** — don't force dependencies. TLS, HTTP/2, compression should all be optional, behind vtable interfaces or compile-time flags.

2. **No allocation in the hot path** — new features must not introduce per-request malloc in the event loop or state machine. Pre-allocate, pool, or arena-allocate.

3. **Backwards-compatible API evolution** — new `KlConfig` fields default to zero/NULL (disabled). Existing code recompiles and runs unchanged.

4. **Single-header consumption remains possible** — the library should remain simple enough to vendor as a static archive with a single umbrella header.

5. **Measure before optimizing** — every performance claim should be backed by `bench.sh` numbers. Don't add complexity for theoretical gains.
