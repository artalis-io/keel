# KEEL — Roadmap

## v1.0.0 (March 2026)

### Status

Keel is **production-ready for embedded/edge workloads**. 31 orthogonal modules with clean vtable-based pluggability. 671 tests (40 suites), 4 fuzz targets, ASan+UBSan+static analysis in CI. The architecture, module design, and security posture are professional-grade. Test coverage (11K+ lines of tests vs ~14K of implementation) is well above average for C projects this size.

### Strengths

- **Architecture**: 31 orthogonal modules with clean vtable-based pluggability (allocator, parser, TLS, body reader, H2 session, DNS resolver). `KlEventCtx` composition pattern is well-designed — embeddable in `KlServer` but usable standalone.
- **Zero-allocation hot path**: Pre-allocated connection pool, zero-copy header parsing into `read_buf`, `writev` scatter-gather, `sendfile` with `TCP_CORK`, pre-built status lines.
- **Security posture**: CRLF injection guards, `SIZE_MAX/2` overflow checks throughout, dual-layer body timeouts (idle + absolute deadline to defeat slow-chunk attacks), TLS vtable validation, WebSocket frame validation, `FORTIFY_SOURCE + stack-protector-strong`, ASan+UBSan+fuzz in CI.
- **Testing**: 40 suites, 671 tests, dedicated overflow boundary tests, end-to-end async suspend/resume tests, cross-module integration tests, 4 fuzz targets.
- **Two-phase middleware**: Pre-body and post-body middleware with correct keep-alive semantics is a design not found in other C HTTP libraries.

### What's in 1.0.0

- **Core**: 100-continue, HEAD auto-strip, graceful drain, signal handling, HTTP/1.0 compat, IPv6, chunked decoder, growable read buffer (431 on overflow), null-terminated header values, buffer body send fix (`try_writev` + `send_offset`)
- **Pluggable interfaces**: TLS vtable, parser vtable, body reader vtable, resolver vtable, compress/decompress vtables, file I/O vtable (io_uring splice), H2 session vtable
- **Client**: HTTP/1.1 (sync + async + streaming), H2 client, WebSocket client, connection pool (keep-alive reuse), redirect following (RFC 7231/7538), proxy support (HTTP + CONNECT tunnel), response decompression
- **Server features**: Router with `:param` capture, two-phase middleware (pre-body + post-body), CORS middleware, H2 server, WebSocket server (auto-ping keep-alive), SSE helper, response compression, server stats for load introspection
- **Infrastructure**: `KlEventCtx` composition, `KlWatcher` FD callbacks, `KlAsyncOp` suspend/resume, `KlThreadPool` with pipe-based wakeup, `KlDrain` backpressure buffer, `KlTimer` min-heap scheduling, `KlError` diagnostics (23 codes + `kl_strerror`), `KlAllocator` vtable, `KlResolverCache` decorator
- **Build/test**: Static analysis (scan-build + cppcheck), fuzz testing (4 targets), Doxygen API docs, `make install` + pkg-config, Cosmopolitan C support, code coverage

### Correctness Issues Fixed

| Issue | Resolution |
|-------|------------|
| `writev_all` spins on EAGAIN | Replaced with single-attempt `try_writev` for buffer bodies. Added `send_offset` for partial send resume. |
| `kl_response_body_copy` silent failure | Response API functions now return `int` (0 success, -1 failure). Header append includes rollback on partial failure. |
| Blocking DNS in async client | Added `KlResolver` vtable for pluggable async DNS. Client state machine has `KL_HCLIENT_RESOLVING` state with cancel support. |
| Non-null-terminated header values | Method, path, query, and all header names/values null-terminated in-place after parsing. Zero allocation, still zero-copy. |

### Architectural Gaps Fixed

| Gap | Resolution |
|-----|------------|
| 8KB fixed read buffer | Heap-allocated, growable buffer (doubles up to `max_header_size`). Returns 431 when exceeded. Shrinks back on keep-alive reset. |
| `connection.c` monolith | Extracted static helpers, unified `HEADERS_OK`/`PARSE_OK` dispatch path (~85 lines removed). |

**Deliberate design choices** (not gaps):

