# Keel Examples

20 example programs demonstrating KEEL's HTTP server, client, WebSocket, HTTP/2, async, and utility APIs.

## Quick Start

```bash
make examples
./examples/hello_server
# → curl localhost:8080/hello
```

## Server Examples

| Example | Port | Description | Key APIs |
|---------|------|-------------|----------|
| `hello_server` | 8080 (configurable) | Minimal JSON server | `KlServer`, `KlConfig`, `kl_response_json` |
| `rest_api_server` | 8080 | REST API with route params, POST body | `:id` params, `KlBufReader`, `kl_body_reader_buffer` |
| `middleware` | 8080 | Pre/post-body middleware, CORS, auth | `kl_server_use`, `kl_server_use_post`, `kl_cors_middleware` |
| `static_files` | 8080 | Static file server with sendfile | `kl_response_file`, MIME detection, path traversal guard |
| `streaming` | 8080 | Chunked streaming responses | `kl_response_begin_stream`, `KlWriteFn` |
| `body_readers` | 8080 | Buffer + multipart body readers | `KlBufReader`, `KlMultipartReader`, `kl_body_reader_multipart` |
| `async` | 8080 | Async suspend/resume with FD watchers | `KlWatcher`, `KlAsyncOp`, `kl_async_suspend/complete` |
| `thread_pool` | 8080 | Blocking work on worker threads | `KlThreadPool`, `KlWorkItem`, `work_fn/done_fn/cancel_fn` |
| `async_thread_pool` | 8080 | Thread pool + async with deadlines | `KlThreadPool` + `KlAsyncOp` together |
| `websocket_server` | 8080 (configurable) | WebSocket echo server | `kl_server_ws`, `KlWsServerConfig`, `on_message` |
| `h2_server` | 8080 (configurable) | HTTP/2 server with stub session | `KlH2ServerSession`, pluggable vtable, h2c |

### Running Server Examples

```bash
# Most servers use port 8080:
./examples/rest_api_server

# hello_server, websocket_server, h2_server accept a port argument:
./examples/hello_server 9090

# static_files requires a ./public directory:
mkdir -p public && echo "<h1>Hi</h1>" > public/index.html
./examples/static_files
```

### Testing Server Examples

```bash
# hello_server
curl localhost:8080/hello

# rest_api_server
curl localhost:8080/api/users
curl localhost:8080/api/users/42
curl -X POST -d '{"name":"Eve"}' localhost:8080/api/users

# middleware (requires Authorization header for /api/ routes)
curl -H "Authorization: Bearer tok" localhost:8080/api/public
curl -H "Authorization: Bearer tok" localhost:8080/api/private

# static_files
curl localhost:8080/index.html

# streaming
curl localhost:8080/stream

# body_readers
curl -X POST -d 'hello world' localhost:8080/echo
curl -F "name=Alice" -F "file=@photo.jpg" localhost:8080/upload

# async
curl localhost:8080/delay/200

# thread_pool
curl localhost:8080/query

# async_thread_pool
curl localhost:8080/fast
curl localhost:8080/slow

# websocket_server (requires websocat or similar WS client)
websocat ws://localhost:8080/ws
```

## Client Examples

| Example | Requires | Description | Key APIs |
|---------|----------|-------------|----------|
| `client` | `hello_server` on :8080 | Sync + async HTTP client | `kl_client_request`, `kl_client_start`, `KlClientResponse` |
| `async_client` | `hello_server` on :8080 | Concurrent async requests (fan-out) | `kl_client_start`, `KlEventCtx` standalone loop |
| `websocket_client` | `websocket_server` on :8080 | WebSocket echo client | `KlWsClientConn`, `KlWsClientCallbacks` |
| `h2_client` | — | HTTP/2 client with mock session | `KlH2ClientConn`, `KlH2ClientSession` vtable |

### Running Client Examples

```bash
# Start a server first:
./examples/hello_server &

# Then run a client:
./examples/client
./examples/async_client

# WebSocket client (needs websocket_server running):
./examples/websocket_server &
./examples/websocket_client
```

## Standalone Demos

| Example | Description | Key APIs |
|---------|-------------|----------|
| `custom_allocator` | Tracking allocator + URL parsing | `KlAllocator` vtable, `kl_url_parse` |
| `connection_pool` | Connection pool internals | `kl_conn_pool_init`, `kl_conn_acquire/release` |
| `url_parser` | URL parsing (schemes, IPv6, CRLF guard) | `kl_url_parse`, `KlUrl` |

```bash
./examples/custom_allocator
./examples/connection_pool
./examples/url_parser
```

## TLS Examples

Requires building with mbedTLS support:

```bash
make examples KEEL_TLS=mbedtls MBEDTLS_DIR=/path/to/mbedtls
```

| Example | Port | Description | Key APIs |
|---------|------|-------------|----------|
| `tls_server` | 8443 | HTTPS server | `KlTlsConfig`, `KlTls` vtable, cert/key loading |
| `tls_client` | — | HTTPS client | `kl_client_request` with TLS, CA skip for self-signed |

```bash
# Generate self-signed cert:
openssl req -x509 -newkey rsa:2048 -keyout key.pem -out cert.pem \
  -days 365 -nodes -subj '/CN=localhost'

# Run TLS server + client:
./examples/tls_server &
./examples/tls_client
```

## Smoke Tests

Run the full E2E smoke test suite:

```bash
make smoke
```

This builds all examples and runs `tests/e2e_examples.sh`, which tests every example program with curl assertions.
