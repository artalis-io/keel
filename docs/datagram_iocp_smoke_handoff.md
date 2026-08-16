# Handoff — Windows IOCP `smoke-udp` teardown hang (RESOLVED — 7B-7 IOCP firm ✅)

**Status: RESOLVED.** Root cause found via the `KL_IOCP_TEARDOWN_TRACE` instrumentation run on the real
Windows IOCP CI runner (no local Windows rig needed): a **send-only KlUdp socket** (`kl_udp_send_to` with
no `kl_udp_recv_start`) was **never associated with the IOCP completion port** — association lived only in
`kl_udp_recv_start`. So its `WSASendTo` completion had nowhere to post (never reaped during the pump), and
`closesocket` could not post an aborted completion for an unassociated socket either, so
`iocp_quiesce_port_for_close`'s `INFINITE` wait blocked forever on that one `DGRAM_SEND` op. **Fix
(9dec1d9):** associate the socket once at `kl_udp_init()` (completion mode), covering send-only + recv;
drop the redundant `recv_start` association. The instrumentation was reverted (0de075c). CI `windows-iocp`
`smoke-udp` **and** `smoke-datagram` are green on real IOCP → IOCP §10 is firm ✅. The trace evidence
(below) is kept for the record; the INFINITE drain was NOT bounded as a speculative fix.

---
_Original handoff (investigation record):_

The Windows IOCP runtime gate ran for the first time
(PR #240) and the `smoke-udp` step HANGS in teardown, so `smoke-datagram` (the public KlDatagram-over-IOCP
proof) never runs. Needs debugging on a **real Windows / IOCP** environment — it does not reproduce on
macOS/Linux (pollcomp/io_uring pass) and must NOT be "fixed" by bounding the infinite drain speculatively
(that would convert a visible hang into leaked ops / a use-after-free during teardown).

## Repro
```
make OS=windows BACKEND=iocp CC=gcc smoke-udp
```
(MinGW64 / msys2, on `windows-latest`.) The binary links fine, runs `./tests/smoke_udp.exe`, prints
**nothing**, and is killed by the 3-minute step timeout ("Terminate batch job (Y/N)?").

## What is and isn't hanging
- `tests/smoke_udp.c`: two `KlUdp` (rx bound to 127.0.0.1:0, tx), `recv_start`, `send_to`, then a
  **bounded** pump `for (i<200 && g_got<1) kl_event_ctx_run(&ctx,16,10)` (≈2 s max), then
  `kl_udp_free(&tx); kl_udp_free(&rx); kl_event_ctx_free(&ctx);` then the `printf`.
- The IOCP drain **honours its timeout** (`event_iocp.c:779`, `GetQueuedCompletionStatusEx(..., (DWORD)timeout_ms, ...)`),
  and the pump is bounded — so the pump **cannot** be the 3-min hang. No output appears because every
  `printf/fprintf` is AFTER the teardown calls. **Therefore the hang is in teardown**
  (`kl_udp_free` and/or `kl_event_ctx_free`), not the round-trip.
  - First empirical step to confirm: add a `fprintf(stderr,...)+fflush` right after the pump loop and
    before each teardown call to pin the exact hanging call. (Whether `g_got` reached 1 also tells you if
    the datagram was even delivered over IOCP — `smoke-udp` on the **WSAPoll** job succeeds, so the
    WSARecvMsg/WSASendTo logic itself is fine; this is completion-teardown-specific.)

## Suspected path
`kl_udp_free` / `kl_event_ctx_free` → op cancellation → **`iocp_quiesce_port_for_close`** (`src/event_iocp.c` ~line 1009).
It `CancelIoEx`s every op in the global registry `st->ops`, then drains with an **`INFINITE`** wait
(`GetQueuedCompletionStatusEx(..., INFINITE, ...)`, `event_iocp.c:1022`) in `while (st->ops)` until the
registry empties. If a UDP overlapped op's cancelled completion never dequeues — e.g. the datagram
recv/send op is (a) left in `st->ops` after `kl_udp_free` closed its socket, (b) never `CancelIoEx`-reached,
or (c) its completion is dequeued but the op is never unlinked from `st->ops` — this loop blocks forever.
The KlUdp teardown is the legacy path (it does NOT call `cancel_dgram`), so the interaction to inspect is
`kl_udp_free`'s socket close + overlapped-op accounting vs. the port-quiesce registry, NOT the 7B-2c
`cancel_dgram`/`retire_dgram` seam.

## Suggested tracing (on the Windows runner)
Instrument `iocp_quiesce_port_for_close` (and `kl_udp_free`'s close) to log, to stderr with `fflush`:
- for each op at entry: `op->type`, `op->op_sock`, and its datagram `life` pointer (recv/send);
- each `CancelIoEx(sock, &op->ov)` return value **plus `GetLastError()`** (esp. `ERROR_NOT_FOUND`);
- every completion dequeued in the `while (st->ops)` loop (`op->type`, socket, bytes, overlapped status);
- the **remaining `st->ops` registry entries** (type + socket) at the moment progress stops (i.e. when the
  loop is about to block on the next `INFINITE` wait with a non-empty list).
The registry-vs-dequeue mismatch (an op that's in `st->ops` but whose completion never posts) is the bug.

## Evidence (PR #240, run 31971146006)
- Run: https://github.com/artalis-io/keel/actions/runs/31971146006
- **Windows (MinGW / WSAPoll)** — PASS, incl. `smoke-datagram` (public KlDatagram roundtrip):
  https://github.com/artalis-io/keel/actions/runs/31971146006/job/95223688785
- **Windows (IOCP)** — FAIL: `smoke-udp` times out (teardown hang) → `smoke-datagram` **skipped**:
  https://github.com/artalis-io/keel/actions/runs/31971146006/job/95223688800
  - IOCP steps that PASS before it: build (IOCP link gate), `test-win-iocp`, `smoke-iocp` (HTTP),
    `smoke-iocp-tls`, `smoke-iocp-async`. Only the UDP/datagram completion teardown hangs.

## Confirmation
The public **KlDatagram-over-WSAPoll** `smoke-datagram` PASSES; the IOCP `smoke-udp` hangs in teardown
**before** the IOCP `smoke-datagram` step runs. So the facade + protocol logic is sound; the blocker is
purely the IOCP overlapped-op teardown accounting. IOCP remains provisional until a real-Windows
`smoke-udp`→`smoke-datagram` run is green.

## Not this investigation (tracked separately)
- **Linux (no completion)** websocket_client crash — a pre-existing regression (branch was already CI-red
  at `5e290ac`, last green `3edd29aa`). Bisected/handled separately; do NOT fold into the IOCP fix.
- **Static Analysis (cppcheck)** style findings in `src/dns_resolver.c` (`unusedStructMember`,
  `knownConditionTrueFalse`) — separate; blocks a fully-green PR but not the IOCP gate.
