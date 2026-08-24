# Phase 8, IOCP completion backend, Design

**Status:** designed, implementation gated. Depends on Phase 7 (the event/socket
capability negotiation and the reserved `KL_EVENT_CAP_COMPLETION`). Additive, **very
high risk**, the first backend on a different event *model*.

**Scope decisions (locked with the user):**
- **True completion axis.** IOCP owns the overlapped buffers; the socket provider
  posts overlapped `WSARecv`/`WSASend`; a completion carries the data. We do **not**
  flatten completion into readiness (no `\Device\Afd` readiness trick, no
  zero-byte-read fake). This is §6's stated intent and what Phase 7's reserved
  capability was built for.
- **Foundational subset first (8a).** Ship the IOCP backend + selection wiring +
  the core **plaintext TCP HTTP** path (accept → read → parse → buffered response
  write → close → keep-alive), with the Windows core test suite green on it. Defer
  TLS-over-IOCP, UDP-over-IOCP, streaming/`TransmitFile` responses, and
  completion-mode file I/O to **8b**. Mirrors the 6a-core → 6b-breadth staging.

**Overriding constraint (the whole point of the design):** the completion model is
**orthogonal** and **contained**. It must not percolate into orthogonal concepts
(the connection state machine, router, TLS seam, allocator), into other platforms
(POSIX/Winsock-readiness code never references a completion concept), or into
**Keel's public API** (no change to any `include/keel/*.h`). See §2, this is
enforced, not aspirational.

---

## 1. Why IOCP: and what it must not cost

WSAPoll (the current Windows backend) rescans the whole pollset every tick; O(n)
per iteration, the same ceiling `select`/`poll` hit everywhere. IOCP is the
scalable Windows primitive: sockets are associated with a completion port once, and
the kernel delivers *completions* (not readiness) for overlapped operations. That is
the only reason to add it.

The cost it must **not** incur: bending Keel's readiness-shaped core into a second
shape, or leaking Windows/completion concepts into the platform-independent layers.
IOCP earns its place only if the completion model stays sealed behind the same
internal seams Phases 1–7 already established.

---

## 2. Orthogonality: the enforced invariant

The event **model** (readiness vs completion) is one axis. It is orthogonal to, and
must not entangle with, every other concept:

| Concern | How it stays orthogonal to the completion axis |
|---|---|
| **Public API** | Zero change to `include/keel/*.h`. `KlEventLoop`, `kl_event_wait`, `KlSocketOps`, `KlConfig` are untouched. Completion lives only in internal `src/` seams. `KL_EVENT_CAP_COMPLETION` is already an *internal* (`src/event_caps.h`) flag. |
| **Connection state machine** | `kl_conn_on_readable`/`_writable` today mix "get bytes / put bytes" with "parse request / build response". Phase 8 splits them: the **protocol core** (parse/build, model-agnostic) is shared verbatim; only the **transport plumbing** (bytes in/out) has a completion variant. The core never learns which model drove it. |
| **Other platforms** | The IOCP TU (`src/event_iocp.c`) + the completion I/O driver are compiled **only** on Windows under `BACKEND=iocp`, selected in the Makefile; no `#ifdef` in shared `.c`/`.h`. POSIX event backends, `event_poll.c`, and the readiness driver are not touched and contain no completion concept. |
| **Socket provider identity** | Completion is a *capability* (`KL_SOCK_CAP_OVERLAPPED`), negotiated (§5), not a new provider class. The readiness Winsock provider is unchanged; the overlapped provider is a sibling. |
| **TLS / drain / router / allocator** | Not touched in 8a. TLS-over-completion is 8b and, when it lands, is contained to the completion driver + the mbedTLS BIO; never the router or the state machine. |

**Litmus tests (grep-assertable, part of the deliverable):**
- No `include/keel/*.h` diff in the Phase 8 changeset.
- No POSIX/readiness `.c` file references `KL_EVENT_CAP_COMPLETION`, `OVERLAPPED`,
  `iocp`, or any completion symbol.
- `src/event_iocp.c` and the completion driver appear only in the Windows/`iocp`
  branch of the Makefile object list.
- The protocol-core functions are called identically from both drivers.

---

## 3. The completion event model (internal)

A readiness backend answers "which fds can I act on?" (`kl_event_wait` →
`KlEvent{udata, ready}`). A completion backend answers "which operations I posted
have finished, and with what result?" These do not fit the same return shape, and;
per Phase 7's internal-first rule: we do **not** overload public `kl_event_wait`
to carry completions.

Instead, an **internal I/O engine seam** (new `src/io_engine.h`) abstracts "run one
tick" per model. The server loop calls it; it dispatches by the loop's capability:

```c
/* src/io_engine.h: INTERNAL. No ABI commitment. Selects the per-model tick +
 * the per-connection I/O driver, chosen by kl_event_caps(loop). F-safe: pulls
 * only the internal event/connection seams, never a public completion symbol. */
typedef struct KlIoEngine KlIoEngine;

/* Readiness engine  → wraps today's kl_event_wait + kl_event_dispatch, then calls
 *                     the readiness conn driver (recv/send via the socket seam).
 * Completion engine → GetQueuedCompletionStatusEx, maps each completion to its
 *                     (KlConn, op, bytes), then calls the completion conn driver. */
int kl_io_engine_run(KlServer *s, int timeout_ms);   /* one tick; both models */
```

