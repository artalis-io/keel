# KEEL: Audit trail (historical evidence)

These are **append-only, dated evidence logs**, newest pass first. They record what was verified,
and what was still open, *at the date of each pass*. They are **historical: verify any specific
claim against current code before acting on it.** A finding described as a defect in an older pass
may have been fixed in a later one (or in code since). The living, current-state documents are
[architecture.md](../../architecture/overview.md) and
[architecture_invariants.md](../../architecture/invariants.md).

The audit files retain stable names within this archive (`docs/archive/audits/`):

| Audit | Scope | Latest pass |
|---|---|---|
| [keel_axis_audit.md](keel_axis_audit.md) | Networking three-axis separation (Transport / Engine / Provider); protocol independence; operation lifetime; compatibility matrix | Twelfth pass, 2026-08-17 (datagram Phase B `KlDatagram` facade) |
| [keel_audit.md](keel_audit.md) | C security/safety/quality (memory safety, overflow, resource management, build hardening) | Thirteenth pass, 2026-08-17 (datagram Phase B whole-tree) |

## How to read a pass

- Each pass is a dated `## Nth pass -- …` section; the newest is at the top of its file.
- A pass's **verdict** and **findings table** describe the state *on that date*.
- Severity labels (critical/high/medium/low/informational) are as-assessed then; consult the newest
  pass (or the code) for current status.
- The axis audit's **compatibility matrix** marks implemented / tested / runtime-proven per backend,
  it does not infer production-readiness merely from code presence.

## Relationship to the invariants

The [architecture invariants](../../architecture/invariants.md) are the *forward* contract: the rules
new work must preserve. These audits are the *backward* evidence: repeated verification that the
invariants held as the code grew. New audit passes are prepended to the relevant file above; new
invariants (or amendments, like the completion `retain_life` rule) are recorded in the invariants
document with their anchors.
