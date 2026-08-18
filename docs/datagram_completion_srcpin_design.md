# Datagram framework increment — native completion source-pin / TOS — design freeze

Status: **PROPOSED (docs-only)** — no code until reviewed and accepted, per the consolidation workflow.
A **prerequisite framework increment** extracted from M4 at review request: the KlDatagram COMPLETION
send path must carry the source-pin (`msg.local`) and per-packet TOS the frozen §2.5.1 seam already
passes it, natively — with NO hot-path allocation — across io_uring, pollcomp, and IOCP. M4
(`docs/datagram_m4_udpserver_design.md`) lands atop this: its wildcard source-pinned reply is correct
on readiness today and becomes correct on completion backends once this increment lands.

## 0. Problem

`KlDatagram`'s completion send adapter (`dg_comp_submit`) posts an overlapped op carrying `dest`, `src`
(source-pin) and `tos` (frozen §2.5.1 copy-at-submit contract). But the three completion backends
**marshal only `dest`** and silently drop `src`/`tos`:
- io_uring: `iou_comp_post_dgram_send` → `io_uring_prep_sendmsg` with `msg_name` only (`event_iouring.c`).
- pollcomp: `pc_comp_post_dgram_send` → `sendto` at drain (`event_pollcomp.c:591`).
- IOCP: `iocp_comp_post_dgram_send` → overlapped `WSASendTo` (`event_iocp.c:557`).

And the facade's completion recv arm posts `capture = 0` (`dg_comp_arm`), so the completion recv never
captures the pktinfo local (dest) address either. Net: source-pin is unusable on completion backends
(the readiness path always worked — its synchronous provider send emits the cmsg and its recv parses
it). Two withdrawn non-solutions and why:
- a **synchronous provider send** in `dg_comp_submit` for source-pin/TOS (KlUdp's `udp_send_common`
  trick): can WOULD_BLOCK with no in-flight op, and completion mode has no readiness edge → strands.
- a **generic WRITE watcher** to retry: allocates a `KlWatcher` in the send hot path (I8 violation) and,
  on IOCP, `kl_watcher_add(WRITE)` is not a writable watch (it posts a persistent `WSARecv`, waits for
  readability, and can consume an inbound datagram). Rejected.

**The native solution:** the overlapped send itself carries the control message (`sendmsg`/`WSASendMsg`
with a pktinfo + TOS cmsg). Overlapped sends complete asynchronously — they never return WOULD_BLOCK to
the caller (the op stays in flight and completes when the socket drains) — so there is **no retry, no
watcher, no stranding**. Backpressure remains the single-flight in-flight op.

## 1. Send-side control message, per backend (Decision D-CS-1)

Each completion backend's `post_dgram_send`, when `sop->src` is set (source-pin) or `sop->tos >= 0`,
builds a control message and issues the control-carrying overlapped send; a plain send (no src/tos)
keeps the existing fast path (`sendto`/`WSASendTo`/`sendmsg` name-only).

- **io_uring:** add a fixed control buffer to `KlIouOp` (like the existing recv `udp_ctrl`); build the
  pktinfo(src)+TOS cmsg into it, set `op->msgh.msg_control`/`msg_controllen`, and keep
  `io_uring_prep_sendmsg`. No new allocation — the buffer is a field of the op that `iou_op_alloc`
  already returns (the send already copies the payload into `op->sendbuf`).
- **pollcomp:** when src/tos present, drain via `sendmsg` (control buffer a fixed `KlPcOp` field)
  instead of `sendto`.
- **IOCP:** when src/tos present, switch the overlapped `WSASendTo` to overlapped **`WSASendMsg`**
  (`WSAID_WSASENDMSG`, already fetched for the synchronous path) with a control buffer (fixed `KlIocpOp`
  field). Plain sends keep `WSASendTo`.

## 2. Recv-side pktinfo capture (Decision D-CS-2)

The facade's completion recv arm requests the local-address capture, consistent with the readiness recv
(which always parses the pktinfo cmsg):
```c
/* dg_comp_arm (datagram.c) */
KlDgramRecvOp op = { .fd = dg->fd, ..., .capture = KL_DGRAM_RX_PKTINFO, ... };
```
The backends already honor `capture & KL_DGRAM_RX_PKTINFO` on recv (`dg_pktinfo`), delivering `ev->local`
→ the facade sets `KL_DGRAM_HAS_LOCAL`. Harmless when the socket has no `IP_PKTINFO` (no cmsg → no
local). This is a fixed op field, no allocation. (`dg_comp_submit` itself is UNCHANGED — it keeps posting
the overlapped op with `src`/`tos`, now honored by §1.)

## 3. Shared cmsg-build helper (Decision D-CS-3)

Today the send cmsg logic is duplicated as `static dgram_build_control` in `socket_dgram_posix.c` and
`socket_dgram_win.c` (used only by the synchronous provider sends). Promote a shared builder so the
io_uring/pollcomp sends and the POSIX provider share one implementation, and IOCP shares the winsock one:
- **POSIX:** `size_t kl_udp_build_control(void *buf, size_t cap, const KlSockAddr *src, int tos, int family)`
  in `udp_cmsg.h`/`.c` (sibling of the existing `kl_udp_parse_local`). `socket_dgram_posix.c`,
  `event_iouring.c`, `event_pollcomp.c` call it.
