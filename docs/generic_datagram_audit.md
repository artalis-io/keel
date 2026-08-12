# Generic Datagram Audit (Phase 1)

**Date:** 2026-08-10
**Status:** **APPROVED as the implementation roadmap (2026-08-10).** Phase A may begin. Sibling
of `docs/generic_transport_audit.md` (the stream audit). Feeds the now-frozen Tier-1
`docs/datagram_contract.md`.

> Scope: map the existing datagram stack (`KlUdp`, `KlUdpServer`, the built-in DNS
> resolver) across every event backend and socket provider; identify platform/protocol
> leakage and ownership differences; and locate exactly what a datagram-transport
> consolidation must build. The goal is a neutral **`KlDatagram`** transport object — a
> *sibling* of `KlStream`, **not** an extension of it — with UDP as its first provider,
> DNS as a consumer, and QUIC explicitly out of the baseline.

---

## 1. Executive summary

The datagram stack is **further along than the stream stack was at its Phase-1 audit** in one
dimension and **behind** in another:

- **Ahead — the data-plane provider seam already exists.** `KlDatagramOps`
  (`include/keel/datagram.h:66-112`) is a provider vtable hung off `KlSocketProvider.dgram`
  (`include/keel/socket.h:146`), gated by `KL_SOCK_CAP_DATAGRAM` (`socket.h:156`). All raw
  syscalls, cmsg parsing, and `struct sockaddr` live in provider TUs; `src/udp.c` is the
  portable machine (queue walk, delivery, interest, backpressure) over that seam. This is the
  datagram analogue of the socket axis and is documented in
  `docs/keel_datagram_ops_design.md`. **KEEP AS-IS.**

- **Behind — there is no neutral datagram *object*.** Where the stream stack has an opaque
  `KlStream` that the completion backends target (`KL_COMP_READ`/`KL_COMP_WRITE` →
  `KlStream*`; `post_recv(KlStream*, buf, cap)`), the datagram completion path **hard-types
  `KlUdp*`** (`KL_COMP_UDP_RECV`/`KL_COMP_UDP_SEND` → `KlUdp*`; `post_udp_recv(struct KlUdp*)`
  / `post_udp_send(struct KlUdp*, …)` — `src/completion.h:36,64,128-130`, entrypoints in
  `src/io_engine.h:89,95`). Every completion backend reaches into concrete `KlUdp` fields
  (`udp->recv_buf`, `udp->recv_buf_size`, `udp->pktinfo`, `udp->recv_gro`). This is the
  central thing the consolidation must fix: **introduce a `KlDatagram` handle so backends
  target an opaque datagram object, exactly as `KlStream` decoupled I/O from `KlConn`.**

- **`KlUdp` is simultaneously the object and the machine.** It is embedded *by value* in both
  consumers (`KlUdpServer.udp` `udp_server.h:70`; `dns_resolver`'s `KlUdp sock`
  `dns_resolver.c:124`). The consolidation carves the neutral transport-object + lifecycle
  contract out of `KlUdp`, leaving `KlUdp` as the first *provider-config surface* over
  `KlDatagram`.

Four Tier-1 gaps stand between today and the contract:

1. **Send status is lossy** — every send returns `0`/`-1`; would-block (→ silently queued),
   too-large, alloc-fail, overflow, and IO-error all collapse into `-1` + `last_error`
   (`udp.c:79-150`). No `WOULD_BLOCK` vs `TOO_LARGE` distinction reaches the caller.
2. **Queue capacity is byte-budgeted, not packet slots** (`udp.c:79`,
   `max_send_queue` default 256 KiB) — a flood of tiny datagrams allocates unboundedly many
   nodes under one byte budget.
3. **Truncation is silent-truncate-and-deliver** — `MSG_TRUNC` is detected by the provider
   (`socket_dgram_posix.c:194`) and counted (`kl_udp_truncated`), but the *partial* payload is
   still delivered to the callback with the smaller length; the caller cannot tell. **IOCP does
   not detect truncation at all** (`event_iocp.c:805-836`).
