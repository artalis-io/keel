# Phase 4 — Public socket-provider selection — Design

Status: **designed (2026-07-20); implementation deferred until after a Winsock
prototype (Phase 6).** Follow-on to PAL Phases 1–3 + server adoption
(`docs/pal_transformation_design.md`, `docs/server_provider_adoption_design.md`).

Phase 4 makes the socket provider **user-selectable and user-authorable** —
today the `KlSocketProvider` vtable, `kl_socket_provider_posix()`, and the
capability helpers live in the internal `src/socket.h` (not installed), so an
embedder can inject a provider only from inside the tree. This doc specifies the
public surface **and why it is sequenced after Phase 6.**

---

## 1. Decision: build Winsock (Phase 6) first, then go public

The PAL design's own discipline: *don't make the provider API public until it is
proven by (1) POSIX, (2) a mock, and (3) a real alternative provider or a
detailed prototype.* We have (1) + (2) + a Winsock **design**, but no Winsock
**implementation**. Freezing the public vtable now risks a public **breaking**
change once a real provider lands, on three known fronts:

1. **Handle type.** The vtable is `int fd`-based. A Winsock `SOCKET` is a
   pointer-width handle on Win64 — it does not fit `int` cleanly. Phase 5
   (portable handles) is explicitly "driven by the next real provider." Exposing
   `int fd` signatures publicly, then widening them for Winsock, breaks callers.
2. **writev / sendfile ops.** Server adoption (stages 3–4) capability-gated the
   POSIX `writev`/`sendfile` fast paths and left *ops* for them as a documented
   Phase 6 hook (Winsock uses `WSASend`/`TransmitFile`). A performant Winsock
   needs those ops in the vtable — added when Winsock lands, not speculatively.
3. **accept/connect shape.** Winsock's `AcceptEx`/`ConnectEx` (and any future
   completion path) may want richer accept/connect signatures than the current
   readiness-style ops.

Doing Winsock first validates all three, so the public vtable is **frozen once,
correctly.** Net sequencing: **Phase 1→2→3→server-adoption→6(Winsock
prototype)→4(public)→5(portable handles, if needed).** This reorders "4 before
6" from the original roadmap — recorded here and in the PAL doc.

**Interim (already shipped):** `KlEventCtx.sockets` is a public *opaque* field
(`const struct KlSocketProvider *`, forward-declared). In-tree tests already
inject via it. That is enough for internal experimentation; no public
provider-authoring API is committed until this phase executes.

---

## 2. Public surface (to expose when this phase runs)

### 2.1 New installed header `include/keel/socket.h`
Moves the stable subset of `src/socket.h` public. Contents:
- `KlSocketProvider` (`{ const KlSocketOps *ops; void *context; uint64_t
  capabilities; }`) and `KlSocketOps` (the ops table — **shape frozen by the
  Phase 6 provider**, incl. any `writev`/`sendfile` ops and the final handle
  type).
- Capability flags: `KL_SOCK_CAP_NATIVE_FD`, `KL_SOCK_CAP_WRITEV`,
  `KL_SOCK_CAP_SENDFILE` (+ whatever Winsock adds).
- `const KlSocketProvider *kl_socket_provider_posix(void);`
- Query helpers: `kl_socket_provider_has_cap()`, `kl_sock_native_fd()`,
  `kl_socket_provider_destroy()`.
- `KlError kl_sock_errno_to_error(int)` — the **coarse** mapping (decision:
  keep existing `KlError` codes; no new public enum values).

**Stays internal** (`src/socket.h`, not installed): the inline `kl_sock_send/recv/
connect/...` wrappers — those are Keel's *consumers* of a provider, not part of
the authoring API. A custom provider implements `KlSocketOps`; it never needs the
wrappers.

### 2.2 `event_ctx.h` — de-opaque the field
`KlEventCtx.sockets` already exists as `const struct KlSocketProvider *`. Once
`keel/socket.h` is public and included by `event_ctx.h`, the type is nameable and
users can assign `ctx.sockets = kl_socket_provider_posix()` (or a custom one).

## 3. Selection wiring

### 3.1 Server — `KlConfig.sockets`
Add `const KlSocketProvider *sockets;` to `KlConfig` (default NULL = POSIX).
`kl_server_init` copies it to `s->ev.sockets` **before** `bind` — so, unlike the
server-adoption interim (post-init injection, listen socket kept the default),
the **listen socket itself** is created through the provider. This is also where
the **native-fd guard** activates:

```c
if (cfg->sockets && !kl_socket_provider_has_cap(cfg->sockets, KL_SOCK_CAP_NATIVE_FD)) {
    s->last_error = KL_ERR_SOCKET;   /* can't poll a non-native provider */
    return -1;                        /* explicit unsupported-combination error */
}
```

### 3.2 Client — `ctx.sockets` directly
Async clients already take a user-owned `KlEventCtx`; the user sets
`ctx.sockets` before creating the client. No new client-config field needed (the
client does not own its ctx). Document this in the client guide. (A convenience
`KlClientConfig.sockets` could be added later if demand appears; not in the
minimal surface.)

## 4. Compile-time / linking model
Providers are plain structs the embedder supplies; "link only one provider" is
automatic — POSIX is the only built-in, custom ones are user code. The
compile-time default is NULL (POSIX). No registry, no global mutable state
(unchanged from the internal design).

## 5. Deliverables (when executed, after Phase 6)
- `include/keel/socket.h` (installed) + `event_ctx.h` include.
- `KlConfig.sockets` + the pre-bind native-fd guard.
- **Example:** `examples/custom_socket_provider.c` — a decorator over POSIX that
  counts bytes / logs (mirrors the existing `custom_allocator` example), proving
  the authoring API on a real program (the doc's "example custom provider").
- **Migration doc:** a short section in the server/client guides — "selecting a
  socket provider", the native-fd requirement, and the capability contract.
- Tests: server + client over `kl_socket_provider_posix()` selected via the
  public config (conformance); the native-fd guard rejection; the decorator
  example exercised.

## 6. Error taxonomy (decision: coarse)
`kl_sock_errno_to_error` stays mapped to the existing `KlError` network codes
(`TIMEOUT`/`CONNECT`/`BIND`/`ALLOC`/`INVALID_ARG`/`IO`). No new public enum
values in Phase 4. If a consumer later needs distinct would-block/reset/
unreachable categories, that is an additive follow-up.

## 7. Stop conditions / risks
- **Executing before Phase 6** → re-freeze risk (§1). Gate: do not implement §2–3
  until the Winsock vtable shape is settled.
- **Handle-type churn** → if Phase 5 widens the handle, `keel/socket.h` must ship
  with the final type; don't publish `int fd` ops if Winsock needs otherwise.
- **Scope creep into a provider registry / runtime plugin loading** → out of
  scope; providers are compile-time/link-time structs.

## 8. Out of scope
Portable handle/address types (Phase 5, only if Winsock needs them); the Winsock
provider itself (Phase 6); IOCP/completion (Phase 8); finer public error codes
(deferred).
