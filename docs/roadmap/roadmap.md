# Keel Refactor and Improvement Roadmap

Status: canonical forward roadmap.

This document replaces the overlapping restructure and improvement-roadmap numbering. It describes
the remaining work from the current repository state. Completed implementation history belongs in Git
and, when it retains architectural value, under `docs/archive/`.

## Governing principles

1. Generic transport and execution machinery belongs to the substrate.
2. Protocol-specific code belongs to `src/protocols/<family>/`.
3. Optional providers and adapters belong to `integrations/<role>/<backend>/`.
4. Tests follow the component that owns their assertions and dependencies.
5. Public headers remain flat under `include/keel/` unless a separate public-API review rules otherwise.
6. Readiness and completion are execution axes, not filesystem ownership categories.
7. Active comments explain current invariants, ownership, and behavior, not implementation history.
8. Documents and comments use project punctuation conventions, including no Unicode em dash.
9. Every structural change lands with its build, CI, cleanup, documentation, and enforcement updates.
10. Each consequential phase starts with a docs-only inventory and design freeze.

## Phase S: Finish structural ownership

### S1 through S3.5, and F1: completed work

The following work is complete and CI-protected; treat it as history rather than as future sequencing:

- The HTTP public taxonomy uses `KlHttp*` and `kl_http_*` names.
- `KlUdp` and `KlUdpServer` were consolidated into `KlDatagram` and removed.
- Built-in protocols live under `src/protocols/<family>/`.
- Protocol tests live under `tests/protocols/<family>/`.
- Codec, TLS, HTTP/2, lwIP, and UEFI integrations are grouped by role under `integrations/<role>/<backend>/`.
- Integration-owned tests live under each backend's `tests/` directory.
- The UEFI integration lives under `integrations/platform/uefi/` with production separated from test
  machinery; the live EFI ABI headers are promoted into the backend and `spikes/` is removed (former S3.5).
- The freestanding client, server, datagram, and DNS compositions link and run under CI; the
  `kl_dgram_life_*` composition gap is resolved (former F1).

Do not preserve this chronology in active source comments. The final architecture documentation should
describe the resulting system directly.

### S4: structural enforcement and reconciliation

Finalize and wire permanent checks into CI:

- `check-substrate-purity`
- `check-protocol-no-integration`
- `check-integration-seam`
- `check-protocol-home`
- `check-test-layout`
- `check-old-layout`
- `check-no-kludp`
- `check-no-httplegacy`

The old-layout check must reject resurrection of:

- Top-level `protocols/`.
- Old flat integration homes.
- `spikes/`.
- Deleted source, header, and test paths.
- Integration tests outside their backend's `tests/` directory.
- Protocol tests at the root of `tests/`.

Reconcile all active build manifests, CI labels, clean rules, contributor instructions, and living path
references in the same phase.

## Phase C: Consolidate documentation and repository hygiene

### C1 through C5: completed work

Phase C is complete and CI-protected; treat it as history rather than as future sequencing. The
consolidation ran before any further broad source-layout change, so later audits no longer inherit
stale documentation.

- C1: one documentation taxonomy under `docs/` (architecture / contracts / operations / roadmap /
  archive); every legacy document classified as living, archived, or deleted.
- C2: implementation-history narration removed from active `.c` and `.h` comments, enforced by the
  `check-no-milestones` gate.
- C3: the Unicode em dash eliminated across all tracked files, enforced by the `check-no-em-dash` gate.
- C4: `AGENTS.md`, `CLAUDE.md`, `CONTRIBUTING.md`, and the audit skills reconciled to current names,
  paths, and ownership, with volatile suite and module counts removed.
- C5: `site/` rebuilt around the transport-substrate architecture (offline, with no runtime
  third-party dependencies), leading with `KlEventCtx` / `KlStream` / `KlListener` / `KlDatagram`, an
  evidence-based backend coverage table, and an offline `check-site` gate.

The `check-no-milestones`, `check-no-em-dash`, and `check-site` gates run together in the Static
Analysis CI job. The subsection requirements below are retained as the delivered scope of record.

### C1: establish one documentation taxonomy

Target structure:

```text
docs/
  README.md

  architecture/
    overview.md
    transport_substrate.md
    event_axis.md
    protocol_ownership.md
    integration_model.md
    public_api.md

  contracts/
    stream.md
    datagram.md
    listener.md
    async_lifecycle.md
    streaming.md
    compatibility.md

  operations/
    capability_matrix.md
    backend_confidence.md
    testing.md
    fuzzing.md

  roadmap/
    roadmap.md

  archive/
    audits/
    designs/
    phases/
    freezes/
```

Classify every existing document as exactly one of:

- Living specification: update it and place it outside `archive/`.
- Historically useful record: move it under the appropriate `docs/archive/` category.
- Fully superseded with no independent value: delete it.
- Temporary prompt or planning aid: delete it once the accepted freeze captures its decisions.

Do not retain a document merely because it exists. Do not keep multiple active documents claiming to
be the canonical architecture, compatibility policy, capability matrix, or roadmap.

The project-restructure prompt should not remain a parallel source of truth. Delete it after the final
freeze captures its applicable contracts, or archive it only if it has genuine historical value.

