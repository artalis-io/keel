# Server socket-provider adoption — Design

Status: **stages 1–4 implemented (2026-07-20).** Follow-on to PAL Phases 1–3
(`docs/pal_transformation_design.md`). The server is now provider-aware for its
lifecycle, byte I/O, and the vectored/zero-copy response fast paths.

**Stage 3 (writev/sendfile).** `KlResponse` gains a `KlEventCtx *ctx`
back-pointer (bound with `conn_fd`/`tls`, preserved across reset; set for h2
streams too). `try_writev`/`stream_writev_all` route through a `seam_writev`
helper: the raw `writev()` when the provider has `KL_SOCK_CAP_WRITEV`
(POSIX/NULL — unchanged fast path), else a serialized `kl_sock_send` over the
iov. The non-TLS `KL_BODY_FILE` path uses `sendfile()` when the provider has
`KL_SOCK_CAP_SENDFILE`, else a `pread` + `kl_sock_send` fallback. POSIX
advertises both caps, so the default build is byte-identical — bench flat.

**The capability-gate uses the raw POSIX `writev`/`sendfile` syscall**, valid
only for native-POSIX-fd providers (POSIX, a mock over real fds). A future
non-POSIX native provider (Winsock) will instead add `writev`/`sendfile` **ops**
to the vtable (`WSASend`/`TransmitFile`) in Phase 6 — adding those ops now, with
no consumer, would be speculative. So today: POSIX fast path preserved; minimal
providers serialize.

**Stage 4 (tests).** `serialized_writev_fallback` (64 KB body round-trips
byte-correct through a provider with no `WRITEV` cap) and `sendfile_fallback`
(a ~40 KB file served via pread+send when `SENDFILE` is absent), both over a
real server with a native-fd/no-vectored mock provider; plus the stage-1/2
`explicit_posix_provider_roundtrip` conformance test.

**Implemented (stages 1+2):** `KlConn.ctx` back-pointer (set at pool init to
`&s->ev`); the server's `socket`/`bind`/`listen`/`accept`/`set_nonblocking`/
`set_cloexec` now pass `s->ev.sockets` (NULL default = unchanged); `conn_read`/
`conn_write` route through the seam (`conn_provider(c)` = `c->ctx->sockets`; NULL
= inline POSIX). Injection uses post-init `srv.ev.sockets` — no new public API
(covers `accept` + accepted-connection I/O; the listen socket keeps the default).
Conformance test drives a real server through the explicit POSIX provider.
`conn_write` now uses `send(MSG_NOSIGNAL)` (socket-only; `conn.fd` is always an
accepted socket) — one h2 unit test that used a pipe stand-in became a
socketpair. Bench flat (±3%, run-to-run noise). **Pending:** stage 3
(writev/sendfile capability-gating) + stage 4 (fault-injection tests). Makes `KlServer` a
provider-aware transport: its lifecycle and hot-path I/O flow through the
`KlSocketProvider` seam instead of raw syscalls, so a mock (fault injection) or a
future native-fd provider (e.g. Winsock) can back the server — the client
transports already do this; the server was deliberately deferred as the
throughput-critical path.

---

## 1. What the server touches today

Two layers, both currently bypassing or NULL-ing the seam:

**Lifecycle (setup, one-shot).** `server.c` already routes
`socket`/`bind`/`listen`/`accept`/`set_nonblocking`/`set_cloexec` through the
seam **but hardcodes a `NULL` provider** (passthrough). Sites: `96,103,117,225,
231,258,357,807,814,925,933,938`.

**Hot path (per-request / per-byte), NOT in the seam:**
- `internal.h` `conn_read`/`conn_write` — TLS-aware `read()`/`write()` on
  `c->fd`. The request-read path and small/streaming writes.
- `response.c` `try_writev` / `stream_writev_all` — **`writev()`** on
  `res->conn_fd`; the primary buffered + streaming response path (`454,503,628,
  661,683`).
- `response.c` `kl_sendfile_impl` — **`sendfile()`** zero-copy file responses
  (`553`); already skipped when TLS is active (`524`).

So "server adoption" is really: (a) flip the lifecycle `NULL`s to the server's
provider, and (b) bring `conn_read`/`conn_write`, `writev`, and `sendfile` into
the seam without regressing the hot path.

## 2. Hard constraint: the event loop polls real fds

`epoll`/`kqueue`/`poll` register the connection's **`int fd`** and wait on it.
That only works if the fd is a real OS descriptor — i.e. the provider advertises
`KL_SOCK_CAP_NATIVE_FD`. Therefore:

