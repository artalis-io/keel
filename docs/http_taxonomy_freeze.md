# HTTP Taxonomy Freeze (T1) — `KlServer`/`KlClient`/`KlRequest`/`KlResponse` → `KlHttp*`

Status: **docs-only design freeze**, branch `roadmap/r0-architecture-baseline`, unpushed.
Source brief: [`claude_code_transport_taxonomy_prompt.md`](claude_code_transport_taxonomy_prompt.md).

This freeze covers the **HTTP protocol-family rename** (prompt steps T2–T4). The prompt's datagram
portion (D1–D3: public `KlDatagram` socket/connect conveniences, `KlUdp`/`KlUdpServer` removal,
`KlUdpConfig` → `KlDatagramSocketConfig`) is **already complete and accepted** — see
[`keel-datagram-consolidation`] / the `datagram(D1..D3-3)` commits — and the `check-no-kludp` gate is
live. Nothing here re-opens it.

**No code is renamed in this increment.** Files are renamed with `git mv` during T2/T3 *after* this
freeze is reviewed and accepted.

---

## 0. Principle

Each public object must name a **real abstraction**, not a client/server *usage label*. HTTP is a
protocol family; its objects join the `KlHttp*` / `kl_http_*` namespace. The generic byte-stream,
datagram, event, socket, and allocator machinery that HTTP merely *consumes* stays put — renaming it
because HTTP uses it would be exactly the category error we are removing. No parallel `KlTcp*` or
`KlUdpSocket`/`KlUdpServer` families are introduced.

`KlHttp*` denotes the HTTP **family**, not HTTP/1.1. Version-specific wire mechanics keep a
version-qualified internal name (`KlHttp1*`, `KlHttp2*`).

---

## 1. HTTP version statement (frozen wording)

> `KlHttpServer` is an HTTP-family server supporting HTTP/1.x and HTTP/2 through version-specific
> protocol engines. HTTP/2 is selected through ALPN, h2c upgrade, or prior knowledge and is supplied
> through the existing `KlHttp2ServerSession` vtable (the renamed `KlH2ServerSession`), including
> integrations such as nghttp2. HTTP/1.x and HTTP/2 converge on the shared router, middleware, and
> handler application model. The former `KlClient` is the HTTP/1.x client facade (`KlHttpClient`),
> while HTTP/2 currently uses the separate `KlHttp2Client` / `KlHttp2ClientSession` surface (renamed
> from `KlH2Client` / `KlH2ClientSession`); their relationship under the `KlHttp*` taxonomy is
> **classified, not assumed unified** (see §5.2). HTTP/1.x parsing and serialization remain
> version-specific internally (`KlHttp1Parser`, `KlHttp1ChunkedDecoder`, the `hdr_buf` textual
> start-line/header serialization). The `KlHttp` naming does **not** assert that every current public
> field or lifecycle contract is directly reusable across all versions or for HTTP/3.

### 1.1 Version-neutrality field audit (honest inventory)

Public fields/contracts that are **HTTP/1.x-specific** and must not be presented as version-neutral:

| Surface | HTTP/1.x-specific element | Why | Ruling |
|---|---|---|---|
| `KlHttpResponse` (`response.h`) | `hdr_buf`/`hdr_len`/`hdr_cap` — raw **textual** start-line + header block; `send_offset` partial-writev resume | HTTP/1 wire serialization; HTTP/2 serializes via `KlHttp2ServerSession::submit_response`, not this buffer | keep on the shared response struct (both engines converge on the handler that fills status/headers/body); the *textual serialization* is an HTTP/1 code path, documented as such |
| `KlHttpResponse` | `keep_alive` | HTTP/1 connection-reuse policy (HTTP/2 multiplexes; no per-response keep-alive) | field stays; documented HTTP/1 semantics |
| `KlHttpRequest` | reason-phrase / raw start-line / textual header storage, one-request-per-connection read model | HTTP/1 framing; HTTP/2 delivers pseudo-headers via the session vtable | keep; document as HTTP/1 read path |
| `KlHttpConn` (`connection.h`) | the read-buffer/parser/keep-alive state machine | HTTP/1 connection lifecycle; hosts the h2 session as an upgrade | see §5.2 (KlConn ruling) |
| `KlHttp1Parser` / `KlHttp1ChunkedDecoder` | chunked TE, textual header parse | HTTP/1.1 only | renamed `KlHttp1*` (§5.3) |

**No field is redesigned in this increment.** The rename is mechanical; the audit above is the honest
documentation the prompt requires, not a change list. HTTP/3 is out of scope and no field is claimed
h3-ready.

---

## 2. Headline public rename map

| Old | New | Header (old → new, §6) |
|---|---|---|
| `KlServer` | `KlHttpServer` | `server.h` → `http_server.h` |
| `KlConfig` | `KlHttpServerConfig` | `server.h` → `http_server.h` |
| `KlServerStats` | `KlHttpServerStats` | `server.h` → `http_server.h` |
| `KlClient` | `KlHttpClient` | `client.h` → `http_client.h` |
| `KlClientConfig` | `KlHttpClientConfig` | `client.h` → `http_client.h` |
| `KlClientResponse` | `KlHttpClientResponse` | `client.h` → `http_client.h` |
| `KlClientHeader` | `KlHttpClientHeader` | `client.h` → `http_client.h` (§11-P2: client-scoped, parallel to `KlHttp2ClientHeader`) |
| `KlProxyConfig` | `KlHttpProxyConfig` | `client.h` → `http_client.h` |
| `KlRequest` | `KlHttpRequest` | `request.h` → `http_request.h` |
| `KlResponse` | `KlHttpResponse` | `response.h` → `http_response.h` |
| `KlBodyMode` | `KlHttpBodyMode` | `response.h` → `http_response.h` (response body-source mode enum) |
| `KlWriteFn` | `KlHttpResponseWriteFn` | `response.h` → `http_response.h` (**shared** response write sink — see §3.6) |

### 2.1 Headline function prefixes

| Old prefix | New prefix | Functions (count) |
|---|---|---|
| `kl_server_*` | `kl_http_server_*` | `_init _free _run _stop _route _route_streaming _route_streaming_async _use _use_post _stats _ws` (11) |
| `kl_client_*` | `kl_http_client_*` | `_start _start_s _start_pooled _request _request_s _request_pooled _response _response_free _error _last_error _cancel _free` (12) |
| `kl_request_*` | `kl_http_request_*` | `_header _header_len _param _conn _peer_addr _peer_sockaddr _peer_cert _peer_cred _peer_label _pause_body _resume_body _parser_llhttp` (12) |
| `kl_response_*` | `kl_http_response_*` | `_init _free _reset _status _header _json _error _body_copy _body_borrow _body_compress _file _send _begin_stream _end_stream _enable_drain _parser_llhttp _parser_llhttp_s` (17) |

