# C Audit Report: KEEL

> **Historical, append-only evidence log: newest pass first.** Each pass records what was verified
> on its date; verify any specific claim against current code before acting on it. Current-state
> docs: [architecture.md](../../architecture/overview.md), [architecture_invariants.md](../../architecture/invariants.md).
> Index: [audits/README.md](README.md).

## Twenty-second pass: PRs #542 and #543 before merge (2026-11-27)

**Revision:** PR #542 head `1d9f5d0b` (part A) and PR #543 head `5586facf` (part B), each over
`main` `b43d3110`. Two independent read-only audits, with the axis audit of #543 alongside
(twenty-first pass in [keel_axis_audit.md](keel_axis_audit.md)). Both PRs had passed their reviews
and CI; neither was merged before these results were reported.

**Result:** no Critical or High in either PR. The audits found:
- **A proven regression** in #543 (axis A-M1, Medium): on readiness loops the graceful drain's
  flush left producer output it could not send without WRITE interest. Reproduced on kqueue and
  epoll; `main` with the flush timer did not have it.
- **A pre-existing memory-safety defect** outside both PRs (P1): the POSIX SIGTERM/SIGINT handler
  could stop a server its caller had freed. Reproduced (SIGSEGV) with a widened window. Its
  practical severity is Low (it needs local signal delivery concurrent with teardown), but it is a
  use-after-free and is fixed, not deferred.
- **An inaccurate premise** in #542 (I1): on the Windows build tested, the console dispatcher already
  serialises the restore with a running handler, so the use-after-free #542 guards against was not
  reproduced through the real dispatcher. #542's lock remains as defence in depth against behaviour
  Windows does not document; its comment, CHANGELOG entry and description now say so, and nothing
  here records the Windows race as reproduced.
- **Lows and documented behaviour:** a two-server lifecycle defect in #542 (C1), four Lows in #543
  (one cost, one weakened guard, two pre-existing behaviours), a Windows deadlock with an
  application's own console handler (W1, pre-existing) and Windows' process termination on close,
  logoff and shutdown (W2, documented OS behaviour).

**Classes used below:** *proven* (reproduced, against the audited head unless it says otherwise);
*pre-existing* (present on `main` before the PR); *theoretical* (no caller reaches it today);
*documented caller contract* or *documented platform behaviour* (the behaviour is specified, by Keel
or by the OS).

### Corrections after the pass

All were made on the PR branches (and one new branch) and verified before the PRs were updated. No
full re-audit was run; each correction had a focused review.

| Finding | Correction | Verification |
|---|---|---|
| A-M1 (axis) | `kl_http2_server_drain_shutdown` flushes through `h2_flush_outside`, which asks for WRITE while output is held | `h2.output_held_by_the_drain_flush_is_sent_without_client_traffic`: WRITE is armed after the drain's flush, and the connection's writable event delivers the producer's bytes with no frame from the client; fails with the old line |
| 543-L1..L4, H1 | see part B, section 4 | each new test fails with its fix reverted |
| C1 | the Windows restore clears the pointer only while it is its own | `ctrl_handler.a_restore_leaves_a_later_server_its_events` |
| I1, W2, T1 | #542 comment, header (`install_signal_handlers`), CHANGELOG and description reworded | review |
| P1 | separate PR: the handler counts itself before it loads the server; the restore clears the pointer, then waits for the count to reach zero; the old dispositions are saved once and restored by the server receiving signals | `signal_handler.the_restore_waits_for_a_handler_that_saw_the_server` (a real SIGTERM sent to a chosen thread) and `signal_handler.overlapping_servers_put_the_original_dispositions_back`; each fails against the pre-fix code, and the first also with the count taken after the load. The audit's probe: SIGSEGV before, clean after (5 of 5) |
| W1 | not changed; see below | |

**P1 correction, why the count closes the race.** The handler's increment, its load of the server,
the restore's store of NULL and the restore's read of the count are all sequentially consistent. A
handler that loaded a server did so before the store of NULL in that single order, and its increment
came before its load, so the restore's read, which follows the store, sees the increment until the
handler's decrement, which follows its `kl_http_server_stop`. A handler that loads after the store
sees NULL (or a later server) and touches nothing. The handler never waits, so a signal that
interrupts the restoring thread runs to completion before the wait resumes. Both atomics must be
lock-free: 0 fails the build, 1 is checked at runtime by the install, which then leaves the handlers
uninstalled. Install and restore themselves take a mutex the handler never touches. The review of
the correction found no defect; from its theoretical notes, the wait yields and then sleeps (so a
real-time restoring thread cannot starve a preempted handler on its CPU), and the comment records
two limits: a fork while a handler runs on another thread leaves the child a count that never
drains, and only the last server to install receives signals.

**W1 investigation.** Cause confirmed by the audit's probe: the console dispatcher holds its own lock
across every handler call, and `SetConsoleCtrlHandler(..., FALSE)` in the restore waits for it, so an
application handler that is called first and waits for `kl_http_server_run` to return deadlocks with
the restore. Smallest structural fix: register Keel's handler once per process and never unregister
it from the loop thread; the restore only clears the pointer under the lock (which #542 adds), and
the handler returns FALSE with no server, so the event passes on. That also lets a later change wait
in the handler for a graceful close (W2). Not implemented: it changes the handler's position in the
chain and its lifetime, and is left for a decision.

### Part A: PR #542, Windows console control handler lifetime

**Revision:** PR #542 head `1d9f5d0b` (on `main` `b43d3110`); the change is
`git diff origin/main...HEAD`. Read-only; no repository file was changed by the audit. Probes and
scratch builds ran from a `git archive` copy on edge and from scratch copies of the two platform TUs.
**Scope:** `src/protocols/http/http_server_plat_win.c` (the console handler, install and restore),
its two callers in `kl_http_server_run` (`http_server.c:428` failed start, `:818` normal return),
everything the handler calls under the new lock (`kl_http_server_stop`, `kl_http_server_stop_claim`
/ `_publish`, `kl_plat_wakeup_signal` in `platform_wakeup_win.c`), `kl_http_server_free`'s wakeup
teardown, the new test `tests/test_ctrl_handler_win.c`, its Makefile rule under MinGW and MSVC
(`mk/toolchain.mk`), the derived suite lists, and the POSIX sibling
`http_server_plat_posix.c` (unchanged by the PR, audited for the same defect class).

**Result:** no Critical, High or Medium defect in the change. The lock is correct: every access to
`kl_signal_server` is under `kl_signal_lock`, nothing under the lock blocks, re-enters or takes the
lock, and `SetConsoleCtrlHandler` is never called with it held. Two observations change how the PR
should be described, and one pre-existing defect of the same class remains on POSIX:
- **On the tested Windows build the OS already serialises the restore with a running handler**
  (I1). The console dispatcher holds its handler-list lock across the handler call, so
  `SetConsoleCtrlHandler(..., FALSE)` waits for it. The pre-fix code, with a 300 ms gap widened
  between the load and the stop and driven by a real console event, never faulted. The fix is
  sound defence in depth against behaviour Microsoft does not document, but on this host #539 is
  not reachable through the real dispatcher. The CHANGELOG sentence "did not wait for a handler
  that was already running" holds only as an absence of any guarantee.
- **The POSIX signal handler has the defect #539 describes, and the PR does not touch it** (P1).
  Reproduced with a widened window: SIGSEGV.

#### Findings

| ID | Class / severity | Location | Finding | Reproduction | Smallest fix | Disposition |
|---|---|---|---|---|---|---|
| P1 | Theoretical in production, proven with a widened window. Low (narrow window; the consequence is a use-after-free write plus a 1-byte `write()` to a possibly reused fd) | `http_server_plat_posix.c:54-55` (handler) and `:82-84` (restore) | A process-directed SIGTERM/SIGINT can run on any thread that does not block it. If that thread is preempted between `atomic_load(&kl_signal_server)` and `kl_http_server_stop(s)`, the loop thread's restore (`sigaction` back, then pointer cleared) neither waits for it nor can tell it is there. `kl_http_server_run` returns, the caller frees the server, and the handler then writes `stop_requested`/`running`/`draining` into freed memory and `write()`s to `stop_wake_wr`, an fd number that may already belong to something else. Unlike Windows (I1), nothing in the OS serialises this. Pre-existing; the PR fixes only the Windows sibling. | `probe_posix_uaf.c`, macOS kqueue: a scratch copy of the POSIX TU with a 300 ms `nanosleep` between load and stop, linked ahead of `libkeel.a`. SIGTERM is unblocked on one thread only, the caller then stops, joins, frees and `munmap`s the server: exit 139 (SIGSEGV in the handler). | An async-signal-safe in-flight count. Handler: `atomic_fetch_add(&inflight,1)`, load, stop, `atomic_fetch_sub`. Restore: clear the pointer, then spin while `inflight != 0`. A handler that interrupts the restoring thread itself runs to completion before the spin resumes, so the spin cannot self-deadlock. `atomic_int` is already required lock-free here. | fixed by a separate PR (`fix/posix-signal-lifetime`); fixed rather than deferred, see the pass summary |
| W1 | Proven (pre-existing, not introduced by the PR). Low | `http_server_plat_win.c:85` (restore calls `SetConsoleCtrlHandler`) | Because the dispatcher holds its lock across handler calls (I1), the restore blocks while any console handler in the chain is still running. Take an application handler registered after the server installed its own (so it is called first) that stops the server and waits for `kl_http_server_run` to return. That is the usual way to get a grace period on `CTRL_CLOSE_EVENT`, see W2. The run is now stuck in restore, waiting for that handler, which is waiting for the run: deadlock. With a bounded wait it ends at the timeout; with `INFINITE`, or until Windows kills the process on close, it does not. With `install_signal_handlers = 0` the same handler sees the run return at once. | `probe_userwait.c` on edge: "run DID NOT RETURN within 4 s (blocked in restore)" with handlers installed; "run returned" without. | Document it at `install_signal_handlers`: a console handler of the application's own must not wait for `kl_http_server_run` while Keel's handler is installed. Use one or the other. (A structural fix would be to not unregister from the loop thread, and leave the handler registered for the process lifetime with the pointer cleared under the lock. The lock this PR adds is exactly what makes that safe.) | investigated; open (structural fix proposed, see "W1 investigation" above) |
| W2 | Pre-existing; documented OS behaviour. Informational to Low | `http_server_plat_win.c:46-57` | For `CTRL_CLOSE_EVENT`, `CTRL_LOGOFF_EVENT` and `CTRL_SHUTDOWN_EVENT`, Windows ends the process once the handler returns. The handler returns `TRUE` right after publishing the stop, so the graceful drain the PR's comments and the CHANGELOG imply for "close, logoff or shutdown" does not run: in-flight requests are cut, `on_cancel` is not delivered, and an owned AF_UNIX node is not unlinked. `CTRL_LOGOFF`/`SHUTDOWN` also reach console processes only in some configurations. | Not driven (closing the console over ssh is not possible); from the `SetConsoleCtrlHandler`/`HandlerRoutine` documentation. | Either document that only Ctrl-C/Ctrl-Break drain gracefully, or wait in the handler (outside `kl_signal_lock`, bounded by the OS grace period) for an event the run sets after its drain. Waiting needs the restore not to unregister from the loop thread first, or it deadlocks exactly as W1 does. | documented in #542 |
| C1 | Documented caller contract ("single instance only; only the last server to call `kl_http_server_run()` receives signals"), but the contract text is inaccurate. Low | `http_server_plat_win.c:69-90`; `include/keel/http_server.h:124-125` | With two servers, A's restore clears the single pointer even though B installed last. B's registration is still there but finds `NULL` and returns `FALSE`, so Ctrl-C falls through to the default handler (ExitProcess) while B runs. So the last server stops receiving console events as soon as any earlier server returns, contrary to the header. Concurrent install(B) and restore(A) can interleave the same way. POSIX is similar: A restores the `sigaction` it saved, possibly the default, and SIGTERM then kills the process. No use-after-free: the pointer is only ever the last installer, or `NULL` after any restore. Duplicate registrations are counted (add, add, remove, remove succeed, and a third remove fails with error 87), so the registration count stays balanced. | `probe_two_servers.c`: "B STILL RUNNING (Ctrl-Break not delivered to it); events that fell through to the next handler: 1". | Reword the header ("only one server at a time; once any server's run returns, none receives signals"), or clear the pointer only when it still equals `s` (`if (kl_signal_server == s)`), which keeps B reachable in the common case. | fixed in #542 |
| I1 | Informational (premise of the PR) | `http_server_plat_win.c:28-33` comment; CHANGELOG entry | Measured on Windows 10.0.26300: while a dispatcher thread runs a handler, both `SetConsoleCtrlHandler(h, TRUE)` and `SetConsoleCtrlHandler(h, FALSE)` block until it returns (2000 ms against a 2000 ms handler). The pre-fix `origin/main` handler, with a 300 ms gap between load and stop and a real `CTRL_BREAK` event, then a stop, join, free and `VirtualFree`, completed cleanly: the run returned 234 ms after the stop, held in restore. The new test calls the handler directly on a test thread, not through the dispatcher, so it proves the lock's guarantee but not that #539 occurs in practice. The behaviour is undocumented and may differ across Windows versions, so the lock is still worth having. It also confirms the PR's rule "never call SetConsoleCtrlHandler with the lock held": doing so would deadlock against a dispatcher holding its lock and waiting for ours. | `probe_console_lock.c`, `probe_remove.c`, `probe_win_main_uaf.c`. | Reword the comment and CHANGELOG: "Windows does not document that unregistering waits for a running handler", rather than stating that it does not wait. | #542 corrected (comment, CHANGELOG, PR description) |
| T1 | Informational | `tests/test_ctrl_handler_win.c` | Correct and not flaky: the 300 ms "still waiting" check cannot false-fail on the fixed code, because the handler holds the shared lock until `g_release` is set. On a failed `ASSERT` the parked threads are left behind and die with the process. It exercises only the direct-call path (see I1) and only `CTRL_C_EVENT`. 10/10 passes (5 under select(), 5 under IOCP). | Run on edge. | None required. | documented in #542; a C1 case added |

#### Areas with no finding

- **Memory safety and C11 UB in the change:** none. `SRWLOCK_INIT` static initialisation is valid.
  The plain pointer has no data race: it is read and written only under the lock, whose
  acquire/release give the ordering. Dropping `<stdatomic.h>` from the TU is sound, and the
  `toolchain.mk` comment edit matches it: `/experimental:c11atomics` is still passed globally for
  `kl_atomic.h`.
- **Work under the lock:** `kl_http_server_stop` does atomics, one CAS, `kl_monotonic_ms`
  (QueryPerformanceCounter) and a `send()` of one byte on a non-blocking loopback socket
  (`platform_wakeup_win.c:94-110`; a full buffer returns WSAEWOULDBLOCK). Nothing blocks, logs,
  calls a user callback, re-enters the handler or takes `kl_signal_lock`. `stop_wake_wr` is closed
  only by `kl_http_server_free`, which by contract follows the restore.
- **Lock order:** dispatcher lock, then `kl_signal_lock` shared (handler). Install takes the
  exclusive lock and releases it before `SetConsoleCtrlHandler`; restore calls
  `SetConsoleCtrlHandler` before taking the exclusive lock. No cycle. SRW writer starvation is not
  a concern: console events are serialised by the dispatcher.
- **Stop from another thread at the same time:** safe. `kl_http_server_stop` is thread-safe by its
  CAS protocol (#536), and the handler's shared hold does not exclude other callers.
- **Failed start:** install happens only after bind, listen, nonblocking, event registration and
  the lock-free check. The one failure after install (`server_accept_listener_start`) restores
  (`http_server.c:428`); the completion-loop failure breaks to the common restore (`:818`).
  `kl_http_server_free` never installs or restores. Freeing while `run` is still executing is a
  caller-contract violation, outside the PR.
- **Repeated install/restore:** one server is run once per init (enforced by `run_started`).
  Sequential servers are balanced: one registration and one removal each. 40 sequential
  init/run/real-Ctrl-Break/free/poison cycles completed with `HeapValidate` clean.
- **Makefile/MSVC:** the explicit rule wins over the Windows `tests/test_%$(EXE)` pattern rule.
  The hooked object goes before `libkeel.a` (after `/link` on MSVC), defines every symbol of the
  archive member, and so never pulls the archive's `http_server_plat_win.o`. There is no
  duplicate-symbol risk under either GNU ld or link.exe. `OBJ_OUT`/`EXE_OUT`/`TEST_OBJ_TMP`/
  `CC_FORCE_INC` are used as in the neighbouring Windows rules. The hooked `.o` name does not
  collide with the `/Fo$(OBJDIR)/tests/` output of the test TU. The rule lists no header
  prerequisites, but it rebuilds whenever `$(KEEL_LIB)` does. MSVC itself was not built (vcvars
  does not initialise over ssh); the MSVC verdict comes from reading the rule.
- **POSIX path:** unaffected by the PR. `TEST_SRC` filters the new suite out (`make -n test`
  never names it), and the POSIX TU is untouched. The explicit target exists on POSIX but is
  reachable only by naming it. The POSIX TU's own defect is P1.

#### What was run

- macOS: `make check-msvc-parity check-pollcomp-suite-list check-test-layout check-no-em-dash
  check-no-milestones check-protocol-home check-substrate-purity`, all OK. `parity[winselect]`
  enrols 126 of 127 and `parity[iocp]` 119 of 120, each with one documented exclusion. `make`
  (kqueue). POSIX probe `probe_posix_uaf.c` (P1).
- edge (MSYS2 UCRT64, Windows 10.0.26300): `make` and `make BACKEND=iocp`;
  `tests/test_ctrl_handler_win.exe` passed 5/5 on select() and 5/5 on IOCP. Probes, each in its own
  console process group (`CREATE_NEW_PROCESS_GROUP`, events sent only to that group):
  `probe_console_lock` and `probe_remove` (I1, duplicate-registration semantics),
  `probe_win_main_uaf` (I1), `probe_two_servers` (C1), `probe_userwait` (W1), and `probe_stress`
  (40 real-event cycles, clean).
- Not counted as a finding: an unbounded `CTRL_BREAK` flood (no sleep between
  `GenerateConsoleCtrlEvent` calls) stalled a server run before bind. The no-Keel control
  `probe_flood_thread` shows the same flood stops any newly created thread from running for more
  than 3 s: each event injects a thread into the process. An earlier unbounded run also sent the
  events to the whole ssh console group and left a hard-to-kill probe process, which later exited
  on its own.
- Not run: native MSVC, `make cppcheck`/`analyze`, the full `test-win` suite (the PR reports it),
  CI.
- Probe sources (`probe_*.c`, `selfgroup.h`, `plat_posix_delay.c`, `plat_win_main_delay.c`) were
  kept outside the repository with the audit's scratch files.

### Part B: PR #543, the HTTP/2 settle pass that replaces the flush timer

**Revision:** PR #543 head `5586facf` (two commits over `main` `b43d3110`: `56968137` and
`5586facf`); motivating issue #540. Read-only; no repository file was changed by the audit. Builds,
probes and runs used scratch copies (`git archive 5586facf`) on macOS, a new `~/keel-aud543c` on
core and a new `C:\Users\Mark\keel-aud543c` on edge.
**Scope:** C safety and correctness of the change and of everything it reaches: the settle pass
(`kl_http_server_settle_h2` / `_due`, `http_server_core.c:579-603`), its two call sites (readiness
run loop `http_server.c:449-471`, `kl_http_server_run_completion_loop` `http_server_core.c:610-621`),
the `settle` / `settle_due` hooks (`http2_server.c:1256-1275`), the stream sink, `h2_flush_ex`,
`h2_flush_outside`, `h2_stream_resume`, `h2_streams_release_all`, `kl_http2_server_comp_sent`,
`on_readable` / `on_writable` / `feed` / `cleanup` / `abandon`, `completion_http2.c`,
`completion_http_server.c` close paths, the stream notifications in `http_response.c`, the sweep,
`kl_http_server_conn_release`, `kl_http_conn_release` and the pool, and the merged
`kl_http_conn_request_write` (`http_server_core.c:1056`). The axis audit of the same PR is a
separate pass.

**Result:** no Critical, High or Medium. No memory-safety defect, lost output, lost wakeup or busy
spin was found in the change; the removal of the flush timer also removes a class of timer-outlives-
`h2c` lifetimes. Four Lows (one PR-introduced cost, one PR-introduced weakening of a guard, two
pre-existing behaviours the new pass leaves in place) and one hygiene note. Dispositions in section 4.

**Engines run (all green unless stated):**
- macOS kqueue: `make`, `make test` (one failure, `cloexec.every_descriptor_keel_creates_is_close_on_exec`,
  only under `-j8` load; passes alone, unrelated to this change); nghttp2 `make e2e`
  (`NGHTTP2_DIR=/opt/homebrew/opt/libnghttp2`); `make freestanding-lib-server` (x86_64 and aarch64
  UEFI archives gated OK; HTTP/2 is not in the freestanding server, its hooks are NULL and both
  settle functions return at once).
- macOS pollcomp: `make BACKEND=pollcomp test-pollcomp` (same flaky cloexec only).
- macOS ASan+UBSan: not obtained on this host. With `DEBUG_CFLAGS`, `test_allocator_validate` and
  `test_http2` spin before `main` (`sample`: `__asan::InitializeShadowMemory` ->
  `StaticSpinMutex::LockSlow` under `dyld_shared_cache_iterate_text_swift`); a Homebrew llvm@21
  build of `test_http2` printed nothing in 120 s. An environment defect, not this change; sanitizer
  coverage comes from Linux.
- Linux core, ASan+UBSan: epoll full `make test` clean (`-O1 -fsanitize=address,undefined`); `test-pollcomp-san` clean; io_uring
  `test-iouring-san` clean (`-luring` added to the overridden `LDFLAGS`); nghttp2 e2e
  (`e2e_h2_stream_control`, `e2e_h2_stream_flow`, `e2e_socket`, `e2e_idle`, `e2e_h2c_upgrade`) clean under ASan on epoll, pollcomp and io_uring.
- Windows edge (MSYS2 UCRT64): `test_http2` on IOCP (75 passed) and select() (74 passed, the
  completion-only case skipped).

#### 1. Findings

| ID | Severity | Class | Location | Finding | Smallest fix |
|---|---|---|---|---|---|
| 543-L1 | Low | proven (measured cost; introduced by the PR) | `http_server_core.c:579-603`; called `http_server.c:449,451`, `http_server_core.c:613,619` | Two more full scans of the connection pool on every loop iteration, unconditional whenever `http2_server.c` is linked (every hosted server: `kl_http_server_init` installs the hooks), including servers with `config.h2 == NULL`, which can never hold an HTTP/2 connection. `KlHttpConn` is 4736 bytes, so each slot is a cache miss. Probe (10 000 slots, idle, macOS O2): the sweep costs 13.4 us per iteration, the two settle scans 18.7 us more, so the per-iteration pool cost is 2.4 times what it was. At the default 256 slots it is about 0.5 us and invisible; at 10 000 slots and tens of thousands of wake-ups per second it is a measurable share of a core. | `if (!s->config.h2) return;` (and `return 0;`) at the top of both functions. A per-server count of HTTP/2 connections, or a server flag set by `h2_schedule_flush`, would make it free for HTTP/2 servers too, but needs a back-pointer the h2 object lacks. |
| 543-L2 | Low | theoretical (introduced by the PR) | `http_server_core.c:1056-1064` | Merging the two request-write seams dropped `kl_http_conn_http2_request_write`'s `c->state == KL_HTTP_CONN_HTTP2` check; the merged function checks only "not SUSPENDED". It re-registers READ\|WRITE, which is correct only for the WEBSOCKET and HTTP2 states (the comment says "call it for those two only"). Safe today: the only callers are `ws_send_frame` (state is WEBSOCKET before `on_open` runs, `http_server_ws.c:398`) and `h2_flush_outside`, reached only from the settle hook, which runs on state HTTP2 and nothing inside a flush changes `c->state` (stream requests carry no `_server_ctx`, so a producer cannot pause, resume or suspend the connection). A future caller in another state would install a WRITE watch the dispatch then acts on for the wrong state. | Replace the SUSPENDED test with `if (c->state != KL_HTTP_CONN_WEBSOCKET && c->state != KL_HTTP_CONN_HTTP2) return;`, which subsumes it and restores the guard both old functions had in effect. |
| 543-L3 | Low | proven behaviour (pre-existing; the PR keeps the window) | `http2_server.c:1145-1162` (`kl_http2_server_feed`), `1256-1275` (settle hooks) | A session that failed (a flush returned -1) still takes the client's input: `kl_http2_server_feed` calls `session->recv` without checking `h2c->failed`, so new requests reach `h2_cb_on_request` and run their handlers on a session that can never answer them. When the failure happens in the settle pass, the pass ends the streams and `settle_due` then returns 0, so the loop waits a full iteration (up to `KL_POLL_TIMEOUT_MS`, 1 s) before the sweep closes the connection; a client frame in that wait runs a handler. Probe A (below): `failed=1`, then a request on stream 3 ran `handler_called` once. The same window existed on `main` (the flush timer fired after the sweep). Impact: a handler with side effects runs for a request whose answer is a connection close without GOAWAY. | `if (h2c->failed) return KL_HTTP_CONN_CLOSED;` at the top of `kl_http2_server_feed` (covers readiness `on_readable` and both completion drives). |
| 543-L4 | Low | theoretical (pre-existing; the settle hook's comment overstates its coverage) | `completion_http2.c:31-37,49-53` with `http2_server.c:1153-1154`; `completion_http_server.c:535-545` | On a completion loop, a flush that fails inside the connection's own drive (`kl_http2_server_feed`, e.g. an output-queue reservation failure) returns CLOSED, and `kl_comp_close_after_output` moves the connection to state CLOSED while queued output remains. The settle pass only visits state HTTP2, so the streams are not released: their producers hear `completed 0` only at the slot release, after the queue drains or, for a client that stopped reading, after `read_timeout_ms`. Their writes are refused meanwhile (`h2c->failed`), so nothing accumulates. The settle hook's comment ("Their streams end here, not when the connection is released") holds only for failures first seen in the settle pass or the drain's GOAWAY flush. `main`'s sweep service hook had the same gate. | In `kl_http2_server_feed`, release the streams (`h2_streams_release_all`) when `h2_flush` fails, before returning CLOSED: that is after the flush has returned, inside the connection's own event, not inside a producer's call. |
| 543-H1 | Info | hygiene | `http2_server.c:21` | `#include <keel/timer.h>  /* the deferred flush of a streamed response's output */` survives the removal of the flush timer; nothing in `src/protocols/http2/` calls `kl_timer_*` now. | Remove the include. |

#### 2. Probes

Scratch copy of `tests/protocols/http2/test_http2.c` with cases appended (mock session and rig as
in the suite), built with the suite's own rule.

- **Probe A** (`probe.failed_session_still_runs_handlers`): open stream 1, settle, set
  `mock.flush_return = -1`, write, settle (the flush fails; streams released, `settle_due` 0), then
  `kl_http2_server_feed` (it calls `session->recv` once on the failed session) and a new request on
  stream 3 delivered through the session callbacks, as a real `recv` would. Output: `failed=1
  left=0 due_after_settle=0 feed_state=CLOSED rc=0 handler_ran=1`. The assertion that no handler
  runs fails: 543-L3.
- **Probe C** (`probe.settle_scan_cost`, `probe.sweep_scan_cost`): `max_connections = 10000`, no
  connection open, 10 000 iterations: settle + due 187 ms (18.7 us per iteration), sweep 134 ms
  (13.4 us): 543-L1. With `config.h2 = NULL` the settle scans still cost 18.3 us per iteration.
- **Fix check** (scratch tree only): with the three smallest fixes from the table applied (the
  `config.h2` gate, the WEBSOCKET/HTTP2 state guard, the `failed` check in `kl_http2_server_feed`)
  and the include removed, `test_http2` (74), `test_websocket` (55) and `test_stream_termination`
  (42) pass on kqueue; Probe A's feed no longer reaches `session->recv` (0 calls, against 1 at
  `5586facf`); Probe C with `config.h2 = NULL` drops to 0.0 us per iteration.

#### 3. Categories with no finding

- **Memory safety and C11 UB:** none. No new arithmetic, no new allocation. The pool is a fixed
  array (`http_connection.c:74`, never reallocated while running), iterated by index with the state
  re-read per slot; no pointer is held across iterations.
- **Callback-triggered destruction and reentrancy in the pass.** Inside `h2_flush_outside` the
  callbacks are the session's `send` (connection write: never releases, `kl_comp_queue_write` only
  refuses a closing connection), `on_stream_reset` (one stream's close notification, slot left in
  place), `read_body`, and after the send the writable edges (`kl_http_response_stream_progress`).
  From a notification or edge a producer can write, end or abort any stream (each only sets
  `flush_due` / `reset_due`), complete another connection's async op, or call
  `kl_http_server_stop` (atomics and a wake-up write only; the drain's GOAWAY flush runs later from
  `kl_http_server_drain_progress`, never synchronously). None of these releases the connection
  being settled, frees `h2c`, `h2c->streams` or the session, or re-enters `h2_flush_ex` on it, so the
  uses of `h2c` after the flush (`want_write`, `failed`, `num_streams`) are sound. Freeing the server
  from a callback while `kl_http_server_run` is on the stack is a caller-contract violation, as for
  every loop callback (`http_server.h:489`).
