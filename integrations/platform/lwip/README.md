# Keel on lwIP (socket + event providers)

A bring-your-own **lwIP platform** for Keel: a matched pair of a `KlSocketProvider`
(over the lwIP BSD socket API) and a `KlEventProvider` (over `lwip_poll`), so Keel
runs on an lwIP TCP/IP stack with no kernel sockets. lwIP is **not vendored** —
build against your own lwIP (`LWIP_DIR`) + `lwipopts.h`.

- `socket_lwip.c` — `KlSocketProvider` mapping `KlSocketOps` → `lwip_*`, **and** the
  datagram data-plane (`KlDatagramOps` — `lwip_sendto`/`lwip_recvfrom`) folded onto
  the same provider, so `KlDatagram` (hence `udp_server` and the built-in async DNS
  resolver) runs on lwIP with no separate link artifact. One runtime provider owns
  both stream + datagram I/O (axis-audit A2), using **only public Keel headers**.
  Per-datagram only (lwIP has no recvmmsg/GSO/GRO/pktinfo).
- `event_lwip.c` — `KlEventProvider` over `lwip_poll` (the runtime event-backend
  seam; its `native_provider()` returns the lwIP socket provider, so installing
  `event_provider` auto-wires the matched `sockets`).
- `resolve_sync_lwip.c` — blocking name resolution over `lwip_getaddrinfo` (the
  client-axis seam; overrides the stock host `kl_resolve_sync` at link time).
- `platform_wakeup_lwip.c` — self-connected lwIP UDP wakeup (responsive
  `kl_http_server_stop`; overrides the generic `src/platform_wakeup_*` seam).