- **Single-threaded event loop** — Same model as Node.js, Redis, Nginx (per-worker), and Python asyncio. No mutexes, no lock contention, no data races — the entire connection state machine is lock-free by construction. `KlThreadPool` offloads blocking work (SQLite, DNS, file I/O) to workers; the event loop stays responsive. Multi-core scaling is horizontal via `SO_REUSEPORT` with multiple processes, not shared-memory threading.
- **O(n) router** — Linear scan over all routes per request. A `memcmp` scan over 20-50 routes costs hundreds of nanoseconds, invisible next to network I/O syscalls. Even at hundreds of routes the overhead is trivial. A trie or radix tree would add complexity to param extraction and middleware matching for no measurable gain in Keel's target workload.
- **O(n) timeout sweep** — Iterates all connection slots once per event loop tick. At `max_connections` = 256 (default), this is a tight loop over a contiguous array — well within L1 cache. A deadline heap or timer wheel would add allocation and pointer chasing for no measurable improvement.

---

## Future

### UDP / DNS follow-ups

Follow-ups to the UDP datagram arc (`KlUdp` + `KlUdpServer` + `dns_resolver`,
2026-07). The active hardening + default-wiring work (resolver anti-spoofing and
opt-out built-in DNS) is specced separately in `docs/dns_resolver_hardening.md`;
the items below are the remaining gaps, roughly in priority order.

- ~~**`IP_PKTINFO` / `IPV6_RECVPKTINFO` — source address on wildcard binds**~~ **(done, 2026-07-18)** — `KlUdpConfig.recv_pktinfo` captures each datagram's local address (delivered to `KlUdpRecvFn`); `kl_udp_send_to_from` pins the reply source; `KlUdpServer` auto-enables it on wildcard binds and replies from the hit address. See `docs/udp_pktinfo_design.md`.
- ~~**`SO_RCVBUF` / `SO_SNDBUF` sizing knobs**~~ **(done, 2026-07-18)** — `KlUdpConfig` / `KlUdpServerConfig` gained `so_rcvbuf` / `so_sndbuf` (bytes; 0 = OS default) to size the kernel socket buffers and bound kernel-side drops under load.
- ~~**DNS parity with `getaddrinfo`**~~ **(done, 2026-07-19)** — `/etc/hosts` lookup, `resolv.conf` `search`/`ndots` expansion, multiple-nameserver failover, EDNS0, concurrent dual-family A+AAAA resolution with a resolution-delay cap returning a family-interleaved address list, and client-side Happy Eyeballs (RFC 8305) connect racing over that list with a configurable Connection Attempt Delay + overall request deadline. See `docs/dns_parity_design.md`.
- **Multicast / broadcast** *(feature)* — `IP_ADD_MEMBERSHIP` (join groups), `IP_MULTICAST_TTL`/`IP_MULTICAST_LOOP`, `SO_BROADCAST`. Unlocks mDNS, SSDP/UPnP, and LAN service discovery.
- **Batching + QoS** *(throughput)* — `recvmmsg`/`sendmmsg` (one syscall per many datagrams), UDP GSO/GRO segmentation offload, and ECN/TOS/DSCP marking. Matters for high-PPS services and QUIC.
- **DNS extras** *(deferred)* — TCP fallback on the truncation (`TC`) bit, DNSSEC validation, DoT/DoH transports (DoH rides the existing HTTPS client), and DNS cookies (RFC 7873, a stronger off-path anti-spoof than 0x20).

### Research / Long-Term

- **QUIC / HTTP/3** — The UDP event model now exists (`KlUdp`); remaining work is a QUIC library (quiche, ngtcp2), `IP_PKTINFO` source-address handling (above), and the connection model shift from persistent TCP streams to multiplexed UDP datagrams with connection migration.
- **Zero-copy receive (MSG_ZEROCOPY)** — Linux `MSG_ZEROCOPY` for `send(2)` avoids copying response data from userspace to kernel. Marginal benefit for small responses but significant for large file transfers.
- **eBPF request steering** — Use eBPF `SO_REUSEPORT` programs to steer connections to specific threads/cores based on request characteristics.
- **WebSocket compression (RFC 7692)** — `permessage-deflate` compression negotiation and per-message compression. Rarely needed in practice due to CPU overhead vs. bandwidth savings.

### Considered and Rejected

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
