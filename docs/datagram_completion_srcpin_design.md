# Datagram framework increment — native completion source-pin / TOS — design freeze

Status: **PROPOSED (docs-only, revision 2)** — no code until reviewed and accepted, per the
consolidation workflow. A **prerequisite framework increment** extracted from M4 at review request: the
KlDatagram COMPLETION send path must carry the source-pin (`msg.local`) and per-packet TOS the frozen
§2.5.1 seam already passes it, natively — with NO hot-path allocation — across io_uring, pollcomp, and
IOCP. M4 (`docs/datagram_m4_udpserver_design.md`, staged at 0a6350a) lands atop this: after this
increment, M4's completion source-pin assertion gate is removed and the combined M4 result is
revalidated and accepted.

**Revision 2** applies the review rulings and fixes three findings. (P1) Connected TOS (dest==NULL,
src==NULL) has no address to pick IP_TOS vs IPV6_TCLASS — family is now resolved via `getsockname`
during post, never defaulting to IPv4 (§1a). (P1) The full overlapped WSASendMsg lifetime (WSAMSG,
WSABUF, name, control) is frozen into `KlIocpOp` — no stack descriptor survives the post (§1b), with
cancellation/late-completion coverage. (P2) "overlapped sends never return WOULD_BLOCK" is restated as
the real guarantee: a successful post yields exactly one tracked in-flight op + exactly one terminal
completion (success OR error), no queued-without-op state (§0/§4). Rulings accepted: shared POSIX +
Winsock cmsg builders; carry both source-pin AND per-packet TOS; Windows IOCP CI is the native gate.

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
with a pktinfo + TOS cmsg). **The guarantee (P2, precise):** a *successful post* transfers ownership and
creates exactly **one tracked in-flight operation that yields exactly one terminal completion** — success
OR a transient/terminal socket error reported by the CQE/overlapped completion. There is **no
queued-without-op state**, hence **no facade-level retry, watcher, or stranding**; backpressure remains
the single-flight in-flight op, and a terminal-error completion retires that op exactly as a success
does (sticky error, no re-post). (It is NOT claimed that the transfer can never fail — a *post* itself
can fail before creating an op, in which case the caller releases its ref and nothing is in flight, per
the existing `post_dgram_send` contract.)

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

### 1a. TOS-cmsg family resolution — never default to IPv4 (Decision D-CS-1a, blocker P1)

The TOS cmsg level is family-specific (`IPPROTO_IP`/`IP_TOS` vs `IPPROTO_IPV6`/`IPV6_TCLASS`), as is the
pktinfo cmsg. A **connected TOS-only send** (`dest == NULL` AND `src == NULL`, `tos >= 0`) carries no
address in `KlDgramSendOp`, so the family must come from the fd itself. Frozen resolution order, applied
at post: **`dest` family, else `src` family, else `getsockname(fd)` family** — the resolved family is
stored in the op and passed to the shared builder. It is **never** defaulted to `AF_INET` (the existing
synchronous provider sends' `... : AF_INET` fallback is a latent bug; the shared builder (§3) is
family-parameterized so the caller resolves it correctly, and the synchronous callers adopt the same
`getsockname` fallback). A `getsockname` failure with no address available → the send fails the post
(`-1`), never a wrong-family cmsg.

### 1b. IOCP overlapped WSASendMsg lifetime (Decision D-CS-1b, blocker P1)

`WSASendMsg` retains, until the terminal completion, **all** of: the `WSAMSG`, the `WSABUF` array, the
destination `name` sockaddr storage, the control buffer, and the internal pointers among them (`WSAMSG`
points at the `WSABUF`, the name, and the control). **Every one of these must be a field of `KlIocpOp`**
— no stack-local `WSAMSG`/`WSABUF`/sockaddr/control may survive the `post` call (the existing overlapped
`WSASendTo` already keeps the payload copy + `OVERLAPPED` in the op; this extends that to the full
`WSASendMsg` descriptor set). The op is freed only on its terminal completion. Coverage: a cancellation
/ late-completion test proving the op (and its `WSAMSG`/name/control) remains valid until the terminal
completion is delivered (mirrors the recv-side `WSARecvMsg` lifetime already covered).

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
  `event_iouring.c`, `event_pollcomp.c` call it. The `family` is the CALLER-resolved fd family (§1a) —
  the builder never guesses it.
- **Winsock:** `kl_udp_win_build_control(...)` in `udp_cmsg_win.h` (sibling of the recv fetch). Used by
  `socket_dgram_win.c` and `event_iocp.c`.
Ruling: the shared builders are used (duplication here is likely to drift). The synchronous provider
sends adopt them, so they inherit the §1a family fix (no more `: AF_INET` default); the resulting cmsg
bytes are otherwise identical, so readiness/synchronous send behavior is unchanged except for the
now-correct IPv6 TOS family on a connected TOS send.

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
- **Connected TOS-only send picks the right family (P1/§1a):** a TOS send with `dest == NULL` and
  `src == NULL` on an `AF_INET6` fd emits `IPV6_TCLASS` (resolved via `getsockname`), never `IP_TOS`;
  and on an `AF_INET` fd emits `IP_TOS`. (A mock/gated provider or a getsockopt round-trip asserts the
  level, since a wrong-family cmsg is silently ignored by the kernel.)
- **One in-flight op → exactly one terminal completion (P2):** a successful post yields a single tracked
  op; both a **success completion** and a forced **terminal-error completion** retire that op exactly
  once (sticky error, no re-post, no queued-without-op state) — verified for source-pin sends, not only
  the success path.
- **IOCP overlapped lifetime (P1/§1b):** a cancellation / late-completion case proves the `KlIocpOp`
  (its `WSAMSG`, `WSABUF`, name, and control) stays valid until the terminal completion — no stack
  descriptor escaped the post (Windows IOCP CI).
- **M4 re-enablement:** `reply_from_hit_address` (Linux) asserts the source-pinned egress on **both**
  readiness and completion (the M4 `KL_EVENT_CAP_COMPLETION` gate on that assertion is removed), and the
  combined M4 result is revalidated and accepted.
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

## 9. Decisions — resolved at review (revision 2)

- **O-CS-share — RESOLVED: use the shared POSIX + Winsock cmsg builders** (`kl_udp_build_control` /
  `kl_udp_win_build_control`), both providers and completion backends calling them (duplication here is
  likely to drift). §3.
- **O-CS-tos-scope — RESOLVED: carry BOTH source-pin AND per-packet TOS** in this increment (shared
  cmsg buffer + code path). §1.
- **O-CS-iocp-gate — RESOLVED: the Windows IOCP CI is the native validation gate** for the
  `WSASendMsg`-control path (no local Windows rig), consistent with prior IOCP increments. §8.
- **P1 family (§1a), P1 IOCP lifetime (§1b), P2 completion guarantee (§0/§4) — RESOLVED** per revision 2.

No open decisions remain; the freeze is ready for implementation.