Note: `kl_request_parser_llhttp` / `kl_response_parser_llhttp*` construct the HTTP/1 parser vtable —
they move to `kl_http1_parser_*` (§5.3), not `kl_http_request_*`.

### 2.2 Headline callbacks / config-embedded types

| Old | New | Note |
|---|---|---|
| `KlHandler` | `KlHttpHandler` | `void (*)(KlHttpRequest*, KlHttpResponse*, void*)` |
| `KlMiddleware` | `KlHttpMiddleware` | `int (*)(KlHttpRequest*, KlHttpResponse*, void*)` |
| `KlClientDoneFn` | `KlHttpClientDoneFn` | signature gains renamed `KlHttpClient*` |
| `KlClientBodyFn` | `KlHttpClientBodyFn` | |
| `KlClientHeadersFn` | `KlHttpClientHeadersFn` | |
| `KlClientReadFn` | `KlHttpClientReadFn` | |
| `KlClientStreamCfg` | `KlHttpClientStreamCfg` | |
| `KlParserFactory` | `KlHttp1ParserFactory` | `KlHttpServerConfig.parser`; produces a `KlHttp1Parser` (HTTP/1-specific) |
| `KlAccessLogFn` | `KlHttpAccessLogFn` | `KlHttpServerConfig.access_log`; signature embeds `KlHttpRequest*` |
| `KlLogFn` | `KlHttpServerLogFn` | `KlHttpServerConfig.log_fn`; server diagnostic sink |
| `KlTransport` | `KlHttpServerTransport` | `KlHttpServerConfig.transport` enum (TCP vs AF_UNIX listener) |

Every callback whose signature embeds a renamed public type (above, plus router/body-reader/sse/etc.
below) is updated in the same commit that renames the embedded type — see the per-module review
requirement in §10.

### 2.3 Connection family (the `KlConn` rename pulls its whole family — §5.2)

Renaming `KlConn` → `KlHttpConn` is incomplete without its state enum, pool, state constants, and
function prefix. All are public (`connection.h`) or model-blind-core public-ish (`conn_internal.h`):

| Old | New |
|---|---|
| `KlConn` | `KlHttpConn` |
| `KlConnState` | `KlHttpConnState` |
| `KlConnPool` | `KlHttpConnPool` |
| `KL_CONN_CLOSED KL_CONN_READING KL_CONN_READING_BODY KL_CONN_PROCESSING KL_CONN_SENDING KL_CONN_SUSPENDED KL_CONN_TLS_HANDSHAKE KL_CONN_PROXY_HEADER KL_CONN_WEBSOCKET KL_CONN_HTTP2` | `KL_HTTP_CONN_*` (same suffixes) |
| `kl_conn_*` → `kl_http_conn_*` | `_acquire _release _peer_addr _on_readable _on_writable _on_handshake _on_file_complete _ingest_proxy _read_proxy_header _pool_init _pool_free _pool_reserve _pool_return_credit` (public, `connection.h`) + `_dispatch_request _ingest_body _run_post_body _send_complete` (`conn_internal.h` model-blind core) |

### 2.4 Exhaustive public constants / enum values (the token inventory the reviewer required)

Beyond structs and functions, these public macros/enumerators are abstraction-specific and rename:

