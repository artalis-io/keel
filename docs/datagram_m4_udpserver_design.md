# Datagram M4 — migrate KlUdpServer → {KlDatagram core + extended layer} — design freeze

Status: **PROPOSED (docs-only)** — no code until reviewed and accepted, per the consolidation workflow.
Sibling of the accepted M0 / synchronous-teardown / M1 / M2 freezes. Authority is
`docs/datagram_consolidation_design.md` §5 (KlUdpServer migration + compat-wrapper requirements) and
roadmap item 4; this freeze turns them into an implementable increment. **Depends on M0 (fd prep), M1
(`BOTH` policy), M2 (source-pin cap gate + multicast).**

## 0. Scope

Re-implement `KlUdpServer` on top of the Tier-1 `KlDatagram` core (M0 prep + M1 `BOTH` policy) and the
M2 extended layer (caps + multicast), replacing its embedded `KlUdp`. The public **surface is preserved
exactly (source)**: the 8 functions (`kl_udp_server_{init,reply,multicast_join,multicast_leave,free,
local_port,fd,last_error}`), the 17-field `KlUdpServerConfig`, and the handler
`KlUdpHandlerFn(KlUdpServer*, data, len, const KlSockAddr *src, void*)`. `KlUdp` itself is **untouched**
by M4 (its reduction to a compat wrapper is M5).

**In scope:** the config→{KlUdpConfig + KlDatagramConfig} mapping; the send-queue sizing onto fixed
slots; source-pinned reply via `msg.local`; recv + local-address capture + handler dispatch; multicast
join-at-init + runtime join/leave via M2; deterministic error mapping; teardown; the documented
behavior caveats (count-bound queue, no mmsg/GRO coalescing).

**Not in scope:** M5 (`KlUdp` compat wrapper); any new public `KlUdpServer` API.

## 1. ABI: surface preserved; the KlUdpServer struct is a pre-consumer layout revision (D-M4-1)

`KlUdpServer` is a **caller-owned, embedded** struct (`KlUdpServer ns;` on the stack) whose current
first field is `KlUdp udp`. M4 replaces that with a `KlDatagram` plus the small amount of
extended/dispatch state, so `sizeof(KlUdpServer)` and its layout change. This is an **intentional
pre-consumer struct-layout ABI revision** — the same project-stage judgment accepted for the M2 vtable:
there are no external consumers requiring binary compatibility, and all in-tree callers (tests,
examples) recompile. The **8 function signatures, the 17-field `KlUdpServerConfig`, and the handler
typedef are byte-for-byte unchanged** (source + ABI). Proposed struct:

```c
struct KlUdpServer {
    KlDatagram      dg;              /* was: KlUdp udp — the migrated transport */
    KlUdpHandlerFn  handler;
    void           *user_data;
    KlError         last_error;
    KlSockAddr      local;           /* current datagram's local (dest) addr (source-pin reply) */
    int             have_local;      /* 0 = unknown → reply uses the socket default */
};
```

## 2. Config mapping (D-M4-2)

`kl_udp_server_init` maps the 17-field `KlUdpServerConfig` onto two existing config types — no new
public config. Step 1 prepares the fd provider-neutrally (M0 `kl_datagram_open` over a `KlUdpConfig`);
step 2 adopts it into a `KlDatagram` (`kl_datagram_init_ex`).

**KlUdpConfig (→ `kl_datagram_open` → provider `configure`)** — the socket-option knobs:
`family` (from `bind_addr`), `bind_addr`, `bind_port = port`, `recv_buf_size`, `reuse_addr = 1`
(servers rebind), `reuse_port`, `recv_pktinfo = wildcard(bind_addr)` (§4), `so_rcvbuf`, `so_sndbuf`,
`tos` (default outgoing TOS via setsockopt), `recv_tos`, `broadcast`, `multicast_ttl`,
`multicast_disable_loop`, `multicast_iface`, `alloc`. **Forced OFF: `recv_gro` and `mmsg_batch`** (§7 —
the single-flight core cannot split GRO-coalesced datagrams nor drive recvmmsg). `multicast_group` is
NOT a configure knob here — it is joined post-init (§6).