> **Server provider adoption is defined only for native-fd providers**
> (POSIX, a mock wrapping real socketpairs/loopback, and eventually Winsock).
> Non-native providers (lwIP raw, UEFI) cannot drive the *current* readiness
> event backends — that requires event-engine/socket capability negotiation
> (Phase 7) and is explicitly out of scope here.

Implementation asserts this at bind/accept: if `!kl_socket_provider_has_cap(p,
KL_SOCK_CAP_NATIVE_FD)`, fail init with `KL_ERR_SOCKET` (a provider that can't be
polled has no business under `KlServer` yet). This keeps the invalid combination
an explicit, early error rather than a mysterious hang.

## 3. Reaching the provider from KlConn / KlResponse — via a ctx backpointer

`KlConn` (connection.h) already carries "set-once-at-pool-init" shared state and
back-pointers (`alloc`, `router`, `h2_config`, `file_io`). Reach the provider the
same way, but through a **`KlEventCtx *` backpointer**, not a dedicated provider
field:

- `KlConn.ctx` (`KlEventCtx *`) — set at pool init to `&server->ev`. Hot-path
  reads `c->ctx->sockets`.
- `KlResponse.ctx` (`KlEventCtx *`) — set when the response is bound to the
  connection (where `res->conn_fd`/`res->tls` are already stamped per request).
  `try_writev`/`stream_writev_all`/sendfile read `res->ctx->sockets`.

**Why the backpointer beats a `sockets` field:** the `KlSocketProvider` tag then
appears in *no* public header — the provider dependency stays entirely behind
`KlEventCtx` (which already forward-declares it). `connection.h`/`response.h`
store only a `KlEventCtx *`, a type they can already name. It also generalizes:
the ctx is the natural handle for any future per-connection platform state, and
`KlConn` already holds analogous back-pointers. NULL `ctx->sockets` = POSIX.

## 4. Bringing the hot path into the seam

