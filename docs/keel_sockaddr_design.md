# KlSockAddr — a platform-neutral address type (design groundwork)

**Status:** design / not yet implemented
**Date:** 2026-08-02
**Motivates / dissolves:** the compile-time socket-ABI finding in
`integrations/lwip/README.md` ("The runtime boundary")

## 1. Problem

Keel already neutralized the *native handle*: `KlSocketHandle` is an `intptr_t`
(`include/keel/handle.h`) precisely because "a socket is not always a Unix `int`
fd." But it left the *address* as a raw platform type — `struct sockaddr` /
`struct sockaddr_storage` — used as the internal currency everywhere:

- The socket vtable **already** carries addresses across its seam, but as the
  platform type: `connect`/`bind`/`accept`/`get_local_addr` take
  `const struct sockaddr *` (`include/keel/socket.h:79-85`).
- Public API exposes the platform layout directly: `KlResolveResult.addrs[]`
  (`sockaddr_storage`), the udp handler/config (`sockaddr` + `sockaddr_storage`
  scratch), `kl_proxy_protocol_parse`/`kl_cidr_match`.
- Protocol TUs construct platform sockaddrs inline — `websocket_client.c`,
  `h2_client.c` build `sockaddr_un`; `dns_resolver.c` builds `sockaddr_in`/`in6`
  from wire bytes; `proxy_protocol.c` builds one from PROXY-header bytes.

`struct sockaddr`'s **layout is compile-time** and differs per platform (lwIP /
BSD carry `sin_len`; Linux does not). Because Keel uses it as core currency, a
host-built `libkeel.a` emits host-layout sockaddrs that a foreign stack's
`bind`/`connect` misreads — so lwIP needs a `-DKEEL_PLATFORM_LWIP` recompile of
the whole library, making it a *hybrid* (runtime event provider + compile-time
socket ABI) rather than a pure runtime provider like the mbedTLS/nghttp2 vtables.

**None of the field access in core is intrinsic.** Every site is either (a) a
pass-through to a syscall the provider already owns, or (b) a marshal between raw
wire bytes and a sockaddr. Neither needs the platform struct.

## 2. Design

Introduce a Keel-owned canonical address with a **fixed layout on every
platform**, symmetric with `KlSocketHandle`. The socket provider becomes the
*only* code that converts between `KlSockAddr` and a platform `struct sockaddr`.

### 2.1 The type (`include/keel/sockaddr.h`)

```c
/* Keel-owned address families — provider maps to/from the platform AF_* values,
 * so core never depends on system AF_* numbering. */
typedef enum {
    KL_AF_UNSPEC = 0,
    KL_AF_INET,
    KL_AF_INET6,
    KL_AF_UNIX,
} KlAddrFamily;

/* Canonical address. Fixed Keel-defined layout — identical on Linux, macOS,
 * Windows, lwIP, UEFI. Core reads .family/.port directly; only providers touch
 * platform sockaddr. ~120 B — smaller than the sockaddr_storage (128 B) it
 * replaces in KlResolveResult. */
typedef struct {
    uint16_t family;      /* KlAddrFamily */
    uint16_t port;        /* HOST byte order (0 for AF_UNIX) */
    uint32_t scope_id;    /* IPv6 sin6_scope_id (link-local); 0 otherwise */
    uint8_t  addr_len;    /* 4 (v4) | 16 (v6) | strlen(path) (unix) */
    union {
        uint8_t ip[16];   /* NETWORK byte order — as on the wire / inet_pton */
        char    path[108];/* AF_UNIX, NUL-terminated within addr_len+1 */
    } u;
} KlSockAddr;
```

**Byte-order convention (fixed, documented once):** `port` is host order (so
core can compare/log without `ntohs`); `u.ip` is network order (so it drops
straight in from `inet_pton`, DNS A/AAAA record bytes, and PROXY-header bytes —
the three construct-from-wire sites need no conversion).

**Family constants are Keel-owned** (`KL_AF_*`), not system `AF_*`, so the public
type has zero dependency on system headers and a provider is free to map them to
its own numbering (lwIP happens to match POSIX; UEFI need not).

### 2.2 Helper module (`src/sockaddr.c`, decl in `include/keel/sockaddr.h`)

Pure, platform-neutral operations on `KlSockAddr` — **no** system socket headers:

```c
/* Construct from raw wire bytes (dns_resolver, proxy_protocol). */
int  kl_sockaddr_from_ipv4(KlSockAddr *out, const uint8_t ip4[4], uint16_t port);
int  kl_sockaddr_from_ipv6(KlSockAddr *out, const uint8_t ip6[16], uint16_t port, uint32_t scope);
int  kl_sockaddr_from_unix(KlSockAddr *out, const char *path);
/* Numeric-literal parse (no DNS): "127.0.0.1", "::1", "[::1]". */
int  kl_sockaddr_parse(KlSockAddr *out, const char *host, uint16_t port);
/* Present for logging / user code (replaces inet_ntop callers). */
int  kl_sockaddr_format(const KlSockAddr *a, char *buf, size_t n); /* "ip:port" */
/* Accessors (core reads these instead of struct fields directly). */
KlAddrFamily kl_sockaddr_family(const KlSockAddr *a);
uint16_t     kl_sockaddr_port(const KlSockAddr *a);
void         kl_sockaddr_set_port(KlSockAddr *a, uint16_t port);
int          kl_sockaddr_equal(const KlSockAddr *a, const KlSockAddr *b); /* incl. port? see §7 */
int          kl_sockaddr_is_loopback(const KlSockAddr *a);
```

### 2.3 Native marshalling (provider-private, `src/sockaddr_native.h`)

The **only** place `KlSockAddr` meets `struct sockaddr`. Included by socket
providers (`socket_posix.c`, `socket_winsock.c`) and by the overlapped providers
in the completion backends — never by core. This header MAY include system
socket headers (it is a platform TU helper, not core):

```c
/* KlSockAddr -> platform sockaddr (for bind/connect/sendto). Returns socklen. */
socklen_t kl_sockaddr_to_native(const KlSockAddr *a, struct sockaddr_storage *out);
/* platform sockaddr -> KlSockAddr (for accept/recvfrom/getsockname). */
int       kl_sockaddr_from_native(KlSockAddr *out, const struct sockaddr *sa, socklen_t len);
```

lwIP's provider (`integrations/lwip/socket_lwip.c`) gets its **own** copy of these
two functions compiled against lwIP headers — that is where, and the only where,
the `sin_len` / layout difference lives. Core is layout-agnostic.

### 2.4 Vtable currency change (`include/keel/socket.h`)

```c
/* before */ int (*connect)(void *ctx, KlSocketHandle fd, const struct sockaddr *addr, socklen_t len);
/* after  */ int (*connect)(void *ctx, KlSocketHandle fd, const KlSockAddr *addr);
```

Same for `bind`, `accept` (out-param `KlSockAddr *peer`), `get_local_addr`, and
the udp datagram ops. The provider calls `kl_sockaddr_to_native` internally. The
seam already exists; we are only changing what crosses it.

## 3. Public API changes (breaking — accepted)

| Header | Before | After |
|---|---|---|
| `resolver.h` | `struct sockaddr_storage addrs[]` | `KlSockAddr addrs[]` |
| `udp_server.h` | `const struct sockaddr *src, socklen_t` | `const KlSockAddr *src` |
| `udp_server.h` | `struct sockaddr_storage local` | `KlSockAddr local` |
| `udp.h` | `sockaddr` in recv/send/connect/reply ops + scratch | `KlSockAddr` |
| `proxy_protocol.h` | `struct sockaddr_storage *peer` / `const struct sockaddr *sa` | `KlSockAddr *peer` / `const KlSockAddr *sa` |
| `server.h` | (unchanged: `bind_addr`/`unix_socket_path` stay strings) | — |
| `client.h` | (unchanged public surface; internal connect uses KlSockAddr) | — |

Consumers that today `memcpy`/cast to `sockaddr_in` and call `inet_ntop` switch
to `kl_sockaddr_format` / `.family` / `.port`. A `kl_sockaddr_to_native` escape
hatch remains for anyone who genuinely needs a platform sockaddr.

## 4. Core call-site inventory (from grep, to migrate)

Construct-from-wire (become `kl_sockaddr_from_*`, lose platform headers):
- `src/dns_resolver.c` (58 refs) — A/AAAA record bytes → `KlSockAddr`.
- `src/proxy_protocol.c` (25) — PROXY v1/v2 addr bytes → `KlSockAddr`.

Pass-through (become vtable calls with `KlSockAddr`, no field access):
- `src/server.c` (22) — bind local addr; `getaddrinfo` numeric → `kl_sockaddr_parse`.
- `src/client.c` (25), `src/h2_client.c` (5), `src/websocket_client.c` (4) —
  connect; the inline `sockaddr_un` builds become `kl_sockaddr_from_unix`.
- `src/connection.c` (2), `src/response.c` (1).

Datagram addressing:
- `src/udp.c` (24), `src/udp_server.c` (4), `src/udp_internal.h` (7),
  `src/udp_cmsg*.h`, `src/udp_io_*.c` — src/local/dest addrs → `KlSockAddr`
  (the cmsg/pktinfo layer stays platform, marshals at the boundary).

Provider / completion (keep platform sockaddr internally, marshal at boundary):
- `src/socket_posix.c`, `src/socket_winsock.c` — implement the two native fns.
- `src/event_iouring.c`, `src/event_iocp.c`, `src/event_pollcomp.c` —
  accept/connect deliver `KlSockAddr` up (marshal `op->peer` on completion).

