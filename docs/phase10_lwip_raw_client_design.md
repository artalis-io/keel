# Phase 10 — lwIP raw-API completion CLIENT (outbound) — Design (proposal for review)

**Status: PROPOSAL. Nothing implemented.** This doc scopes adding **outbound (client) support**
to KEEL's lwIP raw completion backend (`integrations/lwip/event_lwip_raw.c` +
`lwip_raw_glue.c`, `NO_SYS=1`). Today that backend is **server-only**: `lwr_sock_connect`
returns `-1`/`ENOTSUP` (`event_lwip_raw.c:248`), and DNS/TLS/UDP are unsupported
(Stage-C matrix, `docs/phase9_lwip_raw_design.md`). The motivating consumer is **UEFI
HTTP-boot / bare-metal outbound calls** — a `NO_SYS=1` firmware target that has no threads
and no sockets, so the shipped **readiness** socket-API lwIP integration cannot serve it.

Mirror-doc for structure/quality: `docs/completion_axis_runtime_design.md` and
`docs/phase9_lwip_raw_design.md`. The load-bearing decision — the **connect-completion
contract** (§3) — is flagged for review.

---

## 1. Goal + motivating consumer

**Goal.** Let a `KlClient` (`src/client.c`) perform outbound HTTP/1.1 over the lwIP-raw
completion backend, in-process, `NO_SYS=1`, single-thread — the same "KEEL's run loop *is*
the lwIP mainloop" model Phase 9 established for the server, now for the client direction.

**Motivating consumer: UEFI HTTP-boot / bare-metal outbound.** A firmware target (UEFI DXE
phase, or a bare-metal bootloader) that must fetch a boot image / config over HTTP(S) has:

- **No threads.** UEFI is a cooperative, single-context boot environment; there is no
  scheduler to run lwIP's `tcpip_thread`.
- **No BSD sockets.** There is no libc socket layer, no `fcntl`, no `poll`/`epoll` fd to
  wait on.

The shipped readiness lwIP integration (`keel_lwip.h`: `kl_socket_provider_lwip()` +
`kl_event_provider_lwip()`, `integrations/lwip/socket_lwip.c` + `event_lwip.c`) is built for
`NO_SYS=0`: it rides lwIP's **sockets/netconn** API (`lwip_socket`/`lwip_connect`/
`lwip_poll`), which requires the tcpip thread + mailboxes. That is exactly the weight a
`NO_SYS=1` firmware target cannot pay. So the readiness path is architecturally excluded, and
a **raw-`tcp_*` completion client** is the only fit — the same conclusion Phase 9 reached for
the server (`docs/phase9_lwip_raw_design.md`, "Why a second lwIP provider at all" / "The
crux: NO_SYS=1").

**In scope:** plaintext HTTP/1.1 `GET`/small-body requests to a numeric IPv4 address, then
(staged) Happy Eyeballs timers, a raw-DNS `KlResolver`, and TLS-over-raw. **Out of scope:**
IPv6, UDP, a second concurrent raw context, multi-thread (see §10).

---

## 2. lwIP raw client mechanics

lwIP's raw API is natively **completion-shaped** (callbacks), which is why it maps onto
KEEL's completion axis rather than the readiness one. The outbound flow:

```
pcb = tcp_new()                         → an opaque tcp_pcb (client)
tcp_arg(pcb, owner)                     → tag the pcb with its KlConn* (Phase-9 slot pattern)
tcp_err(pcb, connected_err_cb)          → RST / OOM / abort during connect or after
tcp_connect(pcb, &ipaddr, port, connected_cb)
                                        → SYN sent; returns ERR_OK (queued) or an error
        ... lwIP mainloop ticks (sys_check_timeouts + netif_poll) ...
connected_cb(owner, pcb, err)           → err==ERR_OK: connected  |  err!=ERR_OK: failed
        → then tcp_recv(pcb, recv_cb), tcp_sent(pcb, sent_cb) as for the server
```

This reuses the Phase-9 machinery directly:

- **Per-conn slot + `tcp_arg` owner tagging.** Phase 9 keys every recv/sent/err callback to
  its `KlConn*` via `tcp_arg` and a glue per-pcb slot (`docs/phase9_lwip_raw_design.md`, P9-2
  "pcb↔KlConn"; `lwip_raw_glue.h` `KlLwrRecord.owner`). A client pcb is tagged the same way —
  the slot machinery is direction-agnostic once the pcb exists.
- **Retained-recv (Stage A).** The server's `tcp_recv` copies the (chained) pbuf into a
  per-slot staging buffer and calls `tcp_recved` immediately, surfacing `KL_COMP_READ` only
  for *armed* conns (`docs/phase9_lwip_raw_design.md`, P9-2 "recv ordering"). The client's
  **response** body arrives on exactly this path — no new receive code, just a client-created
  pcb feeding the same slot. `KL_LWR_RX_MAX` (64 KiB) bounds it identically.
- **Bounded-TX (Stage B).** The server's `lwr_send_pump` (`tcp_write(COPY)` in
  `tcp_sndbuf`-bounded chunks + `tcp_output`, `ERR_MEM` backpressure, one terminal
  `KL_COMP_WRITE` at `acked==total`) is exactly what the client's **request** send needs
  (`docs/phase9_lwip_raw_design.md`, P9-3). The request line + headers + optional small body
  are pumped through the same owned-copy send path.
