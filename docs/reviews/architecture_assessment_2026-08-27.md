# Keel Architecture & Engineering Assessment

**Date:** 2026-08-27
**Reviewer:** external (independent assessment)
**Scope:** architectural and engineering clarity; separation of concerns across the socket axis,
event axis (completion vs. readiness), protocols, and platforms.
**Status:** review record. This is an external opinion, not an official audit. Claims about code
should be re-verified against current state before acting (code is authoritative; docs may drift).

---

## Verdict summary

| Dimension | Grade | Basis |
|---|---|---|
| Architecture / separation of concerns | **A-** | Real, mechanically-enforced three-axis split; honest event models; model-blind protocol core |
| Engineering clarity | **A- to B+** | Code is clear and defensive; doc corpus is large and has already begun to drift from code |

**One-line takeaway:** an excellent, genuinely enforceable three-axis decomposition with a heavy,
partially-stale documentation corpus, and a real, confirmed instance of documentation drift.

---

## Strengths

### 1. The axis separation is real, not decorative

Three orthogonal, independently replaceable axes:

- **Event axis**: readiness (`event.h`: epoll/kqueue/WSAPoll/poll) vs. completion
  (`completion.h`: io_uring/IOCP/pollcomp).
- **Socket/provider axis**: `KlSocketProvider` vtable + pointer-width `KlSocketHandle` +
  `KL_SOCK_CAP_*` (POSIX/Winsock built in; lwIP/EFI as `integrations/`).
- **Protocol axis**: HTTP/1.1, HTTP/2, WebSocket, SSE, DNS, TLS-above-stream; written once against
  the Tier-1 transports `KlStream` / `KlDatagram` / `KlListener`.

Enforcement is **default-deny and self-canaried** via make gates. Verified passing on this date:

```bash
make check-tier1-boundary          # OK (54 allowlisted infra TUs)
make check-sockaddr-neutral        # OK (16 protocol TUs are KlSockAddr-only)
make check-substrate-purity        # OK (G1)
make check-protocol-no-integration # OK (G2)
make check-integration-seam        # OK (G3)
```

The default-deny rule (every `src/` + `src/protocols/` TU is governed unless allowlisted in
`TIER1_INFRA`) means a newly added protocol file is covered automatically.

### 2. Completion/readiness duality is handled honestly

- Production backends keep their **native** model: no epoll-as-completion or IOCP-as-readiness
  emulation.
- `pollcomp` is **explicitly scoped as a test double** (completion contract over `poll()`), confined
  to CI so the completion driver runs under ASan on any POSIX host; never production.
- The protocol core is genuinely **model-blind**: the completion driver sees `KlStream`, never an
  HTTP type (`src/completion.h` is protocol-neutral); the HTTP adapter (`completion_http.h`) is a
  thin layer that holds all TLS/PROXY/state knowledge on the HTTP side.
- `http_proto_hooks` per-protocol hook tables let the HTTP/1.1 core dispatch into WebSocket/HTTP-2
  without naming them directly and without `#ifdef`; each protocol stays independently linkable.

### 3. The invariants document is a model of what architecture should mean

`docs/architecture/invariants.md` states each invariant once with a *why*, an *anchor* (header
contract, source seam, or executable gate), and an *enforcement* mechanism. It names the genuinely
hard problems:

- **I3** logical close ≠ physical retirement
- **I4** callback reentrancy / synchronous completion
- **I5** completion-batch target lifetime (single-shot + life tokens + `retain_life`)
- **I8** allocation-free hot paths
- **I9** quarantine on uncertain retirement (EFI)
- **I10** protocols depend downward only through Tier-1 transports
- **I11** substrate / protocol / integration ownership (G1–G5 gates)

### 4. Frontier-provider work validates the thesis empirically

The *same* `KlHttpClient` runs unchanged over io_uring, IOCP, pollcomp, lwIP raw (no OS sockets),
and EFI tokens (before any OS). This is the strongest possible evidence the axis separation holds;
it cannot be faked with documentation.

### 5. Engineering discipline

- Overflow guards (`SIZE_MAX/2`), bounds checks at system boundaries.
- Allocator discipline: all allocation through `KlAllocator`.
- Pre-allocated connection pool; no per-request `malloc`.
- ASan/UBSan debug builds; Clang static analyzer; cppcheck; libFuzzer on the parser/multipart.
- Append-only 13-pass axis audit trail (`docs/archive/audits/keel_axis_audit.md`).
- `init`/`run` split makes `pledge`/`unveil` sandboxing natural.
- Code read (e.g. `src/protocols/http/http_connection.c`) is careful, defensive, well-commented.