- **Slot reuse and registration identity.** No accept happens during the pass, so no slot is
  reacquired inside it. A connection released inside it (another connection's, through an async
  completion) leaves state HTTP2 and `c->h2` is NULL after `kl_http2_server_cleanup`; a completion
  connection whose release is deferred (`comp_closing`, state still HTTP2) is flushed into a
  refusing queue, which marks the session failed and ends its streams early: benign. The readiness
  re-registration in `h2_flush_outside` is made only for state HTTP2 on its own `&c->stream` and fd
  (see 543-L2 for the guard). Every completion-loop connection is `comp_driven`, so the readiness
  `kl_event_mod` is never issued on a completion engine.
- **Lost output and wakeups.** Output written from dispatch, the sweep, async deadlines, timers or
  the drain is flushed by the next iteration's pass before that iteration's wait, on both models.
  Output written after the pass (writable-edge or close notifications it fires, HTTP/1.1 writable
  edges in `kl_http_comp_stream_edges`, readiness `kl_async_sync_stream_interest` fires none) is
  caught by `settle_due`, evaluated after everything that can run before the wait on both loops;
  nothing between it and `kl_event_wait` / `kl_http_comp_run` runs user code. A connection settled
  later in the same pass covers writes into it from an earlier one. `kl_http2_server_comp_sent` no
  longer flushes for owed output, which is now always covered by the pass.
- **Busy spinning and starvation.** `settle_due` reports only `flush_due` (set by a producer call)
  or a failed session with streams left; held output (full socket, spent window, `out_refused`) and
  a resume that failed again never force a zero wait. With nghttp2 a blocked send packs at most one
  DATA frame, so a producer writing from every edge costs at most one extra zero-wait iteration
  after the socket fills. A producer that keeps writing while the socket accepts keeps the wait at
  zero, which is the work it asked for; events, the sweep and timers still run each iteration.
  `h2_streams_release_all` always takes `num_streams` to 0 (no stream can be created from a
  notification), so the failed-session report clears in one pass.
- **Failed sessions, resume failures.** `flush_due` is cleared at the start of every flush;
  `resume_due` is retried at the loop's pace (at most a second), skipped for a stream being reset.
  The failed-session handling matches `main` apart from 543-L3 / 543-L4.
- **Drain and stop.** Drain counts HTTP/2 connections as active; `drain_shutdown`'s flush failure is
  settled on the next pass and the sweep closes. A stop that ends the run leaves `flush_due` output
  unsent, as the timer did.
- **h2c upgrade.** `flush_due` set while the HTTP/1.1 connection is mid-upgrade is cleared by the
  upgrade's own flush; a connection is not visited until its state is HTTP2.
- **Freestanding.** HTTP/2 is absent; both settle functions return on NULL hooks; archive gated OK.
- **Error handling and resource ownership.** No new ownership. The removal of the timer removes the
  cancel-on-cleanup and cancel-on-abandon obligations and the timer-heap-full mode entirely.

#### 4. Dispositions

| ID | Disposition |
|---|---|
| 543-L1 | fixed in #543: both settle functions return at once when `config.h2` is NULL |
| 543-L2 | fixed in #543: `kl_http_conn_request_write` refuses every state but WEBSOCKET and HTTP2 (test `h2.request_write_leaves_other_states_alone`) |
| 543-L3 | fixed in #543: `kl_http2_server_feed` refuses a failed session's input (test `h2.a_failed_session_reads_no_more_input`) |
| 543-L4 | fixed in #543: a failed flush inside the connection's own event ends its streams, in `kl_http2_server_feed` and, for the same reason, in `kl_http2_server_comp_sent`; the correction's review found the same gap when the session's `recv` fails, fixed alike (tests `h2.a_failed_flush_in_the_connection_event_ends_its_streams`, `h2.a_failed_flush_in_a_send_completion_ends_its_streams`, `h2.a_fatal_session_error_ends_its_streams`) |
| 543-H1 | fixed in #543: the include is removed |

## Twenty-first pass: 3.6.0 pre-release audit (2026-11-25)

