# Keel vs Mongoose vs libmicrohttpd

A fact-based comparison of three embedded C HTTP libraries. Data gathered March 2026.

## Overview

| | Keel | Mongoose | libmicrohttpd |
|---|------|----------|---------------|
| **First release** | 2025 | 2004 | 2007 |
| **License** | MIT | GPLv2 / Commercial | LGPLv2.1+ |
| **Core LOC** | ~12K | ~33K (amalgamated) | ~19K |
| **Architecture** | 29 independent modules | Monolithic amalgam | Monolithic |
| **Primary target** | Embedded servers, edge services | Bare-metal MCU, IoT | Desktop/server embedding |
| **GitHub stars** | Early stage | 12,600+ | 135 (mirror; canonical repo on GNU Savannah) |

## Where Keel wins

| Dimension | Keel | Mongoose | libmicrohttpd |
|-----------|------|----------|---------------|
| **License** | MIT — no restrictions | GPLv2 or paid commercial license | LGPL — linking constraints |
| **HTTP/2** | Server + client | Not supported | Not supported |
| **Event backends** | epoll, kqueue, io_uring, poll | select/poll only | select, poll, epoll |
| **Modularity** | 29 independent, testable modules | Single amalgamated file | Monolithic library |
| **Allocator** | Runtime vtable (bring-your-own) | Compile-time macros only | None (raw malloc) |
| **TLS model** | Pluggable vtable — any backend | Built-in TLS 1.3 + pluggable | GnuTLS only |
| **HTTP parser** | Pluggable vtable — swappable | Hardcoded | Hardcoded |
| **Router + middleware** | Built-in with `:param` capture, two-phase middleware | None — DIY if/else chains | None — single callback |
| **CORS** | Built-in configurable middleware | DIY | DIY |
| **SSE** | Dedicated zero-alloc API | DIY over chunked | DIY over chunked |
| **HTTP client** | Sync + async + streaming + H2 | Basic client | Server only — no client |
| **Connection pooling** | Client-side pool with keep-alive reuse | None | None |
| **Redirect following** | Built-in (RFC 7231 method transform) | None | None |
| **Compression** | Pluggable vtable (gzip via miniz, extensible) | None | None |
| **Decompression** | Pluggable client-side response decompression | None | None |
| **Backpressure** | Built-in write buffer (`KlDrain`) | None | None |
| **Timers** | Built-in min-heap scheduling | No dedicated API | No dedicated API |
| **Cosmopolitan C** | Supported (APE binaries) | Not supported | Not supported |
| **Test density** | 644 tests (38 suites) for 12K LOC | ~4K LOC tests for 33K LOC | Fewer relative to size |
| **Code size** | ~12K LOC — auditable in a day | ~33K LOC (includes TCP/IP stack, drivers) | ~19K LOC |

## Where Keel loses

| Dimension | Keel | Mongoose | libmicrohttpd |
|-----------|------|----------|---------------|
| **Maturity** | New (2025–2026) | 20+ years, 12.6K stars | GNU project, 18+ years |
| **Production deployments** | Early stage | NASA ISS, Siemens, Samsung, Bosch — hundreds of millions of devices | NASA, Sony, Kodi, systemd |
| **Security audits** | Self-audited (fuzz, ASan, static analysis) | OSS-Fuzz continuous fuzzing | Formal audits by Least Authority (Mozilla) and Ada Logics |
| **Bare-metal / MCU** | Supported via lwIP/picoTCP (BSD socket compat) | Built-in TCP/IP stack for STM32, ESP32 etc. | Requires OS networking |
| **Threading modes** | Single-threaded (+ thread pool offload) | Single-threaded | 4 modes: external, internal, pool, thread-per-connection |
| **MQTT** | Not supported | Built-in | Not supported |
| **Built-in auth** | None (use middleware) | None | Digest + Basic auth built-in |
| **Community size** | Small | Large, corporate-backed | GNU ecosystem, distro-packaged everywhere |
| **Platform breadth** | Linux, macOS, Cosmopolitan | Linux, macOS, Windows, RTOS, bare-metal, 20+ MCU families | Linux, macOS, Windows, FreeBSD, z/OS, vxWorks |
| **Drop-in integration** | Makefile + headers | 2 files (mongoose.c + .h) — simplest possible | Autotools + pkg-config |
| **JSON parsing** | Not included | Built-in | Not included |

## Roughly equal

| Dimension | Notes |
|-----------|-------|
| **HTTP/1.1 compliance** | All three are fully compliant |
| **WebSocket** | Keel and Mongoose: full support. libmicrohttpd: experimental |
| **Keep-alive** | All three support persistent connections |
| **Chunked encoding** | All three handle chunked transfer encoding |
| **Multipart** | All three parse multipart/form-data |
| **Async suspend/resume** | Keel: `KlAsyncOp`. libmicrohttpd: `MHD_suspend/resume`. Mongoose: no direct equivalent |
| **Sanitizer testing** | All three test under ASan/UBSan |
| **Fuzz testing** | All three have fuzz targets |
| **sendfile** | Keel: Linux + macOS. Mongoose: no. libmicrohttpd: Linux only |
| **Streaming responses** | All three support callback/chunked streaming |