4. **No confirmed detachment / cancel-once.** `kl_udp_free` discards queued datagrams and
   tears down (`udp.c:448-484`); on completion backends without a dequeue-before-free
   discipline (io_uring, pollcomp) a recv op referencing `udp->recv_buf` can outlive the free
   → UAF window (IOCP compensates via `iocp_quiesce_port_for_close`, `event_iocp.c:942-976`).

None of these require reworking the provider seam — they are the *object + lifecycle* layer,
which is precisely the Phase A/B carve that the stream stack already proved.

---

## 2. What already exists (the reusable substrate)

### 2.1 Datagram provider seam — `KlDatagramOps` (`include/keel/datagram.h`) — KEEP AS-IS

The data plane is a provider vtable, address-neutral and `KlUdp`-free. Every op takes
`(void *ctx, KlSocketHandle fd, …)` and speaks `KlSockAddr`:

| Op | `datagram.h` | Role |
|---|---|---|
| `send` | :69 | one datagram out (machine decides queue-on-EAGAIN vs drop) |
| `recv` | :74 | one datagram in; fills `*src` + `KlDgramRxMeta` |
| `send_gso` | :79 | one-syscall UDP GSO; optional (may return `EOPNOTSUPP`) |
| `configure` | :87 | init-time socket-option fold; returns accepted `KL_DGRAM_RX_*` mask |
| `set_tos` | :90 | dynamic outgoing TOS/DSCP |
| `mcast_membership` | :92 | join/leave any-source group |
| `rx_batch_new`/`tx_batch_new`/`*_free` | :98-101 | provider-allocated, KlUdp-owned mmsg batch blocks |
| `recv_batch`/`send_batch` | :105,110 | one `recvmmsg`/`sendmmsg` |

Metadata: `KlDgramRxMeta{local,has_local,gro_seg,tos,truncated}` (`datagram.h:39-45`),
`KlDgramRxSlot` (:49-54), `KlDgramTxDesc` (:58-64). RX-capture flags
`KL_DGRAM_RX_PKTINFO|_GRO|_TOS` (:31-33). This seam is the datagram equivalent of
`KlSocketProvider` and needs no change for the consolidation.

### 2.2 The `KlUdp` machine — the de-facto datagram object (`src/udp.c`)

`udp.c` is nearly platform-clean: no `#ifdef __linux__`, no direct `recvmsg`/`sendmsg`/
`setsockopt`, no inline cmsg. It reaches the socket only through `kl_sock_*` and the
`KlDatagramOps` vtable. It holds all the machine state a `KlDatagram` will own:

- **send queue** — whole-datagram singly-linked FIFO, node `KlUdpDatagram{next,dest,src,tos,
  len,data[]}` single-alloc header+payload (`udp_internal.h:25-32`, `udp.c:90-103`);
- **recv** — single reusable `udp->recv_buf` borrowed to the callback (`udp.c:286`,
  `udp.h:60`), or provider-owned batch slots for `recvmmsg`;
- **interest reconciliation** — `kl_udp_update_interest` computes READ/WRITE want and
  reconciles via `kl_watcher_*` on readiness loops (`udp.c:45-72`);
- **two wiring paths** — readiness (`udp_on_ready`/`udp_recv_dgram`/`udp_flush_dgram`) vs
  completion (`kl_comp_post_udp_recv`/`send` + `kl_udp_comp_dispatch`, branch at
  `udp.c:127,513`).

Residual leakage in this "shared" TU (all minor, none are syscalls; see §5): raw `errno`
reads, bare `AF_*`/`SOCK_DGRAM` numeric constants, a leftover `IPV6_JOIN_GROUP` alias `#if`.

### 2.3 Backpressure — a byte-FIFO (exists; re-cast to packet slots)

`max_send_queue` is a **byte** cap enforced against payload bytes (`udp.c:79`), default 256 KiB
(`udp.c:363`); `q_bytes` counts payload only; caller buffers are **copied at enqueue**
(`udp.c:102-103`, safe to reuse immediately). `on_drain` fires only on the full empty→"had
data" transition (`udp.c:245-246`) — no high/low watermark. The contract re-casts this to a
fixed **packet-slot** budget (§Contract), but the enqueue/flush/one-in-flight machinery is
directly reusable.

### 2.4 Consumers — what they rely on

