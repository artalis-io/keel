# AF_UNIX node-lifecycle hardening: threat model and design

Status: PROPOSED (docs-only). No code changes until this design is accepted. Scope: the security
increment tracking issue #250 and CodeQL alert #43 (`cpp/toctou-race-condition`,
`src/protocols/http/http_server_plat_posix.c:101`).

This branch (`security/unix-socket-cleanup`) is based on current `main`, which now includes the
merged Phase C documentation work (PR #249 was merged at `7563556`). It is a separate security
increment. CodeQL alert #43 remains open; alerts #44/#45 were separately triaged as intentional
cleartext forward-proxy semantics (won't-fix) and are unrelated.

This active design may remain at `docs/` root during implementation; it is archived under
`docs/archive/designs/` when the security increment is finalized. After this design is accepted, the
POSIX implementation and the Windows spike land as **separate reviewed increments**.

## 1. Scope

The HTTP server can bind an `AF_UNIX` listener at a caller-supplied path and, opt-in, reclaim a stale
node before bind and remove its own node on teardown, plus set owner/group/mode. Every step that acts
on the path *by name* is a potential time-of-check to time-of-use (TOCTOU) hazard. This document
audits **every** pathname mutation in the lifecycle, states the unavoidable filesystem facts, and
proposes a trust-boundary-based fix. It deliberately does not fix the CodeQL line in isolation.

## 2. Unavoidable filesystem facts

1. **No atomic act-iff-inode.** POSIX has no portable operation meaning "unlink/chown/chmod this
   pathname only if it still identifies inode X". `unlink(2)`, `chown(2)`, and `chmod(2)` resolve the
   *name* at call time; a prior `lstat(2)` resolved the same name independently. A check-then-act pair
   on a name is therefore inherently racy: an actor who can modify the containing directory can
   substitute the entry between check and use.
2. **A socket fd is not the filesystem inode.** On common POSIX systems `fstat(listen_fd)` describes
   the *kernel socket object*, not the filesystem node that `bind()` created at the path. So the
   node's `st_dev`/`st_ino` must be captured with `fstatat(dirfd, base, AT_SYMLINK_NOFOLLOW)`, not
   from the socket fd. For the same reason, `fchown(listen_fd)`/`fchmod(listen_fd)` may fail or act on
   the kernel socket object rather than the pathname node; they are not a reliable mechanism for the
   filesystem entry.
3. **`O_NOFOLLOW` guards one component.** `open(path, O_NOFOLLOW)` rejects a symlink only at the final
   component; it does not prevent symlink traversal or races in intermediate components.
4. **Directory-entry mutation requires directory write permission.** Creating, removing, or renaming
   an entry (the substitution primitive an adversary needs) requires write permission on the
   *containing directory*, not on the entry's inode. Owning the inode does not grant it. This is the
   lever the trust boundary pulls.

Conclusion: the defect cannot be closed by a stronger-sounding comment around the same race, nor by an
unproven fd-based shortcut. It needs an **explicit trust boundary** (who may write the parent
directory) plus component-safe, directory-relative mechanics and fail-closed behavior. The
trusted-directory guarantee is the actual security property; the mechanics only minimize the residual
final-component window and catch unsafe configurations.

## 3. Inventory of pathname mutations (every site)

POSIX (`src/protocols/http/http_server_plat_posix.c`):

| # | Site | Lines | Operation | Current guard | Hazard |
|---|------|-------|-----------|---------------|--------|
| P1 | Startup stale-node reclaim (`unlink_stale_unix_socket`) | 83 -> 101 | `lstat` then `unlink(path)` | `S_ISSOCK` re-check, then unlink by name | **CodeQL #43.** Swap the entry between `lstat` and `unlink`; unlink removes the replacement. Over-strong comment at 98-99, plus an `lgtm[...]` suppression at 100. |
| P2 | Post-bind `chown(path, uid, gid)` | 240 | change owner/group by name | none (acts on `path`) | `chown` follows symlinks: swap `path` to a symlink and, when the server is privileged, `chown` retargets an arbitrary file to the configured uid/gid. |
| P3 | Post-bind `chmod(path, mode)` | 252 | change mode by name | none | `chmod` follows symlinks: swap `path` to a symlink and `chmod` retargets an arbitrary file's mode. |
| P4 | Failure-path cleanup `unlink(path)` | 264 | remove by name | **none** (no type re-check at all) | Weaker than P1: on any owner/mode failure it blind-unlinks whatever is at `path` now. |
| P5 | Shutdown owned-node removal (`kl_http_server_plat_unlink_owned_unix`) | 277 -> 278 | `lstat` then `unlink(path)` | `S_ISSOCK` re-check, then unlink by name | Same class as P1: swap between re-check and unlink. |

Windows (`src/protocols/http/http_server_plat_win.c`):

| # | Site | Lines | Operation | Current guard | Hazard |
|---|------|-------|-----------|---------------|--------|
| W1 | Bind-time reclaim | 86 | `DeleteFileA(path)` | none | Deletes whatever is at `path`; no type/identity check. |
| W2 | Shutdown removal | 115 | `DeleteFileA(path)` | `unix_socket_owned` flag only | Same as W1. |

Config surface (`include/keel/http_server.h`):

- `unix_socket_unlink` (P1/P4/P5/W1/W2 switch): "unlink path before bind and on free after successful
  bind." The opt-in that enables pathname-removing behavior. Its public precondition is currently
  undocumented.
- `unix_socket_owner` / `unix_socket_group` (P2) and `unix_socket_mode` (P3): trigger the ownership
  and mode mutations. These run **even when `unix_socket_unlink` is off**, so they are in scope
  independently of the unlink switch. The resolved `unix_socket_owner` uid also matters for P1/P5
  (see section 5.1 step 3) and drives the tightened trust tier (section 4).
- `unix_socket_owned` (internal): set true after a successful bind; gates P5/W2.
- Adopted `listen_fd` (socket activation): KEEL never unlinks an adopted socket. Out of scope; the
  design must keep it that way.

## 4. Threat model

- **Asset.** The integrity of files outside the intended socket node: the server must never remove,
  chown, or chmod an entry that is not the socket it owns.
- **Adversary.** A local actor with a **different uid** who has write access to the socket's parent
  directory. The adversary races the server between check and use to substitute the directory entry.
  By fact 4, the adversary's ability to substitute depends entirely on holding *directory* write.
- **Same-uid actors are inside the trust boundary.** A process running as the server's own uid can
  already manipulate the server's files arbitrarily; defending against it is neither possible nor
  meaningful.

### 4.1 Trust boundary (the actual security guarantee), in two tiers

Let `D` be the parent directory of the socket path, owned by `owner(D)` with mode `mode(D)`.

**Tier A (removal of a self-owned entry: P1 stale reclaim, P5 teardown, P4 failure cleanup, when no
ownership transfer to a foreign uid is configured).** `D` is trusted iff:

- `owner(D)` is in `{euid, 0}`, and
- `D` is not group/other-writable, **or** it is group/other-writable only with the sticky bit set.

Sticky shared directories (e.g. `/tmp`, mode `1777`) are acceptable here: the entry is owned by the
server's uid, and the sticky bit forbids non-owners from removing or renaming it.

**Tier B (ownership/mode transfer to a foreign principal: `unix_socket_owner` resolves to a uid !=
euid).** After the server `chown`s the node to a different uid, that uid owns the *inode*. By fact 4
this alone does not let it substitute the entry; substitution still requires write on `D`. But a
sticky *shared* `D` grants every uid directory write (sticky only restricts removal of entries you do
not own, and the foreign owner now *does* own this entry), so in a sticky shared `D` the configured
owner could remove/replace its own entry and race the subsequent `chmod`, the teardown check/unlink,
or any later name-based mutation. Therefore, when `unix_socket_owner != euid`, `D` is trusted iff:

- `owner(D)` is in `{euid, 0}`, and
- `D` has **no group or other write permission at all** (the sticky-shared allowance of Tier A is
  **not** accepted).

This is the recommended rule, and it is frozen: automatic ownership/mode mutation to a foreign uid
requires a parent directory writable only within the trust domain. KEEL does **not** take the weaker
alternative of declaring the configured owner part of the privileged trust domain. If `D` fails the
applicable tier, the server fails closed (bind errors; nothing is mutated).

Under Tier B the foreign owner cannot write `D`, so it cannot substitute the entry at any point in the
lifecycle; the operation-ordering concern below is resolved by this, not by any per-call trick.

## 5. Design

Guiding rule: **enforce the applicable trust tier, hold a component-safely-opened parent directory
descriptor for the socket lifetime, act only through it with identity revalidation, and fail closed.**

### 5.1 POSIX

1. **Open the parent directory component-safely and hold it for the socket lifetime.** Split the
   socket path into `dir` + `base`. Resolve `dir` by walking components from a trusted root: start
   from a descriptor for `/` (absolute paths) or a descriptor for the current working directory
   (relative paths), then `openat` each successive component with
   `O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC`, so no intermediate component may be a symlink (fact 3: a
   single `O_NOFOLLOW` open is not enough). `fstat` the final `dirfd` and enforce the section 4.1 tier
   that applies to this configuration (Tier B if `unix_socket_owner` resolves to a uid != euid, else
   Tier A). **Keep `dirfd` open for the entire bound-socket lifetime** (stored in server state,
   reused at teardown, never reopening the textual parent). If the trust check fails and any pathname
   mutation would occur, **fail closed**.
2. **All leaf operations are directory-relative.** `fstatat(dirfd, base, &st, AT_SYMLINK_NOFOLLOW)`,
   `unlinkat(dirfd, base, 0)`, `fchownat(dirfd, base, uid, gid, AT_SYMLINK_NOFOLLOW)`,
   `fchmodat(dirfd, base, mode, 0)`. With the component-walked, lifetime-held `dirfd`, only the
   final-component window remains, and under the applicable tier no in-domain-external actor can write
   `D` to exploit it.
3. **Validate leaf identity before acting (P1, P4, P5).** `fstatat(NOFOLLOW)` and proceed only when
   the leaf is a socket (`S_ISSOCK`) owned by an **accepted uid**: `{euid}` plus the resolved
   `unix_socket_owner` uid when configured (so a socket a privileged prior run chowned to the
   configured owner is reclaimable on restart). Root-owned leaves are not broadly trusted: uid 0 is
   accepted only when it is the euid or the configured owner resolves to it. Any other owner or a
   non-socket leaf fails closed. P4 uses this **same** validated helper, not a blind `unlink`.
4. **Owner/mode via directory-relative operations, not the socket fd (P2, P3).** Per fact 2,
   `fchown`/`fchmod(listen_fd)` are not used. Apply owner/group with
   `fchownat(dirfd, base, ..., AT_SYMLINK_NOFOLLOW)` then mode with `fchmodat(dirfd, base, mode, 0)`,
   each immediately preceded by the step-3 identity revalidation. The order is chown-then-chmod
   because `chown` can clear set-user/set-group-ID bits, so mode is asserted last. This reintroduces a
   privileged name operation *after* ownership transfer, which is safe **only** because Tier B
   requires `D` to have no group/other write, so the newly-foreign owner cannot substitute the entry
   between the `chown` and the `chmod` (fact 4). The design does **not** claim this sequence is safe in
   a sticky shared directory; that configuration is rejected by Tier B. The temporary-umask-at-bind
   window closure is retained. Any fd-based shortcut is out of this design until a per-platform spike
   proves `fchown`/`fchmod` on an `AF_UNIX` socket fd affects the filesystem node.
5. **Capture node identity for teardown via `fstatat` (P5).** Immediately after `bind`,
   `fstatat(dirfd, base, &st, AT_SYMLINK_NOFOLLOW)` and record the filesystem node's `st_dev` +
   `st_ino` (not `fstat(listen_fd)`, per fact 2). At teardown, using the held `dirfd`,
   `fstatat(NOFOLLOW)` and unlink only when the leaf is a socket, its `st_dev`/`st_ino` match, its
   owner is accepted, and `D` still passes its tier. The dev/ino match aborts accidental and
   non-adversarial reuse cases; the adversarial case is precluded by the tier, not by the match.
6. **Fail closed everywhere.** Unsafe parent, ownership mismatch, non-socket leaf, or a dev/ino
   mismatch all result in "do nothing and (for bind) error", never "delete/chown/chmod the ambiguous
   entry."

### 5.2 Windows (frozen pending a spike)

Windows `AF_UNIX` has no `S_ISSOCK` and no uid model. Rejecting directories and reparse points is
**insufficient**: it still permits `DeleteFileA` to remove a *substituted regular file*, and the
earlier claim that `DeleteFileA` follows a reparse point to delete its target is not accurate. Windows
link-deletion semantics are more nuanced and must not be assumed.

**No Windows implementation is written** until a focused API/behavior spike determines whether the
socket node can be handled by identity rather than by name:

- opened with no-follow semantics (e.g. `CreateFileA` with `FILE_FLAG_OPEN_REPARSE_POINT` and
  suitable share/flags),
- identified by file ID and revalidated (e.g. `GetFileInformationByHandle` / `FILE_ID_INFO`),
- deleted through that verified handle (e.g. `FILE_DISPOSITION_INFO` via
  `SetFileInformationByHandle`), an open-validate-delete-by-handle sequence with no name reuse.

If the spike shows this cannot be done safely, Windows automatic stale cleanup must either remain an
explicitly trusted-directory operation or fail closed and require caller-managed cleanup. The spike is
its own reviewed increment and its result is recorded here before any Windows code is added.

### 5.3 Public precondition (documentation)

Update the doc-comments on `unix_socket_unlink`, `unix_socket_owner`, `unix_socket_group`, and
`unix_socket_mode` in `include/keel/http_server.h` to state: automatic stale-node unlink and
owner/group/mode mutation are safe only when the socket's **parent directory is within the server's
trust domain**; and specifically that transferring ownership to a different uid
(`unix_socket_owner != euid`) additionally requires a parent directory with no group/other write (a
sticky shared directory such as `/tmp` is not accepted for that configuration). If the parent is
untrusted for the applicable tier, bind fails closed; place the socket in a server-owned directory or
manage the path yourself.

## 6. CodeQL handling

The fix removes the over-strong comment and the `// lgtm[cpp/toctou-race-condition]` suppression at
`http_server_plat_posix.c:98-100` and replaces the code with the validated, directory-relative,
fail-closed implementation. Alert #43 is resolved by a real code change, not silenced; it stays open
until that lands and CodeQL re-runs clean.

## 7. Test plan (deterministic): verify the real guarantee

The tests verify the trust-boundary guarantee and the detection of pre-validation tampering. They do
**not** attempt to prove act-iff-inode atomicity that fact 1 says does not exist: a same-trust-domain
substitution performed *after* the final `fstatat` and *before* the mutation is an explicitly accepted
residual (section 8), not something the helper defeats. Accordingly there is **no internal seam that
injects a substitution at the validate/act boundary**. Tests manipulate real filesystem state via the
public bind/teardown lifecycle and the OS permission model:

1. **Unsafe parent rejected before mutation.** Group/other-writable non-sticky parent (Tier A), and a
   sticky shared parent under an `unix_socket_owner != euid` configuration (Tier B): bind fails closed
   and nothing is unlinked/chowned/chmod-ed. A regular file at the path is a non-socket leaf and is
   likewise refused, never removed.
2. **The permission model actually forbids substitution in an accepted directory.** In a Tier-A/Tier-B
   accepted `D`, a different-uid actor's attempt to create/remove/rename the entry fails with `EACCES`
   (or is sticky-forbidden). This asserts the real defense (fact 4). Needs a second uid; runs only
   when privileged, otherwise skips with a recorded reason.
3. **Pre-validation replacement is detected and refused.** A substitution prepared *before* the
   helper's `fstatat` (wrong type, wrong owner, or a symlink) is rejected by identity validation; no
   mutation occurs.
4. **Device/inode mismatch at teardown is refused.** After a clean bind, replace the node (in a way
   the real model permits only within the trust domain, e.g. as the same uid) so the recorded dev/ino
   no longer matches; teardown refuses to unlink.
5. **Normal recovery and owned-node teardown.** A self-owned stale socket in a trusted dir is
   reclaimed and rebind succeeds; a clean bind then teardown unlinks exactly the owned node.
6. **Accepted-owner reclaim.** A stale socket owned by the resolved `unix_socket_owner` uid is
   accepted on restart (privileged); one owned by an unrelated uid is refused.
7. **Failure-path cleanup (P4).** Force a chown/chmod failure and assert the failure unlink runs
   through the validated helper (does not blind-unlink a replacement).

Tests avoid timing sleeps; they prepare state deterministically between the public lifecycle calls.

## 8. Non-goals and accepted residual

- Defending against a same-uid actor (inside the trust boundary): out of scope by definition.
- Eliminating the final-component name race in the absolute (impossible per fact 1): a
  same-trust-domain substitution performed after the final `fstatat` and before the mutation is an
  explicitly accepted residual, bounded by the mandatory trust tier (which denies out-of-domain actors
  the directory write needed to substitute), the lifetime-held component-walked `dirfd`, and the
  dev/ino teardown check.
- Changing the default: `unix_socket_unlink` stays opt-in; the new behavior is fail-closed validation
  when it (or owner/group/mode) is enabled, plus the documented precondition.

## 9. Resource and lifecycle details (frozen)

- **`base` storage.** Copy the leaf component into lifecycle-owned fixed storage on the server (a
  fixed-size buffer bounded by `sizeof(sun_path)`); do not retain a pointer into caller-owned
  `unix_socket_path` text, which the caller may free or mutate.
- **Held `dirfd` sentinel.** Initialize the stored parent `dirfd` to an invalid sentinel (`-1`);
  treat `-1` as "not open" everywhere.
- **Close on every exit.** Close the held `dirfd` (and reset to `-1`) on bind failure, on server
  teardown/free, and on every partial-initialization unwind path, so the descriptor never leaks and
  is never reused after close.
- **Compile coverage.** Add compile-time coverage that the selected primitives and flags
  (`openat`, `fstatat`, `unlinkat`, `fchownat`, `fchmodat`; `O_DIRECTORY`, `O_NOFOLLOW`, `O_CLOEXEC`,
  `AT_SYMLINK_NOFOLLOW`) are available on every supported hosted POSIX target, so an unsupported
  target fails at build time rather than silently degrading.

## 10. Deliverables and sequencing

The work lands as **separate reviewed increments**:

- **Increment 1 (POSIX security hardening):**
  1. Component-walked, lifetime-held parent `dirfd` (stored in fixed state, reused at teardown, closed
     on all exits) with the section 4.1 tier check.
  2. `base` copied into lifecycle-owned fixed storage.
  3. A shared validated helper (`dirfd` + `base` + `fstatat(NOFOLLOW)` identity + accepted-owner set)
     used by the reclaim, failure-cleanup, and teardown removals.
  4. `fstatat`-based dev/ino capture at bind and match at teardown.
  5. Directory-relative `unlinkat`/`fchownat`/`fchmodat`, chown-then-chmod, with pre-op revalidation;
     no socket-fd owner/mode.
  6. Header doc-comment precondition updates (section 5.3) and removal of the suppression + over-strong
     comment (section 6).
  7. The deterministic tests (section 7) with **no** boundary-injection seam, plus the compile-coverage
     check (section 9).

  Landed first in the HTTP server-platform TU; a reviewable, self-contained security commit.

- **Increment 2 (behavior-neutral axis extraction):** the AF_UNIX filesystem-node lifecycle is
  transport/socket-axis work, so it moves out of the protocol tree into a substrate module,
  **`src/unix_socket_node.h` + `src/unix_socket_node_posix.c`**. The module names no protocol type,
  takes a neutral policy (`KlUnixNodePolicy`: path, unlink, owner/group NAMES, mode) + the socket
  provider + an allocator seam, owns the opaque per-bind state (held dirfd, copied base, dev/ino,
  accepted-owner), resolves owner/group names internally, and returns a neutral `KlUnixNodeStatus`.
  The HTTP server keeps only a thin adapter: it builds the policy, delegates bind/teardown, and maps
  the status into `KlError` + server logging. `src/protocols/http/` retains no AF_UNIX node
  syscalls, enforced by the ownership gate **`check-no-fsnode-in-protocols`**. No behavior change.

- **Increment 3 (Windows spike):** the section 5.2 API/behavior spike and its recorded decision; a
  Windows implementation of the **same** `unix_socket_node.h` contract
  (`src/unix_socket_node_win.c`) is its future home. Until then Windows keeps its existing inline
  teardown and `UNIX_NODE_SRC` is empty on that platform. No Windows code until the spike resolves.

The substrate home is the final one: `src/unix_socket_node*.c`. When the increments are finalized,
this design is archived under `docs/archive/designs/`.
