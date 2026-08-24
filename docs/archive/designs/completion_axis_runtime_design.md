# Making the completion axis runtime-injectable: Design

**Status (2026-08-04): COMPLETE. RC-1..RC-4 all merged (PRs #184–#187).** The completion axis is
now runtime-injectable: a completion backend installs via `KlEventCtx.event_provider` on a **stock**
`libkeel`: proven by pollcomp serving `GET /` on a default readiness build (RC-2) and lwIP-raw
running the full P9 suite as a runtime provider on a stock libkeel (RC-3, retiring `BACKEND=lwipraw`).
`BACKEND=` remains the compiled-in default-selector; `KEEL_NO_COMPLETION` is the readiness-only
opt-out (CI-gated). `completion_driver.c`/`src` primitives' logic never changed: only their
plumbing (RC-1).

Goal: let a completion event backend be installed at
**runtime** via `KlEventCtx.event_provider` (like the readiness lwIP provider is today), instead of
only at **link time** via `BACKEND=`. This is the deferred alternative from
`docs/phase9_lwip_raw_design.md` ("Architectural shape"): once landed, `event_lwip_raw.c` (and any
future completion backend) can drop onto a **stock `libkeel.a`** + its own objects, no special
`libkeel` build.

## Why

The socket, event(-provider), and datagram axes are already runtime-injectable (a provider vtable
chosen on `KlEventCtx`/`KlConfig`). The **completion axis is the last link-time holdout**: its
primitives (`kl_comp_*`) + generic driver (`completion_driver.c`) are compiled in only for a
completion `BACKEND` (`iouring`/`iocp`/`pollcomp`/`lwipraw`); a readiness build links the
`io_engine.c` **stubs**. Consequences today:

- A runtime-injected completion `KlEventProvider` (e.g. lwIP-raw) into a stock `libkeel` finds
  `kl_comp_run` resolved to the do-nothing stub → it silently doesn't work. Phase 9 therefore had to
  ship as a whole `BACKEND=lwipraw` `libkeel` build.
- Selecting a completion backend is a compile-time, whole-library decision; you can't, say, ship
  one `libkeel` and let the *application* choose io_uring vs a custom completion transport at
  startup, or run two ctxs on different completion backends in one process.

Runtime injection dissolves this, symmetric with the other three axes.

## Decisions (confirmed)

1. **Completion sub-vtable stays INTERNAL.** It references internal types (`KlConn`,
   `KlCompletionEvent`, `KlIoVec`, `KlUdp`); `completion.h`/`io_engine.h` remain internal, and
   completion backends keep building against the `-Isrc` seams (they already do). No new public
   completion ABI is frozen. (See "Attachment mechanism" for the one small public-header touch this
   forces.)
2. **`BACKEND=` stays as the default-selector.** It still picks the *compiled-in default* provider
   (zero churn for existing users + the CI matrix); runtime injection via
   `KlEventCtx.event_provider` becomes the general mechanism. Both coexist: exactly like the
   readiness axis (compiled-in epoll/kqueue *and* a runtime lwIP provider).
3. **`completion_driver.c` is always-linked, with a `KEEL_NO_COMPLETION` opt-out.** Default builds
   carry the driver so runtime completion injection works out of the box;
   `make KEEL_NO_COMPLETION=1` drops it for ultra-constrained readiness-only targets (a completion
   provider is then rejected at init).

## Current state (what changes)

- `include/keel/event.h`, public `KlEventOps` vtable: `init/add/mod/del/wait/close/caps/native_provider`.
  Dispatched by `src/event_dispatch.c`: `loop->ops ? loop->ops->X(...) : kl_event_X_builtin(...)`
  (runtime provider vs compiled-in `_builtin`). This is the pattern the completion axis will copy.
- `src/completion.h` (internal), backend primitives: `kl_comp_drain`, `kl_comp_prime_accepts`,
  `kl_comp_post_recv/send/accept/sendfile`.
- `src/io_engine.h` (internal), the seam: generic entry points implemented by
  `completion_driver.c` (`kl_comp_run`, `kl_io_engine_run_completion`,
  `kl_io_engine_resume_completion`, `kl_io_engine_post_read`) + backend primitives
  (`kl_comp_cancel`, `kl_comp_post_udp_recv/send`). On readiness builds `io_engine.c` stubs them all.
- Callers of the completion entry points: `src/async.c` (`kl_comp_run`,
  `kl_io_engine_resume_completion`), `src/server.c` (`kl_io_engine_run_completion`, `kl_comp_cancel`,
  `kl_io_engine_post_read`), `src/udp.c` (`kl_comp_post_udp_recv/send`), and `completion_driver.c`
  itself (the `kl_comp_post_*` primitives).
- Makefile `BACKEND` block: completion backends set `EVENT_SRC=<backend>.c` +
  `COMPLETION_SRC=src/completion_driver.c` + `IO_ENGINE_SRC=` (empty); readiness builds get
  `IO_ENGINE_SRC=src/io_engine.c` (the stub) + no `COMPLETION_SRC`.

## Design

### The completion sub-vtable (internal)

`src/completion.h` gains a vtable grouping the per-backend primitives:

```c
typedef struct KlCompletionOps {
    int  (*drain)(struct KlEventCtx *ctx, KlCompletionEvent *out, int max, int timeout_ms);
    int  (*prime_accepts)(struct KlServer *s);
    int  (*post_recv)(KlConn *c);
    int  (*post_send)(KlConn *c, const KlIoVec *iov, int iovcnt, size_t total);
    int  (*post_accept)(struct KlServer *s);
    int  (*post_sendfile)(KlConn *c, const KlIoVec *head_iov, int head_n,
                          size_t head_total, int file_fd, uint64_t count);
    void (*cancel)(struct KlEventCtx *ctx, KlSocketHandle fd);
    int  (*post_udp_recv)(struct KlUdp *udp);
    int  (*post_udp_send)(struct KlUdp *udp, const void *data, size_t len,
                          const KlSockAddr *dest);
} KlCompletionOps;
```

The **generic** entry points (`kl_comp_run`, `kl_io_engine_run_completion`,
`kl_io_engine_resume_completion`, `kl_io_engine_post_read`) are backend-independent and stay as
functions in `completion_driver.c`: they just reach the backend primitives through this vtable.

### Attachment mechanism (the one public-header touch)

A runtime provider is reached via `loop->ops` (public `KlEventOps`). To reach an *internal*
completion sub-vtable from it without leaking internal types, add **one opaque field** to
`KlEventOps`:

```c
struct KlEventOps {
    ... existing 8 members ...
    const void *completion;   /* reserved: internal KlCompletionOps* (src/completion.h), or
                               * NULL for a readiness backend. Opaque here so no completion
                               * type enters the public header (cf. KlEventLoop._backend). */
};
```

This is the same discipline as the existing public `KlEventLoop._backend` / `KlEventLoop.fd`
reserved fields, a public *slot*, no public *type*. It is the minimal change; the alternative (a
global mutable registry keyed by `ops`) is rejected as it introduces hidden global event-loop state
(an explicit axis-audit smell) and registration-ordering/teardown concerns, whereas a `const`
`static` vtable literal carrying the pointer has none. **← primary item to confirm on review.**

Dual dispatch, mirroring `event_dispatch.c`, in a new `src/completion_dispatch.c`:

```c
/* One accessor resolves compiled-in vs runtime, exactly like kl_event_caps(). */
static inline const KlCompletionOps *kl_comp_ops(const KlEventLoop *loop) {
    return loop->ops ? (const KlCompletionOps *)loop->ops->completion
                     : kl_comp_ops_builtin();   /* per-EVENT_SRC: the sub-vtable, or NULL */
}
int kl_comp_post_recv(KlConn *c) { return kl_comp_ops(&c->ctx->loop)->post_recv(c); }
/* ...and so on for each primitive; cancel/udp/drain/etc. identically... */
```

`kl_comp_ops_builtin()` is **one** function per event backend TU: a completion backend returns
`&its_completion_ops`; a **readiness** backend returns `NULL`. Because a readiness loop never enters
the completion branch (its `caps()` lacks `KL_EVENT_CAP_COMPLETION`, enforced by
`kl_event_ctx_sockets_compatible`), the `NULL` is never dereferenced. This replaces the N aborting
`io_engine.c` stubs with a single trivial `NULL`-returning function on the readiness side.

### Per-backend changes

- **Completion backends** (`event_iouring.c`, `event_iocp.c`, `event_pollcomp.c`,
  `integrations/lwip/event_lwip_raw.c`): keep their primitive implementations, expose them as a
  `static const KlCompletionOps` literal, set `KlEventProvider.ops->completion = &that`, and
  implement `kl_comp_ops_builtin(){ return &that; }`. No logic change: just packaging. Their
  `KlEventProvider` (already returned by `kl_event_provider_iouring()` etc.) now carries the
  completion vtable, so injecting it at runtime works.
- **Readiness backends** (`event_epoll/kqueue/poll/wsapoll.c`): a shared
  `src/completion_readiness_stub.c` provides `kl_comp_ops_builtin(){ return NULL; }`, linked when
  `EVENT_SRC` is readiness. `KlEventOps.completion` for these is `NULL`.

### Always-linked driver + the opt-out

- Default: `CORE_SRC` unconditionally includes `src/completion_driver.c` + `src/completion_dispatch.c`
  (+ the readiness stub when the compiled-in backend is readiness). The per-`BACKEND`
  `COMPLETION_SRC`/`IO_ENGINE_SRC` juggling is removed.
- `KEEL_NO_COMPLETION=1`: link **neither** the driver nor the dispatch layer; instead link
  `src/completion_absent.c`: aborting stubs for the generic entry points the callers reference
  (`kl_comp_run`, `kl_io_engine_*`, `kl_comp_post_udp_*`): none reachable on a readiness loop. This
  is exactly today's `io_engine.c`-stub role, preserved for the constrained build.
  `kl_event_init_provider` / `kl_event_ctx_sockets_compatible` reject a `KL_EVENT_CAP_COMPLETION`
  provider under `KEEL_NO_COMPLETION` (fail-loud, not silent).

"No `#ifdef` in shared code" is preserved throughout: the callers always reference the same symbols;
the Makefile swaps the *implementation* TU (driver+dispatch vs absent-stub; completion-backend vs
readiness-stub), the established `EVENT_SRC`/`IO_ENGINE_SRC` selection pattern.

## Makefile shape (sketch)

```
COMPLETION_CORE = src/completion_driver.c src/completion_dispatch.c   # always, unless NO_COMPLETION
ifdef KEEL_NO_COMPLETION
  COMPLETION_CORE = src/completion_absent.c
endif
# EVENT_SRC completion backends provide kl_comp_ops_builtin; readiness backends need the stub:
ifeq ($(backend is readiness), yes)
  COMPLETION_CORE += src/completion_readiness_stub.c
endif
```
(`BACKEND=` still sets `EVENT_SRC` as today; it no longer sets `COMPLETION_SRC`/`IO_ENGINE_SRC`.
`io_engine.c` is retired, its stub role splits into the readiness stub + the absent stub.)

## Backend dual-role structure (RC-2 refinement, for actually injecting a completion backend)

RC-1 made the *mechanism* runtime-dispatchable (the dispatchers honor `loop->ops->completion`), but
the existing completion backends (`event_pollcomp.c`, `event_iouring.c`, `event_iocp.c`, and
`event_lwip_raw.c`) are still authored **compiled-in-only**: they define the `kl_event_*_builtin`
free functions + `kl_comp_ops_builtin` and expose **no `KlEventProvider` factory**. To *inject* such
a backend into a `libkeel` that already has a compiled-in backend, its `_builtin` symbols would
**clash** (both define `kl_event_add_builtin`, `kl_comp_ops_builtin`, …). So a completion backend
must be expressible in **two roles** without duplicate symbols; exactly how the readiness lwIP
provider (`event_lwip.c`) already is (static `lwev_*` ops + a `KlEventProvider`, **no** `_builtin`).

The structure (established on `pollcomp` in RC-2, applied to the others in RC-3+):

- **Provider TU** (`event_<x>.c`): the ops as **static** functions grouped into a
  `static const KlEventOps <x>_event_ops = { …, .completion = &<x>_completion_ops }`, plus the
  `static const KlCompletionOps`, the overlapped `kl_socket_provider_<x>()`, and a factory
  `const KlEventProvider *kl_event_provider_<x>(void)`. **No `_builtin` symbols** → links cleanly
  alongside any other backend, injectable at runtime.
- **Builtin-glue TU** (`event_<x>_builtin.c`, linked **only** when `BACKEND=<x>`): the thin
  compiled-in adapters: `kl_event_*_builtin(...) → <x>_*(...)` and
  `kl_comp_ops_builtin() → &<x>_completion_ops`: over an internal `event_<x>_internal.h` that
  exposes the provider TU's ops. This is what the `loop->ops == NULL` compiled-in path binds to; it
  replaces the readiness stub for that backend. No `#ifdef`, the Makefile links the glue only for
  that `BACKEND`.

A completion backend is then usable BOTH as the compiled-in default (`BACKEND=<x>` → provider TU +
glue TU) and as a **runtime provider** linked next to a different default (provider TU only, injected
via `kl_event_provider_<x>()`). `pollcomp` is the RC-2 subject (portable, POSIX, the test double);
`event_lwip_raw.c` gets the same treatment in RC-3 (then it drops onto a stock `libkeel`, retiring
`BACKEND=lwipraw`); `iouring`/`iocp` can follow opportunistically.

## What this buys (and the lwipraw clarification)

- Any completion `KlEventProvider` can be installed at runtime on a **stock** `libkeel`, the driver
  + dispatch are present, so `loop->ops->completion` is honored.
- **lwIP-raw specifically:** it still needs its `event_lwip_raw.c`/`lwip_raw_glue.c` objects compiled
  against BYO lwIP (that's inherent, the code must exist). The win is those become **runtime
  provider objects linked next to a stock `libkeel`** + `kl_event_ctx_init_ex(..., kl_event_provider_lwip_raw())`,
  instead of a bespoke `BACKEND=lwipraw` whole-library build. The `integrations/lwip/loopback-raw`
  target changes from "build a lwipraw libkeel" to "build stock libkeel + the lwip-raw objects +
  inject." `BACKEND=lwipraw` can be retired (or kept as a convenience default).
- io_uring/IOCP/pollcomp are unaffected as defaults (`BACKEND=` unchanged) but ALSO become
  runtime-injectable: enabling e.g. a readiness-default `libkeel` into which an app injects io_uring
  at startup only where available.

## Testing

- **Regression:** every existing `BACKEND=` build + its suites/smokes/gates must stay green
  (compiled-in path via `kl_comp_ops_builtin()`, behaviorally identical). Full matrix: default
  epoll/kqueue, `poll`, `pollcomp`, `iouring` (+ `test-iouring` 56-gate), `iocp` (MinGW),
  `cosmocc`, and `KEEL_NO_COMPLETION`.
- **The actual proof (new):** runtime-inject a completion provider into a **readiness-compiled**
  (default) `libkeel` and serve a request. `pollcomp` is ideal (POSIX, no special deps, a test
  double): a new test/smoke builds a default `libkeel` and does
  `kl_event_ctx_init_ex(&ctx, a, kl_event_provider_pollcomp())` → server answers `GET /`. This is
  the direct evidence the axis is runtime-injectable (today impossible).
- **lwIP-raw:** `loopback-raw` reworked to the stock-libkeel-+-inject form → same `P9-*` cases pass,
  proving a *foreign* completion backend injects at runtime. ASan+UBSan+LSan as in P9-4.
- `KEEL_NO_COMPLETION`: builds; a completion provider is rejected at init (assert the error).

## Staged rollout (each stage green before the next)

- **RC-1: DONE (#184).** Add `KlCompletionOps`, the
  opaque `KlEventOps.completion`, `completion_dispatch.c` with `kl_comp_ops()` +
  `kl_comp_ops_builtin()`, the readiness stub, and the `completion_absent.c` opt-out. Migrate the
  compiled-in completion backends to provide `kl_comp_ops_builtin` + the vtable. `completion_driver.c`
  always-linked. Retire `io_engine.c`. **Gate:** the whole existing matrix stays green (pure
  refactor; `loop->ops==NULL` path identical to today).
- **RC-2: DONE.** Restructured `pollcomp` into the provider-TU + builtin-glue-TU split
  (`event_pollcomp.c` = pure provider, no `_builtin`; `event_pollcomp_builtin.c` = glue, linked only
  for `BACKEND=pollcomp`; `event_pollcomp_internal.h`). New `smoke-completion-inject[-asan]`:
  a DEFAULT (readiness) libkeel + `event_pollcomp.o` (extra object) serves `GET /` over the
  runtime-injected `kl_event_provider_pollcomp()` → `COMPLETION-INJECT PASS`, ASan-clean, CI-gated.
  The direct proof the axis is runtime-injectable. Regressions green (default / `BACKEND=pollcomp` /
  iouring gate / `KEEL_NO_COMPLETION`).
- **RC-3: DONE.** `integrations/lwip/event_lwip_raw.c` is now a **pure runtime provider** (static
  ops + `kl_event_provider_lwip_raw()` + `.completion`, **no `_builtin`**: `nm`-verified; no glue TU
  needed since `BACKEND=lwipraw` is retired). `loopback-raw` builds a **stock libkeel** + the lwip-raw
  provider objects and injects at runtime (`KlConfig.event_provider = kl_event_provider_lwip_raw()`;
  the server auto-wires the paired overlapped socket provider from `native_provider()`). All P9 cases
  (P9-1 tick → P9-4 lifetime) pass via injection on a stock libkeel, ASan+UBSan+LSan-clean. The
  `BACKEND=lwipraw` root-Makefile case is removed. Regressions green (default / pollcomp-inject /
  iouring gate).
- **RC-4: DONE.** `KEEL_NO_COMPLETION` added as a CI matrix cell (build + full readiness suite +
  smoke); `test_event_caps.negotiation_matrix_completion` made correct under the opt-out (branches
  on the runtime `kl_completion_axis_available()`, a completion loop is fail-loud incompatible when
  the axis is compiled out). Cross-ref docs updated (`phase9_lwip_raw_design`, `event_provider_design`,
  `pal_review`). Effort COMPLETE.

## Risks / notes

- **The opaque `KlEventOps.completion` field** is a (tiny) public-struct change, an added trailing
  member. Assess ABI: KEEL shipped as a static lib + headers (no stable .so ABI promised), and the
  field is appended, so source-compat holds; flag in the changelog.
- **`completion_driver.c` size on `KEEL_NO_COMPLETION`-off embedded/cosmo builds**: modest `.text`
  bump; the opt-out exists for those who can't afford it.
- **`kl_comp_ops(loop)` indirection on the compiled-in hot path**: one extra load + predictable
  branch (same as `event_dispatch.c` already accepts for the event axis); negligible, and the
  benchmark suite (`bench_compare`) will confirm no regression vs the current link-time calls.
- **Two-ctx / two-backend in one process** becomes expressible; ensure no residual file-scope global
  state in the backends assumes a single loop (audit during RC-1, the backends currently key state
  off `loop->_backend`, which is per-loop, so this should already hold).