**`KlUdpServer`** (`udp_server.c`) owns a `KlUdp` by value, shares the `KlEventCtx` with the
TCP server, and depends on: one-packet-one-callback (`udp_server.h:32`); **source addr on recv
for the reply** (`udp_server.c:81`); **source-pinned reply** via captured pktinfo local addr +
`kl_udp_send_to_from` (`udp_server.c:80-82`, armed only on wildcard binds `:50`); unconnected
send. `data`/`src` are borrowed across the handler boundary; `local` is copied by value so a
reply from inside the handler is source-correct.

**Built-in DNS resolver** (`dns_resolver.c`) is the demanding consumer:

- **ONE unconnected `KlUdp` shared across all nameservers and all in-flight queries**
  (`dns_resolver.c:124,1447-1452`), family fixed to the first nameserver.
- **Many logical queries/transactions multiplexed over ONE continuously-armed receive path** —
  `r->inflight` list, 2 legs per request (A+AAAA), demuxed by **transaction id** (`dns_find_leg`
  `:365-374`), matched at `:991`. DNS does **not** post one transport receive per query; it keeps
  a single receive continuously armed and demultiplexes every response in `dns_on_recv`. This is
  the defining coupling: **many logical transactions over one shared unconnected socket, with no
  per-request transport handle** — so the datagram recv is one self-re-arming operation, not one
  op per query.
- **Source-address verification is the anti-spoof filter** — because the socket is
  unconnected, `dns_on_recv` drops anything not from a configured nameserver
  (`dns_ns_index` `:691-703`, called `:986`). *A datagram provider that hides `src` or assumes
  a connected peer breaks DNS.*
- **Truncation → TCP fallback (RFC 7766) does NOT use `KlUdp`.** The TC bit is a payload bit
  the resolver reads (`pkt[2] & 0x02` `:998`); the fallback uses a **separate persistent
  per-nameserver TCP stream** (`KlDnsTcp`, `:94-108`) over the raw socket seam + `kl_watcher_*`
  (`:733,749,827`). The **(fd, KlTls\*) helper — the DoT hook — is a byte-stream helper**
  (`dns_tcp_write`/`read` `:710-719`), entirely outside the datagram abstraction. It shares
  only the `KlEventCtx` + timers + socket seam.
- **What DNS needs from a datagram abstraction:** `src` on every recv; send-to-arbitrary-peer
  on an unconnected socket; many logical queries/transactions multiplexed over **one
  continuously-armed, self-re-arming receive** (the consumer demuxes by txid — the transport
  exposes a single receive, not one per query); cancel/teardown. It does **not** need pktinfo,
  multicast, batching, GSO/GRO, TOS, or
  a drain callback (it `(void)local`s the local addr, `:982`).

### 2.5 Address + handle neutrality — `KlSockAddr` (mostly there)

The neutral currency `KlSockAddr` (`sockaddr.h:47-56`, `KL_AF_*`) is already what
`KlDatagramOps` and the completion events speak (`completion.h:72-76`). The one gap: the
**public `KlUdp` API still uses `struct sockaddr`** (`udp_design.md:60-92`), and `udp.c`
carries platform `AF_*`/`SOCK_DGRAM` numbering via `udp->family`. The `KlDatagram` contract
adopts `KlSockAddr` for peer/local, matching the already-neutral vtable. `KlSocketHandle`
(`handle.h`, `intptr_t`, `KL_INVALID_SOCKET`, `kl_handle_valid()` — never `< 0`) is the handle
currency.

---

## 3. Per-backend datagram behavior matrix

How a UDP datagram recv/send actually works on each backend today. "Path": how the data plane
is driven. "In-flight": async op object + cancel/close model.