---

## Weaknesses / fair critique

### 1. Documentation drift is already occurring (confirmed)

Concrete contradiction between two load-bearing docs:

- `README.md` line ~805: states the io_uring backend is **completion-native**, and the earlier
  `IORING_OP_POLL_ADD` **readiness** adapter was **retired** after benchmarks showed it ~2–2.3×
  slower.
- `docs/architecture/overview.md` line ~148: still lists io_uring as
  `IORING_OP_POLL_ADD` (readiness, not async I/O).

`make check-doc-refs` verifies **file links only, not claims**, so this passes CI. Given the size of
the doc corpus this will recur.

### 2. Documentation-to-code ratio is extreme

~41K LOC of C against a very large docs tree (dozens of archived designs, phases, freezes, audits,
contracts). Much is valuable, but a large share is *process record* rather than reference. This is
intimidating for newcomers and raises a maintainability question: design-first ceremony of this
scale is hard to sustain and tends to outlive the code it describes (see finding #1).

### 3. The common path carries complexity for the exotic backends

The `retain_life` / quarantine / life-token machinery, single-shot completion guarantees, R3
watcher-ABA remediation, and batch-bracketed deferred reclamation are extremely subtle. Justified
by the completion + UEFI axes and well-tested, but they load genuine complexity onto every build to
serve a niche use case (UEFI firmware before an OS). Question for long-term maintainers: is this
proportionate for the mainstream epoll/kqueue user, and will it stay maintainable when the original
authors aren't reading `completion.h`?

### 4. Honest maturity caveat

The README's own comparison concedes **Maturity: New (2025–2026)** vs. Mongoose (20+ years,
NASA/Siemens) and libmicrohttpd (18+ years, systemd). The architecture is more sophisticated, but
sophistication ≠ battle-testing. The "101K req/s" is a single-benchmark claim (Apple M1, kqueue),
not a cross-platform guarantee. The README honestly notes the C security posture is "not a
language-level guarantee."

### 5. Naming / vocabulary overhead

Several parallel vocabularies (readiness/completion, engine/provider, transport/Tier-1,
`kl_comp_*` / `kl_comp_*_raw` / `kl_http_comp_*`) take effort to map onto each other. The
public-vs-internal `socket.h` split is well-reasoned but adds cognitive load.

---

## On changing the grades if the drift is addressed

**Architecture (A-):** essentially unchanged by doc fixes. The A- rests on the *code*: the
executable gates, honest event models, model-blind core, Tier-1 contracts, EFI/lwIP proof, none of
which change if `overview.md` is corrected. The drift was a blemish within an A-level architecture,
not the reason it wasn't an A.

**Engineering clarity (A- to B+):** moves only if the fix is **systemic**, not cosmetic.

- A **one-off edit** to the stale line → grade does **not** change. The drift is evidence of a
  deeper risk (large corpus will keep drifting, process burden may not stay honest), and one line
  fixes none of that.
- A **systemic fix** → e.g. extending `check-doc-refs` from *link-level* to *claim-level*
  verification of load-bearing invariants, or retiring/trimming stale archive docs → engineering
  clarity firms toward a solid **A-**, because the maintainability concern is directly addressed.
  The tooling culture makes this very feasible (doc *references* are already gated).

**Residual ceiling:** even perfect docs do not reach a flat A, because two concerns are
doc-independent:

1. The completion-lifetime machinery is intrinsically subtle complexity loaded onto every build to
   serve exotic backends.
2. The maturity gap (new project vs. decades of field use) is not fixable in documentation.

---

## Recommended highest-value follow-up

1. Make `docs/architecture/overview.md`'s io_uring claim current (it contradicts the README).
2. Add a **claim-level** doc-vs-code check (or retire stale archive docs) so the load-bearing
   invariants are verified against code, not just file links. This is the one place the current
   rigor has a real hole.

---

## Reference

- `docs/architecture/overview.md` - three-axis model + HTTP-server internals
- `docs/architecture/invariants.md` - enforceable invariants I1–I11
- `docs/archive/audits/keel_axis_audit.md` - append-only axis audit trail (13 passes)
- `src/completion.h` / `src/completion_io.h` - protocol-neutral completion axis
- `include/keel/event.h` / `include/keel/socket.h` - readiness + provider seams
- `src/protocols/http/completion_http.h` - HTTP adapter over the neutral completion axis
- `src/protocols/http/http_proto_hooks.h` - ws/h2 decoupling seam
- `include/keel/stream.h` - Tier-1 raw-transport contract