**Revision:** `main` `65568ff` (#521; the changes since v3.5.0). Read-only; no repository file was
changed by the audit. Probes ran on scratch copies (`git archive 65568ff`).
**Scope:** four parallel C audits, with the axis audit alongside (nineteenth pass in
[keel_axis_audit.md](keel_axis_audit.md)):
- **A:** bounded streaming and compression (`http_response.c`, `http_compress.c`, miniz).
- **B:** server lifecycle (graceful drain, writable edge, hang-up watch, run and stop).
- **C:** Windows event backends (`event_winselect.c` whole file; `event_iocp.c` watch lifecycle).
- **D:** untrusted-input parsers (HTTP/1, multipart, chunked, router, PROXY, WebSocket, DNS,
  client, HTTP/2).

**Historical versus current.** Section 1 describes `main` at `65568ff`, before any fix. Sections 2
to 5 describe `main` after the fixes, at `021dd0a` (#523 to #531 merged); section 6 records the
convergence audit and the defects its reviews found.

**Result at `65568ff`:** no Critical. Four release-relevant correctness defects (R1 to R4), four
Medium DoS or protocol defects (D1 to D4), and a set of Lows. These audits, the sanitizer runs
listed below and fuzzing found no memory-safety defect in the audited revision (no out-of-bounds
access, use-after-free or double free); this is not a claim of formal memory safety. One of the
corrections then introduced one: a remotely triggerable connection-pool corruption (F1, a High
regression from #528), found by the convergence reviews and fixed by #530 before release
(section 6).

**Engines run:** macOS kqueue and pollcomp; Linux epoll, poll and io_uring on core; Windows
select() and IOCP on edge (MSYS2 UCRT64). v3.5.0 was also run on kqueue and pollcomp to classify
regressions. Sanitizers: `smoke-iouring-asan` and `smoke-pollcomp-asan` clean on core; an ASan run
of audit B's probe was stopped unfinished and is not claimed.

### 1. Findings against the audited revision `65568ff`

#### Release-relevant correctness (state at `65568ff`)

| ID | Source | Location | Defect | Status | Regression? |
|---|---|---|---|---|---|
| R1 | A H1 | `http_response.c` `kl_http_response_stream_writev`; `http_compress.c` `kl_http_compress_stream_end` | The legacy writer checks only `stream_error` and `head_request`, never `stream_ended`, `body_mode` or `headers_sent`; the compressed end runs its final feed without checking `stream_ended`. A write after `end_stream` puts `0\r\n\r\n4\r\nlate\r\n` on the wire and returns 0. On a kept-alive connection the stray chunk sits where the next response starts: desync, response splitting behind a relay. | confirmed by probe | root cause pre-3.5; the new compressed path reaches it |
| R2 | B H1 | `drain.c` `drain_notify`; `http_response.c` stream branch of `kl_http_response_send`; `http_connection.c` `conn_send_complete`; `completion_http_server.c` `comp_tls_on_write` | A stream ended from `on_drain` after its op completed loses its tail and the connection is kept alive. Readiness: `drain_notify` decides "drained" before calling `on_drain`, which refills it. Completion: `comp_tls_on_write` completes the send with the drain refilled. The keep-alive reset frees the tail and terminator; the next response is parsed as chunk data. | confirmed on kqueue, epoll, pollcomp, io_uring | completion new in 3.6.0; readiness pre-existing (v3.5.0 kqueue desyncs) |
| R3 | axis M1 | `completion_http_server.c` `comp_tls_on_write`; `http_response.c` `kl_http_response_stream_progress` | Completion loops: a writable callback installed after the WOULD_BLOCK never fires once the queue is idle, against the header's "a refusal before fn was installed is remembered". pollcomp and io_uring stall at 48 KiB. | confirmed | new API in 3.6.0 |
| R4 | C M1, M2 | `event_iocp.c` `iocp_watch_rearm_dispatched`, `iocp_watch_renew`, `iocp_quiesce_port_for_close` | A watched socket closed without `kl_watcher_del`: each dispatch re-arm revives the dead watch (renew clears `watch_dead`) and re-posts on the bare handle value. Once the value is reused by a socket no port knows, readiness is lost and `kl_event_ctx_free` blocks in `GetQueuedCompletionStatusEx(INFINITE)`. M2 (speculative): reuse by a socket on the same port fires the stale watcher on it. | M1 confirmed on edge; M2 not probed | #519 covered only reuse followed by re-registration; needs a del-before-close breach |

#### Medium: DoS and protocol (state at `65568ff`)

| ID | Source | Location | Defect | Status |
|---|---|---|---|---|
| D1 | D M1 | `http_body_reader_multipart.c` HEADERS state | Each `kl_http_multipart_next` re-searches the accumulated part header from offset 0; default `max_headers_size` 0 is unlimited. 8 MiB 2.7 s, 16 MiB 10.7 s, 32 MiB 42.8 s of loop-thread CPU. | confirmed; pre-existing |
| D2 | D M3 | `http_connection.c` READING (`last_active_ms` reset per readable event); `http_server_core.c` sweep | No absolute header-phase deadline. A 47-byte request at one byte per 800 ms with `read_timeout_ms = 1000` was served after 39 s; at the defaults one connection is held about 66 hours. | confirmed; pre-existing |
| D3 | D M2, B M1 | `http_connection.c` `conn_keepalive_reset` (`read_len = 0`) and the completion equivalent | Pipelined HTTP/1.1 requests are silently discarded; the client hangs to its read timeout (RFC 9112 section 9.3). Fails closed, no smuggling. | confirmed; pre-existing |
| D4 | C M3, axis L2 | `event_winselect.c` wait scan; `event_poll.c:224` | The scan starts at slot 0 and stops at `max` (64): 236 of 300 continuously ready sockets never served in 50 waits; the idle sweep then times out active clients. | confirmed; pre-existing in poll, now the Windows default |

#### Low (state at `65568ff`)

- **A (streaming, compression):** A-L1 the response head goes through the drain capped at
  `stream_capacity`, with no minimum capacity (capacity 64 and a 200-byte head: `begin_stream`
  fails); A-L2 `kl_http_response_free` leaves the stream fields, so `try_write` after release
  returns ERROR (header promises CLOSED) and stale producers could write into a reused slot; A-L3
  compressed `try_write` answering CLOSED keeps the codec (about 300 KB with miniz); A-L4
  `kl_http_response_enable_drain` on an enabled drain leaks the buffer and drops buffered bytes.
  Informational: duplicate `Connection` header when the handler set one; a buffered response begun
  before a drain says keep-alive then closes; `kl_http_response_stream_admit` sets sticky
  `stream_error` with no drain.
- **B (lifecycle):** B-L1 run-once-per-init documented but not enforced (a second run re-adopts a
  closed `listen_fd` number); B-L2 `kl_http_server_draining()` stays 1 after the run returns; B-L3
  two threads stopping at once race on non-atomic `drain_deadline_ms` (torn on 32-bit); B-L4 the
  drain keep-alive close re-issues `kl_comp_cancel` every sweep tick; B-L5 `on_drain` re-entrancy
  undocumented; B-L6 `kl_watcher_mod` failure leaves `KlWatcher.mask` and `stream_write_watched`
  disagreeing. Informational: a stream ended from `on_resume` with the drain pending is kept alive
  on readiness but closed on completion; peer credentials query a non-host handle (B-I3, axis L3).
- **C (Windows backends):** C-L1 winselect dispatches a stale registration with the old udata when
  its value is reused by an unregistered socket; C-L2 the two Windows backends disagree on dead
  registrations; C-L3 one extra `CreateIoCompletionPort` per watcher dispatch; C-L4
  `ERROR_INVALID_PARAMETER` read as "already on this port" although another port also returns it;
  C-L5 winselect `kl_event_add` failures leave errno unset.
- **D (parsers):** D-L1 `max_header_size` below 8192 ignored (a 6000-byte header with a 1024 limit
  got 200); D-L2 body-reader routes unbounded by default (HTTP/2 applies `max_body_size`); D-L3 a
  partial PROXY header from a trusted peer spins on level-triggered backends; D-L4 response-parser
  and h2-client header strings freed at `strlen()+1`; D-L5 WebSocket client accepts
  `HTTP/1.1 1010` as 101; D-L6 CORS origins over 255 bytes truncated; D-L7 h2 response header names
  keep HTTP/1 case and headers past 64 drop silently; D-L8 gzip one-shot accepts bytes between the
  deflate end and the trailer; D-L9 DNS answer owner and class unchecked (mitigated by id, 0x20,
  question echo, source check and cookies); D-L10 redirect A to B to A re-sends A's credentials to A.
- **Axis:** axis-L1 the Windows sync client waits with WSAPoll.

#### Verified sound (state at `65568ff`)

- **Streaming arithmetic:** `chunk_framed_len` and its `SIZE_MAX/2` guards, `format_chunk_hdr` (16
  hex digits in `hdr[24]`), saturating `stream_pending`, 32-bit `size_t`; the admission invariant
  pending + framed <= capacity <= `drain.max_size`; the 5-byte terminator allowance.
- **Compressed staging:** `stage_reserve` / `stage_release` pairing, `stage_emit` bounds, admission
  before allocation and before the codec; `try_end` release rules; miniz `retained` and
  `flush_bound` overflow to TOO_LARGE.
- **Reset and edge:** `kl_http_response_reset` clears every new field; writable-edge arm and disarm;
  liveness re-checks after each callback.
- **Lifecycle:** deadline sweep cursor (`async_retire` advances it before `on_cancel`); exactly one
  terminal callback; queue accounting overflow guards; quiesce life release; drain deadline stored
  before the seq-cst flag; listener close on both axes.
- **winselect:** `KlWinFdSet` layout, growth and its `INT_MAX/2` guard, all-or-nothing `resize()`
  under OOM, WSAENOTSOCK retry, WRITE-only reset reporting, non-consuming except-set `SO_ERROR`,
  timeouts. **IOCP:** renew, reinterest, del and the quiesce pre-pass for the re-registration path.
- **Parsers:** llhttp CL+TE and duplicate Content-Length rejected (live); chunked, router, PROXY
  v1/v2 and CIDR, WebSocket, DNS, client and HTTP/2 bounds.

### 2. Corrections and their PRs (state on `main` at `021dd0a`)

| Finding | Correction | PR |
|---|---|---|
| R1 | writers (legacy, SSE, compressed) refuse an ended, reset or released stream; `end_stream` with no stream writes nothing; compressed end on a closed response releases the codec | #528 (`ee827fd`) |
| R2 | readiness re-checks the drain after `on_drain`; the completion end mark follows appended drain bytes; a response with unsent output is never kept alive | #528 |
| R3 | HTTP: `kl_http_comp_stream_edges` fires an armed, installed, idle edge once per completion tick before the wait | #524 (`b4e1fab`) |
| R3, `KlStream` sibling | a deferred zero-delay check on the stream's ctx when a callback is installed with nothing pending | #525 (`74ead39`) |
| R4 (C-M1, C-M2) | IOCP probes posted only when the owner names the handle and only on a handle verifiably on this port; an owner tag per op; `kl_event_close` bounded at 2 s for orphaned probes | #527 (`3f47f2d`) |
| D1 | resumed in-place header search; offset consume (a second quadratic path found in review); default `max_headers_size` 8 KiB | #526 (`2791a2f`) |
| D2 | `KlHttpServerConfig.header_timeout_ms`: absolute head deadline from accept (first request, covering TLS handshake, PROXY header, h2 preface) or from first bytes (keep-alive) | #526 |
| D3 | mitigated, not implemented: a request with bytes after it is answered with `Connection: close`, then a half-close drain | #528 |
| D4 | lap-based fair reporting in poll, Windows select() and pollcomp (`src/event_turn.h`) | #527 |
| A-L2 | `kl_http_response_free` seals the response: every stream call answers CLOSED / -1 | #528 |
| A-L3 | compressed `try_write` releases the codec when it answers CLOSED | #528 |
| B-L1 | a second `kl_http_server_run` returns -1 (`KL_ERR_INVALID_ARG`) before touching any descriptor | #523 (`180f905`) |
| B-L2 | `kl_http_server_draining()` returns `draining && running` | #523 |
| B-I3 / axis-L3 | peer credentials and label fail closed unless the provider is NULL or the built-in ops table | #523 |
| C-L3 | the unchanged re-arm skips the association call (`KlEventCtx.rearming`); add and mod still make one | #527 |
| C-L4 | `ERROR_INVALID_PARAMETER` no longer trusted alone; another loop's ops dropped by owner tag | #527 |
| D-L1 | `max_header_size` below 8 KiB enforced on every read on both loops | #526 |

B-I1 (a stream ended from `on_resume` with the drain pending: kept alive on readiness, closed on
completion): #528 routes every finished stream through the connection's send completion on both
models; the convergence audit (section 6) verified that both now keep the connection alive.

**Defects found while fixing and reviewing:**

| Defect | Found | Correction |
|---|---|---|
| Header-phase rejections skipped the reject drain: with nothing parsed the request looked complete, so a 431 was followed by a close while the client still sent, and on Windows the RST discarded the 431 | while fixing D-L1; pre-existing | #526: 431s and a 408 on a partial head half-close and drain |
| A `kl_http_server_stop` before `kl_http_server_run` published `running` was overwritten, so the server ran on | while fixing B-L1/B-L2 | #523 (`stop_requested`) |
| HTTP/1.0 requests got a chunked, kept-alive streamed response | in review of #528 | #528: HTTP/1.0 streams always close |
| Rejection after a streamed answer: a raw error written after the answer, the half-close cutting off buffered output, a close over unread input truncating a queued answer | while fixing R2/D3 | #528: discard while sending, a bounded post-answer linger, read on to EOF after a hang-up report |
| Readiness TLS: a pipelined request already decrypted in the engine waited for socket data | while fixing D3 | #528 |
| Completion-driven double send: a handler's direct `kl_http_response_send` was rebuilt and re-posted by the driver (plaintext twice; TLS a head inside the head; a timer send after reset wrote a stray 200) on io_uring, IOCP and pollcomp | while fixing; pre-existing | #529 (`6689f32`): one output driver per response |
| CI step timeouts left no headroom with the new suites | n/a | readiness matrix Test step 5 to 10 min (#526); MSVC runtime-probe step 5 to 10 min (#528) |

### 3. Remaining platform and API limitations (documented limits)

- **IOCP, del-before-close breached:** posts in a watch's IDLE period (the unchanged re-arm, an
  error watch's re-arm, a retried post) are not re-checked; `kl_event_close` gives up on such
  probes after about 2 s and retains their records. Documented at `kl_event_del` (#527).
- **Closed-without-del behaviour per backend** (C-L2): documented at `kl_event_del` (#527);
  violating the contract is damage limitation, not a guarantee.
- **Pipelining is not supported** (D3): the client resends on a new connection (#528,
  CHANGELOG upgrade notes).
- **No hang-up report** on completion loops, macOS poll and Windows select(): the post-answer
  linger runs to the read deadline (`docs/contracts/early_rejection_drain.md`, #528).
- **`max_header_size`** applies to an incomplete header block; a head that arrives complete in one
  read is accepted up to the read buffer (#526).
- **Tightened defaults with opt-outs:** `header_timeout_ms < 0` disables the head deadline;
  `max_headers_size = SIZE_MAX` restores unlimited multipart part headers
  (`docs/contracts/compatibility.md`, "Correctness, security and default behavior"; #526).
- **Peer credentials** return -1 off the built-in provider; `kl_peer_cred_fd` is host-descriptor
  only (#523).
- **`KlStream`** must be freed before its event ctx; a check still pending at `kl_event_ctx_free`
  never fires (#525).
- **Completion TLS** pending counts ciphertext (`docs/contracts/streaming.md`).

### 4. Low-severity findings deliberately deferred

- **From the audit:** A-L1 (no minimum `stream_capacity`; head held against the cap), A-L4
  (`enable_drain` twice leaks and drops bytes), the A informationals (duplicate `Connection`
  header; drain-race keep-alive head; `stream_admit` sticky error), B-L3 (concurrent stop from two
  threads races on `drain_deadline_ms`, torn on 32-bit), B-L4 (`kl_comp_cancel` every sweep tick),
  B-L5 (`on_drain` re-entrancy undocumented), B-L6 (`kl_watcher_mod` failure bookkeeping), C-L1
  (select(): reuse before any wait sees WSAENOTSOCK still reports to the old owner; noted only in a
  source comment), C-L5 (errno unset on add failures), D-L2 to D-L10, axis-L1 (the Windows sync
  client still waits with WSAPoll).
- **From the fix reviews:** quadratic `drain_push`; `pipe_ready` ignores a `pipe_watch` failure;
  stale IOCP identity records retained (#527); a stray CRLF holds a slot about 500 ms, a TLS
  partial record at the head deadline gets 408, and no test covers the 408-with-head-bytes rule
  (#526); a custom parser reporting HTTP version 0.0 (#528); no TLS close_notify on the SHUT_WR
  drain path (pre-existing); completion loops hold the linger slot to the deadline (#528).
- **From the #529 review:** the refusal's -1 is indistinguishable from a transport error; readiness
  HEAD file responses sent directly by a handler emit the body (pre-existing); pre-body middleware
  that sends directly advertises keep-alive and then closes (pre-existing).

### 5. Regression and mutation evidence (as reported in the PR bodies)

| PR | New tests | Reverts and mutants caught |
|---|---|---|
| #523 | `peer_credentials_fail_closed_off_the_builtin_provider`, `ws_peer_credentials_fail_closed_off_the_builtin_provider`, `second_run_after_drain_leaves_a_reused_listen_fd_alone` (`tests/test_unix_socket.c`); `a_second_run_is_refused_and_opens_no_listener`; `draining_reads_zero_once_the_loop_has_stopped` | each fails on `main` without the fix (independent review); the label assertion is exercised only on Linux with an active LSM |
| #524 | eight `late_install_case` cases in `test_completion_output.c` (late install, replacement, cancel, complete, re-arm, mock TLS, white-box spin), run on readiness, poll, select(), IOCP, io_uring, pollcomp | fix reverted; synchronous fire; replacement ignored; fire regardless of op state; unconditional pass; pass moved after the wait; spin guard removed (22,114 fires) |
| #525 | 19 late-install cases in `tests/test_stream_writable.c`; live cases in `test_anon_pipe.c`; IOCP cases in `test_pipe_stream.c` | without the deferral the late-install tests fail; without the owner hold ASan reports a use-after-free; deferring with bytes pending fails the no-spin test; an id-only cancel fails the stale-id test (Linux, ASan; the Windows free case is not sanitizer-checked) |
| #526 | multipart linearity (internal work counter), terminator split at every position, 8192/8193 boundary (`test_http_multipart_stream.c`); head-deadline drips incl. TLS record, handshake, h2 preface, keep-alive restart (`test_timeout.c`, `test_tls_integration.c`); `max_header_size` below the read buffer and `head_rejected_while_the_client_still_sends_gets_its_431` (`test_http_split_request.c`); `fuzz_multipart` interleaved pulls with allocation-failure injection | every new test fails with its fix reverted; the 431 test fails 5/5 on Windows without the fix; fuzz coverage 281 to 1018, 3.25M runs, no crash |
| #527 | `tests/test_event_fairness.c`; IOCP reuse, owner-tag and OOM cases in `test_iocp_engine.c`; additions to `test_event_hangup.c` | each fix mutation-checked on edge |
| #528 | `tests/protocols/http/test_stream_termination.c`, 27 cases, each checking the next response starts on the next byte (pipelining, overruns, stale begin, TLS in the engine, a flood past the cap, a half-closing slow reader) | each fix mutation-checked |
| #529 | two cases in `test_completion_output.c`, plaintext and TLS | both fail on pollcomp and io_uring without the fix; a reviewer's 17-case probe (async resume, middleware, file, HEAD, TLS, stale timer send) duplicated before it |

Sanitizer runs reported in the PRs, all ASan/UBSan clean on Linux: `debug-test` (#523, #525,
#526), `debug-iouring` (#524), the core lanes of #528 and #529.

### 6. Convergence audit (audited at `6689f32`; dispositions on `main` at `021dd0a`)

A focused re-check of the corrected invariants and their interactions. It ran on `f775e46`
(#529's head), whose tree is identical to `6689f32`. It covered: framing and termination,
connection reuse, one output driver, rejection and drain bounds, multipart linearity, head
deadlines, server lifecycle, HTTP and `KlStream` notification liveness, IOCP teardown and
dispatch fairness.
- **Linux (core):** every suite passed on epoll, poll, io_uring and pollcomp, including the
  ASan/UBSan lanes.
- **macOS:** kqueue, poll and pollcomp passed.
- **Windows (edge):** IOCP and select() passed.
- **Probes:** a probe server drove each invariant on kqueue, epoll, pollcomp and io_uring. A
  `KlStream` stress harness ran on all eight backends: every refusal got exactly one edge, with no
  spurious edge and no idle spin.
- **High:** none found.

| Item | Result | Disposition |
|---|---|---|
| M1 (new, Medium; readiness only; pre-existing, narrowed by #528). Shape: a buffered or file response whose head was already sent is truncated when the request body then fails (413 at the reader cap, or the body-timeout 408). Cause: `kl_http_conn_reject_final` finished first only for streams, and half-closed under the unsent rest. | probe: `Content-Length: 4194304`, then 4 to 6 KiB and FIN on kqueue and epoll; a clean 413 on completion loops (#529 refuses the direct send) | fixed by #531 (`021dd0a`) |
| B-I1 (stream ended from `on_resume`: kept alive on readiness, closed on completion at `65568ff`) | kept alive on both axes, next response on the next byte | resolved by #528 |
| B-L3 (two concurrent `kl_http_server_stop` calls write `drain_deadline_ms`, a plain `uint64_t`) | a genuine C11 data race, so undefined behaviour. **Observed behaviour, not a language guarantee:** 64-bit aligned stores do not tear; on 32-bit a torn value needs the two clocks to straddle a 2^32 ms boundary. No crash, missed stop or unbounded hang was observed. The language gives no bound on the consequences. | deferred (Low by practical exposure) |
| `drain_push` quadratic copy | bounded by `KlDrain.max_size` (1 MiB default). A peer cannot force byte-sized writes (at least about 1 KiB per send on loopback). Worst case about 20 to 30 ms CPU per MB, against 0.4 ms normally. `kl_ws_server_enable_drain(ws, 0)` has no cap, as already for memory. | Low, accepted |
| `pipe_ready` ignores a `pipe_watch` failure | unreachable: in readiness mode every narrowing is a `kl_watcher_del`, which cannot fail. Latent only for a future duplex readiness stream. | Informational |
| L1: on readiness, a stale `kl_http_response_send` between requests writes a stray response (documented misuse; completion refuses it) | reproduced | deferred |
| L2: an idle keep-alive connection is sent `408 Connection: close` at `read_timeout_ms` | reproduced on all four loops (predates 3.6) | deferred |

**Defects found by the focused reviews of the convergence fixes.** These were found while
reviewing #531 (the M1 fix) and #530. F1 is a regression one of the corrections introduced; F2 and
the cut file head predate 3.6.

| Defect | Origin | Disposition |
|---|---|---|
| F1 (High, remotely triggerable; macOS/BSD kqueue). kqueue reports read and write readiness as two events in one batch. During a rejected request's answer (`SENDING` with `reject_discard`, both armed), a client reset made the first event release the connection, and the second released it again. `active_count` went negative, the free list looped on one slot, and later accepts shared one `KlHttpConn`, so concurrent clients overwrote each other's state. | regression introduced by #528 (discard-while-sending); not in any release | fixed by #530 (`a46bf76`): each batched connection event records the connection's release generation (`comp_gen`), and dispatch drops an event whose connection was released, or released and re-accepted, since the wait |
| F2 (Medium). After a partial direct send, 16 sites cleared `res.keep_alive` and the overrun path could add a close notice. That changed the head layout `send_offset` counts into, so a body arrived 24 bytes short and shifted, or a head lost its keep-alive line. | pre-existing | fixed by #531: once any head byte is sent only `req.keep_alive` is cleared; the layout of a committed response is immutable |
| Cut file head (Low to Medium). A file response's head write could fail after some bytes went out while the head still counted as unsent, and a later rejection wrote a 413 inside the half-sent 200 head. | pre-existing | fixed by #531: such a response counts as committed and failed |

Evidence:
- **F1:** before the fix it reproduced on `6689f32` (3/3, `active_count -1`, free-list self-loop). The three new tests fail on `main` on kqueue, and the two batch-shaping ones also on poll. ASan/UBSan were clean with the fix on macOS (Homebrew clang) and Linux. A stress run on #531's branch with #530 applied was clean: 20 interleaved resets, `max_connections=1` with a queued client, and 150 keep-alive and reset races.
- **M1, F2 and the cut file head:** each test fails on the unfixed code on kqueue and epoll, guards excepted.

Further Lows from these reviews, deferred:
- a macOS file response stalls when the application sets SO_SNDBUF below the loopback MSS (TCP_NOPUSH is not lifted on EAGAIN; the idle timeout ends it);
- a partial head write inside `kl_http_response_begin_stream` (narrowed by #531, not tested);
- a second `kl_http_conn_release` of one slot is not refused loudly (defence in depth);
- the public response mutators are not refused after the head is committed. Their contract is now documented at `kl_http_response_send`: a committed response's layout is immutable.

## Low-finding triage after the twentieth pass (2026-10-08)

**Revision:** `main` `9679885` (after #475 to #485). Records a decision for every Low left open by
the nineteenth and twentieth passes and the twentieth pass's re-review, so they are decisions rather
than a backlog. Status was checked against the code at this revision. The items already recorded as
deliberately unfixed (S5, S9, E7, W11, X8; axis I1) keep their earlier decisions and are not
repeated.

- **Fix if a consumer hits it:** the defect is real and reachable, but narrow. Fix when a consumer
  reports it or touches the code around it.
- **Won't fix:** unreachable in practice, a test double, diagnostics only, already documented as
  the contract, or not worth the risk of the change.

### Reclassified: fix next

One Low is a defect any consumer of a documented feature hits, so it is not left waiting.

| Item | Location | Why |
|---|---|---|
| A readiness streamed response that is drained but not ended spins on WRITE | `http_response.c` `kl_http_response_send` (returns 1 for a drained, unended stream), `http_server.c` SENDING arms WRITE, `http_connection.c` `kl_http_conn_on_writable` refreshes `last_active_ms` | A streamed (SSE or chunked) response left unended by a handler, or an async resume, that returned without suspending while some of its output was still buffered keeps the connection writable-armed once that buffer is out, with nothing to send: on epoll, kqueue, poll or WSAPoll the loop wakes on every tick, and the idle sweep never ends it because each wakeup refreshes the activity time. The documented push-later pattern (suspend, write from a timer or watcher, end in the resume) never spun: a suspended connection is out of the loop. Fixed in #487. |

### Fix if a consumer hits it

| Item | Location | Note |
|---|---|---|
| Free or cancel inside a client streaming callback | `http_client.h` body/headers/read callbacks | Not guarded and not documented; freeing from `on_body` is a use-after-free. A doc note is the minimum. |
| Caller headers duplicating Host/Content-Length/Transfer-Encoding; names not token-checked | `http_client_common.c` `kl_http_client_build_request` | Smuggling risk only when callers pass untrusted header names. |
| Chunked trailers merged into the response; a `Location` trailer is followed | `http1_response_parser_llhttp.c`, `http_redirect.c` `find_location` | Needs a hostile origin; credential stripping still applies. |
| Stale pooled connection not retried for idempotent methods | client pooled paths | The borrow peek catches most; a race with the server's idle close still fails the request. |
| Readiness WebSocket and h2 server reads treat would-block as close | `http_server_ws.c`, `http2_server.c` readable handlers | A spurious wakeup drops the connection. |
| WebSocket Close echo lost when the drain is pending | `http_server_ws.c` `ws_handle_close` | Peer behind a slow-read backlog gets no Close echo. |
| h2 teardown destroys body readers without `on_error` | `http2_server.c` cleanup | A streaming upload reader is not told the body was cut off. |
| nghttp2 server adapter drops a header on copy failure; h2 responses cap at 64 headers | `http2_nghttp2_server.c` `ng_on_header_cb`, `http2_server.c` `H2_MAX_RESP_HEADERS` | OOM-only drop; many `Set-Cookie` headers truncated on h2 only. |
| WebSocket client reports a Close-less EOF as 1001 (should be 1006); some protocol errors fail without a Close | `websocket_client.c` | Applications branching on 1006 misbranch. |
| nghttp2 minimum version not stated (CONTINUATION flood fix is 1.61) | nghttp2 integration README/Makefile | Docs only. |
| Partial PROXY header from a trusted peer re-arms READ every tick until the idle timeout | `http_connection.c` `kl_http_conn_read_proxy_header` | Only peers inside `proxy_trusted_cidrs`; spins on edge-triggered engines too. |
| `max_header_size` below 8192 not enforced | `http_connection.c`, `completion_http_server.c` | Silent 8 KiB floor; only for lowered limits. |
| Windows listeners use `SO_REUSEADDR`, not `SO_EXCLUSIVEADDRUSE` | `socket_winsock.c` | Another local process can co-bind the port. |
| Handler headers can duplicate the server's framing headers; 204/304 get a Content-Length and body | `http_response.c` | Handler bugs only; desync through proxies. |
| Graceful stop waits out idle keep-alive connections | `http_server_core.c` `kl_http_server_drain_progress` | Stop takes up to the read timeout or `drain_timeout_ms`. |
| Readiness TLS rejection drain counts ciphertext | `http_connection.c` drain | A 413/431 on TLS likely lost to an RST. |
| Learned DNS server cookie never forgotten | `dns_resolver.c` `dns_on_recv` | Anycast or mixed-support resolvers time out and fail over. |
| OpenSSL ALPN reconfiguration can read past the array after a failed update | `tls_openssl.c` | Misuse path. |
| mbedTLS: out-of-range `client_auth` fails open to none; mTLS without a CA not rejected at creation (fails closed at handshake); `reset_failed` never clears; `write(0)` returns -1 | `tls_mbedtls.c` | |
| `PEM_read_bio_PrivateKey` prompts on a TTY for an encrypted key | `tls_openssl.c` | Blocks startup in an interactive shell. |
| Thread pool's nested-tick return skips the wakeup drain | `thread_pool.c` | Embedders ticking the loop inside `done_fn`. |
| io_uring watcher whose POLL_ADD fails goes silent; splice stage 2 returning 0 reports success | `event_iouring.c` | Rare kernel errors. |
| Inheritable handle window after `socket()`/`pipe()` where no atomic flag is used | Winsock, wakeup PALs, macOS pipes | Multithreaded embedders that spawn processes. |
| Named-pipe listener re-arm after every instance closed uses `first = 0` | `pipe_stream.c` | Narrow name-claim window; Windows IOCP only. |
| IPv4 multicast `iface_index` ignored off Linux | `socket_dgram_posix.c`, `socket_dgram_win.c` | Multi-homed hosts join on the default NIC. |
| Winsock `writev` truncates a buffered body above 4 GiB | `socket_winsock.c` | Buffered bodies of that size only. |
| Readiness accept on Winsock: `WSA_NOT_ENOUGH_MEMORY` maps to EIO, so no back-off | `socket_winsock.c` `kl_wsa_set_errno` | |
| No-allocator response fallback sends synchronously on a completion loop | `http_response.c` `stream_writev_all` | |
| WebSocket and h2 output buffers are not shrunk until the slot is released | `completion_http_server.c` | Only the HTTP response path shrinks. |
| `kl_datagram_last_error` set only by `kl_datagram_send`, not batch or GSO sends | `datagram.c` | |
| EFI graceful close, receive and connect waits still pump up to about 60 s | `socket_efi_tcp4.c` `efi_sock_close` | UEFI only; a stuck Transmit closes abortively since #485. |
| Datagram contract says "allocation-free steady state" for completion backends | `docs/contracts/datagram.md` | Docs only. |

### Won't fix

| Item | Reason |
|---|---|
| pollcomp loses abort events' refs on an allocation failure | Test double; OOM only. |
| Unguarded batch products in the POSIX datagram provider | Needs an absurd count on a 32-bit build; the core allocator is guarded. |
| Timer heap doubling is signed-overflow-prone before its check | Needs about 2^30 timers. |
| Sync redirect asks the policy about one hop past the limit | The URL is never fetched. |
| Size-limit failures reported as `KL_ERR_PARSE`; a failed redirect hop start always `KL_ERR_IO` | Diagnostics only. |
| Proxy port not range-checked | The field is `uint16_t`; only port 0 is unchecked and it fails at connect. |
| Truncated UDP DNS answer used without TCP fallback | Needs a server exceeding the advertised 1232-byte payload. |
| A DNS retransmit that hits would-block replaces the leg id | Needs a full send queue. |
| Vendored llhttp built without the stack protector and FORTIFY | Vendor code policy; defence in depth only. |
| pollcomp `post_connect` connects through `ctx->sockets` | Test double; documented. |
| A buffered response above io_uring's `send_max` hint (about 2 GiB) is copied to the queue | Negligible. |
| `KlHttpConn.comp_ws_refused` is a production field read only by tests | One increment on a refusal path. |
| A persistent `EINVAL` from the kernel drops every datagram rather than latching | Documented contract (each drop counted, `last_error` `KL_ERR_IO`). |
| The io_uring bounded wait can spin while the SQ is full on kernels without EXT_ARG | Documented; bounded per drain. |
| lwIP raw cancel sends RST on an idle reap | Contract met (one terminal per op). |
| A third-party h2 session that reports a stream close from inside `submit_response` | Documented as not allowed (#484). |
| `kl_http_client_pool_host_count` counts across providers | Documented (this change). |

## Twentieth pass: final audit after the audit-19 fixes (2026-10-07)

**Revision:** `main` `cd17c9c` (after #464 to #474). Read-only.
**Scope:** the whole `src/` tree, `include/keel/`, the integrations and the build, in five parallel
reviews: the HTTP/1 server; the HTTP/1 client; HTTP/2 and WebSocket with the nghttp2 adapter; the
event and completion substrate; DNS, PROXY, the integrations and the build. Each review first
re-read the fix diffs #464 to #474 adversarially, then swept its area. The axis review ran alongside
(eighteenth pass in `keel_axis_audit.md`). Every High and every Medium marked **C** was re-checked
against the code by hand. **C** means the path was traced end to end; **P** means plausible and
needing a test. Items recorded earlier as deliberately unfixed, and the nineteenth pass's Lows, were
not re-reported.

**Fixes since the nineteenth pass:** N1 to N8 and M1 to M15 hold where they were traced: the TLS
WebSocket admission bound, `dispatch_depth` and the in-frame complete and cancel, the lwIP raw
per-op terminals and slot handles, the EFI cancel completions, `SIO_UDP_CONNRESET`, the timer
watermark, the io_uring send cap and -EBUSY reap, the drain push and the WebSocket WRITE arm, the
kept body bytes, strict chunk metadata, v4-mapped PROXY trust, the split gzip header and the
link-local nameserver skip. One of them introduced a Medium: M1, in the #473 kept-body change.

**Result:** no Critical. **1 High, 4 Medium**, and a set of Lows. The High predates the audit-19
fixes and sits in a path no test drives with a real HTTP/2 session.

### High

| # | Area | Location | Defect | Failure scenario | Smallest fix |
|---|---|---|---|---|---|
| H1 | HTTP/2 server (C in Keel; relies on nghttp2 closing a half-closed stream inside `session_send`) | `http2_server.c` `kl_http2_server_upgrade_from_h1` (~789), `h2_cb_on_stream_end` (528-548), `h2_cb_on_stream_reset` (552-564), `h2_stream_destroy` (44-64); `http2_nghttp2_server.c` `ng_on_stream_close_cb`, `ng_server_flush` | The h2c Upgrade answers stream 1 outside `session->recv`, so `in_recv == 0` and the flush really sends. Stream 1 is half-closed (remote) after `nghttp2_session_upgrade2`; END_STREAM closes it inside the send, and `on_stream_close` destroys it through `h2_cb_on_stream_reset`. `h2_cb_on_stream_end` then destroys the same pointer again, and `num_streams` becomes -1. | Unauthenticated, any plaintext server with h2 enabled: `GET /` with `Upgrade: h2c` and an empty `HTTP2-Settings`, then the preface and HEADERS for stream 3. `h2_stream_create` passes `-1 >= max` and memsets `streams[-1]`, several KiB before the heap block (heap corruption). The stream is never found again and the connection is never idle. | Destroy the stream before the flush at every submit-then-destroy site, and have `h2_stream_destroy` refuse a pointer outside `[0, num_streams)`. Add an nghttp2-backed test: upgrade, then a second request. |

### Medium

| # | Area | Location | Defect and scenario | Fix |
|---|---|---|---|---|
| M1 | Server, completion (C; introduced by #473) | `completion_http_server.c` `kl_http_comp_post_read` (~1295), `comp_post_recv_buf`; `http_connection.c` suspend at dispatch (~831) and the SUSPENDED return after the leftover feed (~944) | A streaming-async handler that suspends at dispatch with no body bytes read along with the headers (`leftover_len == 0`, typical with `Expect: 100-continue`) leaves `read_len` at the head length. A resume that awaits and un-pauses the body posts the plaintext receive at `read_buf + read_len`; `comp_start_body_read` returns on `comp_recv_posted` before its `read_len = 0`, and the body core is fed the request line and headers as body. Header values (auth tokens, cookies a front end injected) reach the body reader; chunked bodies fail with 413. IOCP, io_uring and pollcomp, plaintext only. | Reset `read_len` to 0 in `kl_http_comp_post_read` before posting (kept bytes are already guarded by `body_kept`), or in the dispatch whenever it suspends with nothing kept. Test: the #473 un-pause test with the whole body sent after the head, for Content-Length and chunked. |
| M2 | Substrate, datagram (C) | `datagram_send.c` `kl_dgram_send` fast path (326), `send_pump` (232-235), `kl_dgram_send_on_complete` (484-485) | Any failure of an ordinary send other than would-block latches the sticky `s->err`, and every later send returns `KL_DATAGRAM_ERROR`. The nineteenth pass's M1 covered only receive. ENETUNREACH, EHOSTUNREACH, ECONNREFUSED (a connected peer's ICMP report, which Linux and the BSDs return from the next send), ENOBUFS and EPERM concern one datagram. One query sent while the host has no route (laptop offline, container network not up) makes the built-in DNS resolver, the async client's default, fail every later query with `DNS_TX_FAILED`, with no nameserver failover. | Classify per-datagram errors (the ICMP-report errnos plus ENOBUFS, EPERM, EACCES, EMSGSIZE, EHOSTDOWN and the Winsock equivalents) as a recoverable drop through `send_drop_head`/`on_drop`, or as a per-call error that is not latched on the fast path; keep `err` sticky for EBADF and ENOTSOCK. Completion backends carry the errno so `on_complete` can make the same split. |
| M3 | Server, readiness TLS (P) | `http_connection.c` `HEADERS_OK` returning `conn_dispatch_request` (1160-1165), the pending drain only on INCOMPLETE (~1153); `async.c` READING_BODY arm (179-185) | The header read is capped at the free buffer (8 KiB). OpenSSL and mbedTLS hand back part of a record and keep the rest (`pending() > 0`). After dispatch returns READING_BODY nothing reads that remainder, and the socket never signals it. A client that sends 8.2 to 16 KiB of headers and body in one TLS write (Python `http.client` does) waits for the body timeout and gets a 408. The existing test uses a mock engine that leaves the bytes in the socket. | When the dispatch (or the async resume) lands in READING_BODY on TLS with `pending() > 0`, continue at `read_more_body`. Test with real OpenSSL on readiness and one 12 KiB write. |
| M4 | EFI (P) | `integrations/platform/uefi/socket_efi_tcp4.c` `efi_sock_send` (640-686), `pump_or_cancel`/`pump_until` (`KL_EFI_PUMP_SPINS`, 60000 x 1 ms); `event_efi.c` `el_drain` SEND (662-670) | The drain sends synchronously, and `efi_sock_send` pumps the Transmit token for up to about 60 s. A timeout ends in EIO and quarantine, never would-block, so the drain's would-block arm is unreachable on this path. A client that pipelines and stops reading fills the send buffer and the window; if EDK2's TcpDxe queues the Transmit token, every other connection stalls for 60 s per attempt, and reconnecting repeats it. | Submit Transmit and return EAGAIN while the token is pending, polling it with CheckEvent on later drains (as `kl_uefi_socket_recv_ready` does for Receive). At minimum, a short pump bound for server sends. |

### Low (grouped)

- **Server:** kqueue keeps a READ filter enabled once after a nested complete inside `on_resume`.
  `async_rearm` with mask 0 is an empty `kl_event_add` that succeeds (`async.c:64-67`); use
  `kl_event_mod`.
- **Client:**
  - A successful async start writes `cfg->sockets` permanently into the shared `KlEventCtx`. A start
    that fails after selection (DNS failure, allocation, connect) also leaves it switched. The server
    and other clients then run their I/O through that provider. Fix: keep the provider on the client
    and never write `ev_ctx->sockets`.
  - A protocol-relative `Location: //name` against an `http+unix://` base becomes a relative socket
    path (`url.c:282-296`); reject it.
- **HTTP/2 and WebSocket:**
  - The h2 client's recv-failure path still fires `on_error` after a close from `on_resp`
    (`http2_client.c:515-517`); add the `H2C_CLOSED` guard.
  - A completion TLS WebSocket without the drain retries `conn_write` 256 times before failing a
    frame over the 1 MiB bound. This is intended since N2 and documented; checking once in
    `ws_send_frame` would fail fast.
- **Substrate:**
  - IOCP fails a datagram receive for good after 16 immediate ICMP reports in a row, on a socket
    adopted without configure; readiness returns would-block there instead.
  - io_uring -EAGAIN from submit with an empty CQ ring returns at once, and the loop spins while the
    kernel is short of memory. Wait for one CQE on -EAGAIN when nothing was reaped.
- **Integrations:**
  - The EFI N7 fix tells this connection's ops from an earlier one's with `kl_uefi_conn_valid_h`. That
    is also false for a live-generation connection marked `dead` by a failed cancel-drain, so the
    other op is freed without a completion and the connection leaks. Compare the generation only.
  - lwIP raw fails an in-flight response on a peer FIN (`lwr_srv_recv`, `p == NULL`), so a client that
    half-closes after its request gets a truncated response. Fail the send only on an error or a reset.

### Verified clean

The nineteenth pass's fixes as listed above, including:
- `dispatch_depth` bookkeeping on every frame, and the nested-complete add/mod fallback on epoll,
  poll and WSAPoll.
- The kept-body accounting.
- The chunked decoder caps: inclusive, kept across split feeds, obs-text allowed, bare LF and control
  bytes rejected.
- The TLS WebSocket bound on data, control, Close and pong frames and on drain flushes.
- The WebSocket drain push ordering and the empty-to-pending WRITE arm.
- The h2 client's close-during-send guard.
- The timer watermark: int64 ids, ties broken by id, `kl_timer_next_timeout` 0 for a due new timer.
- The io_uring send cap on every send SQE, and the zero-result rule.
- The POSIX and Windows ICMP receive skips: bounded, no busy loop.
- The datagram seam check and the per-segment GSO fallback.
- v4-mapped PROXY trust: narrows, never widens.
- The gzip optional-header state machine.
- The link-local nameserver skip.
- lwIP raw per-op terminals and slot generations.
- EFI cancel completions, with no double completion.

Whole-area sweeps:
- **Client:** lifecycle, Happy Eyeballs, redirects and credential stripping, proxy CONNECT, the
  response parser caps, pool keying and reuse, decompression caps, `resolver_cache.c`.
- **WebSocket:** framing, masking, UTF-8, fragmentation and close.
- **HTTP/2:** stream limits, header caps and adapter records.
- **Substrate:** completion core, the IOCP, pollcomp and io_uring op lifetimes, pipe streams, thread
  pool, wakeup.
- **DNS, PROXY and integrations:** DNS parsing and spoof checks, TCP framing, PROXY v1/v2 bounds, TLS
  verification defaults, miniz caps, production build flags.

The mechanical scan found no `strcpy`, `sprintf`, `gets` or `atoi` in core: the one `strcpy`
definition is the UEFI freestanding libc shim. There is no direct `malloc` or `free` outside the
default allocator and the thread trampolines.

**Local checks (Windows host, MinGW UCRT64 GCC):** the structural gates pass (check-no-milestones, check-no-em-dash, check-old-layout, check-test-layout, check-protocol-home, check-substrate-purity, check-protocol-no-integration, check-integration-seam, check-sockaddr-neutral, check-cloexec, check-no-dgram-life, check-pipe-seam, check-readiness-identity, check-no-kludp, check-no-httplegacy, check-msvc-parity, check-doc-refs, wx-guard, check-allocator-boundaries, check-backend-isolation, check-no-fsnode-in-protocols), and `test-win` (WSAPoll) and `test-win-iocp` are green. No POSIX, io_uring, sanitizer, cppcheck or scan-build run is claimed for this pass; those are CI lanes.

No source was changed by this pass. Suggested fix order:
1. H1.
2. M1 (a regression in a fix) and M2.
3. M3 and M4, which both need a real engine or firmware model to test.
4. The Lows.

## Nineteenth pass: final comprehensive audit after the re-audit loop (2026-10-06)

**Revision:** `main` `03cef23` (after #459 to #462). Read-only.
**Scope:** the whole `src/` tree, `include/keel/`, every integration (TLS backends, nghttp2, miniz,
lwIP raw, UEFI) and the build, in five parallel reviews:
- the HTTP/1 server, readiness and completion;
- the HTTP/1 client stack;
- HTTP/2 and WebSocket, server and client, with the nghttp2 adapter;
- the event and completion axis and the substrate;
- DNS, PROXY, the integrations and the build.

The axis review ran alongside (seventeenth pass in `keel_axis_audit.md`); its High is listed here
too, as N2. Every High below was re-checked against the code by hand. Confidence: **C** means the
path was traced end to end; **P** means plausible and needing a test. Items recorded earlier as
deliberately unfixed (S5, S9, E7, W11, X8 and the others listed in the eighteenth pass) were not
re-reported.

**Fixes since the eighteenth pass:** the re-audit fixes #452 to #459 and #461/#462 hold where they
were traced (completion queue pinning, `send_max`, the stream backpressure gate, `comp_closing` at
release, the async cancel at release, the WebSocket close and auto-ping changes, the mbedTLS sticky
failure, the DNS hook, deferred client errors, atomic hook registries, the completion-table check).
One of them introduced a High: N2, the TLS flush added to `conn_write` in #457.

**Result:** no Critical. **8 High, 15 Medium**, and a set of Lows. The Highs fall in three groups:
- **The async op lifecycle** (N3, N4): two call patterns the API allows corrupt or leak a
  connection.
- **Error and backpressure gaps on the main backends** (N1, N2).
- **The lwIP raw and EFI completion providers** (N5 to N8), which the core suites reach only through
  doubles: their cancel, terminal and handle semantics differ from what the HTTP driver relies on.

### High

| # | Area | Location | Defect | Failure scenario | Smallest fix |
|---|---|---|---|---|---|
| N1 | Substrate (C) | `src/socket_dgram_win.c:152-186`, `src/event_iocp.c:1267-1276`, `src/datagram_recv.c:210-247` | Windows reports an earlier `sendto`'s ICMP port-unreachable as WSAECONNRESET on the next receive, even on an unconnected UDP socket. It is mapped to EIO, and `recv_fail` stops the receiver for good. Nothing sets `SIO_UDP_CONNRESET`. | One unreachable nameserver (127.0.0.1 with no local resolver) and the built-in DNS resolver never receives again on Windows. A spoofed datagram that makes a `KlDatagram` server reply to a closed port stops it too. | `WSAIoctl(SIO_UDP_CONNRESET/NETRESET, FALSE)` in the configure path, and treat WSAECONNRESET/WSAENETRESET on a datagram receive as skip-and-continue (covers adopted fds). |
| N2 | Completion (C) | `http_internal.h` `conn_write` (TLS branch), `completion_http_server.c` `comp_tlsq_absorb_ring` | Since #457 every TLS write is moved from the engine's bounded ring into the uncapped output queue at once, so `tls->write` never refuses. TLS WebSocket output has no admission bound: the plaintext WebSocket bound and the stream gate (`kl_http_comp_stream_tls_full`) do not cover it. | A wss client that stops reading while the server broadcasts: every send succeeds and memory grows without bound, on io_uring, IOCP, pollcomp, lwIP raw and EFI. | Check the WebSocket admission bound before `tls->write` (an exported helper beside `kl_http_comp_stream_tls_full`), so the frame goes to the drain or fails; TLS twins of the two WebSocket bound tests. |
| N3 | Server (C) | `async.c:62-157`, `http_connection.c:415-443` `conn_process` | A handler may suspend and then complete in the same call (suspend, the thread-pool submit fails, write a 503, `kl_async_complete`). `kl_async_complete` drives the connection, then `conn_process` sets SENDING again. | Readiness with `Connection: close`: the slot is released twice and the free list self-loops (two accepts share one connection). Keep-alive: a phantom second response. Completion: the response is posted twice. | An `in_handler` flag set around the handler; `kl_async_complete` inside it runs `on_resume` and leaves the driving to the dispatch in progress. |
| N4 | Server (C) | `async.c:159-168`, `http_server_core.c:670-671` | `kl_async_cancel`, documented for deadline-as-failure, only retires the op. The connection stays SUSPENDED, out of the loop, with nothing posted, and the sweep skips every SUSPENDED connection. | Each upstream timeout resolved with cancel leaks a slot and an fd; after `max_connections` of them the server stops accepting. | Release a SUSPENDED connection whose op was cancelled (from the sweep, or from `kl_async_cancel` with a guard for the call from release itself). |
| N5 | lwIP raw (C) | `integrations/platform/lwip/lwip_raw_glue.c:752-780` | `lwr_srv_sent` runs the send pump, which can `tcp_abort` the pcb, and still returns `ERR_OK`; lwIP's `tcp_input` then keeps using the freed pcb. | A file response whose file is truncated mid-transfer: the next ACK aborts inside the sent callback, and lwIP writes to the freed `MEMP_TCP_PCB`. | The pump reports the abort and the callback returns `ERR_ABRT`. |
| N6 | lwIP raw (C) | `lwip_raw_glue.c:447-451, 1393-1394, 1459-1462`, `event_lwip_raw.c:640-655` | One terminal completion per connection, while a connection can hold a send and a recv. The second op never completes, and the cancel cannot find the dead slot. | A peer reset during a TLS handshake (or on WebSocket/h2) with both ops posted: the connection and its glue slot are never released; repetition exhausts the pool. | Emit one failed completion per outstanding op; surface READ failure for an armed dead slot. |
| N7 | EFI (C) | `integrations/platform/uefi/event_efi.c:297-301, 619-621` | `el_cancel` frees posted server recv/send ops without a completion. The driver releases a connection only at its last op's completion. | Every idle keep-alive connection the sweep times out leaks its slot; about seven idle clients take the firmware server down (`KL_EFI_MAX_CONNS` 8). | Mark cancelled I/O ops and have `el_drain` emit a failed READ/WRITE for each. |
| N8 | lwIP raw (C) | `lwip_raw_glue.c:389-397, 1355-1360`, `event_lwip_raw.c:655` | The `tcp_pcb*` doubles as the socket handle, and lookup prefers a live slot, so a dead connection's close finds a new connection that reused the pcb address. | A connection is aborted; the next accept reuses its pcb from lwIP's LIFO pool; the old connection's close tears down the new one and disarms it. | A slot handle (index plus generation) as the server `KlSocketHandle`, as the EFI provider does. |

### Medium

| # | Area | Location | Defect and scenario | Fix |
|---|---|---|---|---|
| M1 | Substrate (C) | `socket_dgram_posix.c:~205`, `datagram.c` `dg_rdy_pull` | A connected POSIX UDP socket stops receiving for good after one ECONNREFUSED/EHOSTUNREACH (the peer restarted). | Treat ICMP-induced receive errors as consumed; same on the completion paths. |
| M2 | Substrate (C) | `completion_dispatch.c`, `datagram.c` `kl_datagram_init_ex` | Optional `KlCompletionOps` slots are called without a NULL check; a provider without the datagram ops crashes at the first receive arm. | Refuse `kl_datagram_init_ex` with `KL_ERR_UNSUPPORTED` when a datagram slot is NULL (or make the routers fail on NULL). |
| M3 | Substrate (C mechanism, P reach) | `timer.c:501-526` | A 0 ms timer re-added from a timer callback fires again in the same `kl_timer_fire`, without end; a retry loop through the deferred-error timers starves all I/O. | Fire only timers that existed at entry (an id watermark). |
| M4 | Substrate (C) | `datagram.c` GSO path, `datagram_send.c:349` | A GSO group the kernel rejects (EINVAL above 64 segments, EMSGSIZE above 64 KiB) is dropped instead of sent per segment. | Force the per-segment path past the kernel limits; treat EINVAL/EMSGSIZE as unsupported for that group. |
| M5 | io_uring (C mechanism, P reach) | `event_iouring.c:497-507` | `sqe->len` is 32-bit: a 4 GiB remainder becomes 0, and the copy-sendfile fallback re-posts it forever. | Cap a post at 0x7ffff000 and set `send_max` to it; a 0 result on a non-empty send fails. |
| M6 | io_uring (P) | `event_iouring.c:1064-1066` | -EBUSY from submit (CQ overflow backlog on 5.5 to 5.18) is fatal to the loop. | Handle -EBUSY/-EAGAIN like -ETIME. |
| M7 | Client (C) | `http_client_async.c:1354-1368, 1752-1766` | A refused async start leaves the caller's shared `KlEventCtx` switched to the incompatible socket provider. | Check compatibility on a local, write `ev_ctx->sockets` only after it passes. |
| M8 | WebSocket (C) | `http_server_ws.c:158-169`, `http_server.c:650-659` | Readiness: a frame sent from outside the connection's own event lands in the drain with no WRITE interest and stalls; with auto-ping on, a healthy receive-only client is closed (1001). | Arm WRITE when the drain goes from empty to pending (or re-arm from the sweep). |
| M9 | HTTP/2 client (P) | `http2_client.c:471-475, 518-521, 161-184` | `on_error` fires on a client the user freed or closed from `on_resp` during a flush. | `h2c_on_send` fails at once when closed; skip `h2c_error` when freeing or closed. |
| M10 | Server (C) | `async.c:147-152`, `http_connection.c:795-811` | A streaming-async handler that suspends and then awaits the body: readiness never re-arms READ (408), and suspending at dispatch drops body bytes read with the headers and leaves the chunked decoder and body deadline unset. | A READING_BODY arm in the readiness resume; initialise body state before the handler; keep the leftover. |
| M11 | Server (C) | `drain.c:82-87` | Readiness: once the drain holds bytes it only appends, and a suspended connection has no WRITE interest, so a stream written while suspended stops after one would-block. | Try one write of the backlog before appending. |
| M12 | Server (P) | `http1_chunked.c:74-80, 157-166` | The chunked decoder accepts bare LF and control bytes in extensions and trailers, without a length cap: a framing mismatch with a front-end (the chunk-extension smuggling class). | Reject bare LF and control bytes; cap extension and trailer length. |
| M13 | PROXY (C) | `proxy_protocol.c:226-239`, `http_server.c:68-69` | On a dual-stack listener IPv4 peers arrive as `::ffff:a.b.c.d`, and the CIDR match compares families strictly: an IPv4 trust list never matches (fails closed, every request 400). | Unmap `::ffff:0:0/96` before matching. |
| M14 | miniz (C) | `decompress_miniz.c:316-340` | Streaming gzip needs the optional header fields in the same feed as the first 10 bytes; a split there fails a valid response (FNAME is gzip's default). | A small header state machine across feeds. |
| M15 | DNS (P) | `dns_sys_win.c:58-64`, `dns_resolver.c:1623-1644` | Windows: an `fe80::` resolver listed first is used without its scope and locks the list to IPv6; DNS fails although IPv4 servers exist. | Skip link-local entries (or keep the scope) and prefer a usable family. |

### Low (grouped)

- **Substrate:** the thread pool's nested-tick early return skips the wakeup drain (a nested loop
  spins); pollcomp leaks abort events' refs on an allocation failure (`return count ? count : -1`);
  an io_uring splice stage returning 0 reports success with a short body; an io_uring watcher whose
  poll fails goes silent; 32-bit length casts on Windows `writev`/IOCP posts; inheritable handles in
  the window after `socket()` on Windows and `pipe()` on POSIX; a named-pipe listener re-arm after
  every instance closed uses `first = 0`; datagram minor bounds (multicast `iface_index` ignored
  off Linux, unguarded batch products, an unchecked GSO total); the timer heap's doubling is
  signed-overflow-prone before its check.
- **Client:** sync redirect consults the policy for a hop past the limit; size-limit failures are
  reported as `KL_ERR_PARSE`; chunked trailers are merged into the response headers (a `Location`
  trailer is followed); freeing or cancelling inside a streaming callback is unsafe and
  undocumented; caller headers duplicating `Host`/`Content-Length`/`Transfer-Encoding` are sent and
  names are not checked as tokens; the proxy port is not range-checked; a failed redirect hop start
  always reports `KL_ERR_IO`; a stale pooled connection is not retried for idempotent methods.
- **HTTP/2 and WebSocket:** readiness WebSocket and h2 reads treat would-block as close; the Close
  echo is lost when the drain is pending; h2 cleanup destroys body readers without `on_error`; an
  nghttp2 header copy failure drops the header silently and response headers past 64 are dropped;
  the WebSocket client reports a Close-less EOF as 1001 (RFC: 1006) and fails some protocol errors
  without a Close; would-block is read from `errno` instead of `kl_sock_io_status` in the h2 and
  WebSocket clients; the documented nghttp2 floor (1.57) predates the CONTINUATION fix (1.61).
- **Server:** a partial PROXY header from a trusted source busy-loops readiness until the timeout;
  `max_header_size` below 8192 is not enforced; Windows listeners use `SO_REUSEADDR` rather than
  `SO_EXCLUSIVEADDRUSE`; handler headers can duplicate the server's framing headers, and 204/304
  get a Content-Length and body; a readiness stream drained but not ended spins on WRITE; graceful
  stop waits out idle keep-alive connections; the readiness TLS rejection drain counts ciphertext.
- **DNS, TLS, build:** a truncated (`KL_DGRAM_TRUNCATED`) DNS answer is used instead of falling back
  to TCP; a retransmit that hits would-block replaces the leg id; a learned server cookie is never
  forgotten; vendored llhttp builds without the stack protector and FORTIFY; OpenSSL ALPN
  reconfiguration can read past its array on a failed update; mbedTLS maps an out-of-range
  `client_auth` to none and accepts mTLS with no CA, `reset_failed` never clears, `write(0)`
  returns -1; `PEM_read_bio_PrivateKey` can prompt on a TTY; `efi_sock_close` can block the loop
  for a graceful close.

### Verified clean

Request parsing and CL/TE (llhttp strict, both rejected together), header limits, response header
injection guards, router, CORS, body readers, the completion output queue, connection reuse and the
drain teardown; the client URL parser, redirect method and credential rules, response parser caps,
pool keying and reuse, proxy CONNECT, the #462 deadline path, Happy Eyeballs, decompression caps,
`resolver_cache.c`; WebSocket framing, masking, fragmentation, UTF-8, size caps, close and liveness;
the h2 stream table, upgrade and session-done close; the readiness backends, provider validation,
watcher dispatch, listener credit, `KlCompLife`, backend op lifetimes on IOCP, io_uring and
pollcomp, datagram queues and close; DNS parsing (pointers never followed), spoof checks, TCP
framing; PROXY v1/v2 bounds; TLS verification defaults and adapters' rings; miniz bomb caps;
production build flags.

No source was changed by this pass. The fix order suggested: N1 to N4 first (main backends and the
public async API), then N5 to N8 (integrations), then the Mediums.

## Final C-audit sweep and targeted fix (2026-10-06)

**Revision:** `7878ef7` (merged #461; source-identical to local `5919644`).
**Scope:** mechanical scans of core C sources and public headers; direct Clang analysis of
applicable hosted POSIX core TUs; manual tracing of HTTP parsers, body readers, response framing,
router/middleware, client connect/error callbacks, completion output, timers, thread pool and
transport retirement. This is not an exhaustive proof of every backend or integration path.
The user redirected discovery to implementation after the finding below; no additional broad
review was pursued.

| Finding | Severity | Location | Proof and resolution |
|---|---|---|---|
| Deferred client error fell back to inline completion when a full timer heap could not grow | High (memory safety; allocation-failure dependent) | `src/protocols/http/http_client_async.c`, `async_complete_error` | A caller freeing the client in its documented `on_done` callback left `co_terminal` reading the freed embedded connect op. A temporary public-API ASan reproducer reported heap-use-after-free in `co_request_cancels`. Fixed by retiring the mandatory request deadline before scheduling its replacement: the deferred callback reuses a heap slot without allocating. Failure to reserve the initial deadline rejects startup without callbacks. |

Four new regressions cover full-heap allocation failure and initial deadline allocation failure
for direct and pooled clients. The original reproducer is sanitizer-clean after the fix. Focused
client free-in-done, deadline, Happy Eyeballs and pool suites passed under ASan/UBSan.
The full post-fix readiness sanitizer suite passed, as did pollcomp sanitizer runs of the
free-in-done and Happy Eyeballs suites (10 and 11 cases).

The pre-fix full readiness ASan/UBSan suite and structural gates passed. Seven separately
instrumented fuzz targets each completed 20,000 runs without a sanitizer finding. The local
fuzzer link required selecting GCC 13's installed C++ runtime explicitly. Direct Clang analysis
reported no diagnostics on the applicable core TUs or the changed client TU; this does not replace
the unavailable `scan-build` and `cppcheck` wrappers. No fresh Windows, native io_uring, or optional
integration execution is claimed. Generated fuzz corpus additions were moved to temporary storage.
No commit or push was performed in this follow-up.

## Eighteenth pass: comprehensive re-audit after the hardening round (2026-10-04)

**Scope:** the whole `src/` tree, `include/keel/`, every integration (TLS backends, nghttp2, miniz,
lwIP, UEFI) and the build, at `main` `d757d20`. That is after the hardening round (#415 to #436)
closed. Six parallel read-only reviews covered:
- the HTTP/1 server, readiness and completion;
- the HTTP/1 client stack;
- HTTP/2 and WebSocket, server and client, with the nghttp2 adapter;
- the engines and the completion axis;
- datagrams, DNS, PROXY, pipes and the platform layer;
- the integrations and the build.

Every High was then re-checked against the code by hand. Confidence: **C** means confirmed by
reading the path end to end. **P** means plausible and needing a test. Two Highs were found by two
reviews independently: S1 (also E1) and S2 (also E4).

**Fixes since the seventeenth pass:** every hardening-round fix was traced in its area and holds.
No Critical was found. The new findings fall in three groups:
- **The plaintext twin of the completion TLS work:** S1 and W4. The round moved TLS output on
  completion loops into an overlapped queue; plaintext streaming, WebSocket and the h2c upgrade
  response still `send()` on the loop thread, on sockets that block on io_uring and IOCP.
- **Peer-driven resource exhaustion on HTTP/2:** W1, W2, W3 and W6.
- **Correctness of the integrations themselves,** which the core tests reach only through mocks: I1,
  I2 and I3.

### Mechanical scans and gates

| Category | Result |
|---|---|
| Unsafe str/format, `atoi`/`atol`/`atof`, `alloca` | none in `src/` or `integrations/` (the UEFI libc shim defines `strcpy` for mbedTLS; a definition, not a use) |
| Direct `malloc`/`free` | only the PAL thread trampolines (I6, unchanged) and the default allocator |
| Unchecked allocations, VLAs | none found |
| Local gates | pass on `main` (the round's PRs ran all of them) |
| cppcheck, scan-build | green in CI at `d757d20` |

### High

| # | Location | Finding | Conf |
|---|---|---|---|
| S1 | `http_response.c:608-623` (`response_drain_writer`) via `drain.c:89-99`; `completion_http_server.c:218-237` (`comp_stream_pump`) | **Plaintext streaming on a completion loop sends synchronously on the loop thread** (also found as E1). `kl_drain_write` with an empty buffer calls `kl_sock_send` straight away, for the stream head and every chunk. io_uring and IOCP accepted sockets are blocking, so a client that stops reading an SSE, chunked or compressed stream blocks the whole server. On pollcomp (non-blocking) a direct send can overtake a queued pump op and reorder the chunked framing. | C (path) |
| W4 | `completion_ws.c:51-58` → `http_server_ws.c:170-171` (`conn_write_all`); `http2_server.c:795-808` → `h2_out_conn_write` | **Plaintext WebSocket, and the h2c upgrade's stream 1, send synchronously on a completion loop.** The same class as S1, on the paths the TLS queue does not cover: a client that sends pings and never reads blocks the loop on the auto-pong. | C (path) |
| E2 | `listener.c:150-155` (`l_pump`); `event_iocp.c:505-533` (`iocp_comp_post_accept`) | **One failed accept post closes the completion listener for good.** `arm_accept` returning -1 is treated as a broken listen socket. On IOCP that happens on a transient `WSASocketW` failure, or when a queued connection is reset before AcceptEx takes it. The server keeps running and never accepts again. | C (close path); P (Windows sync-failure codes) |
| W1 | `integrations/http2/nghttp2/http2_nghttp2_server.c:160-193` (`ng_on_header_cb`), `:103-134` | **HPACK bomb: request headers are stored with no limit.** Each header is copied into the per-stream record, and the arrays grow without a count or byte cap (Keel clamps only later). One header in the dynamic table, referenced by one-byte indices across HEADERS and CONTINUATION frames, costs hundreds of MB per stream. | C |
| W2 | `http2_nghttp2_server.c:484` (empty SETTINGS), `:310-315`; `http2_server.c:544-548` | **No limit on concurrent streams, or on response copies queued for a peer that does not read.** The adapter sends empty SETTINGS, so nghttp2's incoming stream limit stays unlimited; a Keel stream is freed once its response is submitted, while the adapter's copy of the body (up to 16 MiB) waits in nghttp2. `initial_window_size` is never read. | C |
| W3 | `http_server_core.c:684-686` | **HTTP/2 connections never time out.** The sweep skips the HTTP2 state, and the session has no idle handling. A client that completes ALPN h2, or sends only the preface, and goes silent holds its slot forever. | C |
| I1 | `integrations/codec/miniz/compress_miniz.c:146-165` (`miniz_feed`) | **The streaming gzip compressor truncates its final block.** On the finishing call `remaining` is 0, so one `tdefl_compress` runs; when the final block needs more than the 4 KiB output buffer, the trailer follows an incomplete deflate stream. Any streamed compressed response of roughly 15 KiB of text or more is corrupt. Reproduced against miniz. | C (run) |
| I2 | `integrations/tls/openssl/tls_openssl.c:145-150` (`kl_bio_write`, completion mode) | **OpenSSL fails any completion-mode write past ~256 KiB.** A full output ring returns -1 with no retry flag, so `SSL_write` fails and the connection closes; the core expects WANT_WRITE and retries after absorbing the ring. Any buffered HTTPS response body over ~250 KiB on io_uring, IOCP, pollcomp, lwIP-raw or UEFI with OpenSSL, BoringSSL or LibreSSL is dropped. | C |
| I3 | `tls_mbedtls.c:231-244`, `tls_openssl.c:354-371` via `drain.c:90-104` | **The drain breaks the TLS write-retry contract.** After WANT_WRITE the drain retries from its own buffer, with a different pointer and possibly a larger length. mbedTLS flushes the pending record and acknowledges the new length, so bytes are reported sent that were never encrypted (a wss frame stream or HTTPS SSE silently corrupts); OpenSSL fails with `BAD_WRITE_RETRY`. | C (code); library semantics cited |
| T1 | `http_client_sync.c:101`, `:157`, `:971`; handshake `:205-224` | **The sync client runs TLS and sends on a blocking socket, so `timeout_ms` is not enforced there.** The fd is put back to blocking after connect; the handshake, `SSL_read` of a partial record and a large `send` then block with no limit. A server that accepts and never sends a ServerHello blocks the caller forever. | C |

### Medium

| # | Location | Finding | Conf |
|---|---|---|---|
| S2 | `http_connection.c:746-754`; `completion_http_server.c:155-163` | On completion with TLS, `100 Continue` stays in the engine's ring until the final response (also found as E4): `curl -T` stalls a second per upload, and a client that waits for it hits the body timeout. | C |
| S3 | `http1_parser_llhttp.c:135`; `http_request.h:84` | On 32-bit builds Content-Length is truncated to `size_t`: `4294967296` reads as 0, so the body is parsed as the next request (smuggling behind a 64-bit proxy). | C (code); P (no 32-bit lane) |
| E3 | sweep `http_server_core.c:668-720`; backends re-post partial sends without an event | The S13 fix is incomplete: a plaintext buffered or file response is one op until it finishes, so a long download never refreshes `last_active_ms` and is cancelled at `read_timeout_ms` (30 s by default). | C |
| E5 | `event_iocp.c:1207-1226` | An IOCP WRITE completion ignores the operation status: a `CancelIoEx` abort that reports partial bytes is re-posted, so a closing connection whose client stopped reading can leak its slot. | P |
| W5 | `http2_nghttp2_server.c:367-371` | "Graceful" shutdown calls `nghttp2_session_terminate_session`, which stops all output after its GOAWAY: in-flight responses are cut. | C |
| W6 | `http2_nghttp2_client.c:92-116`, `:140-158` | The client side of W1: a malicious server can run the same HPACK amplification against the client. | C |
| I4 | `tls_mbedtls.c:165-166`, `:212-214` | mbedTLS maps a bare TCP EOF to close_notify, so a truncated close-delimited HTTPS response is accepted as complete. | C |
| I5 | `tls_openssl.c:1111-1122` | The OpenSSL mTLS server sets no session ID context, so resumption attempts are rejected with a fatal alert. | P |
| X1 | `dns_resolver.c:1144`, `:1172-1180` | The DNS cookie check is keyed on the reply's source, not on the server the query went to: with several nameservers, a spoofer forging from a secondary bypasses the cookie. | C |
| X2 | `dns_resolver.c:249`, `:1235-1241` | SERVFAIL, REFUSED and NOTIMP end the leg with no failover to the next nameserver. | C |
| T2 | `http_client_sync.c:565-602` | The sync client has no overall deadline: interim 1xx responses or a trickle hold it forever. | C |
| T3 | `http_client_sync.c:57`; `http_client_async.c:1515`, `:1858` | Only the first resolved address is tried (sync always; async on its non-Happy-Eyeballs paths). | C |

### Low

| # | Location | Finding |
|---|---|---|
| S4 | `http_response.c:31-60` | Status codes missing from the table (416, 412, 451, 501, ...) are sent as `500`. |
| S5 | `completion_http_server.c:428-441` | Completion with TLS can complete a stream response its producer has not ended. |
| S6 | `http_router.c:24-27` | A NULL route handler is accepted and crashes the first request. |
| S7 | `http_body_reader_multipart.c:112-128` | Multipart parameter scanning looks inside quoted strings (field-name confusion). |
| S8 | `http_compress.c:56-61` | A failed `Vary` append (OOM) leaves `Content-Encoding` on an uncompressed body. |
| S9 | `file_io.c`; `http_connection.c:1161-1269` | The async file-I/O state machine is unreachable (`kl_file_io_create` returns NULL). |
| T4 | `http_redirect.c:158-190`, `:350` | Redirect API argument checks are weaker than the client's; early returns leave `*resp` unset. |
| T5 | `src/url.c:155-190` | The authority does not end at `?`/`#`; SP/CTL and fragments reach the request line; schemes are case-sensitive; relative references are not resolved. |
| T6 | `http_client_async.c:248-251` | A custom resolver's `ai_socktype`/`ai_protocol` are used for the TCP socket. |
| T7 | `http_client_proxy.c:55-58` | The CONNECT status check reads only the three digits. |
| T8 | `http_client_pool.c:147-165` | `kl_http_client_pool_free` leaves `capacity` set over a NULL table. |
| T9 | `http_client_internal.h:137` | `conn_last_err` is dead. |
| E6 | `event_ctx.c:333-347` | `kl_event_ctx_run` returns -1 on EINTR on epoll, kqueue and poll. |
| E7 | `event_pollcomp.c:500-505`; `event_iouring.c:865` | An accept EMFILE/ENFILE spins. |
| E8 | `event_iocp.c:552-561` | The TransmitFile chunk test seam is read from the environment in production builds. |
| W7 | `http2_nghttp2_server.c:221-224` | A failed response submit gets no RST. |
| W8 | `completion_http2.c:79-83` | Plaintext h2 on completion drops the closing GOAWAY. |
| W9 | `http_server_ws.c:716-723` | No WebSocket liveness check: auto-ping never expects a PONG. |
| W10 | `http_server_ws.c:461-470` | With an unlimited drain, auto-pongs can grow it without bound. |
| W11 | `http2_nghttp2_client.c:328` | The HTTP/2 client never refuses server push. |
| W12 | `http_server_ws.c:36-38`, `:749-754` | Public WebSocket entry points lack NULL guards. |
| W13 | `http2_server.c:458-521`; `http_server_ws.c:206` | Write-only fields; `initial_window_size` is never read. |
| X3 | `dns_resolver.c:656-663` | One bad search candidate stops the whole resolve. |
| X4 | `platform_posix.c:47-62` | Linux entropy falls back to the weak generator when `/dev/urandom` cannot be opened. |
| X5 | `unix_socket_node_posix.c:212-218` | The process-wide `umask` is changed during an AF_UNIX bind. |
| X6 | `proxy_protocol.c:48-58` | PROXY v1 validates only the source address and port. |
| L1 | `tls_mbedtls.c:283` | `feed_input` lacks the overflow-safe length check (caller-only). |
| L2 | `tls_mbedtls.c:845-876` | `kl_tls_mbedtls_client_ctx_create(NULL)` disables verification, unlike OpenSSL. |
| L3 | `decompress_miniz.c:377-398` | Bytes after the gzip trailer are ignored. |
| L4 | `integrations/*/Makefile` | The standalone integration builds lack FORTIFY and PIE. |

Already recorded and still open: E7, W11, W18, X5, X6, X8, S10, S11 (seventeenth-pass IDs), and C4.

### Recommended order

1. **The integration Highs,** small and independent: I1 (one loop condition), I2 (a retry flag),
   I3 (a moving-buffer mode and a pending-length guard).
2. **HTTP/2 resource limits:** W1, W2, W6 (one adapter change: SETTINGS plus header caps), then W3
   and W5.
3. **Completion plaintext output:** S1 and W4, by routing plaintext output on completion loops
   through the same per-connection queue the TLS work uses.
4. **E2, T1,** then the Mediums by area, then the Lows.

## Seventeenth pass: re-audit after the sixteenth-pass fixes (2026-10-02)

**Scope:** the whole `src/` tree, `include/keel/` and the nghttp2 and miniz integrations, at `main`
`cf03091`. That is after #365 to #390 closed the sixteenth pass and #367 added the per-hop redirect
check. Five parallel read-only reviews covered:
- the HTTP/1 server;
- the HTTP/1 client stack;
- WebSocket and HTTP/2;
- the transport substrate;
- engines and DNS.

Each review read the `d686342..cf03091` diff first, to look for regressions in the new fixes. Every
High and Medium was then re-checked against the code by hand. Confidence: **C** means confirmed by
reading the path end to end. **P** means plausible and needing a test.

**Sixteenth-pass fixes:** each was traced and holds for what it targets. The new findings fall in
three groups:
- **Regressions caused by those fixes:**
  - E12: E5 made pollcomp sockets non-blocking, which TLS flushes on that engine do not expect.
  - S16: S9 caps the leftover count on keep-alive, and that hides an over-send on early rejections.
- **Incomplete fixes:**
  - T19: T8 checks the decompression limit only after full inflation.
  - S15: S1 runs post-body middleware, but its header pointers are stale.
  - W24: W1 covers only the event path.
  - E13: E4 is fixed on IOCP only.
  - X9: X4 missed `dg_multicast`.
- **Older defects next to the fixes:** the rest.

### Mechanical scans and gates

| Category | Result |
|---|---|
| Unsafe str/format, `atoi`/`atol`/`atof`, `alloca` | none in `src/` |
| Direct `malloc`/`free` | only the PAL thread trampolines (I6, unchanged) |
| Local gates | all 21 pass (incl. `check-public-headers`, `check-no-fsnode-in-protocols`, `check-tier1-boundary`) |
| `clang --analyze` (MinGW, Windows-branch view) | 0 warnings |
| cppcheck | not available locally; runs in CI (green at `cf03091`'s PRs) |

### High

| # | Location | Finding | Conf |
|---|---|---|---|
| W19 | `integrations/http2/nghttp2/http2_nghttp2_server.c:377-382` (`ng_server_destroy`); client twin `http2_nghttp2_client.c:243-248` | **Per-stream adapter records leak when a session is destroyed with streams open.** `NgServerStream` holds a copy of the response body of up to 16 MiB. It and `NgClientStream` are freed only in `ng_on_stream_close_cb`, and `nghttp2_session_del` frees its streams without calling that callback. A client that requests a large response and drops TCP leaks the copy. This is remote, unauthenticated and repeatable. | C (adapter); P (nghttp2's no-callback-on-delete, per upstream `free_streams`) |
| W20 | `src/protocols/http2/http2_server.c:490-493` (`h2_out_conn_write`) via `ng_send_cb` | **Plaintext readiness HTTP/2 treats a would-block send as fatal.** `conn_write` returns -1 with EAGAIN on the non-blocking socket, and `ng_send_cb` maps -1 to `NGHTTP2_ERR_CALLBACK_FAILURE`. Any h2c response larger than the free send buffer (prior knowledge, or the new Upgrade stream 1) kills the connection and every stream on it. TLS (WANT_WRITE = 0) and completion (capture writer) are unaffected. | C |
| E11 | `src/socket_posix.c:217-222` (`__APPLE__` `kl_sockdef_sendfile`); callers `http_response.c:587-593`, `event_pollcomp.c:555-563` | **macOS: a file response stops at the first full send buffer and is reported complete.** On a non-blocking socket `sendfile` returns -1/EAGAIN with `len == 0`. The wrapper returns 0 for EAGAIN, and both callers read 0 as end of file. A file larger than the send buffer is cut short with Content-Length unmet, and keep-alive desyncs. kqueue (the macOS default) has always had this; since E5 pollcomp on macOS has it too. | C (code); P (not run on macOS) |

### Medium

| # | Location | Finding | Conf |
|---|---|---|---|
| S13 | `completion_http_server.c:813-893` (`comp_on_read`/`comp_on_write`); sweep `http_server_core.c:647` | **On completion loops, active transfers time out.** `last_active_ms` is never refreshed by a completed receive or send; it is set only at accept, at the TLS handshake and at keep-alive reset. A download or upload still running `read_timeout_ms` after the last reset is cancelled mid-transfer on IOCP, io_uring and pollcomp. epoll refreshes the clock, so this breaks the documented "active transfers are never timed out" rule only on completion. | C |
| S14 | `http_server_core.c:752-760` (`kl_http_request_resume_body`); `completion_http_server.c:942-945` | **Resuming a body read on completion can post a second receive.** The resume posts unconditionally, and `comp_recv_posted` is a single yes/no flag. Pausing then resuming in the same dispatch puts two receives into `read_buf`, and the body bytes overlap. Resuming while SENDING can leave a receive in flight when `conn_keepalive_reset` shrinks `read_buf`, so the kernel writes into freed heap. | C (path); P (app pattern) |
| S15 | `http_connection.c:357-369`, `:287-296`, `:1041` | **S1 is only half fixed.** Post-body middleware now runs, but it and the `access_log` callback still get `method`, `path` and header pointers into a `read_buf` that the body read has overwritten from offset 0, with the NUL terminators gone. A CSRF check reads body bytes as `Cookie`/`Origin`. A body that fills `read_cap` with no NUL can make a `strlen` on a header value read out of bounds. Access-log lines can be forged. | C (stale pointers); P (exploit) |
| T19 | `http_client_common.c:316-328`; `integrations/codec/miniz/decompress_miniz.c:132-190` | **T8 is incomplete for buffered bodies.** `max_response_size` is compared only after `decompress()` has inflated the whole body, up to miniz's 256 MB cap. A small gzip body still costs hundreds of MB, allocated synchronously on the loop thread, before `KL_ERR_TOO_LARGE`. The streaming path is bounded. | C |
| E12 | `completion_http_server.c:226-243` (`kl_comp_tls_flush`) vs `event_pollcomp.c:486-506` | **Regression from E5.** `kl_comp_tls_flush` treats any send ≤ 0 as fatal, written for a blocking socket. Since pollcomp accepted sockets became non-blocking, a full send buffer drops TLS streaming responses, SSE, WebSocket-over-TLS and upgrade flushes on pollcomp. io_uring and IOCP sockets are still blocking. | C |
| W21 | `integrations/http2/nghttp2/http2_nghttp2_client.c:114-135` | **HTTP/2 client: the final response after a 1xx is never reported.** nghttp2 delivers it as `HCAT_HEADERS`, and the adapter reports only `HCAT_RESPONSE` while appending headers across blocks. After a `103` then a `200`, the client reports status 103 with the 200's body. | C |
| W22 | `src/protocols/websocket/websocket_client.c:609-639`, `:677-695` | **The WebSocket client handshake does not drain TLS-held plaintext.** A `wss://` 101 response over 511 bytes in one record is read partially, and the connection then waits for socket readiness that never comes. It hangs, and there is no handshake timeout (W11). | C (code); P (trigger) |
| W23 | `src/protocols/http2/http2_client.c:161-169`, `:472-481`, `:714-715` | **The HTTP/2 client never asks for WRITE readiness.** A short `on_send` leaves output buffered in nghttp2 while only READ is armed. A request body larger than the send buffer can deadlock against a peer that sends nothing. TLS WANT_WRITE now reaches this. | P |
| W24 | `src/protocols/http2/http2_client.c:714-715`, `:271-290` | **W1 covers only the event path.** `kl_http2_client_request` flushes outside `in_event`. A stream close emitted there runs `on_resp`, and a `kl_http2_client_free` from that callback destroys the session under `nghttp2_session_send`. | P |
| W25 | `http2_server.c:425-433`; adapter `:195-196` | **One stream's body limit kills the whole HTTP/2 connection.** An over-limit body, or a reader returning -1, is mapped to a fatal session error. No 413 is sent, and every multiplexed stream dies (W6's sibling). | C |
| W26 | `http_server_ws.c:156-160` with `http_internal.h:56-71` | **WebSocket server sends without the drain can leave a truncated frame on the wire.** `conn_write_all` gives up mid-frame on EAGAIN, or after the TLS spin limit, and the connection stays open. The next frame starts inside the old payload, and framing desyncs. | C |

### Low

| # | Location | Finding | Conf |
|---|---|---|---|
| S16 | `http_connection.c:579-585` | **Regression from S9:** the leftover count is capped at Content-Length on keep-alive. `kl_http_conn_begin_drain` runs only after keep-alive is cleared, typically on an early rejection (401/413/415 from pre-body middleware), and the cap hides an over-send there. The connection then closes on unread bytes and the response is reset. | C |
| S17 | `http_connection.c:716-733`, `:751-770`, `:822-827` | A body-reader rejection or a malformed chunk inside the leftover bytes closes the connection with no 413 and no drain; the body-phase path sends a 413. | C |
| S18 | `completion_http_server.c:580-597` | Completion with TLS ignores `read_paused`: `comp_tls_drive` always posts the next receive. | C |
| S19 | `http_server_core.c:272-409` | `proxy_cidrs` leaks on every `kl_http_server_init` failure after it is allocated, including the new NULL-h2-factory path. | C |
| S20 | `http_cors.c:92-116` | `Vary: Origin` is sent only when the origin is allowed; responses to a missing or disallowed Origin also vary by Origin. | C |
| S21 | `http_response.c:719-730` | `kl_http_response_end_stream` returns for HEAD before setting `stream_ended`, so a HEAD stream that reaches SENDING never completes. | P |
| S22 | `http_server_core.c:678-679`; `completion_http_server.c:205-206` | S4 covers READING_BODY only. A completion SENDING stream with nothing posted, waiting on a quiet producer, is never released by the sweep. | P |
| T20 | `http_client_async.c:1118-1127` | The deferred-error window leaves the winning fd's watcher live. An inline deferral (sync resolver, immediate connect) can re-enter CONNECTING and call the TLS factory again, leaking the first `KlTls`. | P |
| T21 | `http_client_common.c:273-281` | An HTTP/1.0 response without `keep-alive` is pooled (version not consulted). Was T18's HTTP/1.0 part, still open. | P |
| T22 | `src/url.c:145-151`; request builders | IPv6 literal hosts lose their brackets in Host, absolute-form and CONNECT, and Host drops a non-default port. | C |
| T23 | `http_redirect.c:428-453` | Async redirect: when `kl_timer_add` fails, the deferred error completes inline before `rc->inner` is assigned. If `on_done` frees `rc`, there is a use-after-free write (OOM only). | P |
| W27 | adapter `http2_nghttp2_server.c:161-170` | `on_request` failure is ignored: the stream gets neither a response nor RST_STREAM. | C |
| W28 | `http2_server.c:124-126` | The HTTP/2 server sends a DATA body for HEAD. | C |
| W29 | `http_connection.c:604`, `http2_server.c:379` | On h2c Upgrade, pre-body middleware runs twice (HTTP/1 phase, then stream 1). | C |
| W30 | `http_connection.c:631-639` | `Upgrade: h2c` is honoured on TLS connections, which is cleartext-only per RFC 7540 3.2. | C |
| W31 | `http2_server.c:661-672` | A malformed `HTTP2-Settings` is detected only after the 101 has been sent. | C |
| W32 | `http2_server.c:583-588`, `:521-527` | `h2_conn_abandon` frees the stream table without destroying live streams (prior-knowledge failure path). | C |
| W33 | `websocket_client.c:934` | The TLS pending drain runs only in OPEN, so a close echo held by TLS in CLOSING waits for the next socket event. | C |
| W34 | `websocket_client.c:934`, `http2_client.c:479` | Both clients call `tls->pending` without validating the factory's vtable; a backend without it crashes. | P |
| W35 | `http2_server.c:390-402` | A reader is created even for bodiless requests (END_STREAM on HEADERS), so a factory that requires a body type answers 415. | C |
| W36 | adapter `http2_nghttp2_client.c:122` | A header-copy failure in the client adapter is ignored (the client twin of I2). | C |
| W37 | `http_server_ws.c:83-92` | `ws_drain_writer` reads errno after a TLS -1 (the stale-errno class). | C |
| W38 | adapter `http2_nghttp2_client.c:219` | `:scheme` is hard-coded to `https`, including for h2c over `http://`. | C |
| W39 | adapter `http2_nghttp2_server.c:80-98` | A partial `ng_sstream_grow` failure frees arrays at the wrong size (OOM only). | C |
| X9 | `src/datagram.c:861-876` | X4 follow-on: `dg_multicast` does not check for a cleared fd. It passes `KL_INVALID_SOCKET` to the provider and reports `KL_ERR_IO`. | C |
| X10 | `src/url.c:236-249`; `http_redirect.c:77-93` | A redirect between two `http+unix` sockets counts as same-origin (host and port are empty), so `Authorization` and `Cookie` follow it to another local socket. | C |
| X11 | `src/platform_wakeup_win.c:130-187` | The Windows wakeup pair keeps Nagle on, so a 1-byte signal can wait up to the delayed-ACK timer (~200 ms). | C (code); P (latency) |
| X12 | `src/sockaddr_native.h:93-96` | The AF_UNIX path length is bounded by `sizeof sun_path`, not the returned `len`, so stale bytes can appear in a peer path (in bounds). | P |
| X13 | `src/datagram_recv.c:226-244` | No per-event receive budget: a UDP flood keeps one readable callback running and starves timers. | P |
| X14 | `src/platform_pipe_win.c:39-56` | The pipe-name locality check is textual only. Whether `..` after `\\.\pipe\` can reach a remote pipe is untested. | P |
| X15 | `src/timer.c:68` | `now + delay_ms` wraps for "never" values near `UINT64_MAX`, so the timer fires at once. | C |
| E13 | `event_pollcomp.c:562`; `event_iouring.c:608-611`, `:925-928`; `http_response.c:593` | E4 is fixed on IOCP only. A file that shrinks after sizing is still reported fully sent on pollcomp, io_uring and readiness. | C |
| E14 | `src/event_iocp.c:1082-1101` | Any watcher re-post failure, including a transient one, marks the watcher dead. A healthy socket is then reported every tick, and the loop spins. | P |
| E15 | `dns_resolver.c:1189` | The cookie drop is permanent per nameserver. A mixed fleet behind one address (some members without cookies) loses valid answers to timeouts. | P |
| E16 | `dns_resolver.c:1167-1181` | The TC branch runs before the cookie check, so a cookie-less spoofed TC answer can still force TCP fallback (no poisoning). | C |

Already recorded and still open: E7, W11, W18, X5, X6, X8, S10, S11.

### Recommended order

1. **The three Highs:**
   - W20: map a plaintext would-block to 0 in `h2_out_conn_write`.
   - E11: return -1/EAGAIN from the Apple `sendfile` wrapper when nothing was sent.
   - W19: track live per-stream records in each nghttp2 adapter and free them on destroy.
2. **The two regressions from the sixteenth-pass fixes:** E12 and S16.
3. **Completion server correctness:** S13, S14, S18 and S22, with S15 as its own change (keep the header region valid through the body read).
4. **The rest of the Mediums** (T19, W21 to W26), then the Lows by area.

## Sixteenth pass: re-audit after the fifteenth-pass fixes (2026-10-01)

**Scope:** the whole `src/` tree plus `include/keel/` at `main` `d686342`, after #348 to #364 closed
every fifteenth-pass finding. Five parallel read-only reviews covered:
- the HTTP/1 server;
- the HTTP/1 client stack;
- WebSocket and HTTP/2;
- the transport substrate;
- engines, sockets and DNS.

The significant findings were then re-checked against the code by hand. Confidence: **C** means
confirmed by reading the path end to end, and **C, run** means reproduced. **P** means plausible and
needing a test.

**Fifteenth-pass fixes:** all verified complete, apart from two follow-ons recorded below:
- T1: the H1 deferral leaves the request deadline armed.
- W-L: L12 missed one `upgrade_cap` free.

I7 (`h2c_on_response` leaking on a second call for a stream) is still open.

### Mechanical scans and gates

| Category | Result |
|---|---|
| Unsafe str/format, `atoi`/`atol`/`atof` | none in `src/` |
| Direct `malloc`/`free` | only the default-allocator seam and the PAL thread trampolines (I6, unchanged) |
| Dead code, VLAs, stack arrays | none; every macro-sized stack buffer is at most 16 KiB |
| Build hardening | unchanged (`-fstack-protector-strong`, `_FORTIFY_SOURCE=3`, PIE, RELRO/now, noexecstack) |
| Local gates | all 18 pass |
| `clang --analyze` (MinGW, Windows-branch view) | 87 TUs analyzed, 0 failed, 0 warnings |

### High

| # | Location | Finding | Conf |
|---|---|---|---|
| S1 | `http_connection.c:766,826,962`; `http_router.c:248-258`; completion twin in `completion_http_server.c` | **Post-body middleware is matched against overwritten bytes.** Body reads land at `read_buf[0]`, where `req->method`/`req->path` point. The documented limit covers handlers: header pointers die once body reading starts (`overview.md:222`). But Keel's own `kl_http_router_run_post_middleware` then matches on those pointers. A POST whose body arrives in a later read skips its post-body middleware, the documented home of CSRF checks, while the handler still runs. | C |
| X3 | `thread_pool.c:225-232` (with `:140`, `:158-159`) | **Work-queue ring overflow.** The ring holds `queue_cap` items, but admission only checks `inflight < queue_cap + num_workers`. A burst of submits before idle workers dequeue wraps the tail over unconsumed items. Those items are lost, so a suspended connection hangs. The items that overwrote them run twice: a double `done_fn`, double `kl_async_complete` or double free. | C |

### Medium

| # | Location | Finding | Conf |
|---|---|---|---|
| S2 | `http1_parser_llhttp.c:85-101`; `http_connection.c:543-552` | llhttp reports an empty header value (`X:\r\n`) at the first byte of the NEXT line, with length 0. NUL-termination then blanks that header's name, so it vanishes from lookups. With `Content-Type` gone, for example, multipart returns 415. | C, run |
| S3 | `http_server_core.c:729-740`; HTTP conns never re-run the read-facet init | `read_paused` survives release. A client that disconnects while its body is paused poisons the pool slot, and every later upload on that slot stalls (408 on readiness). | C |
| S4 | `http_server_core.c:609-610,663-664` | On a completion loop, the sweep reclaims a timed-out connection only by cancelling an in-flight op. A connection with nothing in flight (a paused body read, S3) is never released, leaking the slot, fd and credit. | C (logic) |
| S6 | `http_router.c:129-130` vs `:166-167` | Route matching tolerates a trailing `/`, but exact middleware matching does not. `GET /admin/` reaches the `/admin` handler and skips the `/admin` middleware (auth bypass). | C |
| T1 | `http_client_async.c:1087-1093`, `:418-427`, `:1073-1078`, `:1558` | H1 follow-on: the deferred-error path leaves `deadline_timer` armed. If both timers are due in one pass, `he_on_deadline` completes inline (the connect op is now DETACHED), and `async_deferred_error` completes again because it has no DONE check. The result is a double `on_done`, or a UAF if the first call freed the client: free skips `done_timer` once the state is DONE. | C |
| T2 | `http_client_async.c:536-561` | `https://` with `cfg->tls` set but `factory == NULL` falls to the plaintext branch (fail-open). The pooled client then files the plain connection under `is_tls=1`. The sync and tunnel paths fail closed. | C |
| T3 | `http_client_async.c:733-737` and the other send sites; TLS backends return 0 for WANT_WRITE | A TLS write that hits backpressure is treated as a fatal I/O error. Async HTTPS uploads larger than the socket buffer fail with `KL_ERR_IO`. | C |
| T4 | `http_client_pool.c:27-42` | Pooled connections are keyed by host/port/is_tls/proxy only, not by TLS config. A strict-verify request can reuse a connection made with verification off, or under another mTLS identity. The keying is documented, but it is a security hazard. | C |
| T5 | `http_client_sync.c:546-550`; `http_client_pool.c:173` | Sync pooled HTTPS polls the socket while TLS holds buffered plaintext, stalling until `timeout_ms`. Reused connections also stay non-blocking, so TLS "retry" zeros are read as EOF. | C / P |
| X1 | `stream_write.c:229-230`, `:123` | An abortive close (`kl_stream_cancel`) still pumps the next queued batch after a write completion, and never cancels it. A peer that stops reading then pins the stream, and `on_close` never fires. | C (path) |
| X2 | `datagram.c:557-565` | `kl_datagram_send` has no `dispatch_begin/end` bracket, unlike the batch and GSO paths. `on_drain` can call `close_cancel`, whose `on_close` can call `kl_datagram_free` inside the send; `dg_reconcile_write` then touches freed state. | C |
| W1 | `http2_client.c:255-258` | The HTTP/2 client has no free-from-callback protection. `kl_http2_client_free` in `on_resp` frees `c` and the session, then `h2c_stream_remove` and nghttp2 run on freed memory. The WebSocket client got this protection in #361. | C |
| W2 | `http_server_ws.c:600-604`; `http2_server.c:624-625`; `websocket_client.c:563-576,833-841`; `http2_client.c:416-419` | The readiness WS and H2 paths treat a TLS `read() == 0` (WANT_READ) as EOF and `write() == 0` as fatal. A TLS record split across TCP segments drops the connection. The completion twins are correct. | C (code) / P (trigger) |
| W3 | `http_server_ws.c:596-607`; `websocket_client.c:820-846` | The WS server and client read one 8 KiB buffer per event without draining `tls->pending()`. The rest of a 16 KiB record waits for the peer's next send. | P |
| W4 | `http_server_ws.c:401-419` | The WS server does not validate close frames (the client does since #361), and echoes 1005 on the wire for an empty close. | C |
| W5 | `http_server_ws.c:352-353` | With no `on_message` handler, `ws_deliver_message` returns before resetting message state, so the second message fails the connection with 1002. | C |
| W6 | `http2_server.c:403-404` | DATA for a stream Keel already finished (a pre-body rejection) returns -1. The nghttp2 adapter maps that to a fatal session error, aborting every multiplexed stream. | P |
| W7 | `http2_server.c:579-586` | h2c Upgrade sends 101 but never serves the upgrading request on stream 1 (RFC 7540 3.2), so `curl --http2` waits forever. | C |
| W8 | `http2_client.c:596-617` | After an immediate connect, the watcher keeps WRITE interest and every rearm fires again: a busy loop on `http+unix`. | P |
| S5 | `integrations/codec/miniz/compress_miniz.c:62-95`; `http_compress.c:48-68` | miniz allocates `10 + bound + 8` bytes but reports the smaller `total`, and core frees with the reported size. Every compressed response has an allocator size mismatch. | C |

### Low

| # | Location | Finding | Conf |
|---|---|---|---|
| S7 | `http_sse.c:26-48` | SSE field injection: a bare `\r` in `data`, or CR/LF in `event`/`id`, injects fields. Line `:46` also advances the pointer past `end + 1`. | C |
| S8 | `completion_http_server.c:456-462` | h2c prior-knowledge on completion passes the preface without its magic, while readiness passes it whole. With h2 configured but no hooks, a `memcmp` over-reads the 25-byte preface array. | C / P |
| S9 | `http_connection.c:687-754` | Body bytes that arrive with the headers are not counted. A rejected or `Connection: close` request lingers in DRAINING until EOF or 500 ms. | C |
| S10 | `http1_chunked.c:74-80,157-166` | Chunk extensions and trailers accept bare LF and CTLs, with no length bound, outside `max_body_size`. | P |
| S11 | `http_connection.c:462,476-478` | A partial PROXY header from a trusted peer busy-loops on level-triggered readiness. | P |
| T6 | `http1_response_parser_llhttp.c:219,323`; `http_client_common.c:411` | After a 1xx with headers and then a final response with none, the 16-slot header array is freed with size 0. | C |
| T7 | `http_client_async.c:1155-1157` | A user resolver without `cancel` is accepted, though the contract requires it. A timeout during RESOLVING then leads to a callback into freed memory. | C |
| T8 | `http_client_common.c:254-316`; miniz 256 MB cap | `max_response_size` does not bound decompressed output, and the async client ignores a decompression failure. | C |
| T9 | `http_client_async.c:432-464` | The request deadline starts after resolution, so a stuck resolver hangs the request. | C |
| T10-T18 | client (see review notes) | <ul><li>Pool peek treats stray bytes as a healthy connection.</li><li>`set_hostname` is optional, so a backend without it skips the hostname check.</li><li>Proxy auth is not checked for CR/LF.</li><li>A 101 ending at a read boundary is pooled.</li><li>The final decompress flush result is ignored.</li><li>A `body_read` that returns too much is not rejected.</li><li>An OOM during transfer leaks the body.</li><li>`reset` keeps `body_streamed`.</li><li>`Connection: close, x` is pooled.</li></ul> | C / P |
| W9-W18 | WS/H2 (see review notes) | <ul><li>A full stream table, or RST(NO_ERROR), leaks a slot.</li><li>The request is submitted before its stream is created.</li><li>The client `timeout_ms` is never used.</li><li>Fragmentation edge cases.</li><li>An OOM while copying leftover bytes drops them.</li><li>The handshake checks too few headers.</li><li>Calling `enable_drain` twice.</li><li>An H2 body without content-length gets no reader.</li><li>A NULL h2 factory is not rejected.</li><li>Swap-remove moves `req`.</li><li>W-L: `websocket_client.c:1110` still frees `upgrade_buf` with `upgrade_len`.</li></ul> | C / P |
| X4-X8 | substrate | <ul><li>Datagram calls reuse a stale `dg->fd` after close.</li><li>Freeing the thread pool from `done_fn`.</li><li>A 0 ms timer re-armed in its own callback starves I/O.</li><li>`kl_free(NULL, size)` in a batch unwind.</li><li>A wrong-family accept leaks the accepted value.</li></ul> | C / P |
| E1-E10 | engines/DNS | <ul><li>E1: a failed IOCP re-arm retires the watcher silently.</li><li>E2: io_uring `kl_event_del` drops POLL_REMOVE when there is no SQE.</li><li>E3: IOCP TransmitFile with count 0 sends the whole file.</li><li>E4: a truncated file is reported as fully sent.</li><li>E5: pollcomp makes accepted sockets blocking.</li><li>E6: DNS cookies are bypassed by omitting them.</li><li>E7: the UDP source port is fixed.</li><li>E8: the `/dev/urandom` fallback is constant.</li><li>E9: `open`/`fopen` are outside the cloexec gate.</li><li>E10: Windows hosts-path init is not thread-safe.</li></ul> | C / P |

### Recommended order

1. **The two Highs.**
   - S1: match post-body middleware at header time (a per-request bitmask) rather than after the body.
   - X3: bound admission on `work_count < work_cap`.
2. **The client and server correctness Mediums:** T1, T2, S2, S3+S4, S6, X2, W1.
3. **The TLS readiness group** (T3, T5, W2, W3) as one change, with a split-record test.
4. **The rest**, grouped by family as in the fifteenth pass.

## Fifteenth pass: whole-tree re-audit after the transport and pipe work (2026-09-30)

**Scope:** whole `src/` (103 `.c`, 33,411 lines) including `src/protocols/`, the internal and public
headers, and the `Makefile`, at `main` `f0d9085`. Trigger: everything merged since the fourteenth pass
(2026-08-26): the `KlStream` facets and writable-again edge, `KlListener`, Windows Named Pipes,
anonymous pipe pairs (Windows IOCP and POSIX readiness), close-on-exec on Keel's own descriptors, and
the MSVC/allocator-validation work. Five read-only reviewers swept the tree by subsystem (transport
substrate + readiness core; completion engines + socket axis; datagram + DNS; HTTP server + parsers;
clients + HTTP/2 + WebSocket); every Critical, High and Medium below was then re-traced against the
code, and C1 was reproduced end to end.

**Verdict: NOT CLEAN.** 1 Critical / 3 High / 9 Medium / 12 Low / 7 Informational. The Critical is
a long-standing HTTP/1 server defect that the suite never exercised: a request whose request line or
headers arrive in more than one read is rejected on both event models. None of the findings is in the
new transport or pipe code; those modules came out clean.

**Verification key:** **R** = re-traced in code by the auditor (and **E2E** = reproduced by running
it); **C** = cited from a slice reviewer, read but not independently re-traced.

### Mechanical scans (whole tree)

| Category | Result |
|---|---|
| Unsafe str/format (`strcpy`/`strcat`/`sprintf`/`vsprintf`/`gets`) | none in `src/` |
| Unsafe int parse (`atoi`/`atol`/`atof`) | none |
| Direct `malloc`/`free` | only the PAL thread trampolines (`platform_thread_posix.c:17,24,28`, `platform_thread_win.c:28,35,39`), deliberate and commented (the trampoline state is the thread's own and freed by it); see I6 |
| Dead code (`#if 0` / `if (0)`) | none |
| VLAs / large stack arrays | none found |
| Build hardening | `-fstack-protector-strong`, `-D_FORTIFY_SOURCE=3`, PIE, `-z relro -z now -z noexecstack` in production flags; `make debug` = ASan+UBSan |
| CI (PR #346 head `d8b7bb4`, tree identical to `f0d9085`) | 26/26 green incl. ASan+UBSan, sanitized io_uring and pollcomp, cppcheck, scan-build, all gates |

### Critical

| # | File:Line | Issue | Evidence | Fix |
|---|---|---|---|---|
| C1 | `src/protocols/http/http_connection.c:886` (readiness), `src/protocols/http/completion_http_server.c:468` (completion); parser `http1_parser_llhttp.c:161` | **A request split across reads is re-fed from byte 0 to a stateful parser.** After `KL_HTTP1_PARSE_INCOMPLETE` the connection returns to READING; the next read appends to `read_buf` and `parse()` is called again on `read_buf[0..read_len)`. `llhttp_execute` continues from where it stopped, so it parses the already-seen bytes a second time. The parser is reset only on keep-alive and when the header buffer grows. | **E2E, R.** Parser probe, request `GET /x HTTP/1.1\r\nHost: a\r\nX-Y: zz\r\n\r\n` split at every one of 36 points: re-feeding the whole buffer (as the connection does) is wrong at **36/36** (most `PARSE_ERROR`; splits 22-24 and 30-33 return `HEADERS_OK` with phantom headers; splits 5-6 leave `path_len = 4272` for a 2-byte path, an out-of-bounds length that `on_url_complete` then scans); feeding only the new bytes is correct at 36/36. A real `KlHttpServer` on loopback: the request in one write gets `200 OK`, the same request in two writes 200 ms apart is closed without a response at every split tried, on **WSAPoll and IOCP** alike. | Track a parsed offset per connection and pass only `read_buf + parsed_off .. read_len` to `parse`, advancing it by `consumed` on INCOMPLETE; reset it wherever the parser is reset. On the completion grow path also reset the parser and `c->req` (as readiness does), since a realloc moves `read_buf`. Add a split-request test at many split points on both event models. |

Why it survived: loopback clients (the test suite's own client, curl) deliver a small request in one
segment, and the existing partial-header tests (`test_timeout.c` `partial_headers`, the slowloris
smokes) never complete the request. Real traffic splits: large cookies, slow links, TLS records.

### High

| # | File:Line | Issue | Evidence | Fix |
|---|---|---|---|---|
| H1 | `src/connect_op.c:81-84` `co_terminal`; `src/protocols/http/http_client_async.c:331-344` `cli_co_on_done`, `:1491` `kl_http_client_free` | **Use-after-free when the client is freed inside `on_done` after a DNS or connect failure.** `cli_co_on_done` calls `async_complete_error`, which runs the user's `on_done`; `kl_http_client_free` frees the client unconditionally, and the `KlConnectOp` embedded in it is still mid-dispatch (`co_request_cancels`, `in_dispatch--`, `co_finalize` follow). With an owned resolver, the resolver is also destroyed inside its own callback. The shipped `examples/async_client.c:39` frees in `on_done`; the redirect wrapper does the equivalent. | **R.** NXDOMAIN, or every address refusing, reaches it. Tests free after the loop returns, so ASan never saw it. | Complete the request from `cli_co_on_detach` (the point where freeing is legal) instead of from inside the terminal dispatch, and document on `KlHttpClientDoneFn` whether freeing inside it is allowed. |
| H2 | `src/protocols/http/http_client_async.c:150` `start_connect`; same pattern `websocket_client.c:985`, `http2_client.c:570` | **TLS (and a proxy CONNECT tunnel) is skipped when a non-blocking connect completes at once.** `c->state = (rc == 0) ? KL_HTTP_CLIENT_SENDING : ...` bypasses `he_proceed_after_connect`, where the TLS session and the tunnel are set up. AF_UNIX connects complete at once on Linux, so `https+unix://` / `wss+unix://` send in plaintext with no certificate check: fails open. | **R.** `tests/test_unix_socket.c` uses a pass-through mock TLS and the sync client, so the plaintext send is invisible to it. | On `rc == 0`, go through `he_proceed_after_connect` (and the WebSocket/HTTP/2 equivalents) exactly as the asynchronous path does. |
| H3 | `src/protocols/websocket/http_server_ws.c:505-513`; `src/protocols/websocket/websocket_client.c:690-693` | **A WebSocket data frame larger than one read breaks.** `is_first` is derived from the opcode alone, so the second payload chunk of the same TEXT/BINARY frame looks like a new message. Server: `msg_len > 0` closes with 1002, so any message over about one read buffer, or one straddling a TCP read, is rejected. Client: the earlier chunks are discarded and `on_message` sees only the tail. Control-frame payloads split across reads are handled from their tail only. | **R** (server path); client path **C**. | Treat a frame as the start of a message only when its payload offset (`payload_before`) is 0; same for control frames. Add a test that sends one frame in several writes. |

### Medium

| # | File:Line | Issue | Evidence | Fix |
|---|---|---|---|---|
| M1 | `src/protocols/dns/dns_resolver.c:1051-1066` `dns_tcp_on_event`, `:969-1007` `dns_tcp_deliver`, `:920-935` `dns_tcp_fail`; `:453-456` | **Use-after-free when the resolver is destroyed from `done()` during a DNS-over-TCP completion.** `dns_complete`'s tail runs `dns_teardown`, which frees `r` synchronously when no datagram frame is active; the TCP path runs from a plain watcher, so after `dns_leg_settle` the deliver loop (`t->rneed`, `t->rlen`), `dns_tcp_fail`'s rescan and `dns_tcp_arm_idle` use freed memory. A nameserver that answers TC=1 drives the path. | **R.** | Bracket `dns_tcp_on_event` with `r->in_done++ ... --` and run the deferred teardown at its tail; return early after any settle once `destroy_requested` is set. |
| M2 | `src/resolver_cache.c:246-263` `cache_resolve`; `src/protocols/http/http_client_async.c:204-209` | **Every resolver-cache hit leaks the request handle under `KlHttpClient`.** A hit completes synchronously and returns a live `KlResCacheReq` that only `cache_cancel` frees; the client drops the handle whenever resolution completed inline. The resolver contract is also inconsistent: the DNS resolver frees its request inside `done`. | **R.** | Free the handle on synchronous completion and return NULL; state in `resolver.h` that a handle is dead once `done_fn` has run. |
| M3 | `src/event_iocp.c:259-279` `iocp_watch_reinterest` | **Each IOCP watcher interest change made from a callback leaks an op until loop close.** Between a dispatched completion and its re-arm the op owns no kernel I/O (`watch_rearm = 1`); reinterest marks it removed and calls `CancelIoEx`, which cancels nothing. The delete path handles this state (`:304-309`), reinterest does not. The async HTTP and WebSocket clients do exactly this (WRITE to READ after connect), so it is at least one op per request. | **R.** | If `old->watch_rearm`, just update its mask and udata and return; the next drain re-posts it. |
| M4 | `src/event_iocp.c:1060-1066` `iocp_watch_rearm_dispatched` | **A failed watcher re-arm leaves an op nothing frees, and `kl_event_close` then waits forever.** On failure `watch_rearm` is cleared and `watcher_removed` set; the delete path takes the `CancelIoEx` branch (nothing pending) and the quiesce pre-pass frees only `watch_rearm` ops, so the drain loop blocks with `INFINITE`. | **R.** | On failure keep `watch_rearm = 1` (with `watcher_removed = 1`) so both existing retirement paths free it. |
| M5 | `src/socket_posix.c:102-104` `kl_sockdef_socket`; `src/event_iouring.c:662`; `src/event_pollcomp.c:497`; `src/event_iocp.c:481`; `socket_winsock.c` | **Sockets can leak into child processes.** The default POSIX/Winsock socket creators and the completion-engine accepts (io_uring flags 0, pollcomp plain `accept`, IOCP `WSASocketW` without `WSA_FLAG_NO_HANDLE_INHERIT`) create inheritable sockets; only the readiness server's accept and the listener set close-on-exec. `check-cloexec` does not list socket creators. An embedder that spawns children (Hull) hands them open client and accepted connections, so peers never see EOF. | **R.** | `SOCK_CLOEXEC` / `accept4` / `WSA_FLAG_NO_HANDLE_INHERIT` at each creator; extend `check-cloexec` to `socket(`, `accept(`, `WSASocketW`, `io_uring_prep_accept`. |
| M6 | `src/platform_wakeup_posix.c:20-54`; `src/platform_wakeup_win.c:68-87`; `src/thread_pool.c:82,97` | **The wakeup write end is blocking and the drain reads 64 bytes, so a saturated pool can deadlock.** One byte per completed item; one 64-byte read per watcher callback. Under sustained completion rates above 64 per tick the pipe fills, workers block in `write`, and `kl_thread_pool_free` then joins a worker that nobody will unblock. `wakeup.h` promises that a full channel only loses a coalesced signal. | **R.** | Make the write end non-blocking and ignore EAGAIN; optionally drain until EAGAIN. |
| M7 | `src/protocols/http/http_client_async.c:1611-1640` `kl_http_client_start_pooled` | **Pooled requests cancel timer id 0 and have no deadline.** After `memset` the timer ids stay 0 (timer ids start at 0) and `timeout_ms` is never copied, so every completion or cancel calls `kl_timer_cancel(ev, 0)`, killing whichever timer holds id 0, and a pooled request to a silent server never times out. | **R** (no `-1` init or `timeout_ms` copy on this path; `start_s` has both at `:1248-1250`). | Mirror `start_s`'s initialisation and arm the deadline on the pool-hit path. |
| M8 | `src/protocols/http/http_redirect.c:103` `should_drop_header` | **Credentials follow a cross-origin redirect.** Only `Authorization` is stripped; caller-supplied `Cookie` and `Proxy-Authorization` go to whatever host a 3xx names. | **R.** | Also drop `Cookie` and `Proxy-Authorization` when `cross_origin`. |
| M9 | `src/protocols/http/http_client_async.c:915`; `http_client_sync.c:526-532` | **A truncated response is reported as success.** `status` is set in `on_status` but headers and body move into `resp` only at message-complete, and `llhttp_finish` is never called; EOF partway through headers or a Content-Length body returns success with no headers. | **C.** | Add a `finish` step to the response-parser vtable; at EOF succeed only if the parser reports complete. |

### Low

| # | File:Line | Issue | Evidence |
|---|---|---|---|
| L1 | `dns_resolver.c:1136` `dns_on_recv` | A duplicated TC=1 UDP answer, arriving after TCP recovery started, falls through and settles the leg (usually empty), so the real TCP answer is dropped; a later TCP failure can re-settle the done leg. Fix: ignore UDP for a leg once `tcp_pending`. | R |
| L2 | `http_conn_internal.h:100-103`, `http_connection.c` | `request_body_received`, `request_body_complete`, `drain_framing_usable` are never reset (only `c->req` is zeroed at `:142,845,944`), so a later request on the same pool slot can skip the #278 drain and RST. Fix: zero them on acquire and keep-alive reset. | R |
| L3 | `event_iocp.c:1211` | A successful zero-length datagram send completes with `bytes == 0`, is reported `ok = 0`, and poisons the send path; io_uring/pollcomp report `res >= 0`. | C |
| L4 | `event_iouring.c` re-prep paths (`iou_prep_send_tail`, `iou_prep_splice_*`), `iou_comp_cancel*` | Residual of the previous pass's stranded-op item: a re-prep or cancel that gets no SQE marks the op aborted with nothing queued, so it never completes. Only reachable if `io_uring_submit` cannot free a slot. | C |
| L5 | `thread_pool.c:182,194,282` | After a partial worker start the thread array is freed with the started count, not the allocated count (wrong size to a sized `KlAllocator.free`). | C |
| L6 | `platform_wakeup_win.c:32-39` | The loopback wakeup pair accepts whoever connects first; a local racer could take the `rd` end. Fix: verify the accepted peer against the client's local address. | C |
| L7 | `pipe_stream.c:561-566` via `listener.c:150-154` | A `kl_pipe_listen` whose first arm fails fires the owner's `on_close` for a listener it never received, then returns an error. | C |
| L8 | `http1_response_parser_llhttp.c`; `http_client_async.c:927` | Response framing: a 1xx completes as the final response; a HEAD response with Content-Length waits for a body; bytes after a complete response are dropped yet the connection is pooled. | C |
| L9 | `http_client_async.c` pooled path | Pooled requests ignore `cfg->proxy` (always direct). | C |
| L10 | `http_client_common.c:280-281`; `kl_http_client_remove_header`; `websocket_client.c` `wsc_build_upgrade` | Allocator size mismatches on three free paths. | C |
| L11 | `http2_client.c:210,248` | No response-size cap on HTTP/2 client data; an `RST_STREAM` is delivered as a normal response. | C |
| L12 | `websocket_client.c` | Masked server frames accepted; close code/reason unvalidated; handshake buffer uncapped; `on_close` runs before the connection is closed (freeing in it is a UAF). | C |

### Informational

- I1: pipelined HTTP/1 requests in the same read are discarded (`conn_keepalive_reset` sets `read_len = 0`); safe, but the client waits for the read timeout. Decide and document. (C)
- I2: a `max_header_size` below the 8192-byte base read buffer is not enforced (the 431 check runs only when the buffer is full). (C)
- I3: `event_iocp.c:766-769` treats `ERROR_MORE_DATA` as "no packet queued"; correct for byte read mode, which every Keel pipe uses, but a message-mode pipe would get two completions. Hardening only. (C)
- I4: WSABUF lengths are cast to `ULONG` without a clamp (`event_iocp.c:468,1145`, `socket_winsock.c:250`); not reachable at 4 GiB from the response writer. (C)
- I5: `pipe_stream.c` `rd_write` returns -1 after a short write when the watcher cannot be registered (ENOMEM); the stream is failed either way. (C)
- I6: the PAL thread trampolines allocate with `malloc`/`free` directly, contrary to the fourteenth pass's "none outside the default-allocator seam"; deliberate and commented. (R)
- I7: the fourteenth pass's L4 (`h2c_on_response` leak on a second call) is still open. (C)

### Checked clean (highlights)

- **New transport and pipe code:** `KlStream` facets (arm trampoline, `len > read_cap` fail-closed, writable edge with `in_writable` deferral, sticky `wq_err` releasing graceful close, exactly-once detach); `pipe_stream.c` `KlCompLife` retain/release on every post, failed post and dispatch, final release after every op, readiness mode holding a ref across callbacks and removing the watcher before the fd closes; `platform_pipe_posix.c` (`pipe2`/`FD_CLOEXEC`, non-blocking parent end only, per-thread SIGPIPE handling that leaves a pending SIGPIPE alone, fd+1 encoding); `platform_pipe_win.c` (bounded path conversion, length-checked SDDL, identification SQOS, anonymous pair client-PID check, every handle closed on failure); `listener.c`; `drain.c`; `event_ctx.c` watcher deletion during dispatch.
- **Named-pipe ops on IOCP:** exactly one completion per issue, life-token transfer, quiesce frees without re-issue, byte counts clamped.
- **DNS parser:** compression pointers skipped, never followed; label cap; `size_t` bounds on every field; exact 0x20 question echo; EDNS0/cookie option walk bounded; client-cookie mismatch rejected before learning; BADCOOKIE retries bounded; TCP framing capped at 65537 bytes.
- **Datagram layer:** cmsg length checks, overflow-safe control buffers, GRO split clamps, batch unwind, teardown deferral.
- **HTTP parsing limits:** `KL_MAX_HEADERS`, header-buffer growth capped with a `SIZE_MAX/2` guard, Content-Length zeroed under chunked, early 413, bounded drain; PROXY header peek bounded.
- **Clients:** WebSocket frame length/opcode/control limits and unmask offset; CONNECT build and status parse; HTTP/2 server header storage and `max_body_size`; Happy Eyeballs loser/straggler teardown (apart from H1); pool stale-connection peek.

### Coverage note

Read-only static audit on a Windows host, plus two local reproductions for C1 (a parser-level probe
and an end-to-end `KlHttpServer` run on WSAPoll and IOCP). Linux, macOS, io_uring and pollcomp
execution evidence is CI's. Slices not re-derived in depth because they are unchanged since the
fourteenth pass: `timer.c`, `connect_op.c` (apart from H1's path), `completion_core/dispatch.c`,
`event_poll.c`, `event_wsapoll.c`, `sockaddr.c`, `unix_socket_node_*.c`. No code was changed.

### Recommended order

1. **C1** first: it breaks ordinary HTTP/1 traffic on every engine and carries an out-of-bounds length.
2. **H1-H3**: a use-after-free reachable by following the shipped example, TLS failing open on an immediate connect, and WebSocket messages larger than one read.
3. The Mediums, grouped: DNS/resolver lifetime (M1, M2), IOCP watcher lifecycle (M3, M4), close-on-exec for sockets (M5, which matters to Hull), wakeup back-pressure (M6), client request hygiene (M7-M9).
4. Each fix with a test that fails first, per this repo's practice.

## Fourteenth pass: AF_UNIX node-cleanup security increment (#250/#251) + whole-tree re-audit (2026-08-26)

**Scope:** whole `src/` (93 `.c`) + `src/protocols/` + `integrations/` (67 `.c`) + headers +
`Makefile`. Trigger: the AF_UNIX node-lifecycle hardening merged at `4a5ac7e` (PR #251, issue #250 /
CodeQL #43): `src/unix_socket_node_posix.c` + the identity-anchored `src/unix_socket_node_win.c`. A
fresh six-subsystem sweep re-audited the rest of the tree (substrate core, datagram, event/completion
backends, HTTP server core + body/parse, client/http2/websocket/dns, integrations).

**Verdict: CLEAN.** 0 Critical / 0 High / 0 Medium / 4 Low + 2 Informational. The build is
`-Werror`-clean (0 warnings under `-Wall -Wextra -Wpedantic -Wshadow -Wformat=2`), `cppcheck` exits 0,
`scan-build` reports no bugs (verified on `unix_socket_node_posix.c` this pass), and the full suite is
ASan/UBSan-clean (`make debug-test`, 90 suites, 0 sanitizer hits). None of the Low/Info items is a
reachable memory-safety defect; the security-critical node-cleanup modules are clean.

### Mechanical scans (whole tree)

| Category | Result |
|---|---|
| Unsafe str/format (`strcpy`/`strcat`/`sprintf`/`gets`/`vsprintf`) | none in `src/`; grep hits are the word "gets" in comments. One freestanding `strcpy` *definition* in `integrations/platform/uefi/mbedtls_platform_uefi.c` (a libc shim, bounded, expected) |
| Unsafe int parse (`atoi`/`atol`/`atof`) | none |
| Direct `malloc`/`free`/`calloc`/`realloc` in `src/` | none outside the default-allocator seam (`allocator_default_stdlib.c`); all app allocation via `kl_malloc`/`kl_free` (with size) |
| Overflow guards (`SIZE_MAX`/`INT_MAX` before size arithmetic) | present across substrate, datagram slots/batch, HTTP header/body growth, websocket/DNS/proxy length fields |
| Dead code (`#if 0` / `if (0)`) | none |
| VLAs / large stack arrays | none |

### Findings

| # | Severity | file:line | Issue | Suggested fix |
|---|----------|-----------|-------|---------------|
| L1 | Low (security: mTLS authz) | `integrations/tls/mbedtls/tls_mbedtls.c:338-373` (`x509_extract_cn`/`x509_extract_san`) | The mbedTLS adapter copies raw ASN.1 CommonName / DNS-SAN bytes verbatim with no canonicalization. An embedded NUL in a client-cert CN (`"admin\0evil"`) yields `subject_cn = "admin\0evil"`, so an app doing `strcmp(subject_cn, "admin")` for authorization matches. The OpenSSL sibling explicitly guards this (`identity_bytes_safe` rejects `<0x20`/`0x7f`; `dns_san_bytes_safe` allows only LDH+`.`/`_`/`*`, rejecting NUL/control and the `,` that would make the joined SAN list ambiguous). Bounds are correct: this is an identity-spoofing asymmetry between the two TLS backends, not memory unsafety. | Mirror the OpenSSL policy in the mbedTLS extractor: reject/skip a CN or DNS-SAN whose bytes contain NUL, a C0 control, DEL, or (SAN) a comma. |
| L2 | Low (liveness) | `src/event_iouring.c:476` (also 502, 513; callers 545/578/617) | On submission-queue exhaustion at the *initial* post, `iou_prep_send_tail` sets `op->aborted=1` and returns, but no SQE was queued and the post function still returns 0 (success). The drain (`io_uring_for_each_cqe`) only processes CQEs and `->aborted` is consumed only inside `iou_fill_event` when a CQE is dequeued, so no completion is ever delivered; the idle-timeout sweep skips already-aborted ops, so the WRITE/SENDFILE is stranded until the connection idle-closes. Not a UAF (kernel never owned the buffer). Requires SQ-depth (1024) exhaustion, so hard to trigger. The "surface as error next drain" comment is aspirational; there is no aborted-op sweep. | Treat an initial `!sqe` as a hard post failure: unlink + `iou_op_free` the op and return -1 (as the other `!sqe` post paths do), so the driver closes the connection instead of waiting on a completion that never arrives. |
| L3 | Low (integrity) | `integrations/codec/miniz/decompress_miniz.c:338-367` | The streaming gzip CRC32 + ISIZE verification runs only in the `flush && trail_len>=8` branch. A stream whose final block plus full 8-byte trailer arrives without a subsequent `flush`, or that ends with `<8` trailer bytes, is accepted with no integrity check; corrupted payload passes as valid. Contained by the 256 MB anti-bomb cap; ISIZE compares 32-bit `total_out` (wraps >4 GB). Not memory-unsafe. | Verify the trailer whenever `done` and 8 bytes are accumulated (independent of `flush`); treat a DONE stream with `<8` trailer bytes at flush as an error. |
| L4 | Low (not attacker-reachable) | `src/protocols/http2/http2_client.c:175-207` (`h2c_on_response`) | If invoked twice for one stream while `n <= headers_cap`, the realloc block is skipped and the previous `name`/`value` heap strings are overwritten in place without `kl_free` (leak). Only reachable if a peer delivers two `HCAT_RESPONSE` HEADERS frames on one stream, which nghttp2 rejects as a protocol error and RSTs before a second delivery. | Hardening: free existing `st->resp.headers[*]` and reset `num_headers` at the top of the copy block before re-filling. |
| I1 | Info | `integrations/platform/uefi/mbedtls_platform_uefi.c:234-266` | The freestanding `snprintf` skips `l`/`z`/`h` length modifiers but always `va_arg`s a 32-bit `int`/`unsigned` for `%x`/`%u`/`%d`. A `%zx`/`%lx` with a 64-bit arg would read the wrong vararg slot and desync the rest. Latent only: current mbedTLS call sites (ERROR_C/DEBUG_C off) use 32-bit conversions. | Honor the length modifier if a 64-bit/`z` conversion is ever introduced; otherwise acceptable under the documented "not a general printf" contract. |
| I2 | Info | `integrations/http2/nghttp2/http2_nghttp2_server.c:147` (`ng_on_header_cb`) | On `ng_sstream_grow` realloc failure mid-headers the callback returns 0, so the request is later delivered missing that header (graceful degradation). An app that authorizes/routes on the full header set could be bypassed by a truncated set. Not memory-unsafe. | Return `NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE` to RST the stream on allocation failure. |

**Update (same session, 2026-08-26): L1 FIXED.** `integrations/tls/mbedtls/tls_mbedtls.c` now mirrors
the OpenSSL backend's identity canonicalization: added `identity_bytes_safe` (rejects `<0x20`/`0x7f`)
and `dns_san_bytes_safe` (LDH+`.`/`_`/`*` only, rejecting NUL/control/comma); `x509_extract_cn` fails
closed on an invalid-byte CN, a would-truncate CN, or more than one CN attribute; the DNS-SAN branch
skips an invalid-byte or over-long entry instead of truncating. The adapter compiles clean under
`-Werror` and the mbedTLS `e2e` peer-cert suite passes (valid-cert extraction unregressed). The other
Low/Info items (L2/L3/L4, I1/I2) remain as recorded.

### Coverage note

Six independent subsystem sweeps read the real code (not pattern matching) across substrate core
(allocator/kl_cstr/error/drain/timer/thread_pool/url/sockaddr/file_io/resolve_sync/stream/listener/
connect_op/completion_core/completion_dispatch/event_ctx/event_dispatch), the datagram data plane
(`datagram*.c`, `udp_cmsg.c`, `socket_dgram_posix.c`), all event/completion + socket backends
(epoll/kqueue/poll/WSAPoll/io_uring/IOCP/pollcomp + posix/winsock providers), the HTTP server core +
untrusted-input body/parse path (connection/server/response/router/body readers/chunked/multipart/
cors/sse/compress/async), the client + secondary protocols (http_client_*/http2/websocket/dns/
proxy_protocol/decompress/resolver_cache), and the `integrations/` adapter glue (lwIP raw + EFI
TCP4/UDP4 token/quarantine lifetime, TLS mbedtls/openssl, nghttp2, miniz, UEFI freestanding shims).
Verified clean at scrutiny: the substrate size/index overflow guards and the synchronous-inline-
completion re-entrancy discipline (set-flag-before-hook + depth counter + deferred finalize) in
stream/listener/connect_op; the datagram life-token refcount (created at 1, retain-per-post,
release-exactly-once per completion, QUARANTINE borrowed-ref intentional leak) and fixed-slot/ring/
batch overflow guards; the completion-backend op/buffer ownership (io_uring queue_exit-before-free,
IOCP deliberate leak-on-port-error to avoid UAF, token transfer-and-NULL discipline); CL/TE
resolution (llhttp zeroes Content-Length when chunked), the chunked 16-hex-digit overflow guard and
multipart underflow-safe loop guards, response-header CRLF-injection rejection, and the Happy-Eyeballs
terminal-once teardown; the DNS/WebSocket/PROXY length-field bounds and decompression output cap; and
the two highest-risk integration lifetime domains (lwIP pcb/pbuf/token, EFI generation+quarantine).

The two security-critical modules from the merged increment were covered by the concurrent #250/#251
review this session rather than re-audited redundantly: `unix_socket_node_posix.c` (component-walked
`O_NOFOLLOW`/`O_DIRECTORY` trust validation on every path component, dev/ino identity captured at bind
and re-verified at teardown, `out_fd`-required NULL guard) is `scan-build`-clean; `unix_socket_node_win.c`
(per-component no-follow walk rejecting reparse points, ACL default-deny that skips inherit-only ACEs,
held/pinned parent, `FILE_ID_INFO`-verified delete-by-handle via `FileDispositionInfoEx` only) is
`cppcheck`-clean and contains no `DeleteFileA`/pathname-deletion path. Both fail closed on an untrusted
component or an identity mismatch.

## Thirteenth pass: datagram Phase B (public `KlDatagram` + `KlUdp`/dns) whole-tree re-audit (2026-08-17)

**Scope:** whole `src/` (69 `.c`) + `parsers/` + headers + `Makefile`, focused on the code added
since the twelfth pass; the datagram consolidation arc (`src/udp.c`, `src/socket_dgram_posix.c`,
`src/datagram.c`, `src/datagram_core.*`, `src/completion_core.c`, `src/dns_resolver.c`,
`src/udp_server.c`, `integrations/uefi/event_efi.c`) plus the IOCP association-at-create fix.

**Verdict: CLEAN.** 0 Critical / 0 High / 0 Medium / 1 Low (informational). The build is
`-Werror`-clean (**0 warnings** under `-Wall -Wextra -Wpedantic -Wshadow -Wformat=2`), `cppcheck`
exits 0, and the completion axis is ASan/UBSan/LSan-clean natively (`smoke-pollcomp-asan`) and in
the container (`smoke-iouring-asan`).

### Mechanical scans (whole tree)

| Category | Result |
|---|---|
| Unsafe str/format (`strcpy`/`strcat`/`sprintf`/`gets`/`vsprintf`) | **none** (grep hits are the word "gets" in comments) |
| Unsafe int parse (`atoi`/`atol`/`atof`/`atoll`) | **none** |
| Direct `malloc`/`free`/`calloc`/`realloc` | **none**: all via `kl_malloc`/`kl_free` (with size) |
| Unchecked `kl_malloc` (spot-checked incl. dgram batch, websocket, decompress) | all NULL-checked; multi-alloc uses batch-then-check + free-on-error |
| Overflow guards (`SIZE_MAX`/`INT_MAX` before size arithmetic) | present in 39 `.c` files |
| Dead code (`#if 0` / `if (0)`) | **none** |
| VLAs / large stack arrays | **none** |
| SIGPIPE | ignored at server init; `SO_NOSIGPIPE` + `MSG_NOSIGNAL` (POSIX); no-op on Windows |
| `-Werror` build warnings | **0** |

### Automated safety net (cross-referenced, not re-derived)

83 test suites · 8 fuzz targets (parser/multipart/websocket/response/dns/proxy/url + decompress)
· `make analyze` (scan-build) · `make cppcheck` (**rc=0**) · `make debug` ASan+UBSan · the
completion-axis smokes above.

### Low (informational)

| # | File:line | Note |
|---|-----------|------|
| L1 | `src/socket_dgram_posix.c:500-504` | `pdg_rx_batch_new` computes `(size_t)n * sizeof(...)` for the `recvmmsg` batch without an explicit overflow guard on `n`. Not exploitable: `n` is the app-set `mmsg_batch` config knob (not network input) and is 64-bit-safe; on 32-bit a pathological value could overflow. Defense-in-depth: cap `mmsg_batch` or guard the multiply. |

**Conclusion:** no actionable memory-safety, resource, or hardening issues in the datagram arc or
the wider tree; the sole Low item is a config-controlled, 64-bit-safe robustness nit. No fixes
applied (audit-only pass).

---

## Twelfth pass: Phase 10 UEFI server (S-1..S-7) + client/server orchestration refactors (2026-08-08)

**Scope:** the code added/changed since the eleventh pass; the freestanding UEFI HTTP(S)
**server** (S-1..S-7) and the two review-driven orchestration refactors: `src/server_core.c`
(freestanding `kl_server_init`/`kl_server_free` carves), `src/client_proxy.c` (shared proxy
CONNECT), `src/server_activation.c` (systemd socket-activation split), the readiness→hook
dispatch unification (`src/server.c`, `src/server_ws.c`, `src/server_h2.c`, `src/proto_hooks.h`),
and the EFI provider data-plane (`integrations/uefi/event_efi.c`, `socket_efi_tcp4.c`).

**Verdict: CLEAN.** No new Critical/High/Medium findings. This pass is corroborated by the
strongest available checks rather than inspection alone:

| Check | Result |
|-------|--------|
| Unsafe fns (`strcpy`/`strcat`/`sprintf`/`gets`/`atoi`/`atof`) in `src/` + `parsers/` | none |
| Direct `malloc`/`free`/`realloc` in `src/` (allocator discipline) | none; all via `kl_*` |
| `kl_malloc` NULL-check discipline (new code) | the one new call site (server_core.c proxy_cidrs) is checked; client_proxy.c / server_activation.c are alloc-free |
| Full test suite under **ASan + UBSan** (`make debug-test`) | **65/65 suites, 0 leaks, 0 UB, 0 errors** |
| mock-EFI failure-path harness (ASan/UBSan) | PASS incl. the new `t_accept_backpressure` |
| Build hardening (prod CFLAGS) | `-Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Werror -O2 -fstack-protector-strong` |
| Dead code (`-Wunused` via `-Werror`) | clean build |
| Cross-compile: gcc-14 (all touched TUs) + MinGW (server_activation/server_plat_win/server.c) | clean |
| Freestanding archives (client + server) symbol-closure gate, x86_64 + aarch64 | gated OK |

**Bounds/injection spot-checks (session's new attack surface):**
- EFI server send copies into a fixed inline `sndbuf` with a `total > KL_EFI_SNDBUF` guard
  **and** a per-iovec `off + iov[i].len > KL_EFI_SNDBUF` guard (fail-closed → conn closed).
- EFI server recv is bounded by `read_cap - read_len`; the TLS-recv scratch is a fixed
  `KL_EFI_TLS_CIPHER` buffer consumed by `feed_input` each drain.
- Response header CRLF-injection guard (`contains_crlf`) unchanged in `response.c`.
- Accept backpressure bounds armed EFI tokens to free Keel pool slots (no accept-into-drop).

**Informational (not a finding):** `integrations/uefi/mbedtls_platform_uefi.c` *defines*
`strcpy` (and other libc primitives), it is the freestanding libc backing the vendored
mbedTLS build needs on bare firmware, not an unsafe *call* on untrusted input in KEEL code.
A grep for unsafe-function *names* flags the definition; it is benign by construction.

**Prior findings:** all eleventh-pass EFI lifetime bugs remain fixed (verified live on the
branch, commit `71573dd`; see the axis-audit tenth pass and the S-7 review round).

---

## Eleventh pass: Phase 10 UEFI provider failure-path hardening (2026-08-06)

**Scope:** the EFI network provider under `integrations/uefi/` + `spikes/uefi/` (F-8): the
EFI_TCP4 socket provider (`socket_efi_tcp4.c`), the built-in DNS resolver over EFI_UDP4
(`dns_uefi.c`), the completion backend (`event_efi.c`), the mbedTLS platform backing
(`mbedtls_platform_uefi.c`), the lifecycle teardown (`lifecycle_uefi.c`), and the U-4/U-7
self-tests.

**This is NOT a "clean" pass.** Two rounds of adversarial *lifetime* review found **real,
release-blocking EFI completion-token bugs** that QEMU happy-path testing (and the happy-path-focused
earlier passes) never exercised. Every finding below was FOUND, FIXED, and (the load-bearing part)
**covered by a host mock-EFI harness** (`mock_efi_test.c`, 18 scenarios) that compiles the *real*
provider TUs against a scriptable fake `EFI_BOOT_SERVICES`/`EFI_TCP4`/`EFI_UDP4` under ASan+UBSan.
The lesson recorded: **a QEMU happy-path GO ≠ correct**; completion-token providers demand
adversarial timeout/close/cancel/post-EBS host tests before any "production" claim.

**Method.** Host mock-EFI harness (18 scenarios, ASan+UBSan+LSan (`detect_leaks=1` on Linux; `=0` on Darwin)), the deliberate
quarantine "leak" is clean because backing storage is the static `g_conns`/`g_dns_*` pools
(reachable, not leaked). All touched freestanding TUs cross-compiled `--target=x86_64-unknown-windows
-ffreestanding -Wall -Wextra -Werror`. Core `make test` green. QEMU U-7 (plaintext send/recv/close
over real EFI_TCP4) re-run as a happy-path regression guard.

### Findings (all fixed + test-covered)

| # | Severity | File:symbol | Bug | Fix | Test |
|---|----------|-------------|-----|-----|------|
| U1 | **Critical** | `socket_efi_tcp4.c` `efi_sock_send`/`efi_sock_close`; `dns_uefi.c` `kl_uefi_dns_resolve` | A **failed** `Cancel`+drain (token never reaches a terminal state) was still followed by ordinary cleanup (`CloseEvent`/`DestroyChild`/`kl_free`) releasing storage the firmware may still write into (UAF / firmware-write-after-free). | **Quarantine model.** On a drain that does not confirm the token retired, set `c->quarantined` (DNS: `g_dns_quarantined`): never `CloseEvent`/`DestroyChild`/reclaim the slot; its **stable, provider-owned** storage (`static KlUefiConn g_conns[]`; DNS `g_dns_op`) is deliberately leaked until ExitBootServices. DNS fails closed on every subsequent `resolve`. | `t_cancel_fails_quarantine`, `t_dns_cancel_fails_quarantine` (child NOT destroyed; slot not reused; resolve fails-closed) |
| U1b | **Critical** | `dns_uefi.c` `kl_uefi_dns_resolve` (2nd review) | The DNS quarantine preserved the query buffer + descriptor but the **completion tokens themselves** (`tx_tok`/`rx_tok`) were still **stack-local**: on `goto quarantine` the frame died while firmware held their addresses (and would later write `Status`/`Packet.RxData` into freed stack). | Encapsulate the entire one-shot DNS op (tokens + descriptor + query buffer) in **one stable file-scope struct** `g_dns_op`. Invariant: nothing reachable from a submitted token lives on the stack. | `t_dns_cancel_fails_quarantine` extended to **model a delayed firmware write into the quarantined token after `dns_resolve()` returns**; negative control (tokens forced back to stack) reproduces the ASan **stack-use-after-return**, confirming the test bites |
| U2 | **High** | `socket_efi_tcp4.c` `efi_sock_close` | Close used `configured && !connected` as a proxy for "connect token outstanding"; because `CheckEvent` **consumes** the signal, close could wait ~60 s on an already-consumed event. | **Explicit token-state**: `conn_posted`/`tx_posted`/`close_posted` (+ existing `rx_posted`), set only after a successful submit, cleared only after the terminal event is observed. Close drains **only** flagged tokens. | `t_close_no_spin_on_consumed_connect` (`g_tcp_poll_calls` bounded; the mock models `CheckEvent` de-signalling) |
| U3 | **High** | `socket_efi_tcp4.c` `efi_sock_close` | Close ignored failed receive/connect cancellation drains (same root as U1 for the non-tx tokens). | Close tracks `drained_ok` across every posted token; `!drained_ok` → quarantine + early return (no teardown). | covered by U1 close paths + `receive pending during close` |
| U4 | **High** | `socket_efi_tcp4.c` `kl_uefi_socket_recv_ready` | Received `rx_data.DataLength` (and `FragmentTable[0].FragmentLength`) trusted without validating `<= KL_EFI_RXBUF` → OOB read on a malicious/buggy stack. | Validate both against `KL_EFI_RXBUF`; on violation latch `rx_err = EFI_DEVICE_ERROR` and surface a fatal `recv` -1. | `t_bad_rx_length` (recv -1, no OOB under ASan) |
| U5 | **Medium** | `socket_efi_tcp4.c` `kl_uefi_socket_configure`/`_connect_post`; `mbedtls_platform_uefi.c` heap | Post-EBS coverage incomplete: two data-path entries unguarded; mbedTLS heap retained the Boot Services pointer with no shutdown hook (TLS teardown after EBS could `FreePool` torn-down firmware). | `kl_uefi_after_ebs()` guard on the two entries; `uefi_mbed_calloc`/`_free` fail closed post-EBS; new `kl_uefi_mbedtls_platform_shutdown()` drops `g_bs`; U-4 calls it after destroying every TLS object, before EBS. | `calls after simulated EBS` (zero firmware calls across all entries) |
| U6 | **Medium** | `socket_efi_tcp4.c` `kl_uefi_socket_provider_reset` | Reset assumed all conns closed and blanket-zeroed the pool; corrupting quarantined/live firmware-owned slots. | Reset no longer zeroes the pool (preserves quarantined + live slots; only already-free slots are reused). New `kl_uefi_socket_provider_live_count()` exposes the "all-closed-before-shutdown" contract; U-7 asserts it observably. | `stale-guard no-UAF` + U-7 live-count line |
| U7 | **Medium** (load-bearing) | `mock_efi_test.c` (new) | The adversarial host harness itself was missing; the earlier passes could not have caught U1–U6. | Added the 18-scenario mock-EFI harness (cancel succeeds/fails/races, consumed connect event during close, receive outstanding during close, impossible receive length, post-EBS, slot reuse with stale generation, delayed firmware write into a quarantined DNS token). | itself |
| U9 | **Medium** | `build_mock_efi_test.sh` / `mbedtls_platform_uefi.c` (2nd review) | The advertised 13th (entropy fail-closed) test was gated behind `MOCK_WITH_MBEDTLS`, which the build never set; the harness silently ran 12, and the entropy path was "verified by inspection" only. Also: script not executable (0644); `detect_leaks=1` aborts under Apple ASan on Darwin. | Split `mbedtls_hardware_poll` into a mbedTLS-free `entropy_uefi.c` so the harness links + runs the fail-closed test **unconditionally** (genuinely 13). `chmod +x`; select `detect_leaks=0` on Darwin (Linux container keeps the real leak check). | `t_entropy_fail_closed` (now always runs: `r!=0, olen=0` without the insecure macro) |
| U8 | Low (tooling) | `build_u{3,4,5,7}.sh` | `declare -A` fails silently on macOS Bash 3. | Explicit `BASH_VERSINFO < 4` guard with a clear error. | n/a |
| U10 | **Medium** | `mbedtls_config_uefi.h`; new `civil_time`, `wallclock_uefi`, `clock_snapshot`, `time_uefi`; `platform_uefi.c`; `spikes/uefi/efi_min.h` | Certificate **validity-time was not enforced** (`MBEDTLS_HAVE_TIME` off), TLS accepted expired / not-yet-valid certs (CA + hostname only). | Enable `HAVE_TIME`/`HAVE_TIME_DATE`; bind mbedTLS's clock to Runtime Services **GetTime** → pure `civil_time` math. Trust boundary (3rd–5th review): **one TLS-platform-lifetime SNAPSHOT** (`clock_snapshot.c`) captured once **inside `kl_uefi_mbedtls_platform_init()`** (mbedTLS's time callback is global, no session context, so per-session would let concurrent sessions clobber each other's basis), shared by all sessions + advanced by the monotonic clock, cleared at platform shutdown; `mbedtls_time` never calls GetTime mid-handshake (closes the "GetTime fails after setup → epoch 0 → 1970-cert" loophole); **structural fail-closed gate in the mandatory initializer** (no app choreography), untrustworthy clock ⇒ init fails ⇒ no `KlTlsCtx`; **`EFI_TIME` field validation** rejects malformed firmware pre-conversion; **unspecified-TZ rejected by default** (UEFI §8.3 local-time; app offset or `KL_UEFI_ASSUME_UNSPECIFIED_UTC` test flag to opt in), sign `UTC = local + TimeZone`. | `t_civil_time` + `t_wallclock_decode` (unspecified-TZ policy, malformed fields) + `t_clock_snapshot` (snapshot read-not-GetTime; **post-snapshot GetTime failure does not reopen epoch-0**; failed refresh → epoch 0). + t_clock_snapshot platform-not-ready case (frozen monotonic timer → snapshot refused → TLS init fails). **QEMU U-4 prod: valid+good-clock → 200; valid+bad-clock (RTC 2000) → TLS refused, no 200; expired (2020) → `status -1`/`KL_ERR_TLS_HANDSHAKE`/no 200.** |

**No open findings from this pass.** Certificate validation is now complete (CA chain + hostname +
validity-time); the previously-documented cert-time gap (U10) is closed. Remaining items are
operational (real-firmware RTC quality), not missing checks.

> **Retirement note (2026-08):** `dns_uefi.c` (the U1/U1b findings above, and the
> `t_dns_cancel_fails_quarantine` test) has since been **retired**: the stock async
> `src/dns_resolver.c` now runs over the EFI_UDP4 datagram provider (`socket_efi_udp4.c`, 6.4b/6.4c).
> The cancel / quarantine / stable-token disciplines the U1/U1b findings hardened now live in
> `socket_efi_udp4.c`, whose 6.4b mock cases cover them; `t_dns_cancel_fails_quarantine` was removed
> with the retired engine. The findings above are preserved as a point-in-time record.

**Status:** the happy path is proven end-to-end in QEMU; the failure paths are hardened and
host-test-covered under ASan+UBSan; cert-time enforcement is proven with a valid/expired QEMU pair.
The two originally-Critical token-lifetime findings (and the 2nd-review DNS-token + entropy-test
gaps) are closed.

## Tenth pass: freestanding portability phase (2026-08-05)

**Scope:** the freestanding phase merged since the ninth pass (PRs #199–#210): the allocator split
(A1), `completion.h`→`KlSockAddr` (A2), freestanding public headers + `kl_ssize_t`/off_t (A3/F0),
`errno`→`KlIoStatus` (B1) + sync-proxy neutrality, the `completion_driver.c`→core/server/h2/ws and
`client.c`→common/sync/async **TU splits** (B2a/B2b) behind `KlEventCtx` hooks + `src/event_ctx.c`,
F-4 formatted-I/O elimination (`src/kl_cstr.c`) + the optional reference mem*/strlen
(`src/kl_cstr_builtin.c`), and the multi-arch + PE-link gates (B1+B2). The lwip-raw accept-window
UAF (M1, #208) was the ninth pass's M1; now **fixed**.

**Method:** the TU splits are pure code movement (nm-proven client_async.o/completion_core.o have
no cross-stack deps; full suite + harness green), not re-audited. A focused reviewer took the
genuinely-NEW hand-written logic; `kl_cstr.c` (bounded parsers/formatters on **untrusted** URL/
response input), `kl_cstr_builtin.c` (reference mem*), `event_ctx.c` (the split watcher API), plus
mechanical sweeps and the automated gate in the Apple container.

**Automated tools + mechanical: CLEAN.** cppcheck **0 err/warn**; scan-build **"No bugs found"**;
**64 suites** under ASan+UBSan (incl the new `test_kl_cstr` 6/6 + `test_kl_cstr_builtin` 5/5); the
url parser (which now routes through `kl_strchr`/`kl_strstr`/`kl_parse_u16_decimal`) is fuzzed; no
unsafe libc funcs, no raw malloc/free, no VLAs in the new TUs; hardening unchanged.

### Findings

| # | File:Line | Sev | Issue | Fix |
|---|-----------|-----|-------|-----|
| L1 | `include/keel/client.h:42` | Low (cosmetic) | `KL_CLIENT_CHUNK_HDR_SIZE 16` commented "Fits `FFFFFFFFFFFFFFFF\r\n`"; that line is 18B+NUL, doesn't fit 16. **No overflow**: `chunk_buf`=4096 → real chunk sizes ≤3 hex digits, and `kl_buf_append_hex` is bounded (returns -1→error) even adversarially. **Fixed**: bumped to 24 + accurate comment. |

### Verdict (verified clean)
- **`kl_cstr.c`**: the append builders never compute `off+len` directly (guard order `o>cap` →
  `len>cap-o`), so no additive overflow and no NUL off-by-one; `kl_u64_to_dec/hex` bound the
  `tmp[20]`/`tmp[16]` reversal + reject `n>cap`; `kl_parse_u16_decimal` rejects at the exact u16
  boundary before any uint32 overflow; `kl_strstr`/`kl_strchr`/`kl_streq`/`kl_str_startswith` match
  libc (empty needle, NUL, short strings). Every caller (sockaddr/url/client_common/client_async)
  passes `sizeof(buf)` as cap and aborts on append failure; an adversarial oversized host/path/
  header yields a clean failure, not an overflow. `kl_u64_to_*` don't NUL-terminate; the one raw
  caller (`sockaddr.c` v6 formatter) memcpys exactly `r` + terminates itself; all others go through
  `kl_buf_append_*` which write the NUL. **Clean.**
- **`kl_cstr_builtin.c`**: memmove both directions + `d==s`/`n==0` early-out; unsigned memcmp
  ordering; the self-referential-lowering foot-gun is disarmed (`-fno-builtin` base +
  `-fno-tree-loop-distribute-patterns` on the self-contained target, probe-gated); out of CORE_SRC
  so no duplicate-def on hosted/EDK2. **Clean.**
- **`event_ctx.c`**: the split preserved the watcher-list invariants + the **H1 liveness guard**
  (which lives in the `event_ctx.h` inline, so both `kl_event_ctx_run` and the completion driver
  see it); add-failure unlinks+frees; `kl_event_ctx_run` heap path SIZE_MAX-guarded. **Clean.**

Overall: **Low** risk; one cosmetic comment (fixed), no Critical/High/Medium. The new
freestanding logic is correctly bounded, overflow-guarded, and NUL-termination-correct.

---

## Ninth pass: lwIP-raw client axis (LC-0..LC-5) + full re-sweep (2026-08-05)

**Scope:** the completion-native lwIP-raw **client** work merged since the eighth pass;
LC-0 completion-CONNECT contract (`KL_COMP_CONNECT` + `post_connect` across `completion.h`/
`completion_dispatch.c`/`completion_driver.c`/`event_pollcomp.c`/`event_iouring.c`/
`event_iocp.c`/`src/client.c`/`src/async.c`), LC-1/LC-2 plaintext + Happy-Eyeballs raw client,
LC-3a `KlUdp` over lwip-raw, LC-3 DNS via KEEL's `dns_resolver.c` on `KlUdp`-over-raw, LC-4
HTTPS (client socket-BIO through the raw provider + server memory-BIO completion-TLS leg, two
backend fixes), LC-5 caps/docs (PRs #191–#197). Files centered on `integrations/lwip/`
(`event_lwip_raw.c`, `lwip_raw_glue.c/.h`, `lwipopts_raw.h`) + the cross-backend LC-0 seam.

**Method:** two parallel deep-review agents (the lwip-raw backend/glue lifetime + the LC-0
cross-backend connect primitive), each tracing recv/send/connect/close-with-outstanding paths;
plus mechanical sweeps (no `strcpy`/`sprintf`/`atoi`, no raw `malloc`/`free` outside test
harnesses) and the automated gate in the Apple container: **cppcheck 0 errors/warnings,
scan-build "No bugs found," 60 suites passing under ASan+UBSan** (`make debug-test`,
`detect_leaks=1`), and the loopback-raw + raw-tls ASan runs. Build hardening confirmed
unchanged (`-Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Werror -O2 -fstack-protector-strong
-fPIE -D_FORTIFY_SOURCE=3`; SIGPIPE fully handled).

**Automated tools: CLEAN.** cppcheck (0), scan-build (No bugs), no unsafe libc calls in
`src/`+`parsers/` (only `allocator.c`'s stdlib wrapper legitimately wraps `realloc`), no VLAs,
allocator discipline intact in the new backend (all Keel-owned memory via `KlAllocator`, no
hot-path allocation; TX window pre-allocated, only lwIP's own `pbuf_alloc` for a UDP send).

### High

| # | File:Line | Issue | Fix |
|---|-----------|-------|-----|
| H1 | `include/keel/event_ctx.h:140` (`kl_event_dispatch`) + `src/completion_driver.c` (`kl_comp_run` batch) + `src/client.c` (`he_close_attempts`/`client_drop_connect_fd`) | **Same-batch Happy-Eyeballs connect use-after-free.** `kl_comp_run` drains multiple completions into one batch and dispatches them sequentially; `kl_event_dispatch` derefs the tagged `KlWatcher*` with **no liveness check**. When two racing connect attempts (dual-stack A+AAAA) both complete in one drain, dispatching the winner's `KL_COMP_CONNECT` runs `he_win → he_close_attempts → client_drop_connect_fd → kl_watcher_del`, which **synchronously frees the loser's watcher node**; dispatching the loser's still-batched `KL_COMP_CONNECT` then reads freed memory. The default `connect_attempt_delay_ms=250` shields the loopback happy path (only one attempt in flight), so tests pass; a genuinely-racing dual-stack connect (both in flight, both completing in one wait) hits it. Confirmed by direct trace on pollcomp + io_uring. **Fix:** in the `KL_COMP_CONNECT`/`KL_COMP_WATCHER` dispatch, validate the tagged watcher is still linked in `ctx->watchers` before deref (small, backend-agnostic guard); skip if it was freed earlier in the same batch. |

### Medium

| # | File:Line | Issue | Fix |
|---|-----------|-------|-----|
| M1 | `integrations/lwip/lwip_raw_glue.c:766-804` (`lwr_srv_accept`/`lwr_srv_err`) | **Dangling-pcb UAF in the accept→post_recv window.** `lwr_srv_err` keys on `c->owner == arg`, but `owner` is set only later (first `post_recv`), and `lwr_srv_accept` never `tcp_arg`s the new pcb (it inherits the listener's arg). If lwIP aborts the accepted pcb (peer RST / OOM) before the driver's `post_recv`, the err is dropped, the slot keeps `dead=0` + a dangling `->pcb`, and the pending ACCEPT still surfaces → the driver adopts a freed pcb (UAF). Narrow timing (needs a stack-initiated abort in that window; rare on loopback). **Fix:** `tcp_arg(newpcb, slot)` at accept and resolve `lwr_srv_err` by the slot pointer, so a pre-owner abort still marks its slot `dead`. |

### Low

| # | File:Line | Issue | Fix |
|---|-----------|-------|-----|
| L1 | `src/redirect.c:77` (`is_cross_origin`) | **UBSan: NULL passed to `strncasecmp` (declared nonnull).** When both URLs have `host_len==0`, the equal-length guard falls through to `strncasecmp(a->host, b->host, 0)` with NULL hosts; defined-behavior-pedantic UB (0 length, no actual read), but it trips `UBSAN_OPTIONS=halt_on_error=1`. CI stays green only because its UBSan job runs recover-mode (prints, doesn't fail). **Fix:** `if (a->host_len == 0) return 0;` (or guard `a->host && b->host`) before the `strncasecmp`. |
| L2 | `integrations/lwip/event_lwip_raw.c:672-677` | **False EOF vs explicit overflow on a full non-TLS `read_buf`.** When `space==0` with `rx_queued>0`, the drain surfaces `bytes=0` → the driver reads it as peer-close (EOF), where the readiness backend (`event_pollcomp.c:248`) deliberately signals a header-overflow failure. Converges to a close either way; cosmetic (wrong reason). **Fix:** surface a failed READ when `space==0`. |
| L3 | `src/completion_driver.c:768`, `event_pollcomp.c:358`, `event_iouring.c:695` | **Stale comments** claim the client re-reads `SO_ERROR` for `KL_COMP_CONNECT`; the actual path (`he_on_connect_result`, `client.c`) trusts the mask-carried win/fail and does **not** read `SO_ERROR` (correct for io_uring, which drops it). Comment-only. **Fix:** correct the comments to reference `he_on_connect_result` + the mask. |

### Verdict
The new code is disciplined on overflow, allocation, backpressure, and the neutral seam
(`event_lwip_raw.c` stays lwIP-free; all `tcp_*`/`udp_*` in the glue), and the automated tools
are clean. Two real use-after-frees (H1 same-batch HE connect race; M1 accept-window pcb) are
edge-timing bugs a happy-path sanitizer run misses; both warrant fixes; H1 is the priority as
it sits on the outbound connect path for any completion backend. L1 is a trivial UBSan fix. The
underlying feature (raw client + HE + DNS + HTTPS) is functionally sound and end-to-end verified.

---

## Eighth pass: datagram data-plane folded onto the socket provider (2026-08-03)

**Scope:** the four-stage "datagram provider" refactor (#168–#172) that resolves
axis-audit **A2**: the UDP datagram data-plane moves from the compile/link
`udp_io_*` seam onto an optional `KlDatagramOps` vtable on `KlSocketProvider`, so
one runtime provider owns stream **and** datagram I/O. Files: `include/keel/datagram.h`
(the new vtable + `KlDgramRxSlot`/`KlDgramTxDesc`/`KlDgramRxMeta`), `src/socket_dgram_posix.c`
+ `src/socket_dgram_win.c` (the POSIX/Winsock datagram primitives, ~1000 lines),
`src/udp_cmsg.c` + `src/udp_cmsg_win.c` (shared cmsg parsers the completion
backends reuse), `src/udp.c` (the send-queue flush + recv drain **machine loops
moved up here**, provider dispatch, `udp_group_ok` mcast validation, batch
lifecycle, dgram-required init), the `socket.h`/`socket_posix.c`/`socket_winsock.c`
provider wiring + `kl_sockdef_dgram()`, the completion backends' `.dgram`
inheritance, and `integrations/lwip/socket_lwip.c`'s datagram ops. `udp_io_posix.c`,
`udp_io_win.c`, `udp_io.h` were deleted.

**Method:** three parallel deep-review agents (POSIX dgram + udp_cmsg; Winsock dgram
+ udp_cmsg_win; udp.c machine rework + lwIP + provider wiring) tracing cmsg
build/parse bounds, mmsg-batch alloc/index/free symmetry, the data-oriented
recv_batch/send_batch slot/descriptor arrays, address marshalling on untrusted
recv addresses, callback re-entrancy in the machine loops, and the dgram-default
resolution; mechanical sweeps (no `strcpy`/`sprintf`/`atoi`, no raw `malloc`/`free`
in the new TUs); and the Stage-4 gate; **cppcheck + scan-build clean, the full unit
suite under ASan+UBSan (891 tests, 0 failures, 0 sanitizer hits)**, gcc-14 +
cosmocc + MinGW (iocp/wsapoll), and the Apple container: epoll (0 fails, UDP
batching 5/5 via real `recvmmsg`/`sendmmsg`), io_uring (`smoke-iouring-asan`
UDP-over-completion), and the lwIP loopback + HTTPS.

**Verdict: clean.** No Critical or High findings. The memory-safety surface of the
refactor (cmsg bounds, the mmsg batch blocks, the data-oriented slot/descriptor
arrays, address marshalling, and machine-loop callback re-entrancy) is sound. One
**Low** correctness item was **fixed this pass**; the rest are Low/Informational,
pre-existing or unreachable via the public API.

### Low: fixed this pass

| # | File(s) | Issue | Fix |
|---|---------|-------|-----|
| L1 | `src/socket_dgram_posix.c` / `src/socket_dgram_win.c` (`pdg_send`/`wdg_send`) | On a source-pinned/TOS send with **no destination** (a connected socket), the TOS control-message's IP level was guessed `AF_INET`, so a TOS mark on a connected **IPv6** socket would build an `IP_TOS` (v4) cmsg → kernel ignores/rejects it. Unreachable via the public API today (`kl_udp_send_to_tos`/`_from` always pass a dest; `kl_udp_send` uses tos −1), so defense-in-depth. | Derive the family from the dest, else the **source-pin** address, else v4. |

### Reported, not changed (accepted / by design)

| # | Area | Note |
|---|------|------|
| R1 | `src/udp.c` completion-loop src-pin/TOS send | A source-pinned or per-packet-TOS send on a **completion** loop skips the overlapped `kl_comp_post_udp_send` branch (which is plain-send only) and takes the synchronous provider `send()`; on `EAGAIN` it `udp_enqueue`s, which arms a **readiness** WRITE watcher that a completion loop never drives → the datagram stalls. **Pre-existing** (the pre-refactor path did the same via `kl_udp_io_raw_send` + `udp_enqueue`); IOCP-only, and only under transient send-buffer pressure on a source-pinned/marked datagram. Fix would be an overlapped `WSASendMsg` path for src/TOS; deferred as its own change. |
| R2 | `src/udp.c` batch machine loops | `udp_recv_dgram`/`udp_flush_dgram` put a `KlDgramRxSlot[64]`/`KlDgramTxDesc[64]` (~17 KB) on the stack **inside the batch branch only** (mmsg batching is Linux + opt-in `mmsg_batch>1`); the per-datagram path (lwIP/embedded) never allocates them. Fine on host stacks; noted for tiny-stack targets. Optionally cap `UDP_MMSG_MAX` or heap the arrays. |
| R3 | `src/socket_dgram_posix.c` `configure` (macOS IPv4) | Sets `IP_PKTINFO`/`IP_RECVPKTINFO` and reports `KL_DGRAM_RX_PKTINFO`, but macOS delivers the IPv4 local address via `IP_RECVDSTADDR` (not `IP_PKTINFO`), so `meta.has_local` stays 0 there; graceful degradation, but the cap bit overstates. Pre-existing (matches the old `setup_recv_opts`). |
| R4 | Winsock length casts (`wdg_send`/`wdg_recv`) | `size_t len`/`buflen` cast to `ULONG`/`int` without a guard; unreachable for UDP (≤65507 B). |
| R5 | `cfg->tos == 0` in `configure` | `if (cfg->tos)` treats 0 as "OS default / unset" (documented in `udp.h`), so 0 can't be an explicit clear; by design. |

### Areas audited clean (no findings)

- **cmsg build/parse** (posix + win): every parse `memcpy` is length-gated
  (`CMSG_LEN`/`WSA_CMSG_LEN`), the Winsock parsers stop on a runt cmsg
  (`cmsg_len < sizeof(WSACMSGHDR)`), and `dgram_build_control` writes at most the
  `DGRAM_TX_CMSG_SPACE` the buffers are sized to.
- **mmsg batch** (`socket_dgram_posix.c`): `rx/tx_batch_new`/`free` use matching
  `(size_t)n * sizeof/bufsz/ctrl_sz` for every member, sizes set before the
  sub-allocs so a mid-alloc NULL frees cleanly (no partial leak); `recv_batch`/
  `send_batch` clamp to `min(b->n, max/n)`; all per-slot indexing is `< n <= b->n`.
- **data-oriented slots**: `KlDgramRxSlot.data` points into the batch payload and is
  delivered synchronously before the next `recv_batch`; lifetime honored by
  `udp_recv_dgram`.
- **machine loops** (`udp.c`): batch fill double-bounded (`cnt < mmsg_batch && cnt <
  UDP_MMSG_MAX`); `udp_drop_front(sent)` drops exactly what was sent; EAGAIN vs
  hard-error-drop correct; recv loops re-check `recv_active`/`kl_handle_valid` after
  every deliver (no callback-reentrancy UAF); no uninitialised `KlSockAddr` reaches
  `on_recv` (the `family != UNSPEC ? &src : NULL` guard).
- **dgram resolution + lifetime**: `udp_dg()` maps NULL sockets → `kl_sockdef_dgram()`;
  `kl_udp_init` rejects a provider without `.dgram` before opening the fd (no leak);
  the completion providers inherit the underlying `.dgram` for config/opts only (the
  data-plane stays on `kl_comp_post_udp_*`, gated by `KL_EVENT_CAP_COMPLETION`).
- **`udp_group_ok`**: `kl_sockaddr_parse` + first-octet multicast range check
  (IPv4 224/4, IPv6 ff00::/8), family cross-checked; a bad group string rejects
  cleanly (`KL_ERR_INVALID_ARG`).
- **lwIP datagram ops**: send/recv/gso/configure/set_tos/mcast bounds + family
  guards correct; batch NULL → per-datagram; no leak.
- scan-build: clean. cppcheck: clean. ASan+UBSan unit suite: 891 tests, 0 failures.

## Seventh pass: KlSockAddr address-ABI neutralization + lwIP platform (2026-08-02)

**Scope:** everything added/changed since the sixth pass; the runtime event-provider seam
(#150/#151), the **KlSockAddr address-ABI neutralization** series (#153–#159: canonical type +
pure helpers, socket-vtable currency, resolver, udp public API, accept + proxy_protocol + peer
addr, protocol-TU purge + grep-gate, lwIP payoff), and the **lwIP platform** (#152, #160–#165:
socket + event providers, client axis via `resolve_sync_lwip`, responsive stop via
`platform_wakeup_lwip`, `udp_io_lwip` + the `udp_io` seam→KlSockAddr flip, TLS-over-lwIP via the
provider-routed mbedTLS BIO, production `lwipopts.h`). Files: `sockaddr.{h,c}`,
`sockaddr_native.h`, `socket.h` + `socket_{posix,winsock}.c`, `udp.c` +
`udp_io_{posix,win}.c` + `udp_internal.h`, `resolver_cache.c`, `dns_resolver.c`,
`dns_sys_{posix,win}.c`, `completion_driver.c` + `event_{pollcomp,iouring,iocp}.c` (UDP
marshalling), `integrations/lwip/*`, and the `integrations/mbedtls` socket-provider routing.

**Method:** five parallel deep-review agents (sockaddr core; udp seam + I/O; lwIP integration;
completion-backend UDP marshalling; resolver + socket providers) tracing marshalling bounds,
op/buffer/node lifetime, integer math, and untrusted-input parsing; mechanical sweeps
(`src/`+`parsers/`: no `strcpy`/`sprintf`/`gets`, no `atoi`/`atol`/`atof`, raw `malloc`/`free`
only in the allocator wrapper); **scan-build: "No bugs found"**; **cppcheck: clean**; the
**full unit suite under ASan+UBSan (`make debug` + `make test`), 891 tests, 0 failures, 0
sanitizer hits**; gcc-14 + cosmocc + MinGW (IOCP/WSAPoll) compile gates; and the Apple container
for Linux epoll/io_uring + the lwIP loopback/HTTPS runtime tests.

**Verdict: clean after fixes.** No Critical or High findings. Four **Medium** memory-safety
defects on untrusted-input (network) paths were found **and fixed this pass**, plus one Low
hardening fix and the cppcheck gate made version-robust. The KlSockAddr marshalling boundary is
the right place to have caught these; the neutralization concentrated all host↔neutral address
conversion into one reviewed seam.

### Medium: fixed this pass

| # | File(s) | Issue | Concrete risk | Fix |
|---|---------|-------|---------------|-----|
| M1 | `src/sockaddr_native.h` `kl_sockaddr_from_native` | AF_INET/AF_INET6 cases cast an **untrusted** `struct sockaddr` (from `accept`/`recvfrom`) to `sockaddr_in`/`sockaddr_in6` and `memcpy`'d the address **without a lower-bound `len` check** (only AF_UNIX checked `len`). | A short/garbage `socklen` from the kernel or a foreign provider → up to 16-byte out-of-bounds read past the caller's address buffer. | Added `if (len < sizeof(struct sockaddr_in{,6})) return -1;` before each cast/`memcpy`. |
| M2 | `src/udp_io_posix.c` (batched + single recv), `src/udp_io_win.c`, `src/completion_driver.c` (`KL_COMP_UDP_RECV`) | The four datagram-recv paths passed `&ksrc` to the `on_recv` callback **unconditionally**. When `kl_sockaddr_from_native` fails (unrecognised family, or `peer_len == 0` on a connected socket) it leaves the scratch **untouched** → an **uninitialised `KlSockAddr` (stack garbage) delivered as the datagram source** to application code. | Info-leak of stack contents into the app's source-address logic / reply targeting, reachable from network input. | Honor the return value: pass `NULL` (unknown source) instead of uninitialised stack; guard the local (pktinfo) address the same way. |
| M3 | `integrations/lwip/event_lwip.c` `lwev_mod` / `lwev_del` | Negative-fd guard used `!kl_handle_valid(fd)`, which only rejects `-1`; an `fd <= -2` passed and indexed `fd_to_idx[(int)fd]`; a **negative-index OOB** read (and an OOB write in `del`). `src/event_poll.c` guards the full negative range. | Memory corruption from a bogus/underflowed lwIP descriptor reaching mod/del. | `(int)fd < 0 || (int)fd >= cap`, matching `event_poll.c`. |

(M2 is one defect across four sites. Its readiness-posix instance predates this series but shares
the marshalling pattern introduced here and was fixed for completeness.)

### Low / tooling: fixed this pass

| # | File(s) | Issue | Fix |
|---|---------|-------|-----|
| L1 | `src/resolver_cache.c` `cache_insert` | Fixed `host[]` buffer filled via `memcpy(strlen+1)` with no self-check (callers bound it, but the function wasn't self-defending). | `strlen(host) >= KL_CLIENT_HOSTNAME_MAX` early return. |
| L2 | `Makefile` `cppcheck` | A newer cppcheck than CI's failed `make cppcheck` on `staticFunction` **false-positives** (public-API `kl_*` functions flagged should-be-static because cppcheck can't see the header consumers) and `normalCheckLevelMaxBranches` **informational** notes. | Added `--suppress=staticFunction --suppress=normalCheckLevelMaxBranches`; keeps the gate green across cppcheck versions without hiding real defects. |
| L3 | `src/async.c`, `src/dns_resolver.c`, `src/server.c` (×2), `src/body_reader_buffer.c` | Newer-cppcheck `constParameterPointer`/`constVariablePointer` on read-only pointers/params. | Added `const` (4 sites). The `kl_body_reader_buffer` factory param stays `void*` to match the `KlBodyReaderFactory` typedef; inline-suppressed with justification. |

### Reported, not changed (accepted / by design)

| # | File | Note |
|---|------|------|
| R1 | `src/socket_winsock.c` `kl_wsa_set_errno` | On Windows `EAGAIN != EWOULDBLOCK`; the mapping emits `EWOULDBLOCK`. Safe because every would-block test in-tree ORs **both** codes (verified across `dns_resolver.c` + the socket-seam callers). A contract, not a bug; keep the OR convention (a `KL_EWOULDBLOCK()` helper would make it self-enforcing). |
| R2 | `src/dns_resolver.c` `dns_resolve` | An over-long hostname (`> DNS_NAME_MAX`) is `snprintf`-truncated before `dns_build_candidates` rejects it, so a truncated name could be queried rather than hard-failed. No memory-safety impact. Optional: reject `strlen(host) >= DNS_NAME_MAX` up front. |
| R3 | `src/event_iocp.c` | A UDP send with an UNSPEC destination on an *unconnected* socket fails silently at completion (`ok=0`) rather than up-front; unreachable in practice because `kl_udp_send_to_from` validates `dest` at the public API. |
| R4 | `integrations/lwip/{socket_lwip,platform_wakeup_lwip}.c` | `set_reuseport`/`set_cork` return `-1` without setting `errno` (cosmetic diagnostic); the wakeup ignores `lwip_send`'s return (idempotent wakeup, recovered on the next poll tick). |

### Areas audited clean (no findings)

- **DNS response parser** (`kl_dns_parse_response`, `dns_skip_name`, `dns_extract_opt`,
  `dns_question_matches`): no compression-pointer following loop (names are terminated in place),
  label / RDATA / RR-header / address-size (`rdlen==4/16`) / multi-address-array bounds all tight.
  Also fuzzed (`fuzz_dns`).
- **`sockaddr.c`** parsers/formatters (`parse_ipv4`/`parse_ipv6`, `kl_sockaddr_format*`): bounded,
  no overflow; every builder `memset`s the union (no uninitialised-field leak in the round-trip).
- **UDP queue node** (flexible array): alloc-size overflow guard, matching free size on every
  path (send / EAGAIN-requeue / hard-error-drop / free); send-queue byte-cap underflow-guarded;
  GRO-split loop + cmsg build/parse bounds (runt-cmsg underflow guarded); recvmmsg/sendmmsg batch
  alloc/index/partial-free.
- **Completion backends** (pollcomp / io_uring / IOCP): op sockaddr-buffer sizes vs the socklen
  used in send/recv, recv name/control-buffer bounds, and op alloc/free lifetime; including the
  immediate-failure early-return paths and cancelled / zero-byte completions (no stale-buffer
  parse, no double-free/leak).
- **mbedTLS socket-provider routing**: NULL `sp` is byte-for-byte the prior behaviour
  (`kl_sockdef_*` fallback); per-session inheritance from the ctx; borrowed (not owned) provider
  pointer; no leak, no dangling copy; the completion memory-BIO path is unaffected.
- **socket_posix/winsock accept marshalling**: untrusted-peer `socklen` bounded before
  `kl_sockaddr_from_native`; `writev` iovcnt bounded before the stack `iovec[]`.

## Sixth pass: completion-axis feature work (PROXY / streaming / TransmitFile / TLS) (2026-08-01)

**Scope:** everything added/changed since the fifth pass; the completion-backend feature run
(PRs #128, #130, #133, #134, #135, #136): the completion driver's PROXY-header phase
(`comp_drive_proxy` + `kl_conn_ingest_proxy`) and overlapped streaming flush (`comp_stream_pump`,
8g-1); `event_iocp.c` (overlapped `WSARecvMsg` UDP local-addr, chunked `TransmitFile`, the
`post_recv` plaintext-during-PROXY guard); the three backends' `post_recv` guard; `drain.c`'s new
`kl_drain_data`/`kl_drain_consume`; `response.c` streaming-drain wiring; the TLS unit suites ported
to the shared completion-capable `tests/mock_tls.h`; and verification of the (already-landed)
platform-neutral mbedTLS backend on Windows.

**Method:** two parallel deep-review agents (completion axis; protocol/support + broad re-scan)
tracing op/buffer/connection lifetime, integer/offset math, and bounds; mechanical sweeps across
`src/`+`parsers/` (no `strcpy`/`sprintf`/`gets`, no `atoi`/`atol`/`atof`, raw `malloc`/`free` only
in the allocator wrapper, `kl_malloc` NULL-check + free-size discipline); `cppcheck` (clean on the
changed TUs); the **full 55-suite unit test under ASan+UBSan (`make debug-test`), 0 failures, 0
sanitizer hits**; and a MinGW compile-gate on the touched Windows TUs.

**Verdict: clean.** No Critical/High/Medium findings. Two **Low** allocator-discipline defects
(free-size mismatch on a zero-length completion send, latent under a bring-your-own *sized*
allocator; harmless under the default stdlib allocator) were found **and fixed this pass**.

### Low: fixed this pass

| # | File | Issue | Fix |
|---|------|-------|-----|
| L1 | `src/event_pollcomp.c` `pc_op_free` | A zero-length WRITE/SENDFILE/UDP op allocs `sendbuf` as `total ? total : 1` (1 byte) but `pc_op_free` freed it as `send_total ? send_total : KL_PC_CIPHER_SIZE` → frees a 1-byte block as **17408 bytes**. Reachable via a 0-length TLS-ciphertext send. A size-classed/arena `KlAllocator` mis-buckets the free. | Fall back to `1`, not `KL_PC_CIPHER_SIZE` (mirrors the alloc; `send_total` is set to the real size on every alloc path). |
| L2 | `src/event_iocp.c` `iocp_op_free` | Same shape: a zero-length WRITE frees a 1-byte `sendbuf` with `send_total == 0`. | Free `send_total ? send_total : 1`. |

### Informational (no action required)

- **I1** `event_iocp.c` `KL_IOCP_SENDFILE` completion advances `file_done` by the *requested*
  `file_chunk`, not bytes actually transferred. Correct because overlapped `TransmitFile` is
  all-or-nothing per chunk; a short success (never observed) would garble (not OOB) the body.
- **I2** `event_iouring.c` `kl_comp_cancel` leaves an op in-flight if the SQ is full at cancel
  time (liveness edge under SQ pressure, not a safety bug).
- **I3** `dns_resolver.c:976` write-buffer growth omits the project's `SIZE_MAX/2` doubling idiom;
  safe today (values bounded to a few KB), add the guard for uniformity.

### Verified correct (explicitly not findings)

Drain peek/consume is safe because `kl_comp_post_send` **copies** the buffer synchronously in all
three backends (so `kl_drain_consume` after posting is not a UAF); the PROXY `memmove` never
underflows (`kl_proxy_parse`'s `consumed ∈ [0,len]`, and `read_cap` 8192 ≫ `KL_PROXY_HEADER_MAX`
536, bounded by the `-1` guard); the ≤1-in-flight invariant (recv XOR send per conn, posted only
from a completion) means `kl_comp_cancel` yields exactly one aborting completion → one release, no
double-free; every `kl_comp_post_*` frees its op on all error paths; the Windows cmsg walks
(`kl_udp_win_parse_local`/`udp_parse_tos`) stop on a runt cmsg; `response.c` fixed stack buffers
(`cl_buf[48]`, `hdr[24]`) are correctly sized; CRLF header-injection + CL/TE smuggling (llhttp)
guards intact.

---

## Fifth pass: io_uring completion backend + provider auto-wire + stop-wakeup (2026-07-30)

**Scope:** the code added/changed across the io_uring-completion migration (PRs #101–#110):
`src/event_iouring.c` (the new ~750-line completion backend, hand-written op/registered-
buffer/splice/watcher lifecycle, the highest-risk new C), the 5a provider auto-wire
(`kl_event_native_provider` in every event backend + `kl_server_init`/client), the
`kl_server_stop` self-pipe wakeup (`src/server.c`), and the `iouringcomp`→`iouring` rename.

**Method:** mechanical sweeps (unsafe string/parse funcs, raw `malloc`/`free`, VLAs, `kl_malloc`
NULL-check discipline, integer-overflow guards) across `src/`; `cppcheck` on the new TUs; and,
the decisive step, an **ASan + UBSan + LeakSanitizer** run of the io_uring smokes on Linux
(Apple `container` VM, kernel 6.18), which the plain CI smokes don't provide.

**Issues found: 1** (Critical: 0, **High: 1**, Medium: 0, Low: 0), **fixed + verified.**

### High

| # | File | Issue | Fix |
|---|------|-------|-----|
| H1 | `src/event_iouring.c` (`kl_event_del`) | **Memory leak**: a readiness watch (`KlIouWatch`) removed while its `IORING_OP_POLL_ADD` was in flight was *unlinked* from `st->watches` with its free *deferred* to the poll-cancel CQE. At shutdown `kl_event_close`→`io_uring_queue_exit` drops that CQE, so the unlinked watch was never freed (and `kl_event_close`, freeing `st->watches`, no longer saw it). Surfaced as the `kl_server_stop` self-pipe watch leaking 48 B per server (2× in the async smoke). LeakSanitizer-confirmed; missed by CI because the io_uring backend was never run under LSan (only pollcomp was). | Keep the removed watch **linked** (skipped by `add`/`mod` via `!w->removed`); free it in the drain CQE (unlink+free) on the normal path, or in `kl_event_close` if the CQE never arrives (shutdown). Re-verified: both io_uring smokes pass under ASan+UBSan+LSan with **zero leaks**. |

**Systemic fix:** added `make smoke-iouring-asan` + a CI step in the *Completion (io_uring)*
job, so the io_uring op/buffer/splice/watcher lifecycle is now under LeakSanitizer in CI (the
gap that let H1 reach `main`).

### Clean (verified)

- **No unsafe functions** (`strcpy`/`strcat`/`sprintf`/`gets`/`atoi`/`atol`/`atof`) anywhere in
  `src/`+`parsers/`; raw `malloc`/`free` only in the default allocator wrapper (`allocator.c`).
- **`event_iouring.c` allocations**: all `kl_malloc` sites NULL-checked (registered pool is
  best-effort with malloc+SEND fallback; the rest fail cleanly via `iou_op_free`+`-1`).
- **Integer overflow**: `kl_comp_post_sendfile` guards `count`/`head_total` against `SIZE_MAX/2`;
  `sendcap` tracks the exact allocation for a correctly-sized free.
- **`cppcheck`** clean on the new TUs. **Hardening** intact (`-Werror -Wall -Wextra -Wpedantic
  -Wshadow -Wformat=2 -fstack-protector-strong -D_FORTIFY_SOURCE=3`; ASan/UBSan debug build).
- `LIBURING_UDATA_TIMEOUT` / cancel sentinels handled in the drain (no `(void*)-1` deref);
  single in-flight op per conn (driver invariant); idle-timeout cancel via `ASYNC_CANCEL` + abort.

## Fourth pass: mbedTLS backend + test shim + parser re-audit (2026-07-26)

**Scope:** the surface that changed since the third pass; `src/tls_mbedtls.c`
(significantly refactored: BIO callbacks routed through the socket seam, fds
retyped to `KlSocketHandle`, a shared `server_ctx_from_mem` + new
`kl_tls_mbedtls_ctx_create_from_buf`), the new test-network shim
(`tests/net_compat.{h,c}` posix/win + `tests/smoke_tls.c`), and a regression
re-audit of the untrusted-input parsers after the Winsock seam sweep + socket-
handle retype.
**Method:** three parallel source-level auditors; (1) deep `tls_mbedtls.c`
memory-safety (peer-cert extraction on untrusted mTLS input, error-path free
discipline, allocator/key-material handling), (2) the `net_compat` shim +
`smoke_tls` resource safety, (3) a regression re-check of `dns_resolver`,
`proxy_protocol`, `url`, `websocket`, `chunked`, `connection`; plus a tooling
sweep (dangerous functions, VLAs, raw alloc, cppcheck, and an ASan+UBSan real-
handshake run of the mbedTLS backend).

**Issues found: 3** (Critical: 0, High: 0, Medium: 0, **Low: 2, Doc: 1**),
**all fixed.**

The TLS backend came through clean. The auditors confirmed the hard parts sound:
the peer-cert CN/SAN extraction (`x509_extract_cn`/`x509_extract_san`) is
bounds-safe on attacker-controlled certs; every `memcpy` into the fixed `subject_cn`/
`issuer_cn`(256)/`san`(512) buffers is length-clamped before the copy, the
comma-separated SAN accumulator checks `off + ilen (+1) >= outlen` *before* every
write and always NUL-terminates, and the SHA-256 fingerprint fits its `char[65]`
exactly; the refactored ctx-creation error paths free each mbedTLS structure
exactly once and scrub key material (`kl_secure_zero`) on every path that owns the
key buffer (success and every early return); `read_file`'s length handling
(negative `ftell`, 1 MB cap, `len+1`) is safe; the BIO `kl_sockdef_send`/`recv`
errno→mbedTLS mapping is correct on both platforms; allocator create/destroy is
paired with the right sizes and the shared ctx is never double-freed. The
untrusted-input parsers showed **no regression** from the seam/handle changes;
the seam preserves the POSIX `ssize_t` contract and every `fd < 0` check migrated
to `kl_handle_valid`. Tooling: no dangerous functions, no VLAs, no unsanctioned
allocation, cppcheck clean, and the mbedTLS backend runs a real loopback handshake
clean under ASan+UBSan.

---

### Low

**L1: `x509_extract_cn` / `x509_extract_san`: defensive `outlen == 0` guard**
**`src/tls_mbedtls.c`**: *fixed.* Both peer-cert extractors wrote `out[0] = '\0'`
unconditionally and (CN) computed `outlen - 1`; safe with today's callers (fixed
256/512 buffers), but a future `outlen == 0` caller would write OOB / underflow.
Added `if (outlen == 0) return;` at the top of each; defense-in-depth on
untrusted-input functions.

**L2: `tests/smoke_tls.c`: client URL hard-coded the port instead of `SMOKE_PORT`**
**`tests/smoke_tls.c`**: *fixed.* The HTTPS URL literal duplicated `18443`; if
`SMOKE_PORT` changed, the client would silently target the old port and the test
would fail confusingly. Now built with `snprintf` from `SMOKE_PORT`.

### Documentation

**D1: `tests/net_compat_win.c`: undocumented WSAStartup precondition**
**`tests/net_compat_win.c`**: *fixed.* The loopback-pair helper needs Winsock
initialized; the library's load-time `WSAStartup` (socket_winsock.c) covers every
test, but that was implicit. Added a precondition note to the file comment.

### Areas audited clean (no findings)
- **`tls_mbedtls.c` peer-cert extraction**: bounds-safe on untrusted mTLS certs
  (clamped CN/SAN copies, pre-write accumulator bound, exact-fit fingerprint).
- **`tls_mbedtls.c` ctx error paths**: each mbedTLS struct freed once; key
  material scrubbed on all owning paths; no double-free/leak/UAF; correct free
  sizes; shared ctx not freed per-connection.
- **`tls_mbedtls.c` BIO + fd**: seam-routed I/O, correct errno mapping,
  `KlSocketHandle`/`KL_INVALID_SOCKET` throughout, transport owns the fd.
- **`net_compat_win.c` `kl_test_socketpair`**: each of listener/client/server
  closed exactly once on every `goto fail`, no double-close on success, no SOCKET
  leak, `addrlen` initialized before `getsockname`.
- **`smoke_tls.c` lifetimes**: server ctx freed on all paths (direct on init
  fail; via `kl_server_free` otherwise), per-iteration client ctx destroyed once,
  server stopped+joined before ctx destroy.
- **Untrusted-input parsers** (`dns_resolver`, `proxy_protocol`, `url`,
  `websocket`, `chunked`, `connection`), no regression; all bounds checks,
  length math, anti-spoof, and smuggling guards intact after the seam/handle sweep.
- **Tooling**: no `strcpy`/`sprintf`/`atoi`/`alloca`/…; no VLAs; no libc alloc
  outside `allocator.c`; `make cppcheck` clean; mbedTLS backend real-handshake
  ASan+UBSan green; full ASan+UBSan gauntlet green in CI on `main`.

## Recommendation
All three findings fixed. The mbedTLS backend, including the untrusted-input
peer-cert parsers and the refactored error paths, is well-bounded and now
platform-neutral. No further action.

---

## Third pass: Windows/Winsock PAL surface (2026-07-25)

**Scope:** the Windows platform TUs that landed with PAL Phase 6 (the Winsock
port), `src/socket_winsock.c`, `src/dns_sys_win.c`, `src/udp_io_win.c`,
`src/event_wsapoll.c`, `src/platform_win.c`, `src/server_plat_win.c`, and the
compatibility-boundary headers `src/sockcompat.h` / `src/socket.h` /
`src/dns_sys.h` / `src/platform.h` (Windows branches).
**Method:** three parallel source-level auditors (these TUs are Windows-only and
cannot be compiled on the macOS host; no MinGW), each cross-checking the Windows
implementation against its POSIX sibling's contract: (1) `udp_io_win.c` cmsg /
WSARecvMsg / batching; (2) `event_wsapoll.c` + `platform_win.c` +
`server_plat_win.c`; (3) the header shims + re-verification of the two fixes that
were already in the working tree.

**Issues found: 3** (Critical: 0, High: 0, **Medium: 3**, Low: several/Doc),
**all three Medium fixed.** The two fixes already present in the working tree
(`socket_winsock.c` errno translation on the seam ops + writev fail-loud;
`dns_sys_win.c` hosts-path truncation) were re-verified **correct and complete.**

> ⚠️ These fixes touch Windows-only TUs and were **not compiled locally** (no
> MinGW on the audit host). They must go green on the Windows CI job before merge.
> The POSIX build + full test suite remain green (the only shared header edit,
> `sockcompat.h`, is inside the `#if defined(_WIN32)` branch).

### M1: `event_wsapoll.c`: `kl_event_wait` returned `-1` without setting `errno`
**`src/event_wsapoll.c` (`kl_event_wait`)**: *fixed.*

On `WSAPoll` → `SOCKET_ERROR` the backend returned `-1` without touching `errno`
(Winsock reports via `WSAGetLastError()` and never sets the CRT `errno`). The
server accept loop (`server.c:582`) branches on `errno == EINTR` to decide
retry-vs-abort and then feeds `errno` to `kl_log_errno`; on a genuine poll error
it would read a **stale** `errno`; spuriously `continue`-looping if the stale
value happened to be `EINTR`, or logging a bogus reason otherwise. **Fix:**
translate via the now-shared `kl_wsa_set_errno()` before returning `-1`.

### M2: `platform_win.c`: `kl_plat_poll1` returned `-1` without setting `errno`
**`src/platform_win.c` (`kl_plat_poll1`)**: *fixed.*

Same root cause as M1 on the sync-client single-fd poll wrapper: returned
`WSAPoll`'s `-1` verbatim with no `errno`. **Fix:** `kl_wsa_set_errno()` on
`SOCKET_ERROR`, return `-1` (success value passed through unchanged).

*Shared-helper refactor for M1/M2:* `wsa_set_errno()` was file-static in
`socket_winsock.c`; promoted to a non-static `kl_wsa_set_errno()` declared in
`sockcompat.h` (Windows branch) so the event backend and `poll1` translate
identically to the socket seam ops. No behavior change on the existing 14 seam
call sites.

### M3: `udp_io_win.c`: truncated-datagram path parsed an indeterminate cmsg buffer
**`src/udp_io_win.c` (`kl_udp_io_recv_drain`)**: *fixed.*

On the `WSAEMSGSIZE` (truncated) return of `WSARecvMsg`, `msg.Control.buf/len`
are **not** reliably populated, yet the code fell through to
`udp_parse_local`/`udp_parse_tos` with `Control.len` still at the full buffer
size and the control buffer never zero-initialized. The additive
`cmsg_len >= WSA_CMSG_LEN(...)` checks prevent any out-of-bounds read, but the
delivered `recv_local` / `recv_tos_val` could be derived from indeterminate stack
bytes for a truncated datagram. **Fix:** a `have_control` flag set only on the
successful recv path now gates the cmsg parse, so truncated datagrams report no
local-addr/TOS (correct, that control data is unreliable).

### Low / Doc (noted, not fixed)
- **`event_wsapoll.c`**: empty pollset with an *infinite* timeout (`count==0 &&
  timeout_ms<0`) returns 0 immediately rather than blocking (POSIX `poll(...,-1)`
  blocks). Not reachable on the server path (the listen socket is always
  registered and the server passes a finite computed timeout). Low.
- **`platform_win.c`**: `kl_plat_random` casts `len` to `ULONG` for
  `BCryptGenRandom`; a `len > 4 GiB` would truncate. Callers use tiny buffers
  (mask keys, DNS txn-ids); not exploitable. The RNG return **is** checked and
  the buffer is never left uninitialized. Doc/Low.
- **Best-effort non-block failures**: `ioctlsocket(FIONBIO)` on the wakeup
  socket-pair read end ignores failure, matching the POSIX sibling's lenient
  `fcntl(O_NONBLOCK)` contract. Low, symmetry only.
- **`sockcompat.h`**: the `WSAE* → E*` mapping depends on the socket `E*`
  constants existing in `<errno.h>`; MinGW-w64 (the Makefile target) provides
  them but MSVC's `<errno.h>` historically does not. Worth a one-line "MinGW-w64
  required" note. Doc.
- **`kl_sockdef_close`**: returns `closesocket()`'s value without
  `kl_wsa_set_errno()`; no caller branches on errno after close. Doc.

### Verified sound (Windows PAL)
- **`socket_winsock.c`**: every seam op a caller tests for
  `EWOULDBLOCK`/`EINPROGRESS`/`EINTR` sets `errno` on `-1`; `connect` overrides
  `WSAEWOULDBLOCK→EINPROGRESS`; the tuning knobs that intentionally skip errno
  have no errno-inspecting caller; the writev `EINVAL`-on-`iovcnt>16` guard fails
  loud and can never overrun `stackbufs[16]` (real callers use `iov[7]`); the
  translation switch covers all tested codes.
- **`dns_sys_win.c`**: the hosts-path fix's `memcpy`s are bounds-safe (full
  default is 40 B into a `MAX_PATH`=260 buffer); build-once `static` buffer
  lifetime sound under the single-threaded loop.
- **`udp_io_win.c`**: TX/RX cmsg buffer sizing exact (`WSA_CMSG_SPACE`
  additive, no subtraction/underflow); all received-cmsg lengths additively
  checked; no dynamic alloc in the TU; matched `kl_free` size on the send queue;
  `kl_handle_valid` (not `<0`) re-check; `WSAEMSGSIZE` translated. The GSO-unsup
  `EIO` return correctly trips `udp.c:434`'s `!= EAGAIN/EWOULDBLOCK` predicate
  (no retry loop).
- **`platform_win.c`**: `kl_monotonic_ms` sec/rem decomposition avoids
  `ctr*1000` overflow, divide-by-zero guarded; socket-pair emulation closes all
  fds on every error path with no double-close; `kl_plat_file_pread` clamps
  `count > INT_MAX`.
- **`server_plat_win.c`**: `kl_srv_bind_unix` bounds `sun_path` before the
  `+1`-NUL `memcpy`; bind-failure closes + resets the fd; atomic console-ctrl
  handler registration sound.
- **`event_wsapoll.c`**: `grow_arrays` `INT_MAX/2` overflow guard + `size_t`
  math + fds-shrink-back on udata realloc failure; full unwind in init; matched
  free sizes in close.
- **Header shims** (`sockcompat.h`/`socket.h`/`dns_sys.h`/`platform.h`), `ssize_t`
  = `intptr_t` (pointer-width); all handle-carrying ops use `KlSocketHandle`
  (intptr_t) with `KL_INVALID_SOCKET`; no `SOCKET→int` truncation; no
  identifier-hijacking macros; every inline wrapper NULL-guards + falls back to
  `kl_sockdef_*`.

---

## Second pass: UDP feature surface (2026-07-19)

**Date:** 2026-07-19 (second pass, UDP feature surface)
**Scope:** the UDP stack that landed since the first pass; `src/udp.c` (roughly
doubled: multicast, `recvmmsg`/`sendmmsg` batching, GSO/GRO offload, ECN/TOS/DSCP
marking, and a send-path cmsg refactor), `src/udp_server.c`, a re-audit of the
`src/dns_resolver.c` untrusted-input parsers, and a full tooling/hardening sweep.
**Method:** three parallel auditors; (1) deep `udp.c` memory-safety/bounds,
(2) `udp_server.c` + DNS parser re-check, (3) automated tooling.

**Issues found: 2** (Critical: 0, High: 0, Medium: 0, Low: 1, Doc: 1), **both fixed.**

The UDP surface came through clean. The auditors confirmed the hard parts are
sound: RX/TX control-message buffer sizing (`UDP_RX_CMSG_SPACE` fits pktinfo +
UDP_GRO + TOS simultaneously; `UDP_TX_CMSG_SPACE` fits pktinfo + TOS exactly),
the `udp_build_control` CMSG_NXTHDR construction walk, batch alloc/free integer
safety (n∈[1,64], bufsz∈[1,65535], products well under SIZE_MAX; matched
free-sizes; partial-alloc cleanup), the recvmmsg/sendmmsg lifecycle and the
mid-batch use-after-free rechecks, `udp_drop_front`'s self-bounding dequeue (the
prior scan-build fix is genuinely sound), GSO/GRO split bounds, per-packet
`node->tos` initialization, and fd-leak/double-free paths. Tooling: no dangerous
functions, no VLAs, no unsanctioned allocation, `-Werror`-clean production build,
ASan+UBSan green, cppcheck only benign false positives.

---

## Low

### L1: `udp_parse_tos` computed a length by subtraction (size_t underflow risk)
**`src/udp.c` (`udp_parse_tos`)**: *fixed.*

```c
size_t dl = cm->cmsg_len - CMSG_LEN(0);   /* wraps if cmsg_len < CMSG_LEN(0) */
if (dl >= sizeof(int)) { ... }
```

A control message matching the TOS level/type but with `cmsg_len < CMSG_LEN(0)`
would underflow `dl` to a huge value and read `CMSG_DATA(cm)` past a runt cmsg.
Not reachable in practice (these cmsgs originate from the kernel, which always
sets a valid `cmsg_len`, and `CMSG_NXTHDR` won't advance into a runt trailer),
but it was the only one of the three cmsg parsers to subtract rather than use the
safe additive comparison. **Fix:** mirror `udp_parse_local`/`udp_parse_gro`;
`if (cm->cmsg_len >= CMSG_LEN(sizeof(int)))` / `>= CMSG_LEN(1)`. No subtraction,
matches the codebase idiom.

## Documentation

### D1: `kl_dns_parse_all` referenced but never implemented
**`CLAUDE.md`, `docs/dns_parity_design.md`**: *fixed.*

The Phase 2a design proposed a separate `kl_dns_parse_all()`; the implementation
instead folded multi-address collection directly into `kl_dns_parse_response`
(which fills `out->addrs[]` up to `KL_RESOLVE_MAX_ADDRS`). No such function
exists, yet CLAUDE.md claimed it was an implemented, fuzzed parser. **Fix:**
corrected CLAUDE.md to reference only `kl_dns_parse_response`, and noted the
plan-vs-implementation deviation in the design doc.

---

## Areas audited clean (no findings)

- **`udp.c` cmsg buffer sizing**: RX (64B) and TX (48B) buffers exactly cover the
  worst-case simultaneous cmsg sets; `udp_build_control` sets `msg_controllen`
  before the CMSG_NXTHDR walk, NULL-guards every write, and `used ≤ bufsz` always.
- **`udp.c` batch lifecycle**: no integer overflow (clamped n/bufsz), matched
  alloc/free sizes, correct partial-failure cleanup, mid-batch UAF rechecks after
  every callback (recv), self-bounding `udp_drop_front` (send).
- **`udp.c` GSO/GRO**: arg validation + fallback loop bounds; GRO split rechecks
  `recv_active` between segments.
- **`udp.c` public API**: NULL/`fd<0` guards on every entry; idempotent
  `kl_udp_free`; no double-close; fd freed on all `kl_udp_init` error paths.
- **`udp_server.c`**: all public entries NULL-checked; reply source-pinning
  bounds-checked and reset per datagram; all config fields forwarded with correct
  types; no fd leak on partial init.
- **`dns_resolver.c` response parser**: `dns_skip_name` /
  `kl_dns_parse_response` / `dns_question_matches`: every packet read length-checked
  before dereference; compression pointers skipped in place (no forward-follow, no
  loop); RDATA length validated before the fixed 4/16-byte memcpy; additive
  bounds math (no underflow); `naddrs` capped; anti-spoof (txn-id + source +
  question echo) intact. No regression.
- **Tooling**: no `strcpy`/`sprintf`/`atoi`/`alloca`/… ; no VLAs; no libc
  allocation outside `allocator.c`; `make` `-Werror` clean; `make debug-test`
  ASan+UBSan green; `make cppcheck` only benign `staticFunction`/const-suggestion
  false positives; production hardening flags all present.

## Recommendation

Both findings are fixed. The UDP feature surface, including the untrusted-input
cmsg parsers and the batched/offload paths, is well-bounded. No further action.