| Backend | Path | Recv model | Send model | In-flight / cancel | Caps (✅ / fallback / ❌) |
|---|---|---|---|---|---|
| **POSIX readiness** (epoll/kqueue/poll) | `KlDatagramOps` via generic watcher; backends have **zero** datagram code | level-triggered **drain loop** to EAGAIN (`udp.c:283-293`); borrow `recv_buf`; src via `msg_name`, local via `IP_PKTINFO` cmsg | sync seam; enqueue on EAGAIN, flush on WRITE-ready | none (no async op); close = `watcher_del` + free | pktinfo ✅, mcast ✅, bcast ✅, TOS ✅, mmsg ✅(Linux), GSO ✅(Linux), GRO ✅(Linux), src-pin ✅ |
| **Winsock readiness** (WSAPoll) | `KlDatagramOps` via generic watcher | `WSARecvMsg` (pktinfo) else `recvfrom` (`socket_dgram_win.c:177-239`) | `WSASendMsg`/`sendto` | none | pktinfo ✅, mcast ✅ (IPv4 by-index ❌), bcast ✅, TOS ✅, src-pin ✅, **mmsg ❌, GSO ❌, GRO ❌, REUSEPORT ❌** |
| **pollcomp** | completion `post_udp_recv/send`; control-plane = inherited POSIX dgram ops | one `PC_UDP_RECV` op; real `recvmsg` into borrowed `recv_buf`; src/local/gro/TRUNC filled | copy into `op->sendbuf`; real `sendto` on POLLOUT | per-op `aborted` flag; **no reap-before-free → borrowed recv_buf UAF if the object frees while a recv op is pending** | pktinfo/TOS/mcast/GRO ✅ (control plane); **no mmsg/GSO on data path** |
| **io_uring** | completion-native RECVMSG/SENDMSG SQEs (`event_iouring.c:626-681`) | one RECVMSG SQE; borrow `recv_buf` in iovec; src/local/gro/TRUNC on drain | SENDMSG SQE, **mandatory copy** into `op->sendbuf` | per-op on `st->ops`; `IORING_OP_CANCEL` (may miss on full SQ); **no reap-before-free → borrowed recv_buf UAF** | pktinfo ✅, GRO ✅, TRUNC ✅; TOS/mcast via control plane; **mmsg ❌, GSO ❌**; src-pin routed to **sync seam** |
| **IOCP** | completion-native WSARecvMsg/WSASendTo (`event_iocp.c:490-564`) | one overlapped op; borrow `recv_buf`; WSARecvMsg→local, else WSARecvFrom→src only; metadata only when `bytes>0` | overlapped WSASendTo, **copy** into `op->sendbuf`; releases original len | global op registry; teardown **dequeues every completion before free** (`:942-976`) → no UAF (lifetime-safe by construction) | pktinfo ✅, mcast/TOS via control plane, bcast ✅; **mmsg ❌, GSO ❌, GRO ❌; TRUNCATION ❌ (not surfaced)**; overlapped-send TOS routed to sync seam |
| **lwIP-raw** | completion-only `lwr_comp_post_udp_recv/send`; raw provider `.dgram = NULL` | lwIP `udp_recv` copies pbuf into a **16×2 KB bounded ring**, frees pbuf; one slot surfaced per drain (no UAF — payload copied). **⚠ The ring can accumulate >1 transport-owned packet, so it does NOT yet satisfy Tier-1 strict one-held-packet pause — needs a one-held-slot rework (⚙).** | copy into pbuf, `udp_sendto` (loopback), free pbuf | ring drops oldest on overflow; close detaches cb + memsets ring | IPv4/loopback only; **pktinfo ❌, mcast ❌, bcast ❌, TOS ❌, mmsg ❌, GSO ❌, GRO ❌**; connected-send rejected |
| **EFI_UDP4** | **none** — `g_provider.dgram = NULL`, completion `post_udp_* = NULL` | n/a — `KlUdp` does not run over EFI | n/a | bespoke one-shot resolver only (see below) | **none apply** |

**EFI note:** "U-5 DNS over EFI_UDP4" is a *bespoke synchronous one-shot* resolver
(`integrations/uefi/dns_uefi.c`, `kl_uefi_dns_resolve`) that opens `EFI_UDP4_PROTOCOL`, submits
one Tx + one Rx token, pumps to completion, parses, recycles — it does **not** touch `udp.c`,
`KlUdp`, `KlDatagramOps`, or the completion axis. Its `u5_pump_or_cancel` already models
"confirmed retirement or fail-closed" (issues `EFI_UDP4.Cancel`, and if retirement can't be
confirmed it **quarantines** further DNS until ExitBootServices). That token machine is the
seed of a future EFI `KlDatagram` provider, but today EFI is the largest build gap.

