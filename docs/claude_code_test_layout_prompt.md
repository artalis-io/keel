# Claude Code Prompt — Reconcile Tests with the Protocol Layout

Before continuing R2c, revise the accepted protocols/integrations restructure freeze to include test-layout reconciliation.

## Governing rule

Tests must follow the ownership of the implementation they exercise, without inventing a separate substrate directory that does not exist.

## Target layout

```text
src/                              # generic substrate
tests/                            # substrate and genuinely cross-layer tests

protocols/http/
tests/protocols/http/

protocols/http2/
tests/protocols/http2/

protocols/websocket/
tests/protocols/websocket/

protocols/dns/
tests/protocols/dns/

protocols/proxy_protocol/
tests/protocols/proxy_protocol/

integrations/<role>/<backend>/
integrations/<role>/<backend>/tests/
```

## Classification rules

1. Keep a test directly under `tests/` when its primary subject is generic substrate: event loops, streams, listeners, datagrams, sockets, sockaddr, timers, allocators, errors, URLs, TLS interfaces, thread pools, resolver interfaces, drains, file I/O, and cross-layer lifecycle primitives.

2. Move a test to `tests/protocols/<family>/` when its primary subject is a first-party protocol implementation:

   - HTTP/1 and shared HTTP orchestration → `tests/protocols/http/`
   - HTTP/2 → `tests/protocols/http2/`
   - WebSocket → `tests/protocols/websocket/`
   - DNS resolver/wire behavior → `tests/protocols/dns/`
   - PROXY protocol → `tests/protocols/proxy_protocol/`

3. Place a cross-module or end-to-end test with the protocol that owns its assertions and behavior. Do not create `tests/e2e/` merely because a test uses real sockets. Keep a test at `tests/` only when no protocol has clear ownership.

4. Move backend-specific tests and harnesses beside their integration under `integrations/<role>/<backend>/tests/`. This includes tests whose purpose is validating nghttp2, lwIP, EFI/UEFI, a TLS backend, miniz, or another optional provider/adapter.

5. Do not move a generic test merely because it is used by HTTP. Classify by the tested contract, not by incidental consumers or includes.

6. Apply the same per-file and per-test-case responsibility audit used for implementation files. If one test file mixes independently useful substrate and protocol cases, split it rather than assigning it by majority.

7. Preserve public headers flat under `include/keel/`. This task changes test and implementation locations, not the installed include taxonomy.

## Required freeze revision

Update `docs/protocols_restructure_freeze.md` before moving more code:

- Add an exhaustive current `tests/*.c` inventory.
- Classify every test as:
  - `tests/` substrate/cross-layer,
  - `tests/protocols/http/`,
  - `tests/protocols/http2/`,
  - `tests/protocols/websocket/`,
  - `tests/protocols/dns/`,
  - `tests/protocols/proxy_protocol/`,
  - or `integrations/<role>/<backend>/tests/`.
- Identify mixed test files that must be split.
- Record the exact old→new path map.
- Record all Makefile, CI, fuzz, benchmark, script, documentation, and gate references affected.
- Define how test discovery works recursively after the move.
- Define how executable names remain collision-free and stable where CI/scripts depend on them.
- Update clean rules for nested test objects and binaries.
- Extend permanent gates so protocol tests cannot drift back to the wrong home and stale old test paths cannot reappear.
- Preserve the existing validation matrix and backend-specific curated test sets.

## Increment sequencing

Do not amend or rewrite accepted R2a/R2b history.

1. Commit the freeze revision docs-only and pause for review.
2. After acceptance, continue R2c and move each protocol family's tests in the same increment as that family where practical.
3. Reconcile already-moved HTTP and HTTP/2 tests in dedicated reviewable test-layout increments before R4.
4. Move integration-specific tests during R3 with their owning integrations.
5. Finalize recursive discovery, ownership gates, stale-path gates, clean rules, and documentation in R4.

## Constraints

- Behavior-neutral file movement: do not weaken, skip, or temporarily disable tests.
- No compatibility copies or duplicate test sources.
- No second test implementation left at the old path.
- Keep every existing gate effective in the same commit that moves a test.
- Update white-box relative includes to the new implementation paths.
- Ensure macOS GNU Make 3.81 compatibility.
- Preserve all currently accepted protocol/substrate boundary decisions.
- Do not push.
- Do not begin R2c code movement until the revised freeze is reviewed and accepted.

Start now by producing and committing only the docs-only freeze revision. Then report the classification, mixed-test findings, exact move strategy, build/gate implications, and any genuine open decisions, and pause.
