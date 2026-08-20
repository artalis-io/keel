# Claude Code prompt — transport and protocol taxonomy rename

```text
We are continuing work in the KEEL repository on branch
roadmap/r0-architecture-baseline.

Current state:
- M0–M5 of the KlUdp→KlDatagram consolidation are complete and accepted.
- M6.0a (additive KlDatagram prerequisites) is complete and accepted through fe11613:
  optional capabilities, queued-byte reporting, socket-default TOS, and receive-TOS capture/accessor.
- Stop before M6.0b. Subsequent discussion found that neither a KlUdp wrapper nor a KlUdpServer object
  has a unique state-machine role once KlDatagram has safe public socket construction conveniences.
- The earlier post-M5 modified-Option-B wrapper ruling must therefore be revisited in the new freeze; do
  not implement that wrapper plan merely because it appears in historical M6 documents.
- Nothing is pushed. Do not push or open a PR.
- Preserve the existing freeze-then-code, one-reviewed-increment-at-a-time workflow.
- Read and obey AGENTS.md before doing anything.

Objective: canonize KEEL’s public transport/protocol taxonomy so each public object represents a real
abstraction rather than a client/server usage label. This includes a clean HTTP rename and a reviewed
datagram-surface simplification; it must not introduce parallel TCP or UDP object families.

Target taxonomy

HTTP protocol family:
- KlServer → KlHttpServer
- KlClient → KlHttpClient
- KlRequest → KlHttpRequest
- KlResponse → KlHttpResponse
- Associated configs, callbacks, functions, files and documentation should use the `KlHttp*` /
  `kl_http_*` namespace when they are HTTP-specific.
- `KlHttp*` means the HTTP protocol family, not HTTP/1.1 specifically.
- Document the support that already exists: the server dispatches HTTP/1.x and HTTP/2. HTTP/2 enters
  through ALPN, h2c upgrade, or prior knowledge and is implemented by the existing
  `KlH2ServerSession` vtable (commonly backed by the nghttp2 integration). Both versions converge on
  the shared router, middleware and handler application model.
- The client architecture is currently asymmetric: `KlClient` is the HTTP/1.x client facade, while
  HTTP/2 uses the separate `KlH2Client` / `KlH2ClientSession` API. Inventory and freeze how those names
  fit beneath the new taxonomy; do not silently claim that merely renaming `KlClient` produces a
  unified negotiated HTTP/1.x+HTTP/2 client.
- Keep HTTP/1.x wire mechanics version-specific internally, using names such as `KlHttp1Connection`,
  `KlHttp1Parser` and `KlHttp1Writer` where those internal concepts already exist or are renamed.
- Preserve the existing HTTP/2 server hooks, completion paths, ALPN/h2c routing, session-vtable seam,
  shared application-model convergence and nghttp2 integration. This taxonomy increment renames them
  only where the classification requires it; it does not redesign the HTTP/2 engine.
- Do not claim that every current public struct field is automatically suitable for every HTTP version
  or for future HTTP/3.
- Inventory public HTTP/1-specific fields and contracts—keep-alive, chunked transfer, reason phrases,
  raw textual header/start-line storage, one-request-per-connection assumptions and pointer
  lifetimes—and document them honestly.

Canonical byte-stream transport — no KlTcp object family:
- Keep `KlStream`, `KlListener`, and `KlConnectOp` as the generic byte-stream/accept/connect machinery.
- Do NOT introduce `KlTcpServer`, `KlTcpClient`, or `KlTcpConnection`. They would duplicate the existing
  abstractions (`KlTcpConnection` would merely be KlStream; client/server would be compositions of
  KlConnectOp/KlListener + KlStream).
- TCP, Unix-domain, lwIP TCP, TLS-wrapped, or another reliable ordered stream is a provider/composition
  choice beneath KlStream, not a reason for a parallel public state machine.
- Audit whether these canonical primitives lack ergonomic public socket-preparation/connect/listen
  helpers. If a real usability gap exists, propose additive constructors/adapters around
  KlStream/KlListener/KlConnectOp; do not create another object hierarchy in this increment.
- HTTP owns parsing, routing, middleware, request/response semantics and HTTP keep-alive policy. Generic
  stream machinery owns byte movement, connect/accept, backpressure and close/detachment.

Canonical datagram transport — no KlUdp/KlUdpSocket/KlUdpServer object family:
- Keep `KlDatagram` as the single generic connected-or-unconnected message transport.
- Keep `KlDatagramBatch` as its optional caller-preallocated batch/GSO/GRO extension.
- Do NOT rename `KlUdp` to `KlUdpSocket`. Remove the `KlUdp` object API after the canonical KlDatagram
  convenience surface is complete and its tests/integrations have migrated.
- Do NOT retain `KlUdpServer` merely as a usage label. It has no distinct protocol or state machine: it
  prepares/binds a socket, starts KlDatagram receive delivery, remembers peer/local addresses for a
  source-pinned reply, and maps batching/GRO/multicast/TOS configuration. KlDatagram already carries the
  underlying semantics. Remove KlUdpServer after equivalent direct-KlDatagram usage is tested.
- Add a safe PUBLIC one-shot socket-backed KlDatagram initializer/configuration convenience. It must
  create/configure/bind/adopt with exact ownership and failure cleanup, using the accepted M0 preparation
  seam internally. Callers must not need a private helper or manually manufacture an fd for the common
  UDP case.
- Add provider-neutral KlDatagram connect convenience. Capability support and actual connected state
  remain distinct; peerless send is valid only after successful connect.
- Preserve source-pinned send/reply, TOS, multicast, BOTH backpressure, batching, GSO, GRO, pause/resume,
  and confirmed teardown directly on KlDatagram/M5. No hot-path allocation or parallel data plane.
- Retain the socket-option configuration semantics currently carried by `KlUdpConfig`, but rename the
  type to a datagram-neutral name such as `KlDatagramSocketConfig` after inventorying every provider and
  preparation-seam reference.
- DNS already consumes KlDatagram and remains so. Migrate all KlUdp/KlUdpServer tests, smokes,
  integrations and examples to direct KlDatagram use or small TEST-ONLY helpers. Do not replace a public
  wrapper with a hidden production wrapper.
- The accepted M6.0a additions remain valuable and must not be reverted.

Required process

First produce and commit a docs-only inventory/design freeze. Do not write rename code until I review
and accept the freeze.

The freeze must contain:

1. Complete public rename map

Inventory all affected public symbols, not merely the four headline types:
- structs and typedefs
- config types
- callbacks
- function names
- enum/constants when their names are abstraction-specific
- public headers and umbrella includes
- examples
- tests, smokes, integrations and fuzz targets
- documentation and comments
- Makefile/CI target names where user-facing
- pkg-config or installed-header references if any

At minimum, cover:
- KlServer and kl_server_*
- KlClient and kl_client_*
- KlRequest and kl_request_*
- KlResponse and kl_response_*
- KlConfig if it is the HTTP server configuration
- KlUdp and kl_udp_*
- KlUdpServer and kl_udp_server_*
- KlUdpConfig
- KlDatagram construction/connect gaps that currently make the obsolete wrappers convenient
- callbacks whose signatures contain renamed public types

2. Satellite-type classification

Audit related surfaces and classify each as:
- HTTP-specific: rename to KlHttp*/kl_http_*
- genuinely generic: keep unchanged
- HTTP/1.x internal: rename internally to KlHttp1*
- deferred or ambiguous: explain and freeze a ruling

Explicitly inspect:
- router and route types/functions
- middleware
- body readers
- headers/cookies
- multipart/form parsing
- WebSocket upgrade/connection APIs
- TLS configuration
- async request handling
- HTTP client response/request types
- proxy and connection-pool APIs
- listener/connect/event/socket primitives

Do not mechanically rename generic transport/event/provider types merely because HTTP currently
consumes them.

3. File/module map

Propose exact file renames, including headers and sources. For example:
- include/keel/server.h → include/keel/http_server.h
- src/server.c → src/http_server.c
- equivalent client/request/response files where applicable
- removal/migration of include/keel/udp.h, include/keel/udp_server.h, src/udp.c, and src/udp_server.c
- the exact public home for `KlDatagramSocketConfig` and the one-shot KlDatagram socket initializer

Reconcile the umbrella `include/keel/keel.h`, Makefile source manifests, freestanding manifests, CI
scripts and documentation references.

4. Compatibility ruling

This project is pre-1.0 and ABI breaks have been accepted through M5. Prefer a clean rename:
- no permanent typedef aliases
- no duplicate old/new function symbols
- no compatibility macro layer
- no deprecated forwarding headers unless a concrete build/bootstrap reason requires a temporary one

If a temporary compatibility device is necessary to keep a multi-commit rename buildable, specify
exactly when it is introduced and removed. The final state must expose only the new taxonomy.

5. Behavioral boundary

This is a taxonomy + canonical-surface effort:
- no unintended runtime behavior change; deliberate removal of redundant pre-1.0 KlUdp/KlUdpServer
  surfaces is allowed only after direct KlDatagram replacements and migrations are validated
- no state-machine redesign
- no new parallel transport state machine
- no KlUdpSocket wrapper
- no KlTcp object family
- no new HTTP/2 implementation or redesign, and no HTTP/3 implementation
- no ABI compatibility promise

The final public layouts may change only as mechanically required by renamed embedded types; do not
otherwise redesign them.

6. HTTP version statement

Freeze wording equivalent to:

“`KlHttpServer` is an HTTP-family server supporting HTTP/1.x and HTTP/2 through version-specific
protocol engines. HTTP/2 is selected through ALPN, h2c upgrade, or prior knowledge and is supplied
through the existing `KlH2ServerSession` vtable, including integrations such as nghttp2. HTTP/1.x and
HTTP/2 converge on the shared router, middleware and handler application model. The existing
`KlClient` is the HTTP/1.x client facade, while HTTP/2 currently uses the separate `KlH2Client` and
`KlH2ClientSession` surface; their relationship under the `KlHttp*` taxonomy must be classified rather
than assumed unified. HTTP/1.x parsing and serialization remain version-specific internally. The
`KlHttp` naming does not assert that every current public field or lifecycle contract is directly
reusable across all versions or for HTTP/3.”

Identify any public fields that obstruct version neutrality, while distinguishing genuine existing
HTTP/2 convergence from HTTP/1.x-only connection details. Defer redesign unless a rename is impossible
without resolving one.

7. Increment plan

Split implementation into reviewable, always-buildable commits. Recommended shape:

T1 — docs-only taxonomy freeze
D1 — additive public KlDatagram socket-construction + connect conveniences; old UDP objects untouched
D2 — migrate KlUdp and KlUdpServer tests/smokes/integrations/examples to direct KlDatagram
D3 — remove KlUdp and KlUdpServer objects plus obsolete implementation/layout headers; rename
     KlUdpConfig to the frozen datagram-neutral socket-config name
T2 — HTTP public type/function/header rename
T3 — HTTP satellite modules, internal HTTP/1 naming, examples/tests/docs
T4 — final old-name removal, stale-reference gate and documentation reconciliation

Adjust the split if the dependency graph requires it, but every commit must build and be independently
reviewable. Avoid one enormous rename commit if a smaller coherent sequence is possible.

8. Mechanical enforcement

Design a permanent gate that rejects reintroduction of obsolete public names after the migration. It
should be narrow enough not to reject historical migration documents that intentionally mention old
names; use an explicit documentation allowlist if necessary.

Validation required for each code increment:
- full default suite
- debug-test under ASan/UBSan
- relevant pollcomp and io_uring suites
- MinGW cross-compile
- EFI/lwIP/freestanding gates
- cppcheck
- check-tier1-boundary
- check-sockaddr-neutral
- check-doc-refs
- git diff --check
- symbol/reference scan proving no unintended old public names remain

Review standards:
- Preserve all existing behavior.
- Preserve allocator discipline.
- No hot-path allocation introduced.
- Check all ownership/lifetime callbacks after type renames.
- Ensure function-pointer signatures and provider vtables remain type-correct.
- Ensure installed headers are self-contained after file renames.
- Ensure examples, tests and integrations consume the public names rather than private compatibility
  aliases.
- Prove that removing KlUdp/KlUdpServer loses no unique feature or backend coverage: source-pin, TOS,
  multicast, connected send, queue/backpressure, batch, GSO/GRO, readiness/completion, lwIP and EFI must
  remain covered through KlDatagram.
- Do not introduce a production "helper" that recreates KlUdpServer under a private name.
- No Co-Authored-By trailers.
- Do not push.

Start now with T1 only: inspect the complete tree, write the docs-only taxonomy inventory/design freeze,
commit it locally, report the rename/classification tables and any genuine open decisions, then pause
for review. Do not begin T2.
```