---

## 4. The completion datagram identity gap (the key "what to build")

The stream (TCP) completion path is provider-neutral; the datagram path is not.

| | Stream (neutral) | Datagram (concrete) |
|---|---|---|
| Event target | `KL_COMP_READ`/`_WRITE` → opaque `KlStream*` (`completion.h:35,64-66`) | `KL_COMP_UDP_RECV`/`_SEND` → **`KlUdp*`** (`completion.h:36,64`) |
| Ops | `post_recv(KlStream*, buf, cap)` / `post_send(KlStream*, …)` (`completion.h:119-120`) | `post_udp_recv(struct KlUdp*)` / `post_udp_send(struct KlUdp*, data, len, dest)` (`completion.h:128-130`) |
| Public entrypoints | `kl_comp_post_recv_raw` / `_send_raw` in **`completion.h`** (:184,203) | `kl_comp_post_udp_recv` / `_send` in **`io_engine.h`** (:89,95) — asymmetric |
| Payload/metadata | owned by the `KlStream` object | smeared inline on the generic `KlCompletionEvent` (`buf`,`peer`,`local`,`gro_seg`,`truncated` — `completion.h:72-78`) |
| Backend reach-in | recovers `KlConn` via `kl_conn_of_stream`; "a backend never derefs a KlConn" (`completion.h:36`) | every backend derefs `udp->recv_buf`, `udp->recv_buf_size`, `udp->pktinfo`, `udp->recv_gro` |

**Net:** a *working but concrete* datagram completion path exists; there is **no neutral
`KlDatagram` handle** analogous to `KlStream`. The consolidation introduces that handle so the
completion event targets an opaque datagram object (borrowing a caller-supplied recv buffer +
delivering `src`/`local`/meta), and the `post_udp_*` ops re-type to it — mirroring how
`KL_COMP_READ`/`_WRITE` decouple from `KlConn`.

---

## 5. Findings (severity-ranked)

- **[High] Completion `KlUdp*` identity leak** — §4. Blocks provider-neutral datagram
  ownership; every backend duplicates recv-into-`udp->recv_buf` + send-copy + `KL_COMP_UDP_*`
  synthesis and reaches into `KlUdp` internals. *Fix: neutral `KlDatagram` object + re-typed
  ops (Phase A carve).*
- **[High] No confirmed detachment on completion backends** — a recv op referencing
  `udp->recv_buf` can outlive `kl_udp_free` on io_uring & pollcomp (UAF). IOCP compensates by
  dequeue-before-free (`event_iocp.c:942-976`); io_uring/pollcomp do not.
  *Fix: a confirmed-detachment state machine (mirror `stream_close.c`) that guarantees no
  provider op references the object or its buffers after `on_close` — reap/cancel every posted
  op before release, or key ops on a backend-owned stable token that outlives them; prohibit
  free/reuse before detachment. A generation stamp CANNOT replace lifetime ownership (it can
  only reject duplicates while the object is still alive).*
- **[High] Lossy send status** — `WOULD_BLOCK`/`TOO_LARGE`/`ALLOC`/`OVERFLOW`/`IO` collapse to
  `-1`; EAGAIN is *invisible* (returns 0 after silent enqueue) (`udp.c:79-150`). *Fix:
  `KlDatagramSendStatus` enum (Contract §Send).*
- **[Medium] Byte-budget queue, no packet cap** — `udp.c:79`. A tiny-datagram flood allocates
  unbounded nodes. *Fix: packet-slot budget (Contract §Backpressure).*
- **[Medium] Silent truncation; IOCP blind to it** — partial payload delivered with smaller
  len, counter-only (`udp.c:252`); IOCP sets no truncation flag (`event_iocp.c:805-836`).
  *Fix: explicit `KL_DGRAM_TRUNCATED` flag on delivery; IOCP must parse `WSAMSG.dwFlags`.*