| Family | Old tokens | New |
|---|---|---|
| response body mode | `KL_BODY_NONE KL_BODY_BUFFER KL_BODY_FILE KL_BODY_STREAM` | `KL_HTTP_BODY_*` |
| HTTP client sizing | `KL_CLIENT_DEFAULT_TIMEOUT_MS KL_CLIENT_DEFAULT_MAX_RESP KL_CLIENT_CONNECT_ATTEMPT_DELAY_MS KL_CLIENT_RECV_BUF_SIZE KL_CLIENT_REQ_BUF_SIZE KL_CLIENT_CHUNK_BUF_SIZE KL_CLIENT_CHUNK_HDR_SIZE KL_CLIENT_FINAL_CHUNK_LEN KL_CLIENT_HOSTNAME_MAX KL_CLIENT_MAX_REQ_HEADERS` | `KL_HTTP_CLIENT_*` |
| HTTP client state (internal, `client_internal.h`) enumerators of `KlClientState`→`KlHttpClientState` (+ `KlClientConnectAttempt`→`KlHttpClientConnectAttempt`) | `KL_CLIENT_RESOLVING KL_CLIENT_CONNECTING KL_CLIENT_TLS_HANDSHAKE KL_CLIENT_PROXY_CONNECTING KL_CLIENT_PROXY_HANDSHAKE KL_CLIENT_SENDING KL_CLIENT_SENDING_STREAM KL_CLIENT_RECEIVING KL_CLIENT_DONE` | `KL_HTTP_CLIENT_*` (same `KL_CLIENT_`→`KL_HTTP_CLIENT_` prefix shift; subsumed by the gate's prefix scan) |
| HTTP/2 sizing | `KL_H2_DEFAULT_MAX_STREAMS KL_H2_DEFAULT_WINDOW_SIZE KL_H2_CLIENT_DEFAULT_TIMEOUT_MS KL_H2_CLIENT_RECV_BUF_SIZE` | `KL_HTTP2_*` |
| HTTP/1 parse result | `KL_PARSE_OK KL_PARSE_HEADERS_OK KL_PARSE_INCOMPLETE KL_PARSE_ERROR` | `KL_HTTP1_PARSE_*` |
| HTTP/1 chunk state | `KL_CHUNK_SIZE KL_CHUNK_SIZE_CR KL_CHUNK_EXT KL_CHUNK_DATA KL_CHUNK_DATA_CR KL_CHUNK_TRAILER KL_CHUNK_TRAILER_CR KL_CHUNK_DONE KL_CHUNK_ERROR` | `KL_HTTP1_CHUNK_*` |
| multipart events/errors/limits | `KL_MP_EVT_PART_BEGIN KL_MP_EVT_PART_DATA KL_MP_EVT_PART_END KL_MP_EVT_NEED_DATA KL_MP_EVT_DONE KL_MP_EVT_ERROR KL_MP_ERR_NONE KL_MP_ERR_MALFORMED KL_MP_ERR_NOMEM KL_MP_ERR_PART_TOO_LARGE KL_MP_ERR_TOTAL_TOO_LARGE KL_MP_ERR_HEADERS_TOO_LARGE KL_MP_ERR_TOO_MANY_PARTS KL_MP_ERR_PREMATURE_EOF KL_MP_ERR_INPUT_OVERFLOW KL_MP_MAX_BOUNDARY` | `KL_HTTP_MP_*` |
| CORS limits (`cors.h`) | `KL_CORS_MAX_ORIGINS KL_CORS_ORIGIN_SIZE` | `KL_HTTP_CORS_*` |
| client-pool defaults (`client_pool.h`) | `KL_CPOOL_DEFAULT_CAPACITY KL_CPOOL_DEFAULT_MAX_PER_HOST KL_CPOOL_DEFAULT_IDLE_MS` | `KL_HTTP_CLIENT_POOL_DEFAULT_*` |
| redirect default (`redirect.h`) | `KL_REDIRECT_DEFAULT_MAX` | `KL_HTTP_REDIRECT_DEFAULT_MAX` |
| router param cap (`request.h`) | `KL_MAX_PARAMS` | `KL_HTTP_ROUTER_MAX_PARAMS` (explicit — it caps `KlHttpRoute` `:param` extraction) |
| connection read buffer (`connection.h`) | `KL_READ_BUF_SIZE` | `KL_HTTP_CONN_READ_BUF_SIZE` |
| server transport enum (`server.h`) | `KL_TRANSPORT_TCP KL_TRANSPORT_UNIX` | `KL_HTTP_SERVER_TRANSPORT_TCP KL_HTTP_SERVER_TRANSPORT_UNIX` — enumerators of `KlHttpServerTransport` |
| server config defaults (`server.h`) | `KL_DEFAULT_MAX_CONNS KL_DEFAULT_READ_TIMEOUT KL_DEFAULT_MAX_BODY_SIZE` | `KL_HTTP_SERVER_DEFAULT_MAX_CONNS KL_HTTP_SERVER_DEFAULT_READ_TIMEOUT KL_HTTP_SERVER_DEFAULT_MAX_BODY_SIZE` — defaults for `KlHttpServerConfig` fields |
| server log levels (`server.h`) | `KL_LOG_TRACE KL_LOG_DEBUG KL_LOG_INFO KL_LOG_WARN KL_LOG_ERROR KL_LOG_FATAL` | `KL_HTTP_SERVER_LOG_*` (same suffixes) — levels passed to `KlHttpServerLogFn` |
| peer-address source (`connection.h`) | `KL_PEER_SOCKET KL_PEER_PROXY` | `KL_HTTP_PEER_SOCKET KL_HTTP_PEER_PROXY` — **fields of `KlHttpConn`, not generic stream metadata** (they tag whether the peer addr came from the accepted socket or a trusted PROXY header) |

Enum *type* names for the above are already in §2.2/§3.1/§3.3 (`KlHttpBodyMode`, `KlHttpMultipartEvent`,
`KlHttpMultipartErrorCode`, `KlHttp1ParseResult`, `KlHttp1ChunkedState`). Generic constants — e.g.
`KL_EVENT_CAP_*`, `KL_DGRAM_*`, `KL_SOCK_CAP_*`, `KL_TOS`/`KL_DSCP_*`/`KL_ECN_*`, `KL_INVALID_SOCKET`,
`KL_ERR_*`, `KL_AF_*` — are **not** touched (they belong to generic transport/event/error surfaces).

---

## 3. Satellite classification

Each related surface is classified **HTTP-specific** (→ `KlHttp*`), **generic** (keep), **HTTP/1.x
internal** (→ `KlHttp1*`), **HTTP/2** (→ `KlHttp2*`), or **deferred** (a ruling is frozen in §5).

### 3.1 HTTP-specific → `KlHttp*` / `kl_http_*`

| Module | Old symbols | New | Function prefix |
|---|---|---|---|
| router | `KlRouter` `KlRoute` `KlMiddlewareEntry` `KlParam` `KlBodyReaderFactory` | `KlHttpRouter` `KlHttpRoute` `KlHttpMiddlewareEntry` `KlHttpParam` `KlHttpBodyReaderFactory` | `kl_router_*` → `kl_http_router_*` |
| cors | `KlCorsConfig` | `KlHttpCorsConfig` | `kl_cors_*` → `kl_http_cors_*` |
| body_reader | `KlBodyReader` `KlBufReader` | `KlHttpBodyReader` `KlHttpBufReader` | `kl_body_reader_* kl_buf_reader_*` → `kl_http_body_reader_* / kl_http_buf_reader_*` |
| multipart | `KlMultipartReader` `KlMultipartPart(Meta)` `KlMultipartConfig` `KlMultipartEvent` `KlMultipartErrorCode` | `KlHttpMultipart*` | `kl_multipart_*` → `kl_http_multipart_*` |
| sse | `KlSse` | `KlHttpSse` | `kl_sse_*` → `kl_http_sse_*` |
| response-compression adapter | `KlCompressStream` | `KlHttpCompressStream` | `kl_compress_stream_*` → `kl_http_compress_stream_*` (§3.6) |
| redirect | `KlRedirectClient` `KlRedirectConfig` `KlRedirectDoneFn` | `KlHttpRedirectClient` `KlHttpRedirectConfig` `KlHttpRedirectDoneFn` | `kl_redirect_*` → `kl_http_redirect_*` |
| client_pool | `KlClientPool` `KlClientPoolConfig` `KlClientPoolConn` `KlClientPoolEntry` | `KlHttpClientPool` `KlHttpClientPoolConfig` `KlHttpClientPoolConn` `KlHttpClientPoolEntry` | `kl_cpool_*` → `kl_http_client_pool_*` |

Rationale: SSE (line framing over HTTP chunked), CORS, redirect, request-body readers, multipart
form parsing, the router/middleware/handler model, and the HTTP client connection pool are meaningless
outside HTTP.

**`KlWriteFn` correction (reviewer P1).** `KlWriteFn` is **not** SSE-specific: it is defined in
`response.h` (`int (*)(void *ctx, const char *data, size_t len)`) and shared by response streaming,
`KlCompressStream`, and `KlSse` as the underlying chunked-stream write sink. It is therefore the shared
**HTTP response** write callback → `KlHttpResponseWriteFn` (§2.2), and `KlHttpSse` / `KlHttpCompressStream`
**consume that shared callback** rather than declaring their own.

### 3.2 HTTP/2 → `KlHttp2*` (RULED — §5.1)

| Old | New |
|---|---|
| `KlH2ServerSession` `KlH2ServerSessionFactory` `KlH2ServerConfig` `KlH2ServerConn` `KlH2ServerCallbacks` `KlH2ServerStream` | `KlHttp2ServerSession` `…Factory` `KlHttp2ServerConfig` `…Conn` `…Callbacks` `…Stream` |
| `KlH2Client` `KlH2ClientSession(Factory)` `KlH2ClientConfig` `KlH2ClientConn` `KlH2ClientCallbacks` `KlH2ClientStream` `KlH2ClientResponse` `KlH2ClientHeader` `KlH2ClientResponseFn` `KlH2ClientErrorFn` | `KlHttp2Client…` (same suffixes) |
| `KlH2WriteFn` (internal — `src/internal.h`/`h2_internal.h`, the h2 frame output sink) | `KlHttp2WriteFn` |
| `KlH2CompHooks` `KlH2ServerHooks` (internal completion/readiness hook structs, `http_proto_hooks.h`) | `KlHttp2CompHooks` `KlHttp2ServerHooks` |
| `kl_h2_*` (e.g. `kl_h2_server_set_writer`) + `kl_comp_h2_drive` (completion driver, `completion_h2.c`) | `kl_http2_*` + `kl_comp_http2_drive` |

The HTTP/2 **engine is not redesigned** — the session vtable seam, ALPN/h2c routing, completion paths,
nghttp2 integration, and shared application-model convergence are preserved verbatim; only the
classification prefix changes (`H2` → `Http2`).

**Scope boundary (RULED).** The rename covers the three named token classes (`KlH2*`, `kl_h2_*`,
`KL_H2_*`) + the coupled `kl_comp_h2_drive` + the `h2*` files. It deliberately does **not** touch
(a) **file-local static identifiers** inside those TUs (e.g. `h2_stream_find`, `h2_cb_*`,
`h2_submit_response`, `comp_h2_capture_write`, `CompH2Cap`, `g_h2_hooks`, `g_h2_comp_hooks`, `H2_INIT`)
— implementation details with no linkage/API visibility, matched by no gate scan; nor (b) **genuine
HTTP/2 wire/protocol terminology** — the ALPN token `"h2"`, the cleartext-upgrade name `h2c` and its
state machine (`H2cState`, `h2c_*`, `H2C_*`), and the connection preface — which are protocol
identifiers, not taxonomy labels. Renaming either would be churn without contract meaning; both are
outside the frozen public/inter-TU surface.

**Integration adapters (RULED — symbols now, files/prose in T3/T4).** The nghttp2 adapter's *symbols*
are renamed in T2f with everything else (it exposes the `KlHttp2ServerSession` / `KlHttp2ClientSession`
vtables and its `kl_h2_nghttp2_*` factories become `kl_http2_nghttp2_*`), and the integration builds
green against the renamed core headers. Its *filenames* (`integrations/nghttp2/h2_nghttp2_{client,server}.c`,
`keel_h2_nghttp2.h`), its archive (`libkeel_h2_nghttp2.a`), the matching `LIB`/`OBJ` tokens + comments in
`integrations/nghttp2/Makefile`, and `integrations/nghttp2/README.md` prose are NOT in the frozen §4 core
file map and are deferred to the T3/T4 integration + documentation reconciliation — same bucket as the
other `integrations/*/README.md` and Makefile-comment deferrals. The §11 tree-wide scan requires no stale
*symbol* in `integrations/`; it does not mandate integration *filename* renames.

### 3.3 HTTP/1.x internal → `KlHttp1*`

| Old | New | Home |
|---|---|---|
| `KlParser` `KlRequestParser` `KlResponseParser` `KlResponseParserFactory` `KlParseResult` | `KlHttp1Parser` `KlHttp1RequestParser` `KlHttp1ResponseParser` `KlHttp1ResponseParserFactory` `KlHttp1ParseResult` | `parser.h` → `http1_parser.h` |
| `kl_parser_llhttp` `kl_request_parser_llhttp` `kl_response_parser_llhttp(_s)` | `kl_http1_parser_llhttp` `kl_http1_request_parser_llhttp` `kl_http1_response_parser_llhttp(_s)` | — |
| `KlChunkedDecoder` `KlChunkedState` | `KlHttp1ChunkedDecoder` `KlHttp1ChunkedState` | `chunked.h` → `http1_chunked.h` |

Chunked transfer-encoding and the textual request/response parser vtable are HTTP/1.1 wire mechanics.
(The llhttp parser vtable is pluggable but HTTP/1-shaped; HTTP/2 has its own session parser.)

### 3.4 Generic — KEEP unchanged (do NOT rename)

These are consumed by HTTP but are not HTTP:

- **Byte-stream transport:** `KlStream` `KlListener` `KlConnectOp` `KlSlotLease` (+ `*_detail.h`) — the
  prompt forbids `KlTcp*`; TCP/Unix/lwIP/TLS-wrapped is a provider/composition choice below `KlStream`.
  `KlSlotLease` is the generic accept-slot backpressure lease on `KlListener` — **not** HTTP; keep.
- **Datagram:** `KlDatagram` `KlDatagramBatch` `KlDatagramOps` `KlDatagramSocketConfig` `KlDatagramMessage` — already canonical.
- **Event/socket:** `KlEventLoop` `KlEventCtx` `KlEvent(Mask)` `KlWatcher(Fn)` `KlSocketProvider` `KlSocketOps` `KlSocketHandle` `KlSockAddr` `KlIoVec`.
- **Infra:** `KlAllocator` `KlError` `KlUrl` `KlTimer*` `KlThreadPool*` `KlDrain*` `KlFileIO*`.
- **Transport security:** `KlTls` `KlTlsConfig` `KlTlsCtx` `KlTlsResult` `KlTlsFactory` `KlPeerCert` `KlPeerCred` — a vtable that wraps *any* stream; not HTTP.
- **Resolver:** `KlResolver*` `KlDnsResolver*` `KlResolverCache*`.
- **WebSocket:** `KlWsServer*` `KlWsClient*` `KlWsFrameParser*` — see §5.4 (WebSocket is its own protocol, reached *via* HTTP Upgrade, not part of the HTTP family).

### 3.5 Deferred / ruled (see §5)

`KlConn`, `KlAsyncOp`/`KlAsyncFn`, `proxy_protocol` (`KlCidr`/`KlProxyResult` + its `KlConfig`
reference), `KlCompress*`/`KlDecompress*` — each carries a genuine coupling; rulings frozen in §5.

---

## 4. File / module map

`git mv` in T2/T3. Every rename below updates: the file's own `#include` guard + self-include, every
`#include` of it (src/tests/examples/parsers), the umbrella `include/keel/keel.h`, `include/keel/net.h`
if listed, the Makefile source manifests (§7), CI references (§7), and doc links (§8).

### 4.1 Public headers — RENAME

| Old | New |
|---|---|
| `include/keel/server.h` | `include/keel/http_server.h` |
| `include/keel/client.h` | `include/keel/http_client.h` |
| `include/keel/request.h` | `include/keel/http_request.h` |
| `include/keel/response.h` | `include/keel/http_response.h` |
| `include/keel/router.h` | `include/keel/http_router.h` |
| `include/keel/cors.h` | `include/keel/http_cors.h` |
| `include/keel/body_reader.h` | `include/keel/http_body_reader.h` |
| `include/keel/body_reader_multipart.h` | `include/keel/http_body_reader_multipart.h` |
| `include/keel/sse.h` | `include/keel/http_sse.h` |
| `include/keel/redirect.h` | `include/keel/http_redirect.h` |
| `include/keel/client_pool.h` | `include/keel/http_client_pool.h` |
| `include/keel/connection.h` | `include/keel/http_connection.h` (§5.2) |
| `include/keel/parser.h` | `include/keel/http1_parser.h` |
| `include/keel/chunked.h` | `include/keel/http1_chunked.h` |
| `include/keel/h2.h` | `include/keel/http2.h` |
| `include/keel/h2_server.h` | `include/keel/http2_server.h` |
| `include/keel/h2_client.h` | `include/keel/http2_client.h` |

### 4.2 Public headers — KEEP (transport/infra-generic; must NOT be renamed)

`stream.h stream_detail.h listener.h listener_detail.h connect_op.h connect_op_detail.h datagram.h
datagram_batch.h datagram_detail.h socket.h socket_dgram.h event.h event_ctx.h handle.h sockaddr.h
net.h allocator.h error.h url.h timer.h thread_pool.h drain.h tls.h file_io.h async.h (§5.5)
resolver.h resolver_cache.h dns_resolver.h compress.h compress_miniz.h decompress.h decompress_miniz.h
(§5.6) proxy_protocol.h (§5.7) websocket.h websocket_server.h websocket_client.h (§5.4) freestanding.h`
and the umbrella `keel.h` (contents updated, filename unchanged).

### 4.3 Source files (`src/*.c`) — RENAME

| Old | New |
|---|---|
| `server.c server_core.c server_activation.c server_ws.c server_plat_posix.c server_plat_win.c` | `http_server.c http_server_core.c http_server_activation.c http_server_ws.c http_server_plat_posix.c http_server_plat_win.c` |
| `server_h2.c` | `http2_server.c` |
| `client_async.c client_common.c client_sync.c client_proxy.c` | `http_client_async.c http_client_common.c http_client_sync.c http_client_proxy.c` |
| `client_pool.c` | `http_client_pool.c` |
| `response.c` | `http_response.c` |
| `connection.c` | `http_connection.c` (§5.2) |
| `router.c` | `http_router.c` |
| `cors.c` | `http_cors.c` |
| `body_reader_buffer.c body_reader_multipart.c` | `http_body_reader_buffer.c http_body_reader_multipart.c` |
| `chunked.c` | `http1_chunked.c` |
| `sse.c` | `http_sse.c` |
| `redirect.c` | `http_redirect.c` |
| `h2_client.c` | `http2_client.c` |
| `completion_h2.c` | `completion_http2.c` |
| `completion_server.c` | `completion_http_server.c` |
| `proto_hooks.c` (+ `proto_hooks.h`) | `http_proto_hooks.c` / `.h` (§5.8) |
| `parsers/parser_llhttp.c` | `parsers/http1_parser_llhttp.c` |
| `parsers/response_parser_llhttp.c` | `parsers/http1_response_parser_llhttp.c` |

### 4.4 Internal headers (`src/*.h`) — RENAME

| Old | New |
|---|---|
| `client_internal.h` | `http_client_internal.h` |
| `client_proxy.h` | `http_client_proxy.h` |
| `conn_internal.h` | `http_conn_internal.h` |
| `response_internal.h` | `http_response_internal.h` |
| `h2_internal.h` | `http2_internal.h` |
| `server_plat.h` | `http_server_plat.h` |

### 4.5 Source / internal-header — KEEP (generic)

`.c`: `stream*.c listener.c connect_op.c datagram*.c socket*.c event*.c completion_core.c
completion_dispatch.c completion_absent.c completion_readiness_stub.c completion_ws.c (§5.4)
allocator*.c url.c timer.c thread_pool.c drain.c sockaddr.c dns_*.c resolve_sync.c resolver_cache.c
compress*.c decompress*.c proxy_protocol.c async.c file_io.c websocket*.c kl_cstr*.c error.c version.c
udp_cmsg*.c`.
`.h`: `completion*.h io_engine.h event*.h stream*.h listener.h connect_op.h datagram*.h socket*.h
sockaddr_native.h sockcompat.h udp_cmsg*.h base64.h sha1.h utf8.h kl_cstr.h dns_sys.h resolve_sync.h
drain_reserve.h watcher_internal.h platform.h dgram_recv_classify.h internal.h`.

### 4.6 Tests — RENAME (file follows module)

`test_server.c→test_http_server.c  test_server_integration→test_http_server_integration
test_server_stats→test_http_server_stats  test_client→test_http_client
test_client_stream→test_http_client_stream  test_client_pool→test_http_client_pool
test_client_happy_eyeballs→test_http_client_happy_eyeballs  test_request→test_http_request
test_response→test_http_response  test_router→test_http_router  test_cors→test_http_cors
test_connection→test_http_connection  test_body_reader→test_http_body_reader
test_multipart_stream→test_http_multipart_stream  test_chunked→test_http1_chunked
test_sse→test_http_sse  test_redirect→test_http_redirect  test_response_parser→test_http1_response_parser
test_h2→test_http2  test_h2_client→test_http2_client  test_proxy→test_http_client_proxy`.

**HTTP-specific tests with GENERIC names (reviewer audit) — also RENAME:**
`test_integration→test_http_integration` (73 `KlServer` refs), `test_proto_hooks→test_http_proto_hooks`
(exercises the HTTP proto-hook seam, §5.8).

**Tests KEPT (audited generic even though a server may drive them — update references only):**
`test_io_status` (io_status completion seam), `test_drain` (29 `KlDrain` refs — generic backpressure),
`test_async` (§5.5), `test_websocket*`, `test_proxy_protocol` (PROXY parser is generic), plus every
non-HTTP suite (event/socket/stream/datagram/dns/tls/url/timer/thread_pool/…). All kept HTTP-adjacent
suites still **update references** to renamed types.

### 4.7 Examples — update references; filenames UNCHANGED

Example files are named by *scenario* (`hello_server.c`, `rest_api_server.c`, `h2_server.c`,
`tls_server.c`, `websocket_server.c`, `proxy_client.c`, …), not by type, so **no example file is
renamed**; each simply consumes the new public names. (`custom_socket_provider.c`, `url_parser.c`,
`timer.c` touch few/no HTTP types.) The `make examples` target list and the CLAUDE.md example roster
are unchanged in name.

### 4.8 Fuzz targets

`fuzz_parser` (HTTP/1 request parser + chunked) and `fuzz_response_parser` exercise HTTP/1 wire code.
Internal references rename; the fuzz TARGET names stay (`make fuzz` docs reference them) unless the
final gate (§9) requires otherwise — frozen: **keep fuzz target names**, update their source references.

---

## 5. Frozen rulings on the coupled/ambiguous surfaces

**§5.1 `KlH2*` → `KlHttp2*` (RULED: rename).** HTTP/2 *is* the HTTP family; `KlHttp2*` is the
classification-consistent name. The abbreviation `H2` is dropped from public names. Engine unchanged.

**§5.2 `KlConn` → `KlHttpConn` (whole family, §2.3); `connection.{h,c}` → `http_connection.{h,c}`
(RULED).** `KlConn` is the HTTP connection object: it owns the read buffer, the HTTP/1 parse/keep-alive
state machine, and hosts the HTTP/2 session on upgrade/prior-knowledge. It is therefore the **HTTP
connection**, not a generic stream (that is `KlStream`, which it composes). Named `KlHttpConn`
(HTTP-family, both versions ride it); the HTTP/1-only wire specifics inside `http_connection.c` are
documented as the HTTP/1 path, not split into a separate `KlHttp1Connection` type in this increment (no
state-machine redesign). The rename pulls the **entire family** — `KlConnState`→`KlHttpConnState`,
`KlConnPool`→`KlHttpConnPool`, `KL_CONN_*`→`KL_HTTP_CONN_*`, `kl_conn_*`→`kl_http_conn_*` (§2.3) — all of
which appear in the symbol map AND the §9 gate word list.

**§5.3** — folded into §3.3.

**§5.4 WebSocket keeps `KlWs*`; `websocket*.{h,c}` and `completion_ws.c` unchanged (RULED).** WebSocket
is a **distinct protocol** reached *through* an HTTP/1.1 `Upgrade`; it is not HTTP. Renaming it `KlHttp*`
would repeat the usage-label error. Only the HTTP-server **entry point** renames with the server:
`kl_server_ws` → `kl_http_server_ws_upgrade`. `KlWsServerConfig`/`KlWsServerConn`/`KlWsClient*`/
`KlWsFrameParser` stay; their signatures update where they embed a renamed HTTP type (`KlRequest` →
`KlHttpRequest`, `KlConn` → `KlHttpConn`).

**§5.5 `KlAsyncOp`/`KlAsyncFn` stay generic; `async.{h,c}` unchanged (RULED).** The suspend/resume
mechanism is an event-loop primitive (`KlEventCtx`-based), not HTTP. The COUPLING is only in the
function signatures — `kl_async_suspend(KlHttpServer*, KlHttpConn*, …)` — which update to the renamed
HTTP types. The `async.h` header stays; `kl_async_*` names stay. (`test_async` stays.)

**§5.6 Compression/decompression — SPLIT codec-engine (generic) vs HTTP adapter (RULED — reviewer P1).**
The prompt's "wholly generic" wording was wrong; the audit splits each module:

- **Generic codec engine — KEEP:** `KlCompress` (algorithm vtable), `KlCompressFactory`, `KlCompressCtx`,
  `KlCompressConfig` (names a factory+ctx); `KlDecompress`, `KlDecompressConfig`, **and the entire
  decompression stream** `KlDecompressStream` + `kl_decompress_stream_init/feed/free` + `kl_decompress_body`
  — the audit confirms these are pure byte-in/byte-out (their structs embed only `KlDecompress`/
  `KlAllocator`, their functions take `(in, out)` buffers, **no HTTP type**). `compress.{h,c}`,
  `decompress.{h,c}` and the `*_miniz` backends keep their filenames.
- **HTTP response-compression adapter — RENAME `KlHttp*`:** `KlCompressStream` → `KlHttpCompressStream`
  (its struct embeds `KlResponse *res` + the response `KlWriteFn`), and `kl_compress_stream_begin/write/end`
  → `kl_http_compress_stream_*` (each takes `KlHttpResponse*` + an HTTP status). The response helper
  `kl_response_body_compress` → `kl_http_response_body_compress` is already covered by the `kl_response_*`
  rename. These adapters stay physically in `compress.{h,c}` (no file rename) but adopt the HTTP names,
  since they are HTTP response-body machinery, not the codec.

Honest asymmetry: compression has an HTTP adapter (it drives a `KlResponse`); decompression does not (the
HTTP client merely *calls* the generic byte helper). Documented rather than forced symmetric.

**§5.7 `proxy_protocol` stays generic; `proxy_protocol.{h,c}` unchanged (RULED).** The PROXY-protocol
v1/v2 parser (`KlProxyResult`, `KlCidr`, `kl_proxy_*`) is an L4 header decoder. Its only HTTP coupling
is a field reference to the server config (`KlConfig proxy_trusted_cidrs`), which updates to
`KlHttpServerConfig`. `test_proxy_protocol` stays. (Note: the HTTP *client's* forward-proxy config
`KlProxyConfig` — distinct — DOES rename to `KlHttpProxyConfig`, §2.)

**§5.8 `proto_hooks.{c,h}` → `http_proto_hooks.{c,h}` (RULED).** The protocol-dispatch hooks
(`comp_conn_dispatch` installation, h1/h2 handler application) are HTTP-server internals. Renamed with
the HTTP server. (Verify no generic completion-core dependency inverts this during T2.)

---

## 6. Compatibility ruling

Pre-1.0; ABI breaks accepted through M5 and the datagram consolidation. **Clean rename, final state
exposes only the new taxonomy:**

- No permanent typedef aliases (`KlServer`→`KlHttpServer` etc.).
- No duplicate old/new function symbols.
- No compatibility macro layer.
- No deprecated forwarding headers **in the final state**.
- **Temporary buildability device (only if the dependency graph forces it):** a rename that must land
  across >1 commit may keep a *transitional* forwarding header (`server.h` → `#include
  <keel/http_server.h>`) or a `typedef` **only within the increment that introduces it**, removed by
  the end of T4. Any such device is named in the increment's commit body with its removal commit. The
  preferred shape (§8) renames each module atomically (header+source+refs in one commit) so **no
  transitional device is needed**.

---

## 7. Increment plan (each commit builds; independently reviewable)

- **T1 (this doc)** — freeze; committed; pause for review.
- **T2 — headline atomic renames.** One reviewable commit per headline module, each doing header+source
  `git mv` + all `#include`/reference updates + Makefile-manifest + umbrella update, so the tree builds
  after each:
  - T2a `KlResponse`→`KlHttpResponse` (`response.*`) — leaf-most (server + client both depend on it).
  - T2b `KlRequest`→`KlHttpRequest` (`request.h`).
  - T2c `KlConn`→`KlHttpConn` (`connection.*` → `http_connection.*`) + `KlHttp1Parser`/`chunked`.
  - T2d `KlServer`→`KlHttpServer` + `KlHttpServerConfig`/`Stats` (`server*.*` → `http_server*.*`, router/cors/body_reader/multipart/sse under it).
  - T2e `KlClient`→`KlHttpClient` + config/response/header/pool/redirect (`client*.*`).
  - T2f `KlH2*`→`KlHttp2*` (`h2*.*` → `http2*.*`, `completion_h2.c`, `server_h2.c`).
- **T3 — satellites + internal HTTP/1 naming + examples/tests/docs.** Rename the test files (§4.6),
  update all example references, update `docs/` links, `proto_hooks`→`http_proto_hooks`, internal
  headers (§4.4).
- **T4 — final removal + stale-name gate (§9) + doc reconciliation** (CLAUDE.md module list + Key Types,
  README, architecture docs).

Dependency order is response → request → connection → server/client → h2; adjust only if a build edge
requires it. Never a single mega-commit.

### 7.1 Makefile / CI touch-points (exact)

Makefile source manifests that list the renamed `src/*.c` (each must be updated as its file is `git
mv`'d): the core object list (~L187–195), the freestanding/server manifests (~L856–860), TIER1_INFRA
(~L153/178–192), and the freestanding client/server/self-contained lists (~L1002, ~L1187, ~L1299–1302).
`parsers/*.c` renames update the parser manifest. Test enrollment is `$(wildcard tests/test_*.c)` — auto-
discovered, so renamed test files need no manifest edit, but any **explicit** suite lists
(`IOURING_TEST_SUITES`, `WIN_TEST_SUITES`) that name `test_server`/`test_client`/`test_h2`/etc. must be
updated. CI (`.github/workflows/ci.yml`) references to example/smoke names and any `test_*` gate lines
update in T3/T4.

---

## 8. Documentation reconciliation

- `docs/architecture.md` links to `../include/keel/*.h` for the renamed headers — updated in T3/T4;
  `check-doc-refs` enforces the links resolve.
- CLAUDE.md: module list entries (server/client/request/response/router/…/h2) + the Key Types table
  rows + the `Key Types` header column → renamed; `datagram_vs_udp.md` etc. are historical and stay.
- README / CONTRIBUTING: public-name references updated.
- Historical design docs under `docs/` that record the *old* names as history are **preserved** and
  covered by the gate's doc allowlist (§9).

---

## 9. Stale-name gate (T4) — `check-no-httplegacy`

A permanent gate (sibling of `check-no-kludp` / `check-doc-refs`), token-oriented to resist mixed-line
masking (the lesson from the `check-no-kludp` correction):

THREE independent scans (the `check-no-kludp` lesson: token-oriented, not line-filtered):

- **Object types — exact-identifier word list, rejected tree-wide (no allowlist).** The list is the
  complete set of old public type/enum-type names from §2/§2.3/§3, banned as whole words (`grep -wF`-style
  exact identifiers, NOT a broad alternation, because `KlConfig`/`KlConn`/`KlRoute`/`KlParam`/`KlHandler`
  are short/collision-prone): `KlServer KlConfig KlServerStats KlClient KlClientConfig KlClientResponse
  KlClientHeader KlProxyConfig KlRequest KlResponse KlBodyMode KlWriteFn KlConn KlConnState KlConnPool
  KlHandler KlMiddleware KlMiddlewareEntry KlRouter KlRoute KlParam KlBodyReaderFactory KlCorsConfig
  KlBodyReader KlBufReader KlMultipartReader KlMultipartPart KlMultipartPartMeta KlMultipartConfig
  KlMultipartEvent KlMultipartErrorCode KlSse KlCompressStream KlRedirectClient KlRedirectConfig
  KlRedirectDoneFn KlClientPool KlClientPoolConfig KlClientPoolConn KlClientPoolEntry KlChunkedDecoder
  KlChunkedState KlParser KlRequestParser KlResponseParser KlResponseParserFactory KlParseResult
  KlClientDoneFn KlClientBodyFn KlClientHeadersFn KlClientReadFn KlClientStreamCfg KlH2ServerSession
  KlH2ServerSessionFactory KlH2ServerConfig KlH2ServerConn KlH2ServerCallbacks KlH2ServerStream
  KlH2Client KlH2ClientSession KlH2ClientSessionFactory KlH2ClientConfig KlH2ClientConn KlH2ClientCallbacks
  KlH2ClientStream KlH2ClientResponse KlH2ClientHeader KlH2ClientResponseFn KlH2ClientErrorFn KlH2WriteFn
  KlH2CompHooks KlH2ServerHooks
  KlParserFactory KlAccessLogFn KlLogFn KlTransport KlClientState KlClientConnectAttempt`.
  (Not banned — deliberately KEPT generic: `KlCompress KlCompressConfig KlCompressCtx KlCompressFactory
  KlDecompress KlDecompressConfig KlDecompressStream KlSlotLease KlWs* KlCidr KlProxyResult KlAsyncOp
  KlAsyncFn`.)
- **Constant/macro tokens — exact-prefix word list, rejected tree-wide.** Ban the OLD prefixes/tokens:
  `KL_CONN_ KL_BODY_ KL_CLIENT_ KL_H2_ KL_PARSE_ KL_CHUNK_ KL_MP_ KL_CORS_ KL_CPOOL_ KL_REDIRECT_
  KL_TRANSPORT_ KL_LOG_ KL_READ_BUF_SIZE KL_PEER_SOCKET KL_PEER_PROXY KL_MAX_PARAMS KL_DEFAULT_MAX_CONNS
  KL_DEFAULT_READ_TIMEOUT KL_DEFAULT_MAX_BODY_SIZE`. Generic constant prefixes (`KL_EVENT_
  KL_DGRAM_ KL_SOCK_ KL_ERR_ KL_AF_ KL_TOS KL_DSCP_ KL_ECN_ KL_INVALID_SOCKET KL_WS_`) are never matched
  (different roots). Note `KL_MAX_PARAMS`/`KL_READ_BUF_SIZE`/`KL_PEER_*`/`KL_DEFAULT_MAX_CONNS`/
  `KL_DEFAULT_READ_TIMEOUT`/`KL_DEFAULT_MAX_BODY_SIZE` are exact tokens, not prefixes.
- **Function tokens — extracted individually** (`grep -o`, one per line with `file:line`), ban the old
  public families: `kl_server_ kl_client_ kl_request_ kl_response_ kl_conn_ kl_router_ kl_cors_
  kl_body_reader_ kl_buf_reader_ kl_multipart_ kl_sse_ kl_redirect_ kl_cpool_ kl_parser_ kl_chunked_
  kl_h2_` (→ `kl_http2_`) **plus the completion-axis h2 entry `kl_comp_h2_` (→ `kl_comp_http2_`; e.g.
  `kl_comp_h2_drive`, the lib-internal driver defined in `completion_h2.c`→`completion_http2.c` — renamed
  with its file so no `h2` infix survives on an exported symbol; NOT a `kl_h2_` prefix so listed
  explicitly)** **plus the exact internal server-log helpers `kl_log` / `kl_log_errno`**
  (→ `kl_http_server_log` / `kl_http_server_log_errno`; exact tokens, not a `kl_log_` prefix)
  **plus the single HTTP adapter exception `kl_compress_stream_`** (while the
  codec family `kl_compress_*` and all of `kl_decompress_*` stay allowed). Retained generic roots
  (`kl_socket_ kl_stream_ kl_datagram_ kl_event_ kl_watcher_ kl_timer_ kl_tls_ kl_url_ kl_thread_pool_
  kl_async_ kl_drain_ kl_proxy_ kl_resolver_ kl_dns_ kl_ws_`) are never matched.
- **Allowlist:** historical design docs under `docs/` (this freeze, the taxonomy prompt, the datagram
  design docs) — an explicit path list, so recorded history is not rejected.
- Portable BSD+GNU grep, `-I` binary skip, `file:line` diagnostics, permanent mixed-line canary proving
  a co-located new name cannot mask an old one.

The exact word lists above are frozen from the §2–§3 tables; T4 wires them verbatim (adjusting only if a
short identifier like `KlConn`/`KlRoute` proves to collide with an unrelated symbol, in which case an
anchored `\bKlConn\b` word-boundary match is used).

---

## 10. Behavioral boundary + review standard (per code increment)

No runtime behavior change; no state-machine redesign; no new parallel transport state machine; no
`KlUdpSocket`; no `KlTcp*`; no new/redesigned HTTP/2 engine; no HTTP/3; no ABI promise. Public layouts
change **only** as mechanically forced by a renamed embedded type.

Validation each code increment (T2–T4): full default suite; `debug-test` ASan/UBSan; pollcomp +
container io_uring suites; MinGW cross-compile; EFI host-mock + lwIP + freestanding gates; cppcheck;
`make bench-build` (compile-only benchmark — a public-API consumer OUTSIDE src/tests/examples that a
headline rename can otherwise miss, e.g. `bench/bench_server.c`); `make fuzz` (the libFuzzer targets are
public-API consumers under `fuzz/` — a directory NO earlier scan covered, so a headline rename silently
broke `fuzz/fuzz_response_parser.c` at T2e; compile ALL targets, or at minimum the affected fuzz source,
with the normal fuzz configuration — macOS `CC=/opt/homebrew/opt/llvm@18/bin/clang`); `check-tier1-boundary`;
`check-sockaddr-neutral`; `check-doc-refs`; `check-no-kludp`; `git diff --check`; and a tree-wide symbol
scan (src/include/tests/examples/parsers/integrations/bench/**fuzz** + Makefile) proving no unintended old public
name remains. Review each renamed callback/vtable for
type-correct function-pointer signatures and ownership/lifetime; ensure installed headers stay
self-contained after `git mv`; ensure examples/tests consume public names (no private aliases).

---

## 11. Decision log (all RULED — reviewer round 1 folded in)

Accepted by the reviewer in round 1 (now frozen, no longer open): `KlH2*`→`KlHttp2*` (§5.1);
`KlConn`→`KlHttpConn` and its whole family (§5.2/§2.3); scenario-based example filenames kept (§4.7);
`KlAsyncOp`/`KlAsyncFn`, TLS, and the generic transport/event/socket objects stay generic (§3.4/§5.5);
WebSocket stays `KlWs*` (§5.4); `proto_hooks`→`http_proto_hooks` (§5.8).

Reviewer-round-1 corrections folded into this revision:

1. **P1 — connection family fully mapped** (§2.3): `KlConnState`/`KlConnPool`/`KL_CONN_*`/`kl_conn_*`
   added to the symbol map and the §9 gate.
2. **P1 — exhaustive public-token inventory** (§2.4): `KlBodyMode`+`KL_BODY_*`, `KL_CLIENT_*`,
   `KL_H2_*`→`KL_HTTP2_*`, `KL_PARSE_*`→`KL_HTTP1_PARSE_*`, `KL_CHUNK_*`→`KL_HTTP1_CHUNK_*`,
   `KL_MP_*`→`KL_HTTP_MP_*`, plus the multipart/router/body-reader callbacks — all in the map + gate.
3. **P1 — `KlWriteFn` reclassified** (§2.2/§3.1): it is the **shared** `response.h` write sink (response
   streaming + compression + SSE), renamed `KlHttpResponseWriteFn`; `KlHttpSse`/`KlHttpCompressStream`
   consume it — it is NOT SSE-specific.
4. **P1 — compression split** (§5.6): codec engine (`KlCompress`/factory/ctx/config, and the whole
   generic **decompression** side per the audit) stays generic; the HTTP response-compression adapter
   (`KlCompressStream`→`KlHttpCompressStream`, `kl_compress_stream_*`→`kl_http_compress_stream_*`) is
   HTTP-specific.
5. **P2 — `KlClientHeader`→`KlHttpClientHeader`** (client-scoped, §2), per the reviewer's recommendation
   (leaves `KlHttp2ClientHeader` distinct; header-model convergence is a separate future effort).
6. **File audit extended** to HTTP tests with generic names (§4.6): `test_integration`→`test_http_integration`,
   `test_proto_hooks`→`test_http_proto_hooks` (with `test_io_status`/`test_drain`/`test_async` audited and
   kept generic).

Reviewer-round-2 correction folded in (§2.4/§3.2/§3.4/§9): added the remaining exported HTTP-owned
constants — `KL_CORS_*`, `KL_CPOOL_DEFAULT_*`→`KL_HTTP_CLIENT_POOL_DEFAULT_*`, `KL_REDIRECT_DEFAULT_MAX`,
`KL_MAX_PARAMS`→`KL_HTTP_ROUTER_MAX_PARAMS`, `KL_READ_BUF_SIZE`→`KL_HTTP_CONN_READ_BUF_SIZE`,
`KL_PEER_SOCKET`/`KL_PEER_PROXY`→`KL_HTTP_PEER_*` (they are `KlHttpConn` fields, not generic stream
metadata) — plus the internal `KlH2WriteFn`→`KlHttp2WriteFn`; the §9 constant scan now covers them.
Confirmed `KlSlotLease` (listener accept-slot lease) stays **generic** and must not rename.

T2 can begin on acceptance of this revision. Nothing is deferred.