The completion engine and its OVERLAPPED bookkeeping live entirely in
`event_iocp.c`; `io_engine.h` only names the neutral entry point.

---

## 4. The connection I/O driver: hiding readiness vs completion

This is the abstraction §6 asked for ("let the high-level connection interface hide
the difference, contained to the backend"). Today's readiness entry points feed the
state machine:

```
kl_conn_on_readable(c, router)  = recv into read_buf   (transport)  +  parse→handle (core)
kl_conn_on_writable(c)          = build/serialize (core)            +  send from write_buf (transport)
```

Phase 8 factors the **transport half** behind an internal driver, leaving the
**core half** (parse/handle, response serialization, the bulk of connection.c)
shared and model-blind:

```c
/* Readiness driver (POSIX/WSAPoll), unchanged behavior, just named:
 *   on EVENT_READ  : kl_sock_recv → read_buf ; then kl_conn_parse_and_handle(c, router)
 *   on EVENT_WRITE : kl_conn_serialize(c) → write_buf ; kl_sock_send(write_buf)
 *
 * Completion driver (IOCP), the loop already holds the bytes:
 *   on read-complete (n)  : read_buf already filled by the posted WSARecv ;
 *                           kl_conn_parse_and_handle(c, router) ; post next WSARecv
 *   on write-complete (n) : advance write_buf ; if more, post next WSASend ; else done
 *   on accept-complete    : init conn ; post first WSARecv ; re-post AcceptEx
 */
```

`kl_conn_parse_and_handle()` and `kl_conn_serialize()` are the shared core, the
same functions today's `kl_conn_on_readable/_writable` already contain, lifted so
both drivers call them. The completion driver never appears on POSIX; the readiness
driver never learns about OVERLAPPED. WebSocket/HTTP-2 (`kl_ws_server_on_readable`
etc.) stay readiness-only in 8a (they ride the readiness driver); their completion
variants are out of scope.

---

## 5. Phase 7 negotiation extension (COMPLETION ↔ OVERLAPPED)

Phase 7 promised the negotiation "generalizes": a completion loop "requires the
provider to route I/O through the loop's submit path." Phase 8 fills that arm.

- **New internal provider capability** `KL_SOCK_CAP_OVERLAPPED` (`src/socket.h`
  region; internal, not public), the provider posts overlapped ops instead of
  synchronous send/recv. The overlapped Winsock provider advertises it; POSIX does
  not.
- **`event_iocp.c` `kl_event_caps()`** returns `KL_EVENT_CAP_COMPLETION |
  KL_EVENT_CAP_NATIVE_FD` (SOCKETs are still native handles, associated with the
  port; the *model* is completion).
- **`kl_event_ctx_sockets_compatible()`** (async.c, from Phase 7) gains the
  completion arm, today it is:

  ```c
  /* readiness model (shipped): provider native-fd AND loop readiness+native-fd */
  ```

  becomes:

  ```c
  unsigned ev = kl_event_caps(&ctx->loop);
  if (ev & KL_EVENT_CAP_COMPLETION)
      /* completion model: provider must post overlapped ops through the loop */
      return kl_socket_provider_has_cap(ctx->sockets, KL_SOCK_CAP_OVERLAPPED);
  /* else readiness model: unchanged */
  return provider_native && loop_watches_fds;
  ```

  So a completion loop paired with a plain readiness provider is rejected at
  wire-up (and vice-versa), the guard stays truthful across both models. This
  change is confined to the one negotiation function; no call site changes.

---

## 6. IOCP mechanics (8a)

Standard, fully-documented Win32 (`ws2_32` + `mswsock` for `AcceptEx`/
`GetAcceptExSockaddrs`), no undocumented APIs (the AFD path we rejected):

- **Port:** `CreateIoCompletionPort` at `kl_event_init`; each accepted/listening
  SOCKET associated to it.
- **Per-op context:** an `OVERLAPPED` embedded in a small struct tagging `{op:
  ACCEPT|READ|WRITE, KlConn*, WSABUF}`; recovered on completion via
  `CONTAINING_RECORD(lpOverlapped, KlIocpOp, ov)`. A fixed pool of these, sized to
  `max_connections` × in-flight ops (bounded, no per-op malloc on the hot path).
- **Accept:** pre-post N `AcceptEx` calls; a completion yields a ready connection,
  init it, post its first `WSARecv`, re-post an `AcceptEx` to refill.
- **Read:** `WSARecv` with the connection's `read_buf` as the overlapped buffer;
  completion carries the byte count (0 = peer closed).
- **Write:** `WSASend` from `write_buf`; partial completion advances and re-posts.
- **Drain:** `GetQueuedCompletionStatusEx` returns a batch of
  `OVERLAPPED_ENTRY[]`; each maps to a driver call (§4). Buffer ownership: the
  connection owns `read_buf`/`write_buf` (as today); the backend only borrows them
  for the duration of an overlapped op; no extra copy.

Buffer lifetime and the "one outstanding read + one outstanding write per
connection" rule keep the OVERLAPPED pool bounded and the model simple.

---

## 7. Build, selection, CI

- **Makefile:** on Windows, `BACKEND=iocp` selects `EVENT_SRC = src/event_iocp.c`
  (+ the completion driver + overlapped provider objects) and links `mswsock`;
  `BACKEND` unset keeps WSAPoll (the default/fallback). Same pattern as the POSIX
  `BACKEND=poll/iouring` switch: selection in the Makefile, **no `#ifdef` in
  shared code**.
- **Provider default on IOCP:** when the IOCP backend is built, the server's
  default provider is the overlapped Winsock provider (so the negotiation in §5
  passes without the user configuring anything); a user may still pass one.
- **CI:** add a **Windows (IOCP)** job running the core suite against
  `BACKEND=iocp`, alongside the existing WSAPoll job, so both Windows event models
  are gated. WSAPoll stays the default job.

---

## 8. Deliverables (8a)

- `src/event_iocp.c` (new), the IOCP `KlEventLoop` backend + `kl_event_caps()`
  (`COMPLETION | NATIVE_FD`) + the OVERLAPPED op pool and completion drain.
- `src/io_engine.h` (new, internal): `kl_io_engine_run()`; the readiness impl
  wraps today's wait/dispatch, the completion impl lives in `event_iocp.c`.
- `src/connection.c`: lift `kl_conn_parse_and_handle()` / `kl_conn_serialize()`
  as the shared, model-blind core; add the completion driver entry points
  (`kl_conn_on_read_complete` / `_write_complete` / `_accept_complete`). Readiness
  behavior byte-identical.
- Overlapped Winsock provider, a sibling of `socket_winsock.c` advertising
  `KL_SOCK_CAP_OVERLAPPED` (posts `WSARecv`/`WSASend`); `socket.h` gains the
  internal `KL_SOCK_CAP_OVERLAPPED` bit.
- `src/async.c`, the completion arm in `kl_event_ctx_sockets_compatible()` (§5).
- `src/server.c`, the loop calls `kl_io_engine_run()` instead of inlining
  `kl_event_wait` + dispatch; identical behavior on readiness.
- `tests/test_iocp_engine.c` (new, Windows-only): accept/read/write/close/
  keep-alive over a real loopback IOCP server; negotiation accepts overlapped ⋄
  completion and rejects the cross-pairings.
- `Makefile` (BACKEND=iocp branch + link + a WIN test list for the IOCP job),
  `.github/workflows/ci.yml` (the Windows-IOCP job), `docs/pal_transformation_design.md`
  (status).

No `include/keel/*.h` in that list: by construction (§2).

---

## 9. Validation

- **Orthogonality litmus (§2)** all pass: no public-header diff; no completion
  symbol in POSIX/readiness TUs; IOCP objects only in the `iocp` Makefile branch.
- **POSIX/WSAPoll unaffected:** full `make test` green on every existing backend;
  `make debug-test` (ASan/UBSan) green; bench flat (the `io_engine` indirection is
  one call per tick, off the per-byte path).
- **IOCP path:** the new Windows-IOCP CI job runs the core suite green against a
  real completion loop; `test_iocp_engine` exercises accept/read/write/keep-alive.
- **Negotiation:** overlapped-provider ⋄ IOCP-loop accepted; readiness-provider ⋄
  IOCP-loop and overlapped-provider ⋄ readiness-loop both rejected.

---

## 10. Risks & stop conditions

- **The core/transport split is the crux.** If `kl_conn_parse_and_handle` /
  `kl_conn_serialize` cannot be lifted without the completion concept leaking into
  the shared core (§2), **stop**, the abstraction is wrong, and the fallback is to
  reconsider readiness-over-IOCP (the AFD path) as a lower-fidelity Phase 8. The
  orthogonality litmus is the tripwire.
- **TLS-over-completion (8b) is genuinely hard**: mbedTLS's BIO *pulls* bytes,
  which inverts against a completion push. Deferred deliberately; 8a is plaintext
  only. When 8b tackles it, it must stay contained to the completion driver + BIO,
  never the router/state machine.
- **OVERLAPPED buffer lifetime**, a completion referencing a freed connection
  buffer is a use-after-free. Bounded by the "one read + one write outstanding per
  conn" rule and releasing a conn only after its in-flight ops drain.
- **`GetQueuedCompletionStatusEx` batching + partial sends**: standard IOCP
  bookkeeping; covered by `test_iocp_engine`.

## 11. Out of scope (→ 8b or later)

- TLS-over-IOCP, UDP-over-IOCP, streaming/chunked + `TransmitFile` responses,
  completion-mode file I/O, WebSocket/HTTP-2 completion drivers.
- Any public API surface for completion (frozen until a second consumer needs it,
  the Phase 7 internal-first rule still holds).
- Making IOCP the Windows default (WSAPoll stays default; IOCP is opt-in via
  `BACKEND=iocp` until proven in the field).