### 4.1 conn_read / conn_write
Mechanical: the non-TLS branch calls `kl_sock_recv(c->sockets, …)` /
`kl_sock_send(c->sockets, …)` instead of `read`/`write`. The `if (c->tls)` branch
is unchanged. NULL provider → inline POSIX path (today's code), so **no hot-path
delta** for the default build.

### 4.2 writev (the main response path) — capability-gated
`writev` is vectored and has no per-op equivalent in the seam. Two options; the
design picks **B**:

- **A. Add a `writev` op to `KlSocketOps`.** Faithful, but grows every provider
  and most providers just want the POSIX behaviour.
- **B. Capability-gate the POSIX fast path, serialize otherwise.** Add
  `KL_SOCK_CAP_WRITEV` (POSIX sets it). `try_writev(p, fd, tls, iov, n)`:
  - TLS branch unchanged (already loops `tls->write`).
  - else if `p == NULL || has_cap(p, WRITEV)` → real `writev(fd, iov, n)`
    (unchanged fast path — the common case).
  - else → serialize: `kl_sock_send(p, fd, iov[i]...)` across the vector,
    returning the same short-count contract.

  This preserves the vectored fast path verbatim for POSIX and gives non-writev
  providers (a minimal mock) a correct fallback, without bloating the vtable.
  Same treatment for `stream_writev_all`.

### 4.3 sendfile — capability-gated
Add `KL_SOCK_CAP_SENDFILE` (POSIX sets it). Response file path:
- if TLS → existing userspace copy path (unchanged; sendfile can't see TLS).
- else if `p == NULL || has_cap(p, SENDFILE)` → `kl_sendfile_impl` (unchanged).
- else → fall back to the **existing** async/read-then-send file path
  (`file_io` / buffered read + `kl_sock_send`), which already exists for the
  io_uring and TLS cases.

No new syscall wrappers cross the seam; `writev`/`sendfile` stay POSIX
fast-paths, gated so a non-POSIX provider degrades instead of miscompiling.

## 5. Lifecycle flip

Replace the `NULL`s in `server.c` with the server's provider (`s->ev.sockets`)
for `socket`/`bind`/`listen`/`accept`/`set_nonblocking`/`set_cloexec`. The
accepted client fd is set up via the provider and the new `KlConn` is stamped
with it. `KlConfig.listen_fd` adoption (socket-activation / systemd) keeps the
default provider — an inherited fd is a real OS descriptor.

## 6. Performance plan

The default build uses a NULL provider everywhere, so every seam call is the
inline POSIX branch — `conn_read/write`, `try_writev`, `sendfile` compile to
today's code. Expectation: **bench flat**. Required: re-run `make bench` before/
after (4-endpoint wrk, incl. `POST /echo` which exercises the read + writev path,
and add a `sendfile` static-file endpoint to the bench if not covered). Any
measurable regression on the NULL path is a blocker — the branch must be
predicted-not-taken and the writev/sendfile fast paths byte-identical.

## 7. Testing

- **Conformance:** run a real `KlServer` with `ctx.sockets =
  kl_socket_provider_posix()` (explicit POSIX) and assert identical behaviour to
  the default NULL path — a full request/response, a static-file (sendfile) response,
  and a streaming response.
- **Fault injection (mock over real fds):** a mock provider wrapping real
  accepted sockets that forces short `writev`/`send` and `EWOULDBLOCK` on the
  response path; assert the server buffers/retries and the client still receives
  the whole body (drain/backpressure correctness under the seam).
- **Capability gating:** a mock with neither `WRITEV` nor `SENDFILE`; assert the
  serialized fallbacks produce byte-identical responses.
- **Native-fd guard:** a non-native mock → `kl_server_init` fails with
  `KL_ERR_SOCKET` (explicit unsupported-combination error).
- Full gauntlet incl. the **musl** leg; ASan/UBSan on the fault-injection tests.

## 8. Staged delivery (small, reviewable commits)

1. **Plumb the provider** — `KlConn.sockets` + `KlResponse.sockets` set at pool/
   response init; flip the `server.c` lifecycle `NULL`s to `s->ev.sockets`; add
   the native-fd guard. No hot-path change yet. (Bench flat by construction.)
2. **conn_read/conn_write** through the seam. Re-bench.
3. **writev + sendfile** capability-gating (`KL_SOCK_CAP_WRITEV`,
   `KL_SOCK_CAP_SENDFILE`) + serialized fallbacks. Re-bench (the sensitive step).
4. **Tests** — conformance + fault injection + capability + native-fd guard.

Stop after step 1 if plumbing proves noisier than expected; stop before step 3
if step-2 bench shows any regression that can't be eliminated.

## 9. Risks & stop conditions

- **Hot-path regression** on the NULL path → stop, this whole adoption is
  net-negative if it costs the server throughput. (Primary risk.)
- **writev/sendfile semantics drift** in the serialized fallback (short-count,
  partial-iov accounting) → covered by the byte-identical conformance tests;
  if the fallback can't match, keep those ops POSIX-only and document the
  provider requirement instead.
- **KlConn/KlResponse ABI growth** — one `KlEventCtx *` back-pointer appended to
  each (additive/source-compatible; `KlConn` already holds `router`/`alloc`
  back-pointers). Chosen over a dedicated `sockets` field so the provider tag
  stays out of the public headers. One extra indirection on the hot path
  (`c->ctx->sockets`) vs a direct field — negligible (same cache line as `fd`,
  and NULL-tested once).
- **Non-native provider under the server** → early explicit failure, never a
  silent hang.

## 10. Sequencing — now vs after Phase 4/6, and the Winsock/IOCP split

This adoption is **independent of Phase 4** (public API): tests inject via the
internal `ctx.sockets` today, so nothing here needs the public selection API. It
is a *prerequisite-side* step for Phase 6 — do it now and the server is already
provider-ready when a Winsock provider lands.

The decisive caveat is **readiness vs completion**:

- The server's hot path (this design) is **readiness-shaped** — the event loop
  waits for `fd` ready, then calls `kl_sock_send/recv/writev`. Adopting the
  socket provider here makes the server run over any **native-fd, readiness-mode**
  provider: POSIX, a mock, and a **Winsock provider driven by WSAPoll/select**.
  That is exactly the "simpler Winsock first" path the PAL roadmap (Phase 6
  Option A) recommends — *"do not require IOCP in the same phase."*
- **IOCP does not benefit.** IOCP is **completion-shaped** (submit op → get
  completion), which is the *event axis*, not the socket-provider axis. A
  socket provider swapped under a readiness loop cannot turn it into IOCP; that
  needs event-engine capability negotiation (Phase 7) and a dedicated IOCP
  backend (Phase 8), where the server would grow a completion path.

**So:** if the near-term Windows target is **readiness-mode Winsock (WSAPoll)**,
doing server adoption now is worth it — it lands the fault-injection robustness
immediately and makes the server drop-in for that provider. If the target is
specifically **IOCP**, server adoption over the readiness path does *not* move
you toward it; the higher-leverage work is the event-axis (Phase 7/8) first, and
the server's completion adoption comes with the IOCP backend. Recommendation:
proceed now **if** WSAPoll-Winsock is an acceptable first Windows milestone
(it usually is — correctness before peak throughput); otherwise sequence the
event-axis first.

**Out of scope here:** public provider selection (`KlConfig.sockets`, Phase 4);
event-engine/socket capability negotiation for non-native stacks (Phase 7);
IOCP's completion model + the server's completion path (Phase 8).