### C2: remove implementation-history narration from active code

Sweep every active `.c` and `.h` file. Remove or rewrite comments containing:

- Milestone names such as `M5.3`, `R2f`, or `Phase 7B`.
- References to deleted files or former repository layouts.
- Statements such as "formerly", "moved from", or "the old path" when they only record chronology.
- Review notes, migration explanations, and superseded implementation alternatives.

Comments must describe the current invariant instead:

```c
/* Bad: moved from udp.c during M5.2b. */
/* Good: the final segment retires the caller-owned GSO group. */
```

Add a focused gate for known milestone-marker families in active source and headers. Allow an exception
only where a numbered phase is part of an external protocol or standard.

### C3: eliminate Unicode em dash

Replace the Unicode em dash character throughout:

- C source and headers.
- Markdown and HTML.
- Makefiles and shell scripts.
- YAML and other living configuration.
- Archived as well as living documentation.

Use a colon, semicolon, comma, parentheses, or an ASCII hyphen as appropriate. Add a permanent gate:

```sh
rg -nP '\x{2014}' \
  --glob '*.c' --glob '*.h' --glob '*.md' --glob '*.html' \
  --glob 'Makefile*' --glob '*.sh' --glob '*.yml' --glob '*.yaml'
```

The check must fail on any match. Do not maintain contextual exceptions.

### C4: rewrite agent and contributor guidance

Reconcile `AGENTS.md`, `CLAUDE.md`, `CONTRIBUTING.md`, and related instructions:

- Use `KlHttpServer`, `KlHttpRequest`, `KlHttpResponse`, and current function names.
- Describe `src/protocols/`, role-grouped integrations, and test-placement rules.
- Document the flat public-header policy.
- Document platform and event-provider ownership.
- Remove obsolete API examples and paths.
- Avoid volatile hard-coded suite or module counts where possible.
- State that active comments describe present behavior, not migration history.
- State the no-em-dash rule.

### C5: revamp `site/`

Rebuild the site around the current architecture rather than mechanically replacing old names.

Required content:

1. Keel as a transport substrate.
2. Core abstractions: `KlStream`, `KlDatagram`, `KlListener`, and `KlEventCtx`.
3. Built-in application protocols: HTTP/1, HTTP/2, WebSocket, DNS, and PROXY protocol.
4. Optional integrations grouped by TLS, HTTP/2, codecs, and platforms.
5. Readiness and completion as orthogonal execution models.
6. An evidence-based platform and backend capability matrix.
7. Current public-API examples using only valid names.
8. Links to canonical living documentation.
9. Build, sanitizer, fuzz, conformance, and stress evidence.

Site acceptance requirements:

- No obsolete API names, paths, or milestone narration.
- No unsupported marketing claims.
- Responsive and accessible markup.
- Updated navigation, metadata, Open Graph material, and diagrams.
- A CI-checked `site/` build.
- Automated internal-link and referenced-file validation.

## Phase F: Fix known correctness and composition debt

(F1, freestanding composition repair, is complete and recorded in the completed-work summary above.)

### F2: audit public headers and installation (complete)

Status: COMPLETE. The public surface and staged install are now intentional, manifest-driven, and
gate-enforced in standing CI. The frozen plan and decision records are archived under
`docs/archive/f2/`; the live, machine-consumed inventories and classification manifests stay under
`docs/f2/` (regenerated and checked by `tools/f2_public_inventory.sh`). Six standing gates enforce the
outcome: `check-public-headers`, `check-public-coverage`, `check-allocator-boundaries`,
`check-no-eventloop-fd`, `check-install`, `check-installed-consumer`.

`include/keel/` stays flat for consumers; the phase verified and now continuously enforces:

- Only public substrate and built-in protocol headers are installed (manifest-only install).
- Integration adapter headers remain with their integrations.
- Detail headers are intentionally public (opt-in `*_detail.h`) or removed from installation.
- `keel.h` exposes exactly the intended umbrella surface.
- Every public function is classified with concrete evidence (default-deny coverage manifest).
- Examples and an out-of-tree consumer compile and link with installed-style includes via pkg-config.
- Public comments use current taxonomy and contain no implementation history.

### F3: audit dead code and clutter

Audit and remove:

- Unreferenced internal functions and headers.
- Obsolete Makefile variables and cleanup tokens.
- Dead CI jobs and scripts.
- Generated artifacts accidentally tracked.
- Duplicate tests without distinct coverage.
- Compatibility shims for deleted pre-1.0 APIs.
- Empty directories and stale ignore rules.
- Superseded site assets and documentation.

Before deleting an item, perform a reference sweep, build-manifest sweep, CI/script sweep, public-symbol
check, and relevant platform or integration build.

## Phase P: Review platform-specific substrate placement

Start with a docs-only inventory. This phase is not authorization for an immediate bulk move.

### P1: classify platform-owned translation units

Candidate end state:

```text
src/platforms/posix/
  platform_posix.c
  socket_posix.c
  socket_dgram_posix.c
  event_epoll.c
  event_kqueue.c
  event_poll.c
  event_iouring.c
  event_pollcomp.c

src/platforms/windows/
  platform_win.c
  socket_winsock.c
  socket_dgram_win.c
  event_wsapoll.c
  event_iocp.c
```

Classify per function rather than by the majority role of a file. Split a mixed translation unit when
generic and platform-specific functions coexist.

Keep neutral substrate flat, including event-context orchestration, neutral dispatch, completion
coordination, stream and datagram machines, listener machinery, and provider-neutral seams.

### P2: keep readiness and completion as axes

Do not create `src/readiness/` or `src/completion/`. These execution models cross platforms and should
be expressed through vtables, capabilities, neutral dispatch, conformance tests, and architecture
documentation.

### P3: move only clearly platform-owned tests

Keep backend-neutral semantic tests flat. If the inventory proves clear ownership, platform-specific
tests may move to:

```text
tests/platforms/posix/
tests/platforms/windows/
```

Do not classify a test as platform-owned merely because current CI runs it on one operating system.

## Phase E: Expand evidence and hardening

This phase carries forward the useful outstanding work from the former R5 and R6 roadmap items.

### E1: build a unified transport conformance harness

Run the same semantic assertions for `KlStream`, `KlDatagram`, and `KlListener` across supported
providers and engines:

- epoll
- kqueue
- poll
- WSAPoll
- pollcomp
- io_uring
- IOCP
- lwIP
- EFI

Capabilities must produce explicit skips. Backends must not maintain divergent copies of semantic
assertions.

Required scenario families:

- Bounded backpressure and ordered delivery.
- Pause and resume with retained input.
- Synchronous and reentrant completion.
- Graceful and abortive close.
- Callback-triggered teardown.
- Physical retirement before reuse.
- Exact resource release.
- Datagram boundaries, truncation, and metadata.
- Listener credit reservation and transfer.

### E2: run a production-confidence campaign

Add scheduled or manually dispatched stress jobs for:

- io_uring cancellation, descriptor reuse, queue pressure, and fallback behavior.
- IOCP send/receive asymmetry, AcceptEx windows, close, and retirement.
- Real TLS over IOCP.
- pollcomp randomized completion ordering.
- lwIP raw lifecycle stress.
- EFI host-mock and QEMU lifecycle repetition.

Every timeout must report a reproducible seed, backend capabilities, OS and kernel version,
outstanding operations, and ownership/lifecycle state. Do not convert hangs into silent abandonment.

### E3: publish a backend confidence matrix

Use evidence-based labels:

- Architecturally supported.
- Compile-gated.
- Mock-validated.
- Runtime-validated.
- Sanitizer-validated.
- Conformance-tested.
- Stress-tested.
- Production-proven.

README and site claims must use the same categories.

## Phase A: Optional ABI modernization

This phase carries forward the former R7 item as a separately reviewed compatibility decision.

### A1: audit `KlEventLoop.fd`

Determine:

- Every internal reader and writer.
- Whether external consumers access the public field.
- Stack-allocation and embedding requirements.
- Source and binary compatibility impact.
- Freestanding consequences.
- Whether all backend state can live behind `_backend`.

### A2: remove or reserve the field

Preferred pre-1.0 result:

```c
typedef struct KlEventLoop {
    void             *backend;
    const KlEventOps *ops;
    KlAllocator      *alloc;
} KlEventLoop;
```

Move epoll and kqueue descriptors into backend-owned state.

If removal is rejected, mark the field reserved and legacy, ban new consumers, document why it
remains, and gate access to the exact approved backend translation units.

Do not mix this ABI change with filesystem moves.

## Phase B: Add breadth only after consolidation

Candidate projects, each separately designed and frozen:

- Explicit mode-B datagram receive batching.
- Native completion-backend batched receive.
- mDNS or CoAP as bounded-message consumers.
- QUIC experiments over `KlDatagram`, without treating QUIC as merely UDP.
- Additional providers only when they exercise a genuinely new execution or ownership model.

No new feature may push protocol-specific state into the neutral substrate.

## Recommended execution order

```text
S4    structural gates and reconciliation                          [done]
C1-C5 documentation, comments, punctuation, guidance, and site     [done]
F2    public-surface audit and gate enforcement                    [done]
F3    dead-code and clutter audit                                  (next)
P1    platform-layout inventory
P2-P3 optional platform moves and platform-owned tests
E1    unified conformance harness
E2-E3 stress campaign and confidence publication
A1-A2 optional KlEventLoop.fd modernization
B     independently scoped new capabilities
```

## Definition of done

This roadmap is complete when:

- Repository layout communicates real ownership without relying on historical knowledge.
- Active source and header comments explain only current behavior and invariants.
- Living documentation has one canonical home per subject.
- Superseded material is either deleted or clearly archived.
- No Unicode em dash remains in governed files.
- The site accurately presents the current architecture and verified backend confidence.
- Freestanding compositions link and run under CI.
- Public headers and installation contents match the intended API.
- Platform-specific code is isolated only where ownership is unambiguous.
- One semantic harness validates transports across all supported execution models.
- Optional ABI modernization and new protocol breadth remain separately authorized decisions.