- `keel_lwip.h` — `kl_socket_provider_lwip()` + `kl_event_provider_lwip()`.
- `lwipopts.h` — a **production-oriented baseline** config (Keel provider
  requirements + security hardening + a documented sizing block to TUNE per
  deployment). It is what the loopback + HTTPS tests run against, so the
  provider-critical settings are CI-exercised. Copy + tune for your target, and
  replace the placeholder `LWIP_RAND` with a CSPRNG (see the header's warning).

## Build-gate (what CI runs)

```sh
make check LWIP_DIR=/path/to/lwip     # compile the providers against your lwIP
```

A clean compile **is** the validation: it proves Keel's public `KlSocketProvider`
+ `KlEventProvider` API is sufficient to author an lwIP platform using **only**
public Keel headers — no internal Keel or host-POSIX types.

## Pure runtime provider (finding dissolved)

lwIP now runs against a **stock `libkeel.a`** — no `-DKEEL_PLATFORM_LWIP` library
build. It is a pure drop-in like the mbedTLS/nghttp2 vtable integrations.

An earlier pass found a real boundary: `struct sockaddr`'s layout is compile-time
(lwIP has `sin_len`, Linux doesn't), so a host-built libkeel filled a host-layout
`sockaddr` that `lwip_bind` misread (`bind: EIO`), and `getaddrinfo` was baked in.
The **KlSockAddr address-ABI neutralization** (`docs/archive/designs/keel_sockaddr_design.md`)
dissolved it:

- Core speaks the Keel-owned, fixed-layout **`KlSockAddr`** everywhere; a platform
  `struct sockaddr` exists only inside socket providers. `socket_lwip.c` marshals
  `KlSockAddr` ↔ lwIP `sockaddr` at the boundary, so **no host-layout sockaddr
  ever reaches lwIP**.
- Server bind parses the numeric address with the pure `kl_sockaddr_parse` (no
  `getaddrinfo`); the clients' blocking name resolution is confined to a
  platform-swappable `resolve_sync` TU.

So **both axes are runtime-injectable**: the event backend (`KlEventProvider`) and
the socket/address provider (`KlSocketProvider` + `KlSockAddr`). No library
recompile.

`tests/lwip_loopback_test.c` is the proof — the server, client, and datagram axes on
lwIP, linked against a **stock** `libkeel`:
- a raw lwIP client → the Keel **server** (`200 OK`),
- a Keel async **client** on the lwIP providers → the same server (`200`), with
  name resolution via `resolve_sync_lwip.c` (`lwip_getaddrinfo`, linked ahead of
  the stock lib so it overrides the host `kl_resolve_sync`), and
- a Keel **`KlDatagram` echo** on the lwIP providers, bounced by a raw lwIP UDP client
  (the datagram ops on `socket_lwip.c`).

```sh
make -C ../../..                    # stock libkeel.a (any backend)
make loopback LWIP_DIR=/path/to/lwip
# -> keel: listening on 127.0.0.1:8080
#    lwIP loopback: raw client -> Keel server replied 200 OK (correct)
#    lwIP loopback: Keel client on lwIP got 200 (correct)
#    lwIP loopback: Keel UDP echo on lwIP round-tripped (correct)
```

A third phase runs a Keel `KlDatagram` echo on the lwIP providers (the datagram ops
folded onto `socket_lwip.c`), exercised by a raw lwIP UDP client — proving the
datagram axis end to end.

Run in CI by the **Integration (lwIP)** job (clones lwIP + lwip-contrib, stock
libkeel, `make loopback`), so the payoff is regression-protected.

**Responsive stop** is wired: `platform_wakeup_lwip.c` provides a self-connected
lwIP UDP wakeup (overriding the generic `src/platform_wakeup_*` seam at link time),
so `kl_http_server_stop` wakes `lwip_poll` immediately rather than on the next tick.

**TLS over lwIP** needs **no lwIP-specific TLS code**: the mbedTLS integration's
socket-BIO can be routed through a `KlSocketProvider`
(`kl_tls_mbedtls_ctx_set_socket_provider(ctx, kl_socket_provider_lwip())`), so a
genuine TLS handshake + HTTPS request runs over lwIP with the *existing*
`socket_lwip.c`. The loopback's optional Phase 4 proves it end to end:

```sh
make loopback-tls LWIP_DIR=/path/to/lwip MBEDTLS_DIR=/path/to/mbedtls
# -> lwIP loopback: Keel HTTPS (mbedTLS) on lwIP handshake + roundtrip OK (correct)
```

**Complete on lwIP:** server + client + UDP + TLS all run on a stock `libkeel.a`.
`lwipopts.h` is a production-oriented baseline (tune the sizing block + supply a
CSPRNG for `LWIP_RAND` per deployment).

## Tested versions

| lwIP | Status |
|------|--------|
| STABLE-2.2.0 | **Loopback verified: server + client + UDP + HTTPS** (200 OK + UDP echo + mbedTLS handshake on stock libkeel) + build-gate |
| 2.1.x | Expected to work (same `lwip_poll` + BSD socket API) |

## Scope

Two lwIP integrations ship:

1. **Readiness** (the sockets layer via `lwip_poll`) — `kl_socket_provider_lwip()` +
   `kl_event_provider_lwip()`, runtime-injected into a **stock** `libkeel.a` (`NO_SYS=0`).
   Server + client + UDP + TLS, verified on loopback (see above).
2. **Completion — the raw `tcp_*` callback API** (`NO_SYS=1`, "Phase 9", COMPLETE): a pure
   **runtime provider** (`kl_event_provider_lwip_raw()` + `kl_socket_provider_lwip_raw()`)
   injected into a **stock** `libkeel.a` — the always-linked completion driver + dispatch reach
   it via `loop->ops->completion` (`BACKEND=lwipraw` is retired). KEEL's event loop *is* the lwIP
   mainloop (`sys_check_timeouts()` + `netif_poll()`; raw `tcp_*` callbacks feed the completion
   driver). A raw-backed `KlHttpServer` serves HTTP over the loopback netif — accept/recv/send,
   backpressure, file responses, and full close/cancel/idle-timeout lifetime — all in-process (no
   tap, no root), CI-gated (`make -C integrations/platform/lwip loopback-raw`) and ASan+UBSan+LSan-clean.
   The NO_SYS=1 raw lwIP archive (`liblwip_raw.a`, built from `lwipopts_raw.h`) is the shared
   foundation these tests run on. Notably this needed **zero** changes to
   `completion_driver.c` or any `src/` — a third completion backend (beyond io_uring/IOCP) on the
   model-blind completion axis. The raw backend now also does the full **client** axis (plaintext
   + Happy-Eyeballs + DNS + HTTPS) and **UDP** — **IPv4-only** — see the capability matrix below.

See `docs/archive/phases/phase9_lwip_raw_design.md` for the full design + staged record (P9-1..P9-5),
`docs/archive/phases/phase10_lwip_raw_client_design.md` for the client axis (LC-0..LC-5), and
`docs/archive/designs/lwip_platform_design.md` for the platform-port shape.

## Raw completion backend — capabilities, limits, and memory

The raw (`NO_SYS=1`, `tcp_*` completion) backend is a deliberately narrow, embedded-friendly
**server + client** (IPv4-only, in-process over the loopback netif). What it does and does not
support:

| Capability | Status | Notes |
|------------|--------|-------|
| IPv4 TCP **server** (`KlHttpServer`) | **Supported** | accept/recv/send over the loopback netif |
| IPv4 TCP **client** (`KlHttpClient`) | **Supported** (LC-1/2) | outbound connect via the **completion** connect primitive (`kl_comp_post_connect` → `tcp_connect`) + Happy-Eyeballs address racing; send/recv on an emulated readiness watcher |
| HTTP/1.1 incl. keep-alive | **Supported** | rides `KlHttpServer` / `KlHttpClient` |
| **HTTPS** (client + server) | **Supported** (LC-4) | client over the mbedTLS socket-BIO routed through `kl_socket_provider_lwip_raw()`; server over the generic memory-BIO completion-TLS leg. Buffered HTTP/1.1 over TLS (no ALPN-h2, no TLS file/stream body). BYO mbedTLS |
| **UDP** / `udp_server` | **Supported** (LC-3a) | provider exposes datagram ops (`.dgram != NULL`); `kl_datagram_socket_init` runs `KlDatagram` over the raw completion loop |
| **DNS** | **Supported** (LC-3) | KEEL's built-in async resolver (`src/protocols/dns/dns_resolver.c`) over `KlDatagram`-on-raw — one DNS path, no lwIP `dns_gethostbyname` |
| Buffered / streaming / file responses | **Supported** | **unbounded** response size; bounded transmit memory |
| Request bodies | **Supported** | bounded per-conn receive flow-control (`ERR_MEM` backpressure) |
| Router, middleware, CORS, SSE, body readers, compression | **Supported** | the server-path modules that ride `KlHttpServer` |
| Multiple **sequential** event contexts | **Supported** | create → destroy → create |
| The **synchronous** socket-provider `connect` op | **Unsupported by design** | returns `-1` / `ENOTSUP` — a blocking connect is nonsensical on `NO_SYS=1`; the client connects via the **completion** primitive above |
| **IPv6** | **Unsupported (fails early)** | `bind` rejects a non-IPv4 address (the loopif is IPv4) |
| A **second simultaneous** raw context | **Rejected at create** | `NO_SYS=1` lwIP core is process-global |
| WebSocket / HTTP-2 (server **and** client), ALPN-h2 over raw TLS | **Not specifically demonstrated** | ride the generic completion driver / client machine (branches exist for any completion backend) but are not lwip-raw-specifically tested — no claim of tested support |

For **IPv6** or the BSD-socket lwIP model, use the readiness integration above
(`keel_lwip.h`: `kl_socket_provider_lwip()` + `kl_event_provider_lwip()`).

### Max connections + per-connection memory

`conn_cap = KlHttpServerConfig.max_connections` is the **one authoritative capacity** — the same value
sizes the `KlHttpConn` pool and the backend's raw slot table (`kl_lwr_ctx_ensure_cap` at prime).
Over-capacity accepts are rejected (`tcp_abort`), never queued.

Per-connection backend memory is fixed and independent of response/request size:

- **`KL_LWR_RX_MAX` = 64 KiB** — the per-conn receive bound (retained un-delivered pbuf bytes).
- **`KL_LWR_TX_WIN` = 32 KiB + `KL_LWR_TX_HEAD` = 2 KiB = 34 KiB** — the fixed per-conn transmit
  window (+ head-snapshot buffer) the send path pumps through.

So per connection ≈ **64 KiB + 34 KiB ≈ 98 KiB**, and per-backend ≈ **`conn_cap × 98 KiB`** plus
the ctx/slot array, **plus lwIP's own pools** (`lwipopts_raw.h`: `MEM_SIZE`, `PBUF_POOL_SIZE`,
`MEMP_NUM_TCP_PCB`). Response/file sizes are **unbounded by design** (bounded transmit memory);
request receive is bounded (the `ERR_MEM` backpressure gate at `KL_LWR_RX_MAX`).

### Runtime constraints

- **`NO_SYS=1`, single-thread** — KEEL's loop *is* the lwIP mainloop; there is **no separate lwIP
  thread**. The thread-pool `done_fn` still runs on the loop thread, as always.
- **One active raw context per process** (a second simultaneous context is rejected at create).