## API comparison

### Handler registration

**Keel** — per-route handlers with pattern matching:
```c
kl_server_route(&s, "GET", "/users/:id", handle_user, NULL, NULL);
kl_server_use(&s, "*", "/api/*", auth_middleware, &auth);
```

**Mongoose** — single event handler, DIY dispatch:
```c
mg_http_listen(&mgr, "http://0.0.0.0:8080", handler_fn, NULL);
// Inside handler_fn:
if (mg_match(hm->uri, mg_str("/api/users/*"), NULL)) { ... }
```

**libmicrohttpd** — single callback, called multiple times per request:
```c
MHD_start_daemon(MHD_USE_EPOLL, 8080, NULL, NULL, &handler, NULL,
                 MHD_OPTION_END);
// handler called once for headers, then repeatedly for body chunks
```

### Response writing

**Keel:**
```c
kl_response_status(res, 200);
kl_response_header(res, "Content-Type", "application/json");
kl_response_body(res, data, len);
```

**Mongoose:**
```c
mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", data);
```

**libmicrohttpd:**
```c
struct MHD_Response *resp = MHD_create_response_from_buffer(len, data,
                                                            MHD_RESPMEM_PERSISTENT);
MHD_add_response_header(resp, "Content-Type", "application/json");
MHD_queue_response(connection, 200, resp);
MHD_destroy_response(resp);
```

### Custom allocator

**Keel** — runtime vtable:
```c
KlAllocator alloc = { .malloc_fn = my_malloc, .realloc_fn = my_realloc, .free_fn = my_free };
KlConfig cfg = { .alloc = &alloc };
```

**Mongoose** — compile-time macros:
```c
// Set at compile time, cannot change per-instance
#define MG_MALLOC my_malloc
#define MG_FREE my_free
```

**libmicrohttpd** — no custom allocator interface. Uses `malloc`/`free` directly.

## When to use which

**Choose Keel when:**
- You want MIT licensing with no strings attached
- You need HTTP/2 support in an embedded C library
- You want a built-in router, middleware, and client — not just a raw server
- You value pluggability (allocator, parser, TLS all swappable at runtime)
- You're building on Linux or macOS and want io_uring/kqueue/epoll
- You're targeting bare-metal with lwIP/picoTCP (BSD socket compatibility — no transport vtable needed)
- Code auditability matters — 12K LOC is readable in a day

**Choose Mongoose when:**
- You're targeting bare-metal microcontrollers (STM32, ESP32, etc.)
- You need a built-in TCP/IP stack (no OS required)
- You want the simplest possible integration (2 files, copy and compile)
- You need MQTT alongside HTTP
- Battle-tested maturity is the top priority (20 years, NASA, Fortune 500)
- GPLv2 is acceptable, or you can pay for a commercial license

**Choose libmicrohttpd when:**
- You need multi-threaded request handling (thread pool or thread-per-connection)
- You need a library audited by independent security firms
- LGPL licensing works for your use case
- You're embedding an HTTP server in a larger C/C++ application
- You want a GNU project with wide distro packaging
- You only need a server (no client)

## Bare-metal / MCU support

Mongoose embeds its own TCP/IP stack (~8K LOC). Keel takes a different approach: **bring your own**.

lwIP, picoTCP, and CycloneTCP all provide BSD-compatible socket APIs (`accept`, `read`, `write`, `close`, `poll`, `getaddrinfo`). Keel's existing code links against these symbols unchanged — no transport vtable, no abstraction layer, no added complexity.

**What you need:**

1. Compile with `BACKEND=poll` (lwIP provides `poll()` via its socket compat layer)
2. Compile with `-DKL_NO_SIGNAL` (bare-metal has no POSIX signals)
3. Exclude `thread_pool.c` from `CORE_SRC` if no RTOS/pthreads (thread pool is optional)
4. Link against lwIP/picoTCP instead of system libc networking

**What already works without changes:**

- `sendfile()` — already has a `pread` + `write` fallback for non-Linux/macOS platforms
- `SO_REUSEPORT` — already guarded with `#ifdef`
- `MSG_NOSIGNAL` / `SO_NOSIGPIPE` — already guarded with `#ifdef`
- `sysconf(_SC_NPROCESSORS_ONLN)` — already has `KL_TP_DEFAULT_WORKERS` override

**Main loop pattern on bare-metal:**

```c
/* Bare-metal main loop with lwIP */
while (1) {
    sys_check_timeouts();           /* lwIP housekeeping */
    kl_event_ctx_run(&server.ev, 16, 10);  /* Keel event tick */
}
```

This is the same pattern Mongoose uses (`mg_mgr_poll` in a `while(1)` loop). The difference: Mongoose bundles the TCP/IP stack, Keel lets you choose.

**Why no transport vtable?** Every major embedded TCP/IP stack provides POSIX-compatible socket functions. A vtable abstracting `read()` over `read()` adds indirection without value. If a future stack with a fundamentally different API appears, the vtable can be added then — Keel doesn't abstract until there's a real need.
