# KEEL — Kernel Event Engine, Lightweight

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![OpenSSF Scorecard](https://api.scorecard.dev/projects/github.com/artalis-io/keel/badge)](https://scorecard.dev/viewer/?uri=github.com/artalis-io/keel)
[![OpenSSF Best Practices](https://www.bestpractices.dev/projects/12186/badge)](https://www.bestpractices.dev/projects/12186)

Minimal C11 HTTP client/server library over an async I/O core that spans **both** the readiness axis (epoll, kqueue, WSAPoll, poll) and the completion axis (io_uring, IOCP) behind one small event interface, on a platform-neutral socket seam (POSIX, Winsock, lwIP). The event model, the socket/platform implementation, and the protocol stack are three orthogonal axes — swap the event backend or the socket provider without touching a line of protocol code. Both the server and client support sync and async operation — sync handlers return immediately, async handlers suspend and resume via the event loop; the client offers both a blocking API and an event-driven API. Pluggable allocator, pluggable HTTP parser, pluggable TLS, pluggable body readers, per-route middleware, streaming responses, multipart uploads, connection timeouts, thread pool, zero forced buffering.

**101K req/s** on a single thread. **920+ tests** (65 suites) with ASan/UBSan. **One vendored dependency** (llhttp).

## Build

```bash
make                    # build libkeel.a (epoll on Linux, kqueue on macOS)
make BACKEND=iouring    # build with completion-native io_uring backend (Linux 5.6+, requires liburing-dev)
make BACKEND=poll       # build with poll() backend (universal POSIX fallback)
make CC=cosmocc         # build with Cosmopolitan C (Actually Portable Executable)
make test               # run unit tests
make examples           # build all example programs
make debug              # debug build with ASan + UBSan
make analyze            # Clang static analyzer (scan-build)
make cppcheck           # cppcheck static analysis
make fuzz               # build libFuzzer fuzz targets (requires clang)
make clean              # remove artifacts
```

## Hello World

```c
#include <keel/keel.h>

void handle_hello(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_http_response_json(res, 200, "{\"msg\":\"hello\"}", 15);
}

int main(void) {
    KlHttpServer s;
    KlHttpServerConfig cfg = {.port = 8080};
    kl_http_server_init(&s, &cfg);
    kl_http_server_route(&s, "GET", "/hello", handle_hello, NULL, NULL);
    kl_http_server_run(&s);
    kl_http_server_free(&s);
}
```

## UNIX Socket Servers

Set `unix_socket_path` to serve the same HTTP routes over a UNIX domain stream socket instead of TCP. `unix_socket_unlink` removes a stale socket file before binding and removes the socket path again when the server is freed after a successful bind. It refuses to unlink non-socket files; place socket paths in an application-owned directory rather than a shared writable directory.

```c
KlHttpServer s;
KlHttpServerConfig cfg = {
    .unix_socket_path = "/run/myapp/keel.sock",
    .unix_socket_unlink = 1,
    .unix_socket_mode = 0660,       /* optional; 0 leaves umask/default */
    .unix_socket_group = "myapp",   /* optional; chown to this group */
    .unix_socket_owner = NULL,      /* optional; chown to this user (needs root) */
};

kl_http_server_init(&s, &cfg);
kl_http_server_route(&s, "GET", "/health", handle_health, NULL, NULL);
kl_http_server_run(&s);
kl_http_server_free(&s);
```

`unix_socket_group` / `unix_socket_owner` name a group / user to `chown` the
socket to — the classic "only members of group X may connect" pattern (pair with
`unix_socket_mode = 0660`). Ownership is applied *before* `listen()`, so the
socket is never reachable with the wrong owner/group. Setting the group to one
the process belongs to works unprivileged; changing the owner to a different
user needs `CAP_CHOWN`/root. Unknown names fail `kl_http_server_run` with
`KL_ERR_INVALID_ARG`.

`KlHttpServerConfig.transport = KL_HTTP_SERVER_TRANSPORT_UNIX` is also available when you want to be explicit. TCP remains the zero-initialized default.

### Peer credentials

On a UNIX socket you can identify the connecting process from a handler:

```c
KlPeerCred cred;
if (kl_http_request_peer_cred(req, &cred) == 0) {
    /* cred.uid, cred.gid always available; cred.pid when cred.has_pid
     * (Linux SO_PEERCRED, macOS LOCAL_PEERPID). */
}
```

Related accessors:
- `kl_http_request_peer_addr(req, ip, sizeof ip, &port)` — the client's IP/port for
  any request (TCP/TLS); `-1` for UNIX sockets. Also `kl_http_request_peer_sockaddr`
  for the raw address.

Behind an L4 load balancer, set `proxy_trusted_cidrs` to recover the real client
address from a PROXY protocol (v1/v2) header:

```c
KlHttpServerConfig cfg = {
    .proxy_trusted_cidrs = "10.0.0.0/8,::1/128",  /* honor PROXY only from these */
};
```
The header is parsed only when the connection originates from a trusted CIDR (so
an untrusted peer can't spoof its IP); from other sources it's ignored and the
socket address is used. `kl_http_request_peer_addr` then returns the real client.
- `kl_ws_server_peer_cred(ws, &cred)` — same, for a live WebSocket connection
  *after* the HTTP upgrade (when there's no longer a `KlHttpRequest`).
- `kl_http_request_peer_label(req, buf, sizeof buf)` — the peer's SELinux/AppArmor
  security label (Linux `SO_PEERSEC`; `-1` elsewhere).
- `kl_peer_cred_fd(fd, &cred)` — the building block, for any connected fd.

### mTLS client certificates

When the server requests a client certificate (mutual TLS), a handler can read
the verified client identity:

```c
KlPeerCert pc;
if (kl_http_request_peer_cert(req, &pc) == 0 && pc.verified) {
    /* pc.subject_cn, pc.san, pc.issuer_cn, pc.fingerprint_sha256,
     * pc.not_before/not_after, pc.der/der_len — the raw DER. */
}
```

`kl_http_request_peer_cert` returns `-1` for a plaintext connection, when the client
presented no certificate, or on a TLS backend without client-cert support.
`pc.verified` reports whether the certificate passed CA-chain validation — a
certificate can be *present* but *unverified* under optional client auth, so
always check it before trusting the identity. Extraction is provided by the
TLS backend's optional `peer_cert` vtable method (implemented by the bundled
mbedtls backend; the mTLS mode is set via `kl_tls_mbedtls_ctx_create(...,
KL_MTLS_OPTIONAL|KL_MTLS_REQUIRED, ...)`).

### Socket activation (inherited fd)

Instead of binding its own socket, KEEL can adopt a pre-bound, already-listening
fd passed by systemd, launchd, or a supervising parent. `kl_systemd_listen_fd()`
implements the systemd `LISTEN_FDS` protocol; the transport (TCP vs UNIX) is
auto-detected from the fd, and KEEL never unlinks an adopted UNIX socket.

```c
int fd = kl_systemd_listen_fd();          /* -1 if not socket-activated */
KlHttpServerConfig cfg = { .listen_fd = fd > 0 ? fd : 0,
                 .unix_socket_path = fd > 0 ? NULL : "/run/myapp/keel.sock" };
```

For a unit passing several sockets, `kl_systemd_listen_fds(&count)` reports how
many were passed (fds 3 … 3+count-1) and `kl_systemd_listen_fd_by_name("https")`
selects one by its `FileDescriptorName`. Adopt one per `KlHttpServer`.

### Connecting a client over UNIX

The client connects to a UNIX socket via the `http+unix://` (or `https+unix://`)
scheme, where the authority is the percent-encoded socket path:

```c
/* GET /v1/ping over /run/app.sock */
kl_http_client_request(&alloc, NULL, "GET",
                  "http+unix://%2Frun%2Fapp.sock/v1/ping",
                  NULL, 0, NULL, 0, &resp);
```

Both the synchronous (`kl_http_client_request`) and asynchronous (`kl_http_client_start`)
clients support it. The pooled variants (`kl_http_client_request_pooled` /
`kl_http_client_start_pooled`) accept `http+unix://` too but bypass the connection
pool — UNIX sockets have no host:port to key on, and a local-socket connect is
cheap. Proxying is incompatible with UNIX targets.

The WebSocket and HTTP/2 clients speak UNIX too:
`ws+unix://` / `wss+unix://` (`kl_ws_client_connect`) and `http+unix://` /
`https+unix://` (`kl_http2_client_connect`). Redirects are followed over UNIX
(relative `Location` values keep the `http+unix` authority). In every case the
`Host` / `:authority` defaults to `localhost`.

See `examples/unix_socket_server.c` for a server demonstrating all three.

### Limitations

- **Abstract-namespace sockets** (Linux `@name`, leading-NUL `sun_path`) are
  **not** supported: they carry no filesystem permissions, so `unix_socket_mode`
  / ownership cannot restrict access — that conflicts with KEEL's permission
  model. Use a path in an application-owned directory instead.
- **`SO_REUSEPORT` multi-worker scaling does not apply to UNIX sockets.** For
  multiple workers on one UNIX socket, use socket activation / fd passing (bind
  once, share the fd) rather than each worker binding the same path.
- **`kl_systemd_listen_fds(&count)`** reports how many fds were passed
  (consecutive from fd 3); `KlHttpServerConfig.listen_fd` adopts one at a time.

## Custom socket provider

The socket layer is a pluggable vtable (`<keel/socket.h>`), so you can run KEEL
over a non-POSIX stack (Winsock is built in; lwIP / a test mock / an
instrumentation decorator fit the same interface). A provider is an immutable ops
table + context + capability flags:

```c
#include <keel/socket.h>

/* Decorate the built-in provider: count bytes, forward the rest. */
typedef struct { const KlSocketProvider *base; long tx; } Ctx;
static kl_ssize_t my_send(void *c, KlSocketHandle fd, const void *b, size_t n) {
    Ctx *x = c;
    kl_ssize_t r = x->base->ops->send(x->base->context, fd, b, n);
    if (r > 0) x->tx += r;
    return r;
}
static const KlSocketOps ops = { .send = my_send, .name = "counting" };
/* Ops left NULL fall back to KEEL's built-in default. */

Ctx ctx = { .base = kl_socket_provider_posix() };
KlSocketProvider prov = { &ops, &ctx, KL_SOCK_CAP_NATIVE_FD };

/* Select it — server: */
KlHttpServerConfig cfg = { .port = 8080, .sockets = &prov };
/* ...or client: */
KlHttpClientConfig ccfg = { .sockets = &prov };
```

Portability contract: the API exposes no platform-only types — sizes are
`kl_ssize_t`, scatter-gather is `KlIoVec`, the sendfile offset is `uint64_t`; a
provider translates to its native shapes (`struct iovec`/`WSABUF`/`off_t`)
internally. A provider advertises what it supports via `KL_SOCK_CAP_*`
(`NATIVE_FD`, `WRITEV`, `SENDFILE`) and KEEL falls back for anything it lacks —
except the **server requires `KL_SOCK_CAP_NATIVE_FD`** (the readiness event loop
polls real descriptors; a non-native provider is rejected at `kl_http_server_init`).
Full runnable demo: **`examples/custom_socket_provider.c`**.

## Features

- **Two event axes, six backends** — **readiness** (epoll edge-triggered, kqueue edge-triggered, WSAPoll, poll universal fallback) and **completion** (io_uring SQE/CQE, IOCP) behind one small `KlEventLoop` interface, plus a portable `poll()`-based completion double for testing the completion driver on any POSIX host. Each backend honors its native semantics; the protocol layer sees a stable Keel-level contract, not epoll flags or CQEs.
- **Three orthogonal axes** — the event model, the socket/platform implementation, and the protocol stack are independent and separately replaceable; an orthogonality audit (`docs/keel_axis_audit.md`) mechanically confirms protocols never touch a platform socket API or event engine directly
- **TCP or UNIX socket servers** — same HTTP stack over TCP/IP or `AF_UNIX/SOCK_STREAM`
- **Pluggable HTTP parser** — ships with llhttp, swap via `KlHttpServerConfig.parser`
- **Pluggable TLS** — bring your own BearSSL/LibreSSL/OpenSSL via vtable, zero vendored TLS code
- **Pluggable body readers** — vtable interface for request body processing
- **Per-route middleware** — pattern-matched middleware chain with short-circuit support
- **Built-in CORS middleware** — configurable origins, methods, headers, preflight handling
- **Multipart form-data** — RFC 2046 parser with configurable size limits
- **Three response modes** — buffered (writev), file (sendfile zero-copy), stream (chunked transfer encoding)
- **WebSocket** — RFC 6455 server + client with frame encoding/decoding
- **Route parameters** — `:param` capture, no allocation, pointers into read buffer
- **Connection timeouts** — monotonic clock sweep, automatic 408 responses, slow-loris protection
- **Access logging** — pluggable callback after each response, zero overhead when disabled
- **Async server handlers** — suspend connections via `KlAsyncOp`, resume later from watchers or thread pool workers — no stalling the event loop
- **HTTP client (sync + async)** — blocking `kl_http_client_request()` for simple use cases, event-driven `kl_http_client_start()` for non-blocking I/O, both with TLS support
- **Composable event context** — `KlEventCtx` decouples the event loop from the server, enabling standalone clients and thread pools without a `KlHttpServer`
- **Thread pool** — submit blocking work from event loop, execute on workers, resume via pipe wakeup
- **Generic FD watchers** — register any file descriptor for event-driven callbacks via `KlWatcher`
- **Server-Sent Events** — zero-alloc SSE framing (`kl_http_sse_event`, `kl_http_sse_comment`) over chunked streaming
- **Response compression** — pluggable compression vtable; ships with miniz gzip backend
- **Client decompression** — automatic `Content-Encoding: gzip` response decompression
- **Client connection pool** — keep-alive reuse keyed by (host, port, is_tls, proxy) with idle timers
- **Redirect following** — automatic 3xx redirect with RFC 7231/7538 method transformation
- **HTTP proxy** — HTTP forwarding (absolute-form URL) and HTTPS CONNECT tunneling through proxies
- **Backpressure write buffer** — `KlDrain` buffers unsent data on would-block, flushes on write-readiness
- **Timer scheduling** — one-shot timers on the event loop via min-heap
- **Error diagnostics** — sqlite3-style per-struct error codes with `kl_strerror()`
- **UDP datagrams** — the **STABLE** fixed-slot `KlDatagram` API (`<keel/datagram.h>`): the Tier-1 datagram primitive — a caller-owned, single-threaded handle over a prepared UDP fd with one-call socket create/configure/bind/adopt, provider-neutral connect, async per-datagram receive (source + local/destination address via pktinfo), a fixed-slot whole-datagram send queue with on-drain backpressure, source-pinned sends + per-packet TOS/ECN, multicast/broadcast, transparent `recvmmsg`/`sendmmsg` batching, UDP GSO/GRO offload, ECN/TOS/DSCP marking, and confirmed-detachment close. Validated **live across every event backend** — readiness (epoll/kqueue/poll/WSAPoll) and completion (pollcomp/io_uring/IOCP/lwIP-raw/EFI_UDP4). Its function+type contract is ABI-stable; the struct layout is opt-in via `<keel/datagram_detail.h>`. The built-in DNS resolver is built on `KlDatagram`; it is the base intended for new portable message protocols (a future QUIC/HTTP-3, mDNS/CoAP)
- **Pluggable DNS resolver** — bring your own async DNS (c-ares, thread-pool wrapper), with caching decorator; ships a built-in async resolver (dual-family A+AAAA, RFC 8305)
- **Happy Eyeballs client connect** — async client races the resolved address list (RFC 8305): configurable Connection Attempt Delay, first handshake wins, overall request deadline
- **Server load introspection** — `kl_http_server_stats()` for connection counts, enabling user-space load-shedding middleware
- **Pre-allocated connection pool** — no per-request malloc, no fragmentation under load
- **Pluggable allocator** — bring your own arena/pool/tracking allocator
- **Pluggable socket provider** — a platform-neutral socket seam (`KlSocketProvider`): POSIX and Winsock built in, lwIP shipped (both a BSD-socket layer and a raw NO_SYS callback stack with no OS sockets), and a freestanding EFI_TCP4/UDP4 provider for UEFI firmware. Capability-gated (readiness vs overlapped), selectable per server/client. See [Custom socket provider](#custom-socket-provider) and [Frontier providers](#frontier-providers-bare-metal--firmware)
- **pledge/unveil sandboxing** — init/run split makes syscall lockdown natural
- **Zero-copy techniques** — header pointers into read buffer, sendfile, writev batching

## Architecture

Keel is organized along **three orthogonal axes**, so any one can be replaced without disturbing the others. For the full model — the Tier-1 semantic transports (`KlListener`/`KlStream`/`KlDatagram`), the engine/provider split, and the rules that keep them separate — see [`docs/architecture.md`](docs/architecture.md) and the enforceable [`docs/architecture_invariants.md`](docs/architecture_invariants.md).

- **Event axis** (`event.h` + `completion.h`) — how the loop learns work is ready. **Readiness** backends (epoll, kqueue, WSAPoll, poll) register interest and react to EAGAIN; **completion** backends (io_uring, IOCP) submit operations and reap completions. A capability negotiation (`KL_EVENT_CAP_READINESS | _COMPLETION`) matches a loop to a compatible socket provider; each backend keeps its native low-level semantics.
- **Socket axis** (`socket.h` — the `KlSocketProvider` vtable + pointer-width `KlSocketHandle`) — where the bytes actually go. POSIX and Winsock built in; lwIP and a freestanding UEFI EFI_TCP4/UDP4 provider ship as integrations. Error translation, address conversion, and handle ownership live here, never above it.
- **Protocol axis** (`http_connection.c`, `h2.c`, `websocket.c`, `client.c`, the `KlTls` vtable, body readers, `http_response.c`) — HTTP/1.1, HTTP/2, WebSocket, SSE, DNS. Protocol code goes through the connection/stream + event abstractions only; it never includes a platform networking header or calls an event engine directly. `docs/keel_axis_audit.md` audits this separation mechanically.

Below the axes sit orthogonal, independently testable modules:

| Module | Header | Description |
|--------|--------|-------------|
| **allocator** | `allocator.h` | Bring-your-own allocator interface |
| **event** | `event.h` + `completion.h` | Readiness (epoll / kqueue / WSAPoll / poll) + completion (io_uring / IOCP) behind one interface |
| **socket** | `socket.h` | Platform-neutral socket seam (`KlSocketProvider`): POSIX / Winsock built in, lwIP + UEFI shipped |
| **event_ctx** | `event_ctx.h` | Composable event loop context (watchers + allocator) |
| **request** | `http_request.h` | Parsed HTTP request struct (header-only, zero alloc) |
| **parser** | `parser.h` | Pluggable request/response parser vtables |
| **response** | `http_response.h` | Response builder: buffered, sendfile, or streaming chunked |
| **router** | `http_router.h` | Route matching with `:param` capture + middleware chain |
| **connection** | `http_connection.h` | Pre-allocated connection pool + state machine |
| **server** | `http_server.h` | Top-level glue: TCP/UNIX bind, async event loop, stop |
| **body_reader** | `http_body_reader.h` | Pluggable body reader vtable + buffer reader |
| **body_reader_multipart** | `http_body_reader_multipart.h` | RFC 2046 multipart/form-data parser |
| **chunked** | `chunked.h` | Parser-agnostic chunked transfer-encoding decoder |
| **cors** | `http_cors.h` | Built-in CORS middleware with configurable origins |
| **tls** | `tls.h` | Pluggable TLS transport vtable (bring-your-own backend) |
| **async** | `async.h` | Connection suspension for async operations |
| **thread_pool** | `thread_pool.h` | Worker thread pool with pipe-based event loop wakeup |
| **url** | `url.h` | URL parser (http/https/ws/wss, IPv6, CRLF injection guard) |
| **client** | `http_client.h` | HTTP/1.1 client (sync blocking + async event-driven) |
| **websocket** | `websocket.h` + `websocket_server.h` | RFC 6455 WebSocket server (shared frame parser + server API) |
| **websocket_client** | `websocket_client.h` | RFC 6455 WebSocket client (masked frames, async handshake) |
| **h2** | `http2.h` + `http2_server.h` | HTTP/2 server (pluggable session vtable) |
| **h2_client** | `http2_client.h` | HTTP/2 client (multiplexed streams, pluggable session) |
| **resolver** | `resolver.h` | Pluggable async DNS resolver vtable |
| **sse** | `http_sse.h` | Server-Sent Events: line framing over chunked streaming (zero alloc) |
| **error** | `error.h` | Diagnostic error codes (KlError enum) + kl_strerror() |
| **timer** | `timer.h` | One-shot timer scheduling on KlEventCtx (min-heap) |
| **client_pool** | `http_client_pool.h` | HTTP client connection pool with keep-alive reuse |
| **redirect** | `http_redirect.h` | Automatic 3xx redirect following (RFC 7231/7538) |
| **compress** | `compress.h` | Pluggable response compression vtable (buffer + streaming) |
| **decompress** | `decompress.h` | Pluggable response decompression vtable (client-side) |
| **drain** | `drain.h` | Backpressure write buffer with on_drain callback |
| **file_io** | `file_io.h` | Pluggable async file I/O vtable |
| **resolver_cache** | `resolver_cache.h` | Caching DNS resolver decorator with configurable TTL/capacity |

**Deliberate design choices:**

- **Single-threaded event loop** — Same model as Node.js, Redis, Nginx (per-worker), and Python asyncio. No mutexes, no lock contention, no data races — the entire connection state machine is lock-free by construction. `KlThreadPool` offloads blocking work to workers; multi-core scaling is horizontal via `SO_REUSEPORT` with multiple processes.
- **O(n) router** — Linear scan over routes per request. A `memcmp` scan over even hundreds of routes costs nanoseconds, invisible next to network I/O syscalls. A trie or radix tree would add complexity to param extraction and middleware pattern matching for no measurable gain.
- **O(n) timeout sweep** — Iterates all connection slots once per event loop tick. At `max_connections` = 256 (default), this is a tight loop over a contiguous array well within L1 cache.
- **No built-in 503 / load shedding** — `kl_http_server_stats()` exposes connection counts for user-space middleware to make load-shedding decisions. Thresholds and `Retry-After` policy belong in application code, not the framework.
- **No global memory monitoring** — The allocator is pluggable, so the framework can't reliably track total memory. Existing per-resource caps (`max_body_size`, `max_header_size`, `KlDrain.max_size`) bound the main vectors; OS-level OOM handling covers the rest.

## Request Body Handling

KEEL uses a vtable-based body reader interface. Register a body reader factory per-route — the connection layer creates the reader after headers are parsed, feeds it data as it arrives, and makes the finished reader available in the handler via `req->body_reader`.

**Built-in buffer reader** — accumulates the body into a growable buffer:

```c
void handle_post(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)ctx;
    KlHttpBufReader *br = (KlHttpBufReader *)req->body_reader;
    if (!br || br->len == 0) {
        kl_http_response_error(res, 400, "Request body required");
        return;
    }
    kl_http_response_status(res, 200);
    kl_http_response_header(res, "Content-Type", "application/octet-stream");
    kl_http_response_body_borrow(res, br->data, br->len);
}

/* Register with size limit (1 MB) */
kl_http_server_route(&s, "POST", "/api/data", handle_post,
                (void *)(size_t)(1 << 20), kl_http_body_reader_buffer);
```

Pass `NULL` as the body reader factory for routes that don't accept a body. If a request with a body arrives on a route with no reader, KEEL discards the body. If the reader factory returns NULL, KEEL sends 415 Unsupported Media Type.

**Custom readers** — implement the `KlHttpBodyReader` vtable (`on_data`, `on_complete`, `on_error`, `destroy`) and provide a factory function.

## Middleware

Register middleware that runs before handlers. Middleware can inspect/modify the request and response, or short-circuit the chain by returning a non-zero value (e.g., to reject unauthenticated requests).

### Built-in CORS middleware

```c
KlHttpCorsConfig cors;
kl_http_cors_init(&cors);
kl_http_cors_add_origin(&cors, "https://app.example.com");
kl_http_cors_add_origin(&cors, "https://staging.example.com");
// Or parse from an environment variable:
// kl_http_cors_parse_origins(&cors, getenv("ALLOWED_ORIGINS"));

kl_http_server_use(&s, "*", "/*", kl_http_cors_middleware, &cors);
```

Handles `Access-Control-Allow-Origin`, `Allow-Credentials`, and automatically responds to OPTIONS preflight requests with 204 + all required CORS headers.

### Writing custom middleware {#writing-custom-middleware}

Middleware uses the same `(KlHttpRequest *, KlHttpResponse *, void *)` signature. Return 0 to continue, non-zero to short-circuit:

**Logging middleware:**

```c
int log_middleware(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)res; (void)ctx;
    fprintf(stderr, "[req] %.*s %.*s\n",
            (int)req->method_len, req->method,
            (int)req->path_len, req->path);
    return 0;  /* continue to next middleware / handler */
}

kl_http_server_use(&s, "*", "/*", log_middleware, NULL);
```

**Auth middleware:**

```c
typedef struct { const char *api_key; } AuthConfig;

int auth_middleware(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    AuthConfig *cfg = ctx;
    size_t key_len;
    const char *key = kl_http_request_header_len(req, "X-API-Key", &key_len);
    size_t expect_len = strlen(cfg->api_key);
    if (!key || key_len != expect_len ||
        memcmp(key, cfg->api_key, expect_len) != 0) {
        kl_http_response_error(res, 401, "Unauthorized");
        return 1;  /* short-circuit — response already written */
    }
    return 0;  /* continue */
}

AuthConfig auth = {.api_key = "secret-key-123"};
kl_http_server_use(&s, "*", "/api/*", auth_middleware, &auth);
```

**Request context passing (middleware → handler):**

```c
int user_middleware(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)res; (void)ctx;
    /* Validate token, look up user, set context for handler */
    req->ctx = my_user_lookup(req);
    return 0;
}

void handle_profile(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)ctx;
    User *user = req->ctx;  /* set by middleware */
    /* ... */
}
```

### Middleware patterns

- Patterns ending with `/*` are prefix matches: `/api/*` matches `/api`, `/api/users`, `/api/users/123`
- Patterns without `/*` are exact matches: `/health` matches only `/health`
- Method `"*"` matches any HTTP method; `"GET"` also matches HEAD requests
- Middleware runs in registration order, before body reading
- Short-circuiting disables keep-alive (unread body can't be drained)

## Multipart Uploads

```c
static KlHttpMultipartConfig mp_config = {
    .max_part_size  = 4 << 20,   /* 4 MB per part */
    .max_total_size = 16 << 20,  /* 16 MB total */
    .max_parts      = 8,
};

void handle_upload(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)ctx;
    KlHttpMultipartReader *mr = (KlHttpMultipartReader *)req->body_reader;
    if (!mr || mr->num_parts == 0) {
        kl_http_response_error(res, 400, "No parts received");
        return;
    }
    for (int i = 0; i < mr->num_parts; i++) {
        KlHttpMultipartPart *p = &mr->parts[i];
        printf("  %s: %zu bytes (filename=%s)\n",
               p->name, p->data_len, p->filename ? p->filename : "n/a");
    }
    kl_http_response_json(res, 200, "{\"ok\":true}", 11);
}

kl_http_server_route(&s, "POST", "/upload", handle_upload,
                &mp_config, kl_http_body_reader_multipart);
```

```bash
curl -F "name=Alice" -F "file=@photo.jpg" localhost:8080/upload
```

## WebSocket

Register a WebSocket endpoint and get bidirectional communication:

```c
static int ws_on_message(KlWsServerConn *ws, KlWsOpcode opcode,
                         const char *data, size_t len, void *ctx) {
    (void)ctx;
    if (opcode == KL_WS_TEXT) {
        kl_ws_server_send_text(ws, data, len);  /* echo back */
    }
    return 0;
}

static void ws_on_close(KlWsServerConn *ws, void *ctx) {
    (void)ws; (void)ctx;
    printf("WebSocket closed\n");
}

static KlWsServerConfig ws_config = {
    .on_message = ws_on_message,
    .on_close = ws_on_close,
};

int main(void) {
    KlHttpServer s;
    KlHttpServerConfig cfg = {.port = 8080};
    kl_http_server_init(&s, &cfg);
    kl_http_server_ws_upgrade(&s, "/ws", &ws_config);
    kl_http_server_run(&s);
}
```

The WebSocket server module handles frame parsing, masking, and protocol details. The handler receives callbacks for each message — use `kl_ws_server_send_text()` or `kl_ws_server_send_binary()` to reply.

## Async Operations

Server handlers can be **sync** (return immediately with a response set) or **async** (suspend the connection for later resumption). KEEL provides two primitives for async handlers: **KlWatcher** (generic FD callbacks) and **KlAsyncOp** (connection suspension). Together they allow handlers to park a connection, perform work asynchronously, and resume when done — without stalling the event loop.

```c
void handle_async(KlHttpRequest *req, KlHttpResponse *res, void *user_data) {
    KlHttpServer *srv = user_data;
    KlHttpConn *conn = kl_http_request_conn(req);

    /* Allocate context for the async operation */
    MyCtx *ctx = ...;
    ctx->op.on_resume = my_resume_cb;
    ctx->op.on_cancel = my_cancel_cb;

    /* Create a pipe — watcher fires when the pipe is written to */
    socketpair(AF_UNIX, SOCK_STREAM, 0, ctx->pipe_fds);
    kl_watcher_add(&srv->ev, ctx->pipe_fds[0], KL_EVENT_READ, my_watcher, ctx);

    /* Suspend the connection (removes it from event loop, exempt from timeouts) */
    kl_async_suspend(srv, conn, &ctx->op);

    /* Later: write to pipe → watcher fires → kl_async_complete → connection resumes */
}
```

The watcher callback runs on the event loop thread, making it safe to call `kl_async_complete()` which re-registers the connection FD and drives the state machine forward.

## Thread Pool

`KlThreadPool` bridges blocking work (SQLite queries, file I/O, DNS, crypto) and the single-threaded event loop. Submit work items from the event loop, execute on worker threads, resume connections via pipe wakeup.

```c
/* Create pool (auto-detects CPU count, 64-item queue) */
KlThreadPool *pool = kl_thread_pool_create(&server.ev, NULL);

/* Work callbacks */
static void do_query(void *ud) {
    MyWork *w = ud;
    w->status = sqlite3_exec(w->db, w->sql, ...);  /* runs on worker thread */
}
static void query_done(void *ud) {
    MyWork *w = ud;
    kl_async_complete(w->server, &w->op);           /* runs on event loop thread */
}

/* In handler: suspend connection, submit blocking work */
kl_async_suspend(srv, conn, &work->op);
KlWorkItem item = { .work_fn = do_query, .done_fn = query_done, .user_data = work };
kl_thread_pool_submit(pool, &item);
```

Each `KlWorkItem` has three callbacks:

| Callback | Thread | Purpose |
|----------|--------|---------|
| `work_fn` | Worker | Execute blocking work |
| `done_fn` | Event loop | Resume connection (called via pipe watcher) |
| `cancel_fn` | Event loop | Cleanup for items still queued at shutdown (may be NULL) |

Thread safety is guaranteed by construction — workers never touch the event loop directly. They push completed items to a done queue and write a byte to a pipe; the pipe watcher fires on the event loop thread and calls `done_fn`. Backpressure: `submit()` returns `-1` when the queue is full.

## HTTP Client

KEEL includes both **sync** (blocking) and **async** (event-driven) HTTP/1.1 clients with TLS support. The sync client is a single function call for simple use cases. The async client uses `KlEventCtx` (not `KlHttpServer`), so it works standalone — no server required.

**Sync (blocking):**

```c
KlAllocator alloc = kl_allocator_default();
KlHttpClientResponse resp;
if (kl_http_client_request(&alloc, NULL, "GET", "http://api.example.com/data",
                       NULL, 0, NULL, 0, &resp) == 0) {
    printf("status=%d body=%.*s\n", resp.status, (int)resp.body_len, resp.body);
    kl_http_client_response_free(&resp);
}
```

**Async (event-driven):**

```c
static void on_done(KlHttpClient *client, void *user_data) {
    if (kl_http_client_error(client) == 0) {
        const KlHttpClientResponse *r = kl_http_client_response(client);
        printf("status=%d\n", r->status);
    }
    kl_http_client_free(client);
}

/* Works with standalone KlEventCtx — no KlHttpServer needed */
KlAllocator alloc = kl_allocator_default();
KlEventCtx ev;
kl_event_ctx_init(&ev, &alloc);
kl_http_client_start(&ev, &alloc, NULL, "GET", "http://example.com/", NULL, 0, NULL, 0, on_done, NULL);
/* pump ev.loop ... */
kl_event_ctx_free(&ev);
```

The URL parser (`kl_url_parse`) handles `http://`, `https://`, `ws://`, `wss://`, IPv6 `[::1]:port`, default ports, and rejects CRLF injection.

## Static File Serving

Zero-copy file responses via `sendfile(2)`:

```c
void handle_static(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)ctx;
    if (memmem(req->path, req->path_len, "..", 2) != NULL) {
        kl_http_response_error(res, 403, "Forbidden");
        return;
    }
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "./public%.*s",
             (int)req->path_len, req->path);
    int fd = open(filepath, O_RDONLY);
    if (fd < 0) { kl_http_response_error(res, 404, "Not Found"); return; }
    struct stat st;
    fstat(fd, &st);
    kl_http_response_status(res, 200);
    kl_http_response_header(res, "Content-Type", "text/html");
    kl_http_response_file(res, fd, st.st_size);  /* zero-copy sendfile */
}
```

Uses `sendfile(2)` on Linux and macOS, with TCP_CORK coalescing on Linux for optimal throughput.

## Streaming Responses

Write directly to the socket via chunked transfer encoding — zero intermediate buffering:

```c
void handle_stream(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    kl_http_response_header(res, "Content-Type", "application/json");

    KlHttpResponseWriteFn write_fn;
    void *write_ctx;
    kl_http_response_begin_stream(res, 200, &write_fn, &write_ctx);

    write_fn(write_ctx, "{\"data\":", 8);
    // ... write as much as you want, each call becomes a chunk ...
    write_fn(write_ctx, "}", 1);

    kl_http_response_end_stream(res);
}
```

The `KlHttpResponseWriteFn` signature (`int (*)(void *ctx, const char *data, size_t len)`) is designed to be compatible with streaming JSON writers.

## Route Parameters

```c
kl_http_server_route(&s, "GET", "/users/:id/posts/:pid", handler, NULL, NULL);
// Params extracted from path — no allocation, pointers into read buffer
```

The router returns 200 (match), 405 (path matched, wrong method), or 404 (not found).

## Connection Timeouts

```c
KlHttpServerConfig cfg = {
    .port = 8080,
    .read_timeout_ms = 15000,   /* 15 seconds (default: 30000) */
};
```

KEEL stamps each connection with a monotonic clock on every I/O event. A periodic sweep (every ~400ms) closes connections that have been idle longer than `read_timeout_ms` and sends a 408 Request Timeout response. This protects against slow-loris attacks and abandoned connections without affecting active transfers.

## Access Logging

```c
void my_logger(const KlHttpRequest *req, int status,
               size_t body_bytes, double duration_ms, void *user_data) {
    fprintf(stderr, "%.*s %.*s %d %zu %.1fms\n",
            (int)req->method_len, req->method,
            (int)req->path_len, req->path,
            status, body_bytes, duration_ms);
}

KlHttpServerConfig cfg = {
    .port = 8080,
    .access_log = my_logger,       /* NULL = disabled (default) */
    .access_log_data = NULL,       /* passed as user_data */
};
```

Set a callback in `KlHttpServerConfig` and KEEL calls it after each response is fully sent. The callback receives the full request (method, path, headers), response status, body size, and wall-clock duration in milliseconds. Users implement their own formatting (JSON, CLF, custom). `NULL` = no logging, zero overhead.

## Custom Allocator

```c
KlAllocator arena = my_arena_allocator();
KlHttpServerConfig cfg = {
    .port = 8080,
    .alloc = &arena,
};
```

The allocator interface passes `size` to `free` and `old_size` to `realloc` — enabling arena and pool allocators that don't store per-allocation metadata.

## Pluggable Parser

Ships with llhttp (default). Swap by setting `KlHttpServerConfig.parser`:

```c
KlHttpServerConfig cfg = {
    .port = 8080,
    .parser = kl_http1_parser_pico,  // use picohttpparser instead
};
```

Implement the 3-function `KlHttp1RequestParser` vtable (`parse`, `reset`, `destroy`) for any backend. The response parser (`KlHttp1ResponseParser`) uses the same pattern for the HTTP client.

## Pluggable TLS

KEEL doesn't vendor any TLS library. Bring your own backend (BearSSL, LibreSSL, OpenSSL, rustls-ffi) by implementing the 7-function `KlTls` vtable:

```c
KlTlsCtx *ctx = my_bearssl_ctx_new("cert.pem", "key.pem");
KlTlsConfig tls = {
    .ctx = ctx,
    .factory = my_bearssl_factory,
    .ctx_destroy = my_bearssl_ctx_free,
};
KlHttpServerConfig cfg = {
    .port = 8443,
    .tls = &tls,
};
```

The vtable interface (`handshake`, `read`, `write`, `shutdown`, `pending`, `reset`, `destroy`) wraps the transport layer. Everything above it — parser, router, middleware, body readers, handlers — works identically on plaintext and TLS connections.

When TLS is active, `sendfile(2)` falls back to `pread` + TLS write (encryption requires userspace access to plaintext). All other response modes (buffered, streaming) work transparently.

## Sandboxing with pledge/unveil

KEEL deliberately does **not** own your sandbox policy — that's an application concern. The server separates initialization (bind/listen) from the event loop (accept/read/write), so you can lock down syscalls and filesystem access between the two:

```c
#include <keel/keel.h>

int main(void) {
    KlHttpServer s;
    KlHttpServerConfig cfg = {.port = 8080};
    kl_http_server_init(&s, &cfg);    // binds socket — needs inet, rpath
    kl_http_server_route(&s, "GET", "/hello", handle_hello, NULL, NULL);

    // --- Sandbox boundary ---
    unveil("/var/www", "r");     // only serve files from here
    unveil(NULL, NULL);          // lock it down
    pledge("stdio inet rpath", NULL);

    kl_http_server_run(&s);           // enters event loop — sandboxed
    kl_http_server_free(&s);
}
```

On Linux, use the [pledge polyfill](https://github.com/jart/pledge) (seccomp-bpf + Landlock) for the same API. The key insight: KEEL's `init`/`run` split makes this natural — no library changes needed.

## Benchmark

```bash
make bench              # build bench server, run 4 wrk benchmarks with latency
JSON=1 make bench       # JSON output for CI/tooling
```

The benchmark suite runs 4 endpoints against a dedicated bench server:

| Endpoint | What it measures |
|----------|-----------------|
| `GET /hello` | **Baseline** — minimal JSON, no routing params, no middleware |
| `GET /users/:id` | **Router** — param extraction + snprintf response |
| `GET /mw/hello` | **Middleware** — same response through 2 pass-through middleware |
| `POST /echo` | **Body reading** — KlHttpBufReader + echo body back |

Sample results (Apple M1 Max, single thread, 100 connections, kqueue):

| Endpoint | Req/sec | Avg Latency | p99 |
|----------|---------|-------------|-----|
| `GET /hello` (baseline) | 111,650 | 0.89ms | 1.13ms |
| `GET /users/42` (route params) | 109,112 | 0.91ms | 1.15ms |
| `GET /mw/hello` (middleware chain) | 111,247 | 0.89ms | 1.14ms |
| `POST /echo` (body reading) | 109,370 | 0.90ms | 1.15ms |

Route params, middleware, and body reading add no measurable overhead — all within ~2% of the baseline. No GC pauses. No goroutine scheduling. No async runtime overhead. Just `kqueue` → `read` → `write`.

## Platform Support

| Platform | Backend | Build |
|----------|---------|-------|
| macOS / BSD | kqueue (edge-triggered) | `make` |
| Linux | epoll (edge-triggered) | `make` |
| Linux 5.6+ | io_uring (completion, SQE/CQE) | `make BACKEND=iouring` |
| Any POSIX | poll (level-triggered) | `make BACKEND=poll` |
| Windows | WSAPoll (readiness) or IOCP (completion) | `make BACKEND=wsapoll` / `BACKEND=iocp` |
| Any POSIX host | pollcomp (portable completion double) | `make BACKEND=pollcomp` |
| Linux (musl/Alpine) | epoll (edge-triggered) | `make` |
| Cosmopolitan (APE) | poll (auto-selected) | `make CC=cosmocc` |
| Bare-metal + lwIP | poll (via lwIP BSD sockets) | `make BACKEND=poll` + `-DKL_NO_SIGNAL` |
| Bare-metal, no OS sockets | lwIP raw NO_SYS callback provider | see [Frontier providers](#frontier-providers-bare-metal--firmware) |
| UEFI firmware (pre-boot) | EFI_TCP4/UDP4 completion provider (freestanding) | see [Frontier providers](#frontier-providers-bare-metal--firmware) |

The io_uring backend is completion-native: it submits SQEs and reaps CQEs (zero-copy `splice` for file responses, registered send buffers), rather than polling for readiness. The earlier `IORING_OP_POLL_ADD` readiness adapter was retired after benchmarks showed it ~2–2.3× slower than the completion backend. Requires `liburing-dev`.

The poll backend is a universal POSIX fallback that works on any platform with `poll(2)`. It enables Cosmopolitan C support (Actually Portable Executables that run on Linux, macOS, Windows, FreeBSD, OpenBSD, NetBSD from a single binary). When `CC=cosmocc` is detected, the Makefile automatically selects the poll backend.

For bare-metal targets (STM32, ESP32, etc.), link against lwIP or picoTCP — their BSD socket compatibility layers provide all the POSIX functions Keel uses (`accept`, `read`, `write`, `close`, `poll`, `getaddrinfo`). Compile with `-DKL_NO_SIGNAL` to disable POSIX signal handling, and exclude `thread_pool.c` from the build if no RTOS is available. See [docs/comparison.md](docs/comparison.md#bare-metal--mcu-support) for details. For environments with **no OS sockets at all** — a raw lwIP `NO_SYS` stack, or UEFI firmware before an OS exists — see [Frontier providers](#frontier-providers-bare-metal--firmware).

## Frontier providers (bare-metal / firmware)

The socket + completion axes are abstract enough to host stacks that have **no OS sockets and no libc**. These providers live under `integrations/` as bring-your-own adapters — the core `libkeel` (or a freestanding archive) is unchanged; you inject the provider at runtime. They are more experimental than the mainstream backends and are labeled with their exact maturity.

| Provider | What it is | Maturity |
|----------|-----------|----------|
| **lwIP BSD sockets** | Keel over lwIP's POSIX-compatible socket layer (`integrations/platform/lwip`, `event_lwip.c`) | Shipped, BYO |
| **lwIP raw (`NO_SYS`)** | Keel over lwIP's raw callback API — a completion provider (`event_lwip_raw.c`) that runs the whole HTTP stack with **no OS sockets, no threads**. Serves HTTP over loopback, ASan/UBSan/LSan-clean, CI-gated | Shipped, BYO |
| **UEFI EFI_TCP4/UDP4** | A freestanding completion provider (`integrations/uefi`) that runs a stock async `KlHttpClient` inside **UEFI firmware, before any OS** — no epoll, no OS sockets, no errno, no libc. Plaintext + HTTPS (CA + hostname + certificate-validity-time verified over EFI Runtime Services `GetTime`, real EFI_RNG entropy) + DNS over EFI_UDP4, with ExitBootServices-clean teardown | **Client proven + hardened on QEMU/OVMF** (adversarially reviewed for EFI completion-token lifetime; see `docs/phase10_uefi_feasibility_design.md`). A UEFI **server** is scoped but not built (`docs/phase10_uefi_server_design.md`) |

The through-line: `KlHttpClient` is genuinely model-blind — the **same** client code runs unchanged over io_uring, IOCP, the pollcomp double, lwIP raw, and EFI tokens (including TLS via the socket-BIO seam and DNS via the built-in resolver). Adding a provider is a socket-axis + completion-axis exercise; the protocol code does not change. This validates the axis separation the way nothing else can — by hosting the stack somewhere with no operating system underneath it.

## Testing

920+ tests across 65 test suites, covering every module and both event axes:

| Suite | Tests | Covers |
|-------|-------|--------|
| `test_allocator` | 4 | Default + custom tracking allocators |
| `test_async` | 14 | Watchers (KlEventCtx), suspend/resume, deadlines, cancel, e2e async handler |
| `test_body_reader` | 30 | Buffer + multipart: limits, spanning, binary, edge cases |
| `test_chunked` | 17 | Chunked decoder: single/multi chunk, hex, extensions, trailers, errors |
| `test_client` | 18 | Sync/async client, response free, TLS config, error handling |
| `test_client_happy_eyeballs` | 6 | Happy Eyeballs (RFC 8305): first-wins, refused-fallback, all-fail, single-address, slow-first race, deadline timer |
| `test_client_pool` | 24 | Connection pool: acquire/release, per-host limits, idle expiry, stale detection |
| `test_client_stream` | 27 | Response streaming (push), request streaming (pull), chunked body production |
| `test_compress` | 16 | Compression vtable, buffer + streaming, miniz gzip backend |
| `test_connection` | 12 | Pool init, acquire/release, exhaustion, active count, state machine, monotonic clock |
| `test_cors` | 17 | Config, origin whitelist, wildcard, preflight, credentials, middleware |
| `test_cross_module` | 7 | Cross-module integration: compress+drain, TLS+async, middleware+body+async, resolver cache, TLS+middleware+compress, stats during load |
| `test_decompress` | 14 | Decompression vtable, gzip one-shot + streaming, CRC/ISIZE verification |
| `test_drain` | 28 | Backpressure buffer: passthrough, partial, EAGAIN, flush, on_drain, max_size, overreport |
| `test_error` | 11 | Error codes, kl_strerror, per-struct error storage |
| `test_event` | 8 | Event loop init/close, add/wait, del, multiple FDs, timeout, mod mask |
| `test_event_ctx` | 7 | Standalone event context init/free, watcher lifecycle, dispatch helpers |
| `test_file_io` | 14 | Async file I/O vtable: mock submit/cancel/tick, state machine, EAGAIN, TLS fallback |
| `test_h2` | 29 | HTTP/2 sessions, streams, routing, ALPN, goaway, body limits |
| `test_h2_client` | 18 | Mock session vtable, stream tracking, response free, API validation |
| `test_integration` | 27 | Full server: hello, POST, keepalive, multipart, chunked, middleware |
| `test_overflow` | 20 | Integer overflow guards across all modules |
| `test_parser` | 9 | GET, POST, query strings, incomplete, reset, chunked TE |
| `test_proxy` | 11 | HTTP proxy: forwarding, CONNECT tunnel, auth, async proxy states, pool keying |
| `test_redirect` | 33 | 3xx redirect following, method transform, cross-origin auth strip, pooled |
| `test_request` | 14 | Header case-insensitive lookup, params, query strings, empty/missing values |
| `test_response` | 24 | Status, headers, body, JSON, error, streaming, sendfile, compression |
| `test_response_parser` | 10 | HTTP response parsing, chunked, headers, body limits, malformed |
| `test_router` | 27 | Exact match, params, 404, 405, wildcard, middleware chain |
| `test_server_integration` | 6 | Pool exhaustion, backpressure recovery, concurrent requests, drain |
| `test_server_stats` | 4 | Server stats: initial, active count, max connections, null safety |
| `test_resolver_cache` | 13 | DNS cache: hit/miss, TTL expiry, eviction, cancel, error non-caching |
| `test_sse` | 7 | SSE framing: event, data, id, comment, multiline, begin/end |
| `test_thread_pool` | 12 | Create/free, submit, backpressure, FIFO ordering, multi-worker, shutdown, stress |
| `test_timeout` | 8 | Idle, partial headers, partial body, active connections, body timeout, keepalive idle, concurrent |
| `test_timer` | 10 | Min-heap scheduling, cancellation, callback safety, next-timeout |
| `test_tls` | 20 | TLS vtable, handshake FSM, response send/stream/file via mock, shutdown retry, pool teardown |
| `test_tls_integration` | 3 | Passthrough TLS mock: full handshake→read→write path |
| `test_unix_socket` | 5 | UNIX socket server: HTTP round-trip, stale path policy, cleanup, path limits |
| `test_url` | 20 | URL parsing, IPv6, CRLF rejection, default ports, ws/wss schemes |
| `test_websocket` | 48 | Frame parsing, masking, opcode, fragments, close, echo, unmasked rejection |
| `test_websocket_client` | 30 | Client frame encoding, mask XOR, handshake, parser, API, config, auto-ping |

```bash
make test               # run all tests
make debug && make test  # run under ASan + UBSan
make analyze            # Clang static analyzer
make cppcheck           # cppcheck static analysis
```

## Why C

An HTTP server at this level is mostly syscalls, pointer arithmetic, and state machines. C is a natural fit: direct `writev`/`sendfile`/`epoll_wait` access, zero-copy pointers into read buffers, explicit memory layout, no runtime. One vendored dependency (llhttp), 2-second clean builds, runs on everything from io_uring to bare-metal MCUs.

The tradeoff is real — C has no borrow checker, no bounds-checked slices, no RAII. We compensate with defense-in-depth:

- Pre-allocated connection pool (no per-request `malloc`, no fragmentation)
- `SIZE_MAX/2` overflow guards on all arithmetic, bounds checks at system boundaries
- ASan + UBSan in debug builds, Clang static analyzer + cppcheck in CI
- libFuzzer on the HTTP parser and multipart parser (the primary attack surface)
- `pledge()`/`unveil()` sandboxing, `-D_FORTIFY_SOURCE=2 -fstack-protector-strong`
- Pluggable allocator for arena/pool strategies with deterministic cleanup

This is adequate for a focused ~14K LOC library with thorough testing, but it's not a language-level guarantee. If you're evaluating Keel and memory safety is your primary concern, that's a legitimate reason to look elsewhere.

## Not in Scope

KEEL is a transport library — it handles sockets, parsing, routing, and response serialization. Everything above the HTTP layer is an application concern:

- **Authentication / authorization** — Token validation, session management, OAuth flows, RBAC. These are policy decisions that vary per application. KEEL's middleware interface makes auth trivial to implement (see [Auth middleware example](#writing-custom-middleware)) but deliberately doesn't ship one. *[Hull](https://github.com/artalis-io/hull) provides session, JWT, and RBAC middleware.*

- **Request logging** — Log format (JSON, CLF, custom), destination (stderr, file, syslog), and filtering are application choices. KEEL provides a pluggable `access_log` callback with method, path, status, body size, and duration — you bring the formatter. *Hull provides a structured JSON logger middleware.*

- **Request IDs / correlation IDs** — These are application-generated identifiers for tracing requests through your system. Use middleware to read/generate X-Request-ID headers and pass them to your logging/observability. *Hull generates and propagates request IDs automatically.*

- **Rate limiting** — Rate limits depend on your authentication layer, your billing tiers, your abuse patterns. Implement as middleware with whatever backing store (in-memory, Redis, database) fits your architecture. *Hull provides configurable rate limiting middleware.*

- **Request validation** — Schema validation, content-type negotiation, input sanitization. These are application-level concerns that depend on your data model. *Hull provides a validation module with schema-based input checking.*

- **ETag / Last-Modified** — These are application-specific (KEEL doesn't know when your data changes). Use existing `kl_http_request_header()` / `kl_http_response_header()` for the headers; your application handles 304 logic. *Hull handles conditional responses for static assets.*

- **Static file serving** — MIME types, path traversal protection, directory listing are application decisions. See `examples/static_files.c` for the pattern. *Hull auto-serves embedded or filesystem static assets with MIME detection.*

- **CSRF protection** — Cross-site request forgery tokens for form-based applications. *Hull provides `hull.middleware.csrf` with automatic token generation and validation.*

- **Idempotency keys** — Safe POST retry via `Idempotency-Key` header. *Hull provides `hull.middleware.idempotency` with configurable TTL and response caching.*

The general principle: if it requires policy decisions that vary between applications, it belongs in application code, not in the transport library. KEEL provides the hooks (middleware, body readers, access log callback) — you provide the policy.

## Comparison with Alternatives

Three embedded C HTTP libraries compared. See [docs/comparison.md](docs/comparison.md) for full details with API examples.

| | Keel | [Mongoose](https://github.com/cesanta/mongoose) | [GNU libmicrohttpd](https://www.gnu.org/software/libmicrohttpd/) |
|---|------|----------|---------------|
| **License** | MIT | GPLv2 / Commercial | LGPLv2.1+ |
| **LOC** | ~14K | ~33K | ~19K |
| **Architecture** | 35+ independent modules on 3 orthogonal axes | Monolithic amalgam | Monolithic |
| **Maturity** | New (2025–2026) | 20+ years (NASA, Siemens, Samsung) | GNU project, 18+ years (NASA, Sony, systemd) |
| **HTTP/2** | Server + client | No | No |
| **Event backends** | readiness (epoll, kqueue, WSAPoll, poll) + completion (io_uring, IOCP) | select/poll only | select, poll, epoll |
| **Router + middleware** | Built-in with `:param` capture | None (DIY if/else) | None (single callback) |
| **HTTP client** | Sync + async + streaming + H2 | Basic client | Server only |
| **Allocator** | Runtime vtable (bring-your-own) | Compile-time macros | None (raw malloc) |
| **TLS** | Pluggable vtable — any backend | Built-in TLS 1.3 + pluggable | GnuTLS only |
| **Compression** | Pluggable vtable (gzip + extensible) | No | No |
| **Threading** | Single-threaded + thread pool | Single-threaded | 4 modes incl. thread-per-connection |
| **Bare-metal MCU** | lwIP BSD sockets, lwIP raw (no OS sockets), or UEFI firmware | Built-in TCP/IP stack | Requires OS networking |
| **Cosmopolitan C** | Supported (APE binaries) | No | No |
| **Tests** | 920+ (65 suites) | ~4K LOC tests | Fewer relative to size |

**Choose Keel** when you want MIT licensing, HTTP/2, a built-in router/middleware/client, and pluggable everything. **Choose Mongoose** when you're targeting bare-metal MCUs with no OS, need a built-in TCP/IP stack, or need battle-tested maturity. **Choose libmicrohttpd** when you need multi-threaded request handling, independently audited security, or wide distro packaging.

## CI

GitHub Actions runs on every push and PR against `main`:

- **Linux (epoll)** — build, test, examples, smoke test
- **Linux (io_uring)** — build, test, examples, smoke test
- **Linux (poll fallback)** — build, test, examples, smoke test
- **macOS (kqueue)** — build, test, examples, smoke test
- **Linux (musl/Alpine)** — build, test, examples
- **Cosmopolitan (APE)** — build, examples, smoke test
- **ASan + UBSan** — build and test with sanitizers
- **Static Analysis** — scan-build + cppcheck
- **Fuzz Testing** — libFuzzer on HTTP parser + multipart (60s each)

A separate benchmark workflow runs on push to `main` (informational, not gating).

## License

MIT