- **[Medium] Datagram path bypasses the `KlIoStatus` seam** — `udp.c` reads raw `errno`
  (`udp.c:147,225,238,270,288,687,691`) to decide queue-vs-drop, unlike the stream seam which
  already classifies via `KlIoStatus` (`socket.h:119-132`). The one real freestanding leak in
  the "shared" TU. *Fix: route datagram control flow through the status classifier.*
- **[Low] Readiness/completion asymmetries** — over-cap returns `KL_ERR_QUEUE_FULL` (readiness)
  vs `KL_ERR_IO` (completion) (`udp.c:81` vs `:129`); `dropped` counted on readiness not
  completion; `q_bytes` means "queued payload" (readiness) vs "outstanding overlapped bytes"
  (completion). *Fix: unify under the contract's status + counters.*
- **[Low] Platform `AF_*` numbering in `udp.c`** — `udp->family` is a platform int
  (`udp.h:122`, `udp.c:302-306` mixes `AF_INET` with `KL_AF_INET`). *Fix: carry `KlAddrFamily`.*
- **[Low] Duplicate cmsg parser** — provider-private copies in `socket_dgram_posix.c` +
  shared `udp_cmsg.c` for the POSIX completion backends (deliberate for link-override, drift
  risk). *Track, don't fix now.*

---

## 6. Backend gaps vs the Tier-1 contract

Tier-1 goals: serial receive operations (at most one completion recv posted; readiness interest
is an armed source, not an op); atomic whole-packet send; packet-slot bounded send queue; a
dedicated inbound slot; strict pause (post no further recv operation); cancel-once + confirmed
detachment; source-addr on every recv; truncation detection.

| Backend | Blocking gaps |
|---|---|
| POSIX / Winsock readiness | Recv is a *level-triggered drain loop* — reframe as a bounded sequence of **serial receive operations** under one armed READ source, re-checking pause/close after every callback (the interest is an armed source, not an in-flight op; the ≤N/tick drain is a fairness bound). No structural blocker otherwise. |
| pollcomp | ~~No reap-before-free → borrowed recv_buf can outlive the object (UAF); "confirmed detachment" not guaranteed.~~ **RESOLVED (Phase B.6.1):** backend-owned stable token (`datagram_life.c`) — the op captures the buffer + retains a token ref at post, so a late/cancelled completion lands on live token state and the receive storage outlives every op. |
| io_uring | ~~No reap-before-free → borrowed recv_buf UAF~~ (**RESOLVED, Phase B.6.2:** stable token); strict pause `don't re-post` latch landed in Step 3 (`datagram_recv.c`). |
| IOCP | Best positioned (dequeue-before-free = lifetime-safe by construction); truncation surfaced (Step 5, `WSAMSG.dwFlags`); **stable token added (Phase B.6.3)** so the completion never dereferences the freed `KlDatagram`. |
| lwIP-raw | Raw provider `.dgram = NULL` → no control-plane dgram ops (configure/set_tos/mcast) — needs a stub or `kl_sockdef_dgram` fallback; no local addr (pktinfo); IPv4/loopback only; connected-send rejected. Copy-ring already avoids UAF and detects truncation, **but its 16-slot ring accumulates multiple transport-owned packets → does NOT yet satisfy strict one-held-packet pause (⚙ — rework to a single held inbound slot).** |
| EFI_UDP4 | **No datagram integration at all** — build a persistent `KlDatagram` provider from the `dns_uefi.c` token machine (one Rx token in flight, one Tx per send, `EFI_UDP4.Cancel` + confirmed retirement; the quarantine logic already models confirmed-or-fail-closed). No pktinfo/mcast/bcast/TOS. |

