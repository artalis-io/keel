# F2-B decision record: public type classification and layout exposure

Status: DECISION for review (F2-B step 1, revised twice). Docs-only. No header, struct, consumer, test,
or integration changed by this document, and the F2-1a classification scaffolds are NOT filled yet (the
193-row fill is deferred until this taxonomy is accepted). Grounded in the live tree at base commit
`4e667e1` on branch `main`, the F2-1a census (`docs/f2/public_types.tsv`, 193 types), and the accepted
F2-C (append-only, zero-default) and F2-D (remove KlEventLoop.fd) decisions.

This revision finalizes two decisions the prior draft left inconsistent: `KlEventCtx.loop` is resolved
via accessors, and `KlHttpConn` is resolved by making its layout opaque (its storage is pool-owned and
the pool holds a pointer, so the full definition need not be public). The remaining taxonomy and the
decisions for `KlHttpServer`, `KlEventCtx` (as a value object), `KlWatcher`, and `KlTimerEntry` are as
previously accepted.

Scope: classify every public type and decide, per struct and per externally-referenced field, opaque vs
blessed-facet vs accessor. Resolves freeze findings 7.2 and 7.3. Implementation (header edits, new
accessors, definition relocation, the scaffold fill, contract comments) is a separate reviewed step.

## 0. Inputs

- The 193-type census: 89 struct, 25 enum, 16 opaque, 60 callback, 3 alias.
- F2-C: public configs and vtables evolve append-only, zero-default; source compatibility plus static
  relink, not binary compatibility (reinforced by W^X/no-dlopen).
- F2-D: `KlEventLoop.fd` is removed; the epoll/kqueue descriptor moves to a `loop->alloc`-owned backend
  state, shrinking `KlEventLoop` to `{ _backend, alloc, ops }`.

## 1. Classification taxonomy

Each public type is classified into exactly one category. The accepted final vocabulary is eight
categories: the freeze's `caller-constructed`/`caller-inspectable`/`opaque`/`opt-in-unstable-layout`,
extended by `api-signature` and `type-alias` (F2-1a) and by `caller-owned-value` and `enum-constants`
(this decision). `impl-layout-installed` was a working bucket during classification; F2-B1 gave each of
its members a concrete destination (opaque with a migration note, caller-owned-value, caller-constructed,
or caller-inspectable), so it reached zero and is NOT part of the accepted vocabulary. It described a
migration finding, not a surface Keel freezes.

- caller-constructed: the caller/provider allocates the storage AND fills the fields (configs, vtables,
  hook/callback containers, caller-filled currency). Append-only per F2-C.
- caller-inspectable: the library fills it and the caller reads defined fields (result/identity structs).
- caller-owned-value: the caller allocates the storage via an init-in-place function; the library owns
  most fields. The layout is published so the caller can allocate the object. Some fields are documented
  STABLE PUBLIC FACETS (used directly or via an accessor); the rest are visible solely to permit
  allocation and evolve append-only (recompile required). Per-field disposition is in section 3.
- opaque: handle-only; the layout is private to `src/` or an internal header. Callers hold a pointer and
  use accessors only. (A borrowed handle whose owning container references it by pointer belongs here,
  because nothing forces its layout to be public.)
- opt-in-unstable-layout: opaque handle on the main surface whose layout is in a `*_detail.h`.
- api-signature: a function-pointer typedef (callback); signature frozen.
- type-alias: a typedef alias.
- enum-constants: a public enumeration; append-only value set.

The prior draft's `borrowed-handle-published-layout` is removed: the only candidate (`KlHttpConn`) turns
out not to force its layout public (section 4), so it is `opaque`, and no type needs the category.

## 2. Central decision and how the evidence is weighed

Decision: retain `KlHttpServer`, `KlEventCtx`, and `KlEventLoop` as concrete caller-allocated value
objects (not opaque heap handles), because they are init-in-place, stack/embed-allocated, and an opaque
conversion would change the ownership/failure model and rewrite hundreds of call sites plus the
UEFI/lwIP integrations for no compatibility benefit under source-plus-static-relink. The decision is
made per FIELD (section 3).