- **Close/cancel/lifetime (Stage C/P9-4).** `tcp_err` (pcb-already-freed, key-by-owner),
  exactly-one-close, close-with-outstanding-send, `kl_comp_cancel` on timeout — all reused
  verbatim. A **connect failure** (`connected_cb` with `err != ERR_OK`, or `tcp_err` before
  connect) is a new terminal-record source, but it routes through the same
  `lwr_push_terminal` → single terminal completion → driver close path.

**The one genuinely new piece is `tcp_connect` and its `connected_cb`** — how a connect
success/failure becomes a completion the KEEL client consumes. That is the contract question
in §3.

**Connect success/failure → completion.** The glue's `connected_cb`:
- **success** (`err == ERR_OK`): mark the slot connected; wire `tcp_recv`/`tcp_sent`; push a
  new **connect-completion record** (a `KlLwrRecord` of a new kind, or an owner-keyed WRITE
  with a `connected` flag — see §3) so the driver advances the client from CONNECTING →
  SENDING.
- **failure** (`err != ERR_OK`, e.g. `ERR_RST`/`ERR_ABRT`/timeout via `tcp_err`): the pcb is
  freed by lwIP on `tcp_err`; key the slot by owner, push a **failed** connect completion so
  the driver (or the client's Happy-Eyeballs layer) treats it as a failed attempt.

---

## 3. The connect-completion contract question — THE CRUX (decision + justification)

**Question:** does KEEL already have a completion **connect** path the client uses, or must a
new completion primitive be added?

### 3.1 Finding: the async client is built entirely on the READINESS axis

`src/client.c`'s async connect does **not** use the completion axis at all. It uses:

- `kl_sock_connect(...)` returning `EINPROGRESS` on a non-blocking fd
  (`client.c:1296`, `client.c:1390`),
- a **readiness** `KlWatcher` armed for `KL_EVENT_WRITE`
  (`kl_watcher_add(c->ev_ctx, fd, KL_EVENT_WRITE, ...)`, `client.c:1305`, `client.c:1397`),
- writability → `kl_sock_get_so_error(...)` to decide win/fail
  (`he_on_writable`, `client.c:1477–1485`).

There is **no `KL_COMP_CONNECT` kind** in `completion.h` (the kinds are
`KL_COMP_ACCEPT/READ/WRITE/UDP_RECV/UDP_SEND/WATCHER`, `completion.h:30–39`) and **no
`kl_comp_post_connect`** primitive (the `KlCompletionOps` vtable is
`drain/prime_accepts/post_recv/post_send/post_accept/post_sendfile/cancel/post_udp_recv/
post_udp_send`, `completion.h:83–95`). The completion driver never initiates outbound
connections — it only *accepts*.

### 3.2 Finding: the client "works" on completion loops only by accident (pollcomp), and NOT on IOCP

Completion backends implement the readiness `KlEventOps` (`add`/`mod`/`del`/`wait`), but for
**connection fds** those are inert — I/O is posted, not armed (`event_pollcomp.c:112–118`,
`event_iocp.c:125`). The one exception is a **tagged** `KlWatcher` (LSB=1), which the client's
connect uses: completion backends *do* relay a tagged watcher.

- **pollcomp** relays it faithfully: it registers the watch, `poll()`s it with the requested
  mask (`pc_watch_events`: `KL_EVENT_WRITE → POLLOUT`, `event_pollcomp.c:531–536`), and
  surfaces `KL_COMP_WATCHER` on readiness. So a `KL_EVENT_WRITE` connect watcher **genuinely
  fires** on pollcomp — the async client happens to work there.
- **IOCP** relays a watcher only as a **persistent `WSARecv`** (readable-only,
  `iocp_watch_post`, `event_iocp.c:115–121`). There is **no writable/connect watch** — a
  `KL_EVENT_WRITE` connect watcher would never fire. IOCP's real outbound-connect primitive
  is `ConnectEx` (a *completion*), which the codebase does not wire. The async client's
  connect therefore does **not** work over IOCP today; the only IOCP client test
  (`tests/smoke_iocp_async.c:93`) uses the **sync** blocking `kl_client_request`
  (`connect_with_timeout`, host sockets), never the async client on the completion loop.

**lwIP-raw is in IOCP's camp, more so.** A raw backend has **no pollable fd at all**
(`loop->fd == -1`, `docs/phase9_lwip_raw_design.md`, P9-5); its `KlSocketHandle` is a
`tcp_pcb *`. There is nothing for `poll()`/`WSARecv` to watch. A readiness `KL_EVENT_WRITE`
watcher over a pcb is meaningless — connect readiness only exists as the `connected_cb`
callback. So the readiness-watcher connect path the client relies on **cannot** be honored by
lwIP-raw.

### 3.3 Decision: add a completion CONNECT primitive (a cross-backend contract extension)

**Recommended:** add a completion **connect** path to the contract:

- a new kind `KL_COMP_CONNECT` in `completion.h` (target = `KlConn*`; `ok` = connected /
  failed), and
- a new primitive `int kl_comp_post_connect(KlConn *c, const KlSockAddr *addr)` on
  `KlCompletionOps` + the dispatch layer (`completion_dispatch.c`) + `io_engine.h` seam,
- and — critically — make **`src/client.c` drive connect over the completion axis when the
  loop is a completion loop** (branch on `KL_EVENT_CAP_COMPLETION`, exactly as the server
  already branches readiness vs completion), instead of the readiness `KL_EVENT_WRITE`
  watcher.

**Why add, not reuse:**
1. There is nothing to reuse — no completion connect exists (§3.1).
2. The readiness-watcher shim is **not a real portable connect** on completion loops — it is
   pollcomp-only luck and silently broken on IOCP (§3.2); relying on it for lwIP-raw is
   impossible (no fd).
3. `ConnectEx` (IOCP) and `tcp_connect`+`connected_cb` (lwIP-raw) are both natively
   *completion*-shaped. A `KL_COMP_CONNECT`/`kl_comp_post_connect` names the operation
   honestly on the axis it belongs to — the mirror of `KL_COMP_ACCEPT`/`kl_comp_post_accept`
   for the outbound direction. This is the same "name the completion, don't fake readiness"
   discipline the whole completion axis is built on (`completion.h` header comment).

**Scope + blast radius — this is a CROSS-BACKEND contract extension, its own stage series
(like RC-*), NOT lwip-only.** Adding `KL_COMP_CONNECT` touches:

- `src/completion.h` — the kind + the `KlCompletionOps.post_connect` member (+ the doc block).
- `src/completion_dispatch.c` — the `kl_comp_post_connect` dispatcher (compiled-in vs runtime,
  mirroring `kl_comp_post_recv`).
- `src/io_engine.h` / `src/completion_absent.c` — the seam + the `KEEL_NO_COMPLETION` stub.
- `src/completion_driver.c` — a `comp_on_connect` handler that advances the KlConn from
  CONNECTING → SENDING and drives the request (and, later, the TLS handshake).
- `src/client.c` — the completion-vs-readiness branch for connect (the real client wiring;
  the largest change, since Happy Eyeballs, the single-fd path, and the proxy-CONNECT path
  all currently assume readiness watchers — see §4).
- **Every completion backend must implement it**, not just lwIP-raw:
  - `event_pollcomp.c` — a `pc_post_connect` (real `connect()` + POLLOUT poll + `SO_ERROR`,
    since it *is* a real fd) so the completion-path client is testable on a portable POSIX
    double.
  - `event_iouring.c` — `IORING_OP_CONNECT`.
  - `event_iocp.c` — `ConnectEx` (this also *fixes* the currently-broken async client on IOCP
    — a real bonus, but it means the change must be validated on IOCP under MinGW).
  - `integrations/lwip/event_lwip_raw.c` — `tcp_connect` + `connected_cb` (the Phase-10 work).

**Therefore the connect-completion contract extension is proposed as its own foundational
stage (LC-0 below), landed and green on pollcomp / io_uring / IOCP BEFORE the lwIP-raw
client, exactly as the RC-1..RC-4 series hardened the runtime-injection contract across
backends before lwIP-raw rode it.** It must be tested on pollcomp + io_uring (Linux) and
IOCP (MinGW), not lwIP-only. **This is the load-bearing decision to confirm on review.**

*Rejected alternative — "make lwIP-raw fake a WRITE-readiness watcher for the connect pcb"*:
synthesize a fake fd, register it as a tagged watcher, and surface `KL_COMP_WATCHER`+WRITE
when `connected_cb` fires. This keeps `client.c` unchanged but (a) requires inventing a fake
pollable handle where none exists, (b) entrenches the pollcomp-only illusion as if it were a
contract, (c) leaves IOCP's async client broken, and (d) mis-names a completion as readiness —
the exact axis smell `completion.h` warns against. Rejected: it trades a small honest
cross-backend addition for a fragile backend-local hack that hides a real gap.

---

## 4. Happy Eyeballs / async connect over the raw completion loop

RFC 8305 Happy Eyeballs (`src/client.c:1314–1485`) races the resolved address list: it stems
attempts with a **Connection Attempt Delay** timer (`he_arm_delay`, `client.c:1361`), races
multiple in-flight non-blocking connects, the first to become writable with `SO_ERROR==0`
wins (`he_on_writable`, `client.c:1477`), and an overall **deadline** timer bounds the whole
request (`he_on_deadline`, `client.c:1442`). Losers are closed (`he_close_attempts`).

Mapping onto raw `tcp_connect`, once the connect-completion primitive exists (§3):

- **Multiple in-flight pcbs.** Each attempt is a `tcp_new()` + `kl_comp_post_connect(addr)`
  (→ `tcp_connect(pcb, ip, port, connected_cb)`). Several client pcbs can be outstanding at
  once — lwIP handles concurrent connecting pcbs natively. Each carries its own slot + owner.
- **First-connected wins.** The first `connected_cb(ERR_OK)` surfaces a `KL_COMP_CONNECT`
  (`ok=1`); the client's `he_win` adopts that pcb and **aborts the losers**
  (`tcp_abort`/`kl_lwr_tcp_abort` on the other pcbs — the raw analogue of
  `kl_sock_close(loser_fd)`). Since the completion path replaces the per-fd WRITE watcher,
  `he_close_attempts` must drop pcbs via the socket-provider `close` (which the glue maps to
  `tcp_abort` for a half-open connecting pcb), not `kl_watcher_del`.
- **A failed attempt** surfaces `KL_COMP_CONNECT` (`ok=0`); `he_fail_attempt` fast-starts the
  next address without waiting out the delay (§5 of RFC 8305, `client.c:1455`).
- **Timers are unchanged.** `kl_timer_add`/`kl_timer_cancel` ride `KlEventCtx` (min-heap,
  `docs/`), which is checked per event-loop tick — and on the raw loop, every tick already
  calls `sys_check_timeouts()` + `netif_poll()`. The Connection Attempt Delay and overall
  deadline timers work identically; no new timer machinery. This is the cleanest part of the
  mapping — KEEL's timer axis is already backend-agnostic.

**Client-side refactor cost.** The Happy-Eyeballs code is written against readiness watchers
(`kl_watcher_add(KL_EVENT_WRITE)`, `kl_sock_get_so_error`). The completion branch replaces:
`start_connect`/`he_new_attempt`'s `connect+EINPROGRESS+watcher` with `kl_comp_post_connect`;
`he_on_writable`'s `SO_ERROR` check with the `KL_COMP_CONNECT` `ok` flag delivered by the
driver; `he_close_attempts`'s `watcher_del+close` with a `close` (→ `tcp_abort`). The state
machine (CONNECTING/SENDING/…), the delay/deadline timers, and the resolved-list cursor are
**unchanged**. This lands in LC-2 (LC-1 does the simpler single-address path first).

---

## 5. DNS — back a `KlResolver` with lwIP raw `dns_gethostbyname`

The client resolves via the `KlResolver` vtable (`src/resolver.h`:
`resolve`/`cancel`/`destroy`) or, on the sync path, `kl_resolve_sync`. Two constraints on a
`NO_SYS=1` raw target:

- `resolve_sync_lwip.c` uses **`lwip_getaddrinfo`** — the **sockets/netdb** layer, which does
  **not** exist under `NO_SYS=1`. It is unusable here. (It is the *readiness*/`NO_SYS=0`
  client's resolver.)
- lwIP raw mode offers **`LWIP_DNS`** with **`dns_gethostbyname(name, &addr, found_cb, arg)`**
  — a non-blocking, callback-driven resolver usable under `NO_SYS=1` (it rides the same UDP
  core the raw stack already has; requires `LWIP_DNS=1` + a configured DNS server in
  `lwipopts_raw.h`).

**Proposal: a raw-DNS `KlResolver`** (`integrations/lwip/resolve_dns_lwip_raw.c`) implementing
the vtable over `dns_gethostbyname`:

- `resolve()` calls `dns_gethostbyname`; the `found_cb` (fires on a later mainloop tick, or
  **synchronously** if cached) converts the `ip_addr_t` to a `KlSockAddr` and calls the
  resolver's `done_fn`.
- **Sync-completion contract.** `dns_gethostbyname` returns `ERR_OK` immediately when the name
  is cached / is a numeric literal — i.e. it may complete **synchronously** inside
  `resolve()`. This is exactly the resolver sync-completion contract KEEL already documents
  (`CLAUDE.md`, "Resolver sync-completion contract"; `src/resolver_cache.c`'s
  `in_resolve`/`completed` sentinel). The raw-DNS resolver must honor it — set the completed
  sentinel and let the caller detect synchronous completion, never assume the callback is
  deferred.
- lwIP DNS callbacks run on the mainloop thread (no lwIP thread under `NO_SYS=1`), so no
  marshalling / locking is needed — same single-thread guarantee as the rest of the backend.

**Staging: DNS is a SEPARATE sub-stage (LC-3), and the client ships numeric-IP-only first
(LC-1/LC-2).** This lets the connect + Happy-Eyeballs + send/recv core be proven over
loopback (`127.0.0.1`, no DNS) before any resolver work — and firmware HTTP-boot often has a
numeric server address anyway. The built-in async `kl_dns_resolver` (`src/dns_resolver.c`,
over `KlUdp`) is **not** available on raw (UDP is unsupported there, Stage-C matrix), so a raw
target must inject the raw-DNS `KlResolver` explicitly (or pass a numeric address).

---

## 6. TLS — raw-client HTTPS over the completion data plane (memory-BIO)

HTTPS from a raw client needs mbedTLS driven **over the completion data plane**, not the
socket-BIO. The mechanism already exists: **Phase 8b-5** added the caller-driven memory-BIO
mode to `KlTls` — the optional `feed_input(cipher,len)` / `drain_output(cipher,cap)` ops
(`include/keel/tls.h`; `docs/phase8b5_tls_completion_design.md`). The completion driver's TLS
leg (`comp_tls_drive`, 8b-5b) already:

- reads ciphertext into a transient op buffer (`read_buf` stays plaintext),
- feeds it to the engine via `feed_input`,
- drains outgoing ciphertext via `drain_output` and sends it,
- reuses `kl_conn_on_handshake` / `comp_try_reading` / `kl_conn_ingest_body` verbatim,

**with zero mbedTLS knowledge in the backend** — it drives the abstract `KlTls` vtable. A
raw-client HTTPS reuses this exact leg: the raw `tcp_recv`→staging→`KL_COMP_READ` delivers
ciphertext that the driver feeds to mbedTLS; `drain_output` ciphertext is pumped out through
the same `lwr_send_pump` (`tcp_write`) TX path. The lwIP-raw backend needs **no TLS code** —
it only moves bytes, exactly as it does for plaintext.

**Caveats inherited from 8b-5:** buffered HTTP/1.1 over TLS only in the first cut; TLS
file/stream bodies and ALPN-h2 are out of that subset (they close rather than mis-serve). For
a HTTP-boot client (small GETs), buffered is sufficient.

**Staging: TLS is a LATER sub-stage (LC-4); plaintext client ships first (LC-1..LC-3).**
mbedTLS is BYO / out-of-CI (the Phase-8b decision), so LC-4's runtime gate is local/hull, not
the standard CI matrix — same posture as `smoke_tls_completion`.

---

## 7. Socket provider — implement `lwr_sock_connect`

Today `lwr_sock_connect` fails early (`event_lwip_raw.c:248`, `errno=ENOTSUP`, return `-1`)
and the provider advertises `KL_SOCK_CAP_OVERLAPPED` only. Client support changes this:

- **`lwr_sock_connect` becomes real** — but note the completion decision (§3): the client's
  *async* connect goes through `kl_comp_post_connect`, not the synchronous `sock.connect` op.
  The socket-provider `connect` is used by the **sync** blocking client (`connect_with_timeout`,
  `client.c:104`), which is not applicable on a `NO_SYS=1` single-loop target (there is no
  blocking I/O). So `lwr_sock_connect` may stay `ENOTSUP` (sync connect is nonsensical here)
  while the **completion** connect path (`kl_comp_post_connect` → `tcp_connect`) provides the
  actual client capability. **Decide on review:** implement `lwr_sock_connect` as a no-op that
  returns `EINPROGRESS` (so a readiness-shaped caller doesn't hard-fail), or keep it `ENOTSUP`
  and route all client connects through the completion primitive. The latter is cleaner and
  consistent with §3.
- **New capability advertisement.** Until the client path is complete + tested, the backend
  must keep failing outbound connects early (Stage-C honesty — never silently hang). Add a
  capability bit (e.g. an `KL_SOCK_CAP_OUTBOUND` / completion-connect cap, or gate on
  `kl_comp_post_connect != NULL`) that the backend advertises **only when LC-1+ lands**. The
  Stage-C matrix row "Outbound **client** / `connect` — Unsupported (fails early)"
  (`docs/phase9_lwip_raw_design.md`) flips to Supported per-stage (plaintext → +HE → +DNS →
  +TLS). `raw_caps_test.c` gets the new expectations.

---

## 8. Staged plan (RC-series style — each independently testable in-process over loopback)

Each stage is green (build + loopback test + ASan/UBSan/LSan) before the next. Mirrors the
P9-1..P9-5 and RC-1..RC-4 discipline.

> **STATUS: COMPLETE.** LC-0 (PR #191) → LC-1/LC-2 (PR #192/#193) → **LC-3a** (PR #194, KlUdp
> over lwip-raw — inserted before DNS so the resolver rides KEEL's own UDP) → LC-3 (PR #195,
> DNS via KEEL's `src/dns_resolver.c` on KlUdp-over-raw) → LC-4 (PR #196, HTTPS client + server)
> → LC-5 (this doc + caps/README). **LC-3 shipped differently from the bullet below:** on user
> direction it uses KEEL's own `dns_resolver.c` over the LC-3a `KlUdp`-on-raw — NOT lwIP's
> `dns_gethostbyname` — so there is ONE DNS/UDP path everywhere (`src/dns_resolver.c`/`src/udp.c`
> unchanged; they run verbatim over the lwIP transport). LC-4 required two backend-only fixes to
> honor the cross-backend completion-TLS contract (feed ciphertext to `feed_input`; a synchronous
> send on server-accepted pcbs for `comp_tls_flush`) — `src/` untouched.

- **LC-0 — completion CONNECT contract (CROSS-BACKEND, foundational).** Add `KL_COMP_CONNECT`
  + `kl_comp_post_connect` to `completion.h`/`completion_dispatch.c`/`io_engine.h`/
  `completion_absent.c`; a `comp_on_connect` handler in `completion_driver.c`; the
  completion-vs-readiness connect branch in `src/client.c`. Implement the primitive on
  **pollcomp** (`connect`+POLLOUT+`SO_ERROR`), **io_uring** (`IORING_OP_CONNECT`), **IOCP**
  (`ConnectEx` — also fixes the currently-broken async client on IOCP).
  **Test:** the async `KlClient` does `GET /` over a completion loop to a completion server —
  on pollcomp (new `smoke-completion-client`) + the io_uring gate + IOCP under MinGW. **This
  stage does not touch lwIP** — it is the reusable contract, validated on the portable
  backends first. ASan-clean.
- **LC-1 — plaintext numeric-IP raw client.** `tcp_connect` + `connected_cb` in
  `lwip_raw_glue.c`; the lwIP-raw `kl_comp_post_connect`; client pcb → slot → send request →
  retained-recv response. **Test** (`integrations/lwip`, in-process, `NO_SYS=1`): a raw
  `KlClient` `GET`s a raw `KlServer` at `127.0.0.1` in the **same process** over the loopback
  netif → 200 + body verified. → `LC-1 PASS`. ASan+UBSan+LSan-clean.
- **LC-2 — Happy Eyeballs + timers over raw connect.** Multiple in-flight `tcp_connect` pcbs;
  first-connected wins + abort losers; Connection Attempt Delay + overall deadline timers
  (§4). **Test:** a resolved 2-address list (both loopback, one to a closed port) — the client
  fails over / races and still gets 200; deadline-timeout case aborts pcbs cleanly. →
  `LC-2 PASS`. ASan-clean.
- **LC-3a — `KlUdp` over lwip-raw (SHIPPED, inserted before LC-3).** The raw socket provider
  routes `SOCK_DGRAM → udp_new`; `kl_comp_post_udp_recv/send → udp_recv/udp_sendto` over a
  glue udp-slot table. KEEL's `src/udp.c` runs verbatim over the raw completion loop. This is
  the foundation for LC-3 so DNS rides KEEL's own UDP, not lwIP's. → `LC-3a PASS`. ASan-clean.
- **LC-3 — DNS via KEEL's `dns_resolver.c` on `KlUdp`-over-raw (SHIPPED — supersedes the
  `dns_gethostbyname` sketch below).** `kl_dns_resolver_create(ctx, cfg)` on a ctx with
  `ctx.sockets = kl_socket_provider_lwip_raw()` resolves over lwIP with ZERO `src/` changes:
  the resolver's `kl_udp_init`/send/recv become `udp_new`/`udp_sendto`/`udp_recv`, replies parse
  through `kl_dns_parse_response`, TCP-fallback rides the LC-1 `SOCK_STREAM` connect. One DNS
  path everywhere — no lwIP `dns_gethostbyname`, no second UDP stack. **Test:** an in-process
  DNS responder that is itself a `KlUdp` answers `A test.local`, the resolver returns it, and a
  full `KlClient GET http://test.local/` → 200; plus the resolver's deferred-literal fast path.
  → `LC-3 PASS`. ASan-clean. *(Original sketch — resolve_dns_lwip_raw.c over dns_gethostbyname —
  was rejected in review to avoid two separate UDP/DNS paths.)*
- **LC-4 — TLS over raw completion.** Reuse the 8b-5 memory-BIO leg (§6); no TLS code in the
  backend. **Test (local/hull, BYO mbedTLS — out of standard CI, like `smoke_tls_completion`):**
  a raw HTTPS `GET` to a raw TLS server in-process → handshake + 200. → `LC-4 PASS`.
- **LC-5 — caps + docs.** Advertise the outbound capability (§7); flip the Stage-C matrix rows
  (client / DNS / TLS) to Supported with the appropriate scope notes; update `raw_caps_test.c`,
  `keel_lwip_raw.h`, `integrations/lwip/README.md`, and cross-ref this doc from
  `phase9_lwip_raw_design.md` / `pal_transformation_design.md` (Phase 10 row) / `pal_review.md`.

Each stage rides a `make -C integrations/lwip <target>` in the `lwip` CI job (LC-1..LC-3,
LC-5), plus the cross-backend LC-0 gates in the main matrix (pollcomp/iouring/iocp). LC-4 is a
local/hull gate.

---

## 9. Testability (everything gated in-process over loopback)

Per the repo rule (`docs/phase9_lwip_raw_design.md`, "Testability gate"), every stage is
verified **in-process over the loopback netif** in the Apple `container` Linux VM + CI — no
tap device, no root, no second host:

- **The core setup: a raw KEEL client → a raw KEEL server, SAME process, `NO_SYS=1`.** One
  `KlEventCtx` (= the one lwIP mainloop) runs both a `KlServer` (Phase-9 accept/recv/send) and
  a `KlClient` (Phase-10 connect/send/recv). Each mainloop tick drives `sys_check_timeouts()`
  + `netif_poll()`; the client's `connected_cb`/`recv_cb` and the server's
  `accept_cb`/`recv_cb` all fire inline on that one thread — the request round-trips over
  `127.0.0.1` with no data race (the same single-thread marshalling Phase 9 already uses for
  its test client, `docs/phase9_lwip_raw_design.md`, P9-2 "NO_SYS=1 single-thread").
- **Per-stage verification:** LC-0 on pollcomp/iouring/iocp (portable, real fds — proves the
  contract before lwIP); LC-1 plaintext round-trip; LC-2 the 2-address race + deadline; LC-3
  the raw-DNS resolve; LC-4 the TLS handshake (local/hull). Each prints `LC-n PASS`.
- **Sanitizers:** built + run under `-fsanitize=address,undefined
  -fno-sanitize-recover=all` with `ASAN_OPTIONS=detect_leaks=1` → zero findings, as P9-4
  required. The connect/abort-loser and connect-failure paths are the new memory-safety
  surface (freeing half-open pcbs, `tcp_err`-before-connect owner keying) — LSan/ASan must
  cover them explicitly.

No new test infrastructure beyond a client-side counterpart to the existing raw test client
(`integrations/lwip/lwip_raw_testclient.c`).

---

## 10. Scope / limits (what stays unsupported)

- **IPv4-only.** The loopif is IPv4; `tcp_connect` uses an IPv4 `ip_addr_t`. IPv6 stays
  unsupported (same as Phase-9 server).
- **Single raw context, single-thread, `NO_SYS=1`.** One active raw ctx per process
  (`kl_lwr_ctx_create` rejects a second, `lwip_raw_glue.h`); KEEL's loop *is* the lwIP
  mainloop. No lwIP thread.
- **No UDP client / QUIC / HTTP-3.** UDP is unsupported on raw (Stage-C). (The raw-DNS
  resolver uses lwIP's *own* UDP core internally via `dns_gethostbyname`, not KEEL's `KlUdp`.)
- **No client-side WebSocket / HTTP-2 over raw initially.** `websocket_client.c` /
  `h2_client.c` have their own connect (`docs/pal_transformation_design.md` lists them as
  separate connect-dup sites); routing them through `kl_comp_post_connect` is follow-on work,
  not in LC-1..LC-5.
- **TLS: buffered HTTP/1.1 only** (8b-5 subset) — no TLS file/stream bodies, no ALPN-h2.
- **Sync blocking client (`kl_client_request`) is nonsensical on `NO_SYS=1`** (no blocking
  I/O, no host resolver) and is not supported on raw; the async `KlClient` on the shared
  `KlEventCtx` is the only client surface.

---

## 11. Open questions / risks (for review)

1. **[LOAD-BEARING] Confirm the connect-completion contract extension (§3).** Adding
   `KL_COMP_CONNECT` + `kl_comp_post_connect` is a **cross-backend** change (pollcomp,
   io_uring, IOCP, lwIP-raw) + a real refactor of `src/client.c`'s connect path. Agree it is
   the right call vs. the rejected fake-WRITE-watcher hack? Agree it must land + be green on
   pollcomp/iouring/iocp (LC-0) **before** the lwIP-raw client (LC-1)?
2. **`src/client.c` completion branch scope.** The client currently assumes readiness watchers
   throughout connect (Happy Eyeballs, single-fd, proxy-CONNECT). How much of that must be
   dual-pathed (readiness vs completion) vs. can the readiness path stay untouched and only a
   new completion branch be added? Risk: the proxy-CONNECT sub-state and request-streaming
   paths also touch the fd — do they need completion equivalents for a HTTP-boot client, or is
   plain `GET` enough for the motivating consumer (likely yes → keep LC-1 minimal)?
3. **IOCP async-client bonus vs. risk.** LC-0's `ConnectEx` implementation *fixes* the
   currently-broken async client on IOCP — good, but it means LC-0 must be validated on IOCP
   under MinGW (BYO/limited CI). Acceptable, or should IOCP's connect be a follow-on and LC-0
   gate only pollcomp+iouring?
4. **`lwr_sock_connect` fate (§7).** Keep it `ENOTSUP` and route all client connects through
   `kl_comp_post_connect` (recommended, cleaner), or make it a `EINPROGRESS` no-op? Confirm.
5. **Raw-DNS server availability in CI.** LC-3 needs a DNS responder over loopback under
   `NO_SYS=1`. Is a tiny in-process raw UDP DNS stub acceptable, or should LC-3 rely only on
   `dns_gethostbyname`'s literal/cached fast path (keeping the async-callback path
   local/hull-only)? Affects how fully the sync-completion contract is exercised in CI.
6. **Capability advertisement granularity (§7).** One "outbound supported" bit, or per-feature
   (plaintext / TLS / DNS) so a caller can query exactly what the raw client can do? The
   Stage-C honesty principle (fail early + clearly) argues for enough granularity that a
   caller never gets a silent hang.
7. **Response size / TX bounds for the client.** The server's `KL_LWR_RX_MAX` (64 KiB)
   bounds the response; a HTTP-boot image can be many MiB. Is 64 KiB per-conn receive with
   `ERR_MEM` backpressure + streaming consumption sufficient (the response body is *consumed*
   as it arrives, so the retained bound is a window, not the total), or does the client path
   need a different/larger bound? Confirm the streaming-consume assumption holds for the
   client response reader.

---

*Implemented — LC-0..LC-5 shipped (PRs #191–#196; LC-3a #194). See the STATUS banner in §8 for
the per-stage record and the LC-3 design change (KEEL `dns_resolver.c` over `KlUdp`-on-raw
instead of lwIP `dns_gethostbyname`). Authoritative capability matrix:
`integrations/lwip/keel_lwip_raw.h` + `integrations/lwip/README.md`.*