**Cross-cutting:** (1) ~~no lifetime-safe teardown on io_uring/pollcomp — a posted op can
reference freed object memory~~ — **RESOLVED (Phase B.6):** confirmed detachment via the
backend-owned stable token (not a generation stamp) now covers ALL FOUR completion backends
(pollcomp/io_uring/IOCP B.6.1–3; lwIP-raw B.6.4 — token replaces its legacy `ev->target` recovery,
copy-ring kept as staging);
(2) borrowed single `recv_buf` everywhere except lwIP-raw → the neutral object must own the
recv buffer (or copy, as lwIP-raw's ring does); (3) truncation not uniform (IOCP gap);
(4) `KlUdp*` target leaks into the abstract axis (§4); (5) strict pause has no explicit latch.

---

## 7. Coupling — what an extraction must preserve or sever

**Preserve (carry through to the neutral object):**
- `KlUdpServer` embeds `KlUdp` by value (`udp_server.h:70`) and surfaces `kl_udp_fd`/
  `local_port`/`last_error` — the neutral object must support by-value embed (or a documented
  ripple to opaque storage) and an error model. `kl_udp_server_fd` leaks a raw
  `KlSocketHandle` — providers without fds (lwIP/EFI) can't honor it → document as a capability.
- Source-pinned reply (`kl_udp_send_to_from`) and pktinfo `local` on recv — `KlUdpServer`
  multi-homed correctness depends on both being optional provider capabilities, not severed.
- DNS's **`src` on every recv + unconnected send-to-arbitrary-peer + many-logical-queries +
  permanent re-arm** — the single hardest constraint. Design the recv op as *self-re-arming,
  always yields `src` (and optional `local`)*, never "one-shot buffer consumption paired with
  a send."

**Sever cleanly (leave reachable independently, do NOT fold into `KlDatagram`):**
- DNS's TCP fallback + DoT hook — raw `kl_sock_*` **stream** ops + `kl_watcher_*` + `KlTls`
  (`dns_resolver.c:710-719,806-833`). It is byte-stream, not datagram; on a provider without
  stream sockets, TCP-fallback/DoT is simply unavailable — a documented capability gap.
- DNS config discovery (`dns_sys.h`, POSIX files / Windows iphlpapi) and timers — share only
  the `KlEventCtx`.
- `KlUdpConfig`'s 16 UDP-specific knobs (rcvbuf/sndbuf/mmsg/gro/tos/multicast/…) — these belong
  to the **UDP provider config**, not a neutral datagram config; the neutral config carries the
  transport-object knobs (slot count, payload capacity, recv/send caps), UDP knobs ride the
  provider-options surface.

---

## 8. Recommended consolidation — implementation phases

Mirror the proven stream Phase A/B sequence (see `docs/generic_transport_audit.md` §8 and the
TU banners). **Do not begin extraction until every Tier-1 requirement has a defined
implementation or documented limitation for each backend (the matrix in
`docs/datagram_contract.md`).**

**Phase A — structural carve (semantics-preserving, internal):**
- Carve a neutral `KlDatagram` object out of `KlUdp`'s machine-state (send queue, recv buffer
  ownership, interest, in-flight tracking). `KlUdp` becomes the UDP provider-config surface
  over `KlDatagram`, embedded by value in the consumers as today.
- Re-type the completion identity: `KL_COMP_UDP_RECV`/`_SEND` target `KlDatagram*`;
  `post_udp_recv`/`send` take `KlDatagram*`; move the entrypoints alongside the stream raw
  entrypoints in `completion.h`. Backends stop dereferencing `KlUdp` internals — they borrow a
  caller-supplied recv buffer and deliver `src`/`local`/meta on the neutral object.

**Phase B — new machinery, in this order (needs new code; the shippable contract):**
1. **Bounded packet-slot send queue + dedicated inbound slot** — replace the byte-FIFO with a
   fixed outbound slot budget plus one dedicated inbound slot
   (`slot_count × (payload_capacity + metadata) + (inbound_payload_capacity + metadata)`,
   overflow-safe), atomic all-or-none acceptance,
   `KlDatagramSendStatus` (ACCEPTED/WOULD_BLOCK/TOO_LARGE/UNSUPPORTED/CLOSED/ERROR — the refusal
   statuses take no ownership and mutate no queue state).
2. **Strict receive pause** — post/arm no further recv operation on pause; a completion already
   posted is *held in the dedicated inbound slot* as exactly one complete packet and delivered
   once on resume (mirror `stream_read.c:118-162`).
3. **Serial receive operations + lifetime ownership** — at most one completion recv posted (a
   readiness interest is an armed source, not an op); confirmed detachment guarantees no op
   references the object or its buffers after `on_close` (reap before release, or a backend-owned
   stable token that outlives every op); free/reuse
   prohibited before detachment. A generation stamp may reject duplicates but does not replace
   ownership. **Tier-1 delivers one datagram per receive operation — no `recvmmsg`/GRO delivery.**