**KlDatagramConfig (→ `kl_datagram_init_ex`)** — the transport/queue knobs: `fd` (from prep),
`send_slots` + `send_slot_cap` + `send_byte_budget` (§3), `recv_cap = recv_buf_size` (default 2048),
`want_caps = wildcard(bind_addr) ? KL_DGRAM_CAP_SOURCE_PIN : 0` (§4), `sockets = ctx->sockets`,
`ctx`, `alloc`.

## 3. Send-queue sizing — mapping a byte-unbounded queue onto fixed I8 slots (Decision D-M4-3, the crux)

`KlUdp`'s reply queue is byte-bounded (`max_send_queue`) and **count-unbounded** (malloc-per-reply). The
I8 core is `send_slots × send_slot_cap`, preallocated. M1's `BOTH` policy bounds **both** dimensions.
The mapping (per §4 D-Q-2 / D-Q-4, with the documented count-bound caveat):

- `send_byte_budget = max_send_queue` — **verbatim** (M1; no inflation).
- `send_slot_cap = 65507` — a full UDP payload, so **any valid reply is queueable** (preserving
  `KlUdp`'s "can queue any reply" semantics; no surprising per-reply size refusal).
- `send_slots = max(1, max_send_queue / send_slot_cap)` — the slot count is **derived from the byte
  budget**, so preallocation (`send_slots × send_slot_cap`) ≈ `max_send_queue` — the same memory bound
  the user already chose via `max_send_queue`. (Default 256 KiB ⇒ 3 slots.)

**Documented caveat (accepted §4 D-Q-2):** because slots are fixed and full-datagram-sized, a burst of
many *small* replies is count-bounded sooner than `KlUdp`'s byte-only bound — the (N+1)-th concurrent
queued reply gets `WOULD_BLOCK` even with byte budget to spare. `kl_udp_server_reply` maps that to `-1`
/ `KL_ERR_QUEUE_FULL` (§8) — the same lossy-drop surface `KlUdp` presents on overflow, so the
`KlUdpServer` contract (reply sent if capacity, else dropped with `-1`) is preserved. An application
that genuinely needs deep small-reply queuing stays on `KlUdp` (the off-ramp, §D-Q-4).

*(Alternative considered — O-M4-sizing, §12: a nominal `send_slot_cap` (e.g. `recv_buf_size`) with a
deeper `send_slots`, giving deeper small-reply queuing but refusing to QUEUE replies larger than the
nominal cap — case-(a) direct-send-or-refuse. Rejected as the default because it changes reply
queueability by size; kept as the reviewer's alternative.)*

## 4. Source-pinned reply + recv-pktinfo (Decision D-M4-4)

Multi-homed correctness: a reply must leave from the **local (destination) address the request arrived
on**. `KlUdp` enables `recv_pktinfo` only on a wildcard bind (a specific-address bind already fixes the
source), captures the local addr per datagram, and sends the reply source-pinned via `msg.local`.

M4 preserves this and makes the source-pin capability **fail-loud where it is load-bearing** (M2):
- **Wildcard bind:** `recv_pktinfo = 1` (capture the local addr) **and** `want_caps =
  KL_DGRAM_CAP_SOURCE_PIN` (require the send-side source-pin). On the POSIX/Winsock providers
  `KlUdpServer` runs on, source-pin is always available, so init succeeds and behavior is identical to
  `KlUdp`; on a provider lacking it, init fails loudly rather than silently answering from the wrong
  source (a latent `KlUdp` bug on such a provider). Reply passes `msg.local = &s->local` when
  `have_local`.
- **Specific-address bind:** `recv_pktinfo = 0`, `want_caps = 0`, reply `msg.local = NULL` — the
  socket's bound address fixes the source; no source-pin needed.

*(O-M4-srcpin, §12: the alternative is opportunistic source-pin — `want_caps = 0` always, reply
consults `kl_datagram_provider_caps() & SOURCE_PIN` and passes `msg.local` only if supported — exactly
matching `KlUdp`'s graceful degrade. Recommended above is the fail-loud-on-wildcard variant, a no-op on
real platforms; the reviewer may prefer the opportunistic one to preserve `KlUdp` byte-for-byte on all
providers.)*

## 5. Recv path + local capture + dispatch (D-M4-5)

`kl_datagram_recv_start` with a `KlDatagramRecvFn` adapter (mirrors today's `udp_server_on_recv`):
```
static void us_on_recv(void *ud, const void *data, size_t len,
                       const KlSockAddr *peer, const KlSockAddr *local, unsigned flags) {
    KlUdpServer *s = ud;
    if (local && (flags & KL_DGRAM_HAS_LOCAL)) { s->local = *local; s->have_local = 1; }
    else                                       { s->have_local = 0; }
    s->handler(s, data, len, peer, s->user_data);   /* src = peer */
}
```
The handler calls `kl_udp_server_reply(s, data, len, dest)` → `kl_datagram_send(&s->dg, &(KlDatagramMessage){
.data, .len, .peer = dest, .local = s->have_local ? &s->local : NULL, .tos = -1 })`. `tos = -1`: the
reply uses the socket-default TOS (set at configure from `cfg->tos`), exactly as `KlUdp`'s reply does
(no per-packet TOS → no `KL_DGRAM_CAP_TOS` requirement).

## 6. Multicast (D-M4-6)

- **Join-at-init:** if `cfg->multicast_group != NULL`, after a successful `kl_datagram_init_ex` call
  `kl_datagram_multicast_join(&s->dg, cfg->multicast_group, cfg->multicast_iface)`; on failure, tear the
  datagram down and fail init (`last_error` from the join — §8), matching `KlUdp`'s init-time join.
- **Runtime:** `kl_udp_server_multicast_join/leave` → `kl_datagram_multicast_join/leave` (M2), which
  gate on `KL_DGRAM_CAP_MULTICAST` and route to the provider's `mcast_membership`. `last_error` is set
  from the M2 call's error (§8).

## 7. Batching / GRO / GSO / recv-TOS on the single-flight core (D-M4-7)

The Tier-1 core is single-flight (one recv posted, one send in flight), so:
- **`recv_gro` forced OFF.** UDP_GRO makes a plain recv return a *coalesced* super-buffer; the
  single-flight core would mis-deliver it as one datagram. Forcing it off keeps per-datagram delivery
  **correct** (not merely slower). A migrated server loses GRO receive-coalescing.
- **`mmsg_batch` inert.** The core never calls `recv_batch`/`send_batch`; the knob is accepted
  (config-compatible) but unused. No recvmmsg/sendmmsg coalescing.
- **GSO** unused (reply is one datagram; the core send posts one datagram).
- **`recv_tos`** capture may be enabled at configure, but per O-CAP-1 the recv callback does not deliver
  TOS — captured-not-delivered (no consumer reads it through `KlUdpServer`).

These are the M2 §6 no-coalescing caveat made concrete for M4: `KlUdpServer` on the core trades
`KlUdp`'s Linux `recvmmsg`/`sendmmsg`/GRO throughput for the uniform single-flight model. If a workload
needs that throughput, it stays on `KlUdp` (off-ramp). Recorded, not silently dropped.

## 8. Error mapping (D-M4-8)

`kl_udp_server_*` keep their `int 0/-1` + `KlError last_error` contract. Mapping from the core:
- **reply:** `KL_DATAGRAM_ACCEPTED → 0`; `WOULD_BLOCK` / `TOO_LARGE` (budget or slot_cap) → `-1`,
  `last_error = KL_ERR_QUEUE_FULL` (the drop `KlUdp` already reports); `CLOSED` → `-1`,
  `KL_ERR_INVALID_ARG`; `ERROR`/`UNSUPPORTED` → `-1`, `KL_ERR_IO` / `KL_ERR_UNSUPPORTED`.
- **init:** any prep/init/join failure → `-1` with `last_error` carried from the failing step
  (`kl_datagram_last_error` / the multicast error). fd never leaked (M0/M2 contracts).
- **multicast join/leave:** pass through the M2 error precedence (`KL_ERR_UNSUPPORTED` /
  `KL_ERR_INVALID_ARG` / `KL_ERR_IO`).

## 9. Teardown (D-M4-9)

`kl_udp_server_free` → graceful/synchronous datagram teardown. Since `KlUdpServer` is caller-embedded
(not heap-owned by the server) and `free` is a void best-effort call, it uses the confirmed-detachment
close then object free, or the synchronous `kl_datagram_teardown` (Option A) if a close cannot be
driven to completion inline — mirroring how `kl_udp_free` unconditionally reclaims. No user callback is
invoked (a `void free`). Exactly-once fd close via the datagram close machine.

## 10. Test matrix (`tests/test_udp_server.c` + the existing udp-server suite)

Preserve the existing suite (behavior parity) and add M4-specifics:
1. **ABI/surface** — the 8 functions, 17-field config, and handler signature compile + link unchanged;
   existing udp_server tests pass verbatim (loopback bind + echo reply).
2. **Source-pinned reply (wildcard)** — bind `0.0.0.0`, a datagram to a specific local addr; the reply
   is source-pinned to that local (assert via a second bound address / `recv` local on the peer where
   the platform allows); `want_caps = SOURCE_PIN` accepted on POSIX.
3. **Specific-address bind** — no `recv_pktinfo`, `want_caps = 0`, reply from the bound address.
4. **Reply backpressure** — fill the send queue (gating provider / no drain), the next reply returns
   `-1` / `KL_ERR_QUEUE_FULL`; drains and succeeds after progress (count-bound caveat exercised).
5. **Multicast** — join-at-init (`multicast_group` set) succeeds on a multicast-capable provider;
   runtime join/leave route through M2; a malformed group / unsupported provider yields the M2 errors.
6. **Teardown** — `free` closes the fd exactly once (no leak under ASan/LSan); reuse after free.
7. **`recv_gro` forced off** — a server configured with `recv_gro = 1` still delivers one datagram per
   recv (no coalesced mis-delivery).
8. **Shared-loop coexistence** — a `KlUdpServer` and a TCP `KlServer` on one `KlEventCtx` (the headline
   use case) both serve.

## 11. Validation plan

- macOS default (kqueue readiness) `make test` — full suite incl. the udp_server suite, ASan/UBSan.
- `BACKEND=pollcomp` — the completion-model reply path (in-flight slot accounting).
- Linux container `BACKEND=iouring` under ASan/UBSan/**LSan** — reply/recv/close lifetime, no leak;
  and a `SO_REUSEPORT` fan-out smoke if feasible.
- Gates: `check-tier1-boundary` (KlUdpServer routes only through the datagram facade + M2 API — no new
  platform include), `check-sockaddr-neutral`, `check-doc-refs`, `cppcheck`; MinGW winsock compile;
  freestanding unaffected (KlUdpServer is not a freestanding component). No `KlUdp` change → `KlUdp`
  suites unaffected.

## 12. Open decisions for the reviewer (before implementation)

- **O-M4-sizing (the crux, §3).** *Recommended:* `send_slot_cap = 65507`, `send_slots =
  max(1, max_send_queue / send_slot_cap)` — any reply queueable, preallocation ≈ `max_send_queue`,
  count-bound caveat for small-reply bursts. *Alternative:* a nominal `send_slot_cap` (e.g.
  `recv_buf_size`) with deeper `send_slots` — deeper small-reply queuing, but replies larger than the
  nominal cap can only be direct-sent (case-(a)), not queued. Which trade-off for the default?
- **O-M4-srcpin (§4).** *Recommended:* `want_caps = SOURCE_PIN` on a wildcard bind (fail-loud where
  source-pin is load-bearing; a no-op on POSIX/Winsock). *Alternative:* opportunistic source-pin
  (`want_caps = 0`, runtime `provider_caps` check in reply) to preserve `KlUdp`'s graceful degrade
  byte-for-byte on every provider.
- **O-M4-struct (§1).** Confirm the `KlUdpServer` struct-layout change (`KlUdp udp` → `KlDatagram dg`)
  as an accepted pre-consumer ABI revision (the M2-vtable precedent), vs any requirement to preserve
  the struct's binary layout.
- **O-M4-gro (§7).** Confirm forcing `recv_gro` off (correctness on the single-flight core) + `mmsg`
  inert, documented as the M2 §6 caveat — vs deferring M4 until a batched-recv extended layer exists
  (not recommended; that layer is explicitly deferred).