Evidence discipline: in-tree occurrence counts are MIGRATION COST, not proof a field is public. They are
separated into four classes; only the public-example and living-guidance class is contract evidence:

| field | public examples + README/CLAUDE (contract) | integrations (seam) | tests (migration) | core src (migration) |
|---|---|---|---|---|
| KlHttpServer.ev | yes: `&server->ev` (thread_pool.c, async_thread_pool.c, async.c, README, CLAUDE) | 58 | 87 | 122 |
| KlHttpServer.bound_port | yes: `srv.bound_port` (redirect_client.c, proxy_client.c) | 9 | 128 | 7 |
| KlHttpServer.config/.pool/.listen_fd | none | 2/3/25 | 11/35/24 | 111/54/55 |
| KlEventCtx.loop | doc: "pump ev.loop" (README) + `kl_event_caps`/`kl_event_native_provider` take `KlEventLoop *` | 13 | 60 | 112 |
| KlHttpConn.res | yes: `&conn->res` (thread_pool.c, async_thread_pool.c) | 3 (not KlHttpConn: `conn->last_status` is a UEFI comment on a different struct) | 93 | 76 |
| KlHttpConn.stream | none (app) | 0 | 162 | 197 |
| KlHttpConn.req | none | 0 | 1 | 68 |

## 3. Field-level audit and per-field decisions

Each externally-referenced field is classified stable-public-facet / provider-integration-seam /
impl-only-despite-visibility / migrate-to-accessor-v3. Fields with no external reference are
visible-solely-to-permit-allocation.

KlHttpServer (caller-owned-value):
- `.ev` -> stable-public-facet: the documented way to reach the event context (`&server->ev`) for
  watchers and the thread pool. Bless it AND add `kl_http_server_event_ctx(KlHttpServer *)` as the
  preferred forward accessor; both remain valid.
- `.bound_port` -> migrate-to-accessor-v3: add `kl_http_server_bound_port(const KlHttpServer *)`; the
  two examples migrate; the field then becomes impl-only and may change.
- `.config`, `.pool`, `.listen_fd`, `.router`, accept/backend state -> provider/integration-seam or
  impl-only-despite-visibility (freestanding ports and core, never a public example).
- all other fields -> visible-solely-to-permit-allocation.

KlEventCtx (caller-owned-value):
- `.loop` -> migrate-to-accessor-v3. It is not merely a seam: `kl_event_caps(&ctx.loop)`,
  `kl_event_native_provider(&ctx.loop)`, and other lower-level APIs need a `KlEventLoop *`, and
  `kl_event_ctx_run` does not replace them. Add `kl_event_ctx_loop(KlEventCtx *)` and
  `kl_event_ctx_loop_const(const KlEventCtx *)` returning the embedded loop; migrate all public guidance
  (README "pump ev.loop" and any `&ctx.loop`) to the accessors. The concrete field is retained for
  storage and in-tree implementation, but no new source may depend on its name or placement after v3.
- `.sockets` -> provider/integration-seam (set from config; read by backends).
- all other fields -> visible-solely-to-permit-allocation.

KlEventLoop (caller-owned-value): after F2-D it is `{ _backend, alloc, ops }`, all impl-only; no public
facet; visible solely so `KlEventCtx` can embed it. Reached via `kl_event_ctx_loop()`.

KlHttpConn (RESOLVED to opaque; see section 4): the only app-facing field is `.res`, which
migrate-to-accessor-v3 via a new `kl_http_conn_response(KlHttpConn *)`; peer address is already served
by `kl_http_conn_peer_addr`. `.stream`/`.req`/all other fields are impl-only and move to the internal
definition. No application code, and no integration, dereferences any other `KlHttpConn` field.

## 4. Ownership rulings for the connection/watcher/timer family