4. **Close → cancel-once → confirmed detachment** — `close_begin`/`cancel`, dual retirement
   predicate, `on_close` only after physical retirement, reentrancy depth guard (mirror
   `stream_close.c`). Replaces `kl_udp_free`'s silent discard.
5. **Explicit truncation delivery** — surface `KL_DGRAM_TRUNCATED`; fix the IOCP gap.

**Then:** live-wire the existing UDP + DNS + `KlUdpServer` through the neutral object
(behavior-preserving), and **public stabilization** — the boxed STABLE banner +
`<keel/datagram_detail.h>` opt-in ABI split, matching `stream.h`/`listener.h`/`connect_op.h`.

> **Step-6 live-wiring status.** 6.1 routed `kl_udp_recv_start` through the shared serial-receive
> machine (`KlDgramRecv` over the dedicated inbound slot). 6.2 is a **verification checkpoint** for
> `KlUdpServer`: because the server is a thin wrapper over the public `KlUdp` API, its receive already
> rides that machine transitively — no server-side wiring exists or is added. `tests/test_udp_server.c`
> now covers the server's Tier-1 couplings through the machine (serial multi-datagram
> one-packet-one-callback dispatch, validated per-payload with a duplicate-rejecting seen-set; source
> addr on every recv; reply-to-each-sender's-own-source from the handler; Linux-gated,
> getsockopt-verified `SO_REUSEPORT` shared bind with delivery conserved) on both readiness and
> completion backends. The server's **send queue (byte-budget) and close (`kl_udp_free` legacy
> teardown) remain the existing `KlUdp` compatibility behavior** — the fixed-slot atomic send +
> confirmed-detachment close machines land on the public `KlDatagram` path (Step 7), not here.
>
> 6.3 is the same **verification checkpoint** for the built-in DNS resolver: it too rides the receive
> machine transitively through `kl_udp_recv_start(dns_on_recv)`, so no DNS-specific receive seam exists
> or is added. `tests/test_dns_resolver.c` adds coverage for the couplings `dns_on_recv` leans on
> through the machine — a wrong-**source** response (valid content from a non-nameserver socket) dropped
> on the src address+port check, proven two-phase (poison alone leaves the resolution pending; the
> withheld legit reply, released from the nameserver socket, then completes it — independent of
> cross-socket scheduling); and concurrent
> distinct-name resolutions demultiplexed by transaction id across the serial receive re-arms (each
> callback receives only its own name's distinguishable answer) — on both readiness and completion
> backends. As with `KlUdpServer`, DNS's UDP **send (`kl_udp_send_to`) and teardown (`kl_udp_free`)
> keep the existing `KlUdp` compatibility semantics until Step 7**, and its TCP fallback (RFC 7766) is
> an **independent byte-stream path**, not the datagram machine.

**Not in the baseline:** QUIC (consumes `KlDatagram`, does not shape it); `recvmmsg` batching
and GSO/GRO delivery (Tier-2 — a batch of already-received datagrams cannot honor the
one-held-packet pause rule); a variable-size byte-budget queue (optional future capability);
connected-mode as an assumption (it is a config mode). See `docs/datagram_contract.md`
§"Out of contract".

---

## 9. Phase-1 conclusion

The datagram consolidation is a **carve, not a green-field build**: the provider data-plane
(`KlDatagramOps`) and the UDP machine (`udp.c`) already exist and are sound. The missing layer
is the **neutral `KlDatagram` transport object** and its **lifecycle contract** — the exact
analogue of what `KlStream`/`KlListener`/`KlConnectOp` added above `KlSocketProvider`. The two
pivotal semantics are set (Contract): **serial receive operations, one datagram per operation**
(a readiness interest is an armed source, not an op; batching/GRO delivery deferred to Tier 2)
and a **packet-slot** bounded send queue plus a dedicated inbound slot. The
next deliverable is the normative `docs/datagram_contract.md` + the per-backend Tier-1
compatibility matrix; no extraction begins until that matrix is complete.
