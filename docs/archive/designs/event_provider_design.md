# Runtime event-backend provider (KlEventProvider)

Makes the **readiness event backend runtime-pluggable**, parallel to the socket
seam (`KlSocketProvider`). The motivation is a bring-your-own event backend;
lwIP (`lwip_poll`), that can be linked as a separate `.a` and installed at
runtime, without recompiling the Keel core. Companion to
`docs/lwip_platform_design.md` and `docs/capability_matrix.md`.

## The seam

`KlEventOps` (in `keel/event.h`) is a vtable over the eight event-loop
operations the core calls: `init / add / mod / del / wait / close / caps /
native_provider`. A `KlEventProvider` is `{ const KlEventOps *ops; const char
*name; }`. `KlEventLoop` gains an optional `const KlEventOps *ops`.

The public `kl_event_*` API is implemented once in `event_dispatch.c`:

```c
int kl_event_add(loop, …) {
    return loop->ops ? loop->ops->add(loop, …) : kl_event_add_builtin(loop, …);
}
```

Each compiled backend (epoll/kqueue/poll/WSAPoll/io_uring/IOCP/pollcomp) renamed
its eight functions to `*_builtin`; the selected one (Makefile `BACKEND=`) is
still linked in and called directly. So:

- **Default path (all shipped backends): `loop->ops == NULL`** → one
  perfectly-predicted branch, then a direct call to the same `*_builtin` code as
  before. No vtable indirection, no behavior change, no measurable overhead.
- **Runtime provider (lwIP): `loop->ops` set** → the same calls route through the
  provider. Installed via `KlEventCtx.event_provider` →
  `kl_event_init_provider()`; `kl_event_init()` itself always takes the builtin
  path and zeroes `ops` (so a stack-allocated loop is safe).

## Scope: readiness AND completion (completion runtime-injectable since 2026-08-04)

`KlEventOps` is the **readiness** event API. The **completion** axis (io_uring /
IOCP / pollcomp / lwIP-raw) drives its loop through the `kl_comp_*` primitives
(`completion.h` / `io_engine.h`). Completion backends also implement the eight
`kl_event_*` ops (for watcher fds + `native_provider` auto-wiring), routed through
this dispatcher like any other.

**Update: completion is now runtime-injectable too** (see
`docs/completion_axis_runtime_design.md`, RC-1..RC-4). The `kl_comp_*` primitives
are an internal `KlCompletionOps` sub-vtable reached via one opaque `const void
*completion` on `KlEventOps`, dispatched by `completion_dispatch.c` exactly like
this readiness dispatcher (`loop->ops ? loop->ops->completion : kl_comp_ops_builtin()`);
`completion_driver.c` is always-linked (`KEEL_NO_COMPLETION` opt-out). A completion
backend split into a pure-provider TU (no `_builtin`) + an optional builtin-glue TU
(only when it is the compiled-in `BACKEND`) can be injected at runtime on a stock
`libkeel`, **realized**: `kl_event_provider_pollcomp()` serves on a default build,
and `kl_event_provider_lwip_raw()` runs the full P9 suite on a stock `libkeel` (the
old build-time-only policy below is superseded). `BACKEND=` remains the compiled-in
default-selector.

## Matched provider/backend pairing

A socket provider and event backend must agree on what a "pollable handle" is: a
provider watching non-OS handles (lwIP socket indices) pairs with an event
backend that can poll them (`event_lwip`). Today that pairing is a matched set
selected together (the lwIP integration ships both). `KlEventOps.caps` /
`native_provider` + `kl_event_ctx_sockets_compatible` (async.c) already carry the
capability bits for a future negotiated pairing.

## Compatibility

`KlEventLoop` gains a trailing `ops` field and `keel/event.h` gains
`KlEventOps` / `KlEventProvider` / `kl_event_init_provider`: additive,
source + static-relink compatible (see `docs/compatibility.md`). Callers that
allocate a `KlEventLoop` on the stack and call `kl_event_init()` are unaffected
(it zeroes `ops`).