## 5. getaddrinfo strategy

`getaddrinfo` is compile-time on lwIP too, so confine it:
- **Numeric literals** (bind addr, `[::1]`, dotted-quad) → `kl_sockaddr_parse`
  (pure, no DNS) inside the provider / helper — no `getaddrinfo`.
- **Name resolution** → already pluggable via `KlResolver`; the built-in
  `dns_resolver` returns `KlSockAddr` directly. The blocking-`getaddrinfo`
  fallback path (if any remains) stays in a POSIX-only provider TU.

## 6. Migration sequencing (each phase compiles + `make test` green)

- **A. Type + helpers.** Add `sockaddr.h` + `sockaddr.c` + `sockaddr_native.h`
  and unit tests (`tests/test_sockaddr.c`). No call-site changes. Behavior-inert.
- **B. Socket vtable currency.** Flip `socket.h` ops to `KlSockAddr`; implement
  the native marshalling in `socket_posix.c`/`socket_winsock.c`; adapt
  `server.c`/`client.c` internal callers. Readiness backends green.
- **C. Resolver.** `KlResolveResult` → `KlSockAddr`; `dns_resolver.c`
  construct-from-wire; drop its `<arpa/inet.h>` etc. (fuzz_dns still green).
- **D. Datagram public API (udp).** `udp.h`/`udp_server.h` callbacks + send/
  connect/reply signatures → `KlSockAddr`; the marshalling concentrates in
  `udp.c` at the io-seam boundary (the intricate `udp_io_posix.c`/`_win.c` mmsg/
  cmsg engines + the completion backends keep sockaddr internally). `dns_resolver`
  updated as a udp consumer. udp suites + smoke-pollcomp green.
  - **Revised (coupling found):** `proxy_protocol` moved out of D into the accept
    phase — `kl_proxy_parse` writes `KlConn.peer_addr`, which is filled by BOTH
    the readiness accept (`server.c`) AND the completion accept
    (`event_iouring`/`event_iocp`), so proxy/accept/completion-accept migrate
    together (below) rather than splitting readiness from completion.
- **E. Accept-peer + proxy + completion delivery.** Flip the `accept` vtable op +
  `KlConn.peer_addr` + `kl_request_peer_*` + `proxy_protocol`/`kl_cidr_match` to
  `KlSockAddr`, and have `event_iouring`/`event_iocp`/`event_pollcomp` marshal the
  accepted peer to `KlSockAddr` at completion delivery. Container io_uring +
  pollcomp-asan + fuzz_proxy green.
- **F. Purge + gate.** Remove the inline `sockaddr_un` from `websocket_client.c`/
  `h2_client.c`; add a grep-gate: no core protocol TU includes a platform socket
  header or names `struct sockaddr` (mechanical audit, mirrors axis-audit Goal 4).
- **G. lwIP payoff.** `socket_lwip.c` implements the two native fns against lwIP
  headers; delete the `KEEL_PLATFORM_LWIP` addressing requirement; the loopback
  test runs against a **stock** `libkeel.a`. Update `integrations/lwip/README.md`
  (finding dissolved) — lwIP becomes a pure runtime provider.

Phases A–B are the foundation; C–F are independent and parallelizable; G closes
the loop that started this.

## 7. Open decisions (flagged — default chosen, veto welcome)

1. **AF_UNIX in the union (default: yes).** Keeps one canonical type and purifies
   `ws_client`/`h2_client`; costs nothing (union ≤ current `sockaddr_storage`).
   Alternative: INET-only `KlSockAddr` + a separate unix path — rejected as it
   reintroduces two address representations.
2. **`kl_sockaddr_equal` port sensitivity (default: compare family+addr+port).**
   `resolver_cache` / Happy-Eyeballs dedup may want addr-only; provide both
   `_equal` (full) and `_equal_addr` (host only) if a second consumer appears.
3. **Keel-owned `KL_AF_*` vs system `AF_*` (default: Keel-owned).** Zero
   system-header dependency in the public type; provider maps. Trivial mapping
   tables on every platform.

## 8. Invariants preserved

- **No `#ifdef` in core.** All layout knowledge lives in provider TUs +
  `sockaddr_native.h` (a platform helper), never in `sockaddr.c` or protocol code.
- **Exactly one representation above the provider** (`KlSockAddr`); platform
  `sockaddr` exists only inside providers.
- **W^X, static linking, no dynamic loading** — unchanged; this is a type + a
  pure helper + provider marshalling, no new runtime machinery.
- **Source + static-relink compatibility, not ABI** — consumers recompile; the
  breaking public-type changes are explicitly accepted.
- **Symmetry** with the existing `KlSocketHandle` neutralization — this finishes
  the same decision for the address.