- **Winsock:** `kl_udp_win_build_control(...)` in `udp_cmsg_win.h` (sibling of the recv fetch). Used by
  `socket_dgram_win.c` and `event_iocp.c`.
No behavior change to the readiness/synchronous sends — they get the same bytes from the shared builder.

## 4. §2.5.1 copy-at-submit + I8 compliance (Decision D-CS-4)

- **Copy-at-submit:** the control message (derived from `src`/`tos`) is built into the op's own buffer
  **before `post_dgram_send` returns**, exactly as the payload and dest already are. No pointer into the
  caller's memory survives the call. The frozen seam contract (payload AND dest/src/tos COPIED before a
  successful return) is now actually honored — previously src/tos were dropped, which technically
  satisfied "copied" vacuously but lost the data.
- **No new hot allocation (I8):** the control buffer is a **fixed-size field of the existing per-op
  structure** (`KlIouOp`/`KlPcOp`/`KlIocpOp`), which the backend already allocates for every overlapped
  send (the payload copy). This introduces **zero** new allocation and **no** watcher — the withdrawn
  WRITE-watcher's `KlWatcher` alloc in the send path is gone. The Tier-1 core's fixed send slots are
  untouched.

## 5. Capability truthfulness restored (Decision D-CS-5)

With native completion source-pin, `KL_DGRAM_CAP_SOURCE_PIN` (reported by the provider `caps` per M2)
is once again truthful on a completion loop — no caps change is needed, and the M2 "caps must prove
usable behavior" invariant holds on both event models. M4's wildcard `want_caps = SOURCE_PIN` init gate
therefore grants a capability that actually works on completion backends.

## 6. What lands in the facade vs the backends

- **Facade (`datagram.c`):** one line — `dg_comp_arm` requests `KL_DGRAM_RX_PKTINFO`. `dg_comp_submit`
  reverts to a pure overlapped post (no synchronous fallthrough). No watcher code.
- **Backends:** the send control-message marshaling (§1) in `event_iouring.c` / `event_pollcomp.c` /
  `event_iocp.c`, + the shared cmsg builder (§3). These are TIER1_INFRA (they legitimately touch
  platform networking headers).

## 7. Test matrix

- **Source-pin round-trip over a completion loop** (`test_datagram_live`, real loop): a wildcard rx
  captures the local (dest) addr on a completion recv; a source-pinned send egresses from `msg.local`;
  the peer observes it. Runs under `BACKEND=pollcomp` and (container) `BACKEND=iouring`.
- **Per-packet TOS over completion:** a send with `tos >= 0` sets the outgoing DSCP/ECN (verified via a
  recv-TOS capture on the peer where the platform allows, else a getsockopt/round-trip smoke).
- **No WOULD_BLOCK stranding:** the overlapped send never returns WOULD_BLOCK, so the withdrawn
  retry-watcher scenario is structurally absent; a fill-the-send-buffer test shows the source-pin send
  completes via its CQE/overlapped completion, not a readiness edge.
- **M4 re-enablement:** `reply_from_hit_address` (Linux) asserts the source-pinned egress on **both**
  readiness and completion (the M4 `KL_EVENT_CAP_COMPLETION` gate on that assertion is removed).
- **Regression:** DNS (want_caps 0), the datagram send/close/public suites unchanged; ASan/UBSan/LSan
  clean (no new allocation).

## 8. Validation plan

- macOS default (kqueue) — readiness unaffected; full suite + the new live source-pin case.
- `BACKEND=pollcomp` — the pollcomp `sendmsg`-with-control path + recv capture; source-pin round-trip.
- Linux container `BACKEND=iouring` under ASan/UBSan/**LSan** — the io_uring sendmsg-control path;
  source-pin + TOS round-trip; `reply_from_hit_address` source-pinned on io_uring; **no new leak/alloc**.
- MinGW winsock compile of `event_iocp.c` + `socket_dgram_win.c` + `udp_cmsg_win.h`; the IOCP
  `WSASendMsg`-control path is proven on the Windows IOCP CI gate (no local Windows rig).
- Gates: `check-tier1-boundary` (the cmsg marshaling lives in TIER1_INFRA backends), `check-sockaddr-
  neutral`, `check-doc-refs`, `cppcheck`.

## 9. Open decisions for the reviewer (before implementation)

- **O-CS-share — the shared cmsg builder (§3).** *Recommended:* promote `kl_udp_build_control`
  (POSIX, `udp_cmsg.c`) + `kl_udp_win_build_control` (winsock, `udp_cmsg_win`) and have both the
  providers and the completion backends call them (dedup). *Alternative:* keep per-backend inline
  marshaling (no shared helper) — more duplication, less risk of disturbing the readiness sends.
- **O-CS-tos-scope — TOS in this increment.** *Recommended:* carry BOTH source-pin and per-packet TOS
  (they share the same cmsg buffer + code path; splitting them doubles the work). *Alternative:*
  source-pin only now, TOS deferred — but M4/`KlUdpServer` uses socket-default TOS (`tos = -1`), so TOS
  has no current consumer; include it for completeness or defer.
- **O-CS-iocp-gate — IOCP validation.** Confirm landing the IOCP `WSASendMsg`-control path on the
  Windows IOCP CI gate (untestable locally), consistent with prior IOCP increments, vs blocking on a
  local Windows rig (none exists).