- KlHttpConn: opaque (borrowed handle; storage owned by the server's connection pool, application
  receives a borrowed `KlHttpConn *` from `kl_http_request_conn`). RESOLVED to make the layout private,
  not merely described. Decisive fact: `KlHttpConnPool` holds `KlHttpConn *conns` (a pointer to the slot
  array), so the pool needs only a forward declaration, and the full definition need not be public. Plan:
  keep `typedef struct KlHttpConn KlHttpConn;` public; move the complete `struct KlHttpConn` definition
  and the non-accessor `kl_http_conn_*` seam functions to `src/protocols/http/http_conn_internal.h`;
  keep `kl_http_conn_peer_addr` and the new `kl_http_conn_response` public. This does NOT require the
  rejected pool-by-pointer refactor of `KlHttpServer`, because the pool is ALREADY a pointer, so that
  earlier objection does not justify keeping the layout. Migration cost: core protocol TUs that
  dereference connection fields are all under `src/protocols/{http,http2,websocket}/` (async.c,
  http_server.c, completion_http_server.c, http2_server.c, http_server_ws.c, http_client_pool.c) and
  include the internal header (near-zero); INTEGRATIONS dereference no `KlHttpConn` field (the one
  apparent hit, `conn->last_status` in a UEFI comment, is a different struct); the two public examples
  migrate `&conn->res` to `kl_http_conn_response()`. If a future integration needs a connection field,
  it goes through a narrow sanctioned accessor, not the raw layout.
- KlHttpConnPool: caller-owned-value (F2-B1). It is embedded by value inside the caller-owned
  `KlHttpServer` and holds `KlHttpConn *conns`; its fields are visible solely to permit the enclosing
  value layout. Its concreteness does not re-expose `KlHttpConn`.
- KlWatcher: opaque with a migration note (F2-B1). `kl_watcher_add` returns `int`; watchers are
  ctx-owned heap nodes; `KlEventCtx` holds only `KlWatcher *`, so the definition moves private with
  near-zero migration (one white-box test).
- KlTimerEntry: opaque with a migration note (F2-B1). `kl_timer_add` returns an `int64_t` id; callers
  never hold a `KlTimerEntry *`; `KlEventCtx` holds only a pointer; definition moves private.
- KlHttpClientPoolEntry: opaque with a migration note (F2-B1). `KlHttpClientPool` holds
  `KlHttpClientPoolEntry *entries` (a pointer), so the entry definition moves private.
- KlWsServerConn: opaque with a migration note (F2-B1). Applications receive a borrowed handle and
  operate through `kl_ws_server_*`; definition moves private.
- KlHttpMiddlewareEntry: opaque with a migration note (F2-B1). The router stores pointers and no public
  API returns the entry; definition moves private.
- KlHttpResponse: caller-owned-value (F2-B1). `kl_http_response_init(KlHttpResponse *res, ...)` accepts
  caller-owned storage.
- KlResolveReq: caller-constructed (F2-B1). Custom resolver providers embed and return it, so its base
  contract is a provider seam, not an implementation leak.
- KlHttpRoute: caller-inspectable (F2-B1). `kl_http_router_match()` returns a `KlHttpRoute **`, so
  retaining that public API makes the result layout part of the public contract.

## 5. Classification rules for the deferred fill

When `type_classification.tsv` is filled (after this taxonomy is accepted), rows are assigned by:

- kind `callback` -> api-signature; kind `alias` -> type-alias; kind `enum` -> enum-constants.
- kind `opaque`: the four detail-backed (`KlStream`, `KlListener`, `KlConnectOp`, `KlDatagram`) ->
  opt-in-unstable-layout; the other twelve -> opaque.
- kind `struct`:
  - configs, provider/callback vtables, hook containers, caller-filled currency -> caller-constructed;
  - library-filled result/identity structs -> caller-inspectable;
  - init-in-place caller-allocated runtime objects (`KlHttpServer`, `KlEventCtx`, `KlEventLoop`,
    `KlHttpRouter`, `KlHttpResponse`, `KlHttpSse`, `KlDrain`, `KlHttp1ChunkedDecoder`, `KlHttpBufReader`,
    `KlWsFrameParser`, `KlHttpCompressStream`, `KlDecompressStream`, `KlHttpClientPool`, ...) ->
    caller-owned-value;
  - concrete structs with a v3 decision to relocate their definition private -> opaque WITH a migration
    note (`KlHttpConn`, `KlWatcher`, `KlTimerEntry`, `KlHttpClientPoolEntry`, `KlWsServerConn`,
    `KlHttpMiddlewareEntry`); the checker rejects a concrete-def `opaque` that lacks a note.

`TYPE_CATEGORIES` in `tools/f2_public_inventory.sh` gains `caller-owned-value` and `enum-constants` in
the fill increment (not `borrowed-handle-published-layout`, which is removed). Borderline per-struct
calls are settled at fill time and are individually reviewable in the resulting TSV.

## 6. Revised policy for the retained caller-allocated value objects

`KlHttpServer`, `KlEventCtx`, and `KlEventLoop` remain concrete caller-allocated values, under this
field-level contract:

- Fields callers MAY use: `KlHttpServer.ev` (and `kl_http_server_event_ctx()`). The event context is
  reached as a `KlEventLoop *` via `kl_event_ctx_loop()`/`kl_event_ctx_loop_const()` and driven with
  `kl_event_ctx_run`/`kl_watcher_*`. Everything else is reached through functions.
- Fields visible SOLELY to permit allocation: all internal bookkeeping of the three structs; callers
  must not read or write them.
- Accessors that REPLACE currently documented direct access: `kl_http_server_bound_port()` replaces
  `srv.bound_port`; `kl_http_conn_response()` replaces `&conn->res`; `kl_event_ctx_loop()` /
  `kl_event_ctx_loop_const()` replace `&ctx.loop`; `kl_http_server_event_ctx()` complements `&server->ev`
  (the field stays a facet).
- Fields that MAY change under mandatory recompile: every non-facet field (append-only per F2-C);
  `bound_port`, `res`, and direct `.loop` access move behind their accessors.

## 7. What this decision changes, and what it does not

- No caller-allocated value object becomes an opaque handle; no init-in-place API changes shape.
- `KlHttpConn`'s layout goes private (definition relocated to `http_conn_internal.h`); its public
  surface is the forward-declared handle plus `kl_http_conn_peer_addr` and `kl_http_conn_response`. This
  needs no `KlHttpServer` pool-pointer refactor, because `KlHttpConnPool` already holds a pointer.
- `KlWatcher` and `KlTimerEntry` layouts move private (forward-declared).
- Field-level v3 breaks: `KlHttpServer.bound_port`, `KlHttpConn.res`, and direct `KlEventCtx.loop`
  access move behind accessors; `KlHttpServer.ev` stays a blessed facet.
- `http_connection.h` stays installed (per the freeze correction, no umbrella edit isolates it), now as
  a forward-declared handle plus accessors, with the concrete `KlHttpConnPool` shell.

## 8. Implementation follow-ups (deferred; NOT in this docs commit)

1. Add accessors: `kl_http_server_bound_port()`, `kl_http_conn_response()`, `kl_event_ctx_loop()` /
   `kl_event_ctx_loop_const()`, and `kl_http_server_event_ctx()`. Migrate the public examples and
   living guidance off direct field access (except `.ev`, which stays a facet). Additive, F2-C-safe.
2. Relocate the `struct KlHttpConn` definition and the non-accessor `kl_http_conn_*` seam functions to
   `src/protocols/http/http_conn_internal.h`; keep the forward-declared handle and the two accessors
   public. Audit and confirm no integration dereferences a connection field (currently none).
3. Make `KlWatcher` and `KlTimerEntry` opaque (forward-declare in the public header; layout private).
4. Fill `docs/f2/type_classification.tsv` per section 5 and add `caller-owned-value` and
   `enum-constants` to `TYPE_CATEGORIES`; `--check` must remain an exact join with valid values.
5. Add an internal-layout contract comment to each caller-owned-value header naming its facets and
   marking the rest visible-solely-for-allocation; update the README "pump ev.loop" wording to the
   `kl_event_ctx_loop`/`kl_event_ctx_run` idiom (F2-5).

F2-D's field removal now has its settled shape (KlEventLoop stays caller-owned-value inline). Both F2-B
and F2-D implementation remain held until this decision is accepted.

## 9. Validation

Docs-only. `git diff --check`; `make check-doc-refs`, `make check-old-layout`, `make
check-no-milestones`, `make check-no-em-dash`; pure ASCII; no build, header, test, integration, or
scaffold-data file changed. Nothing pushed; no remote CI. The scaffold stays undecided and neither F2-B
nor F2-D is implemented until this final reconciliation is accepted.
