# AF_UNIX node cleanup on Windows: spike findings and design

Status: SPIKE (docs-only, investigative). No Windows code is written until the open questions below
are resolved on real Windows. Companion to docs/unix_socket_cleanup_security_design.md (the POSIX
contract); this is Increment 3 of issue #250. Scope: whether Windows can meet the same security
guarantee the POSIX module now meets, and, if it cannot across supported targets, what fail-closed or
caller-managed policy replaces the current behavior.

Note on method: this is an API/behavior survey plus a decision framework. Several points require an
empirical run on real Windows (marked EXPERIMENT); they are the gating work before any implementation.
The author cannot execute Windows here, so those points are stated as questions with the exact probe
to run, not as settled facts.

## 1. The guarantee to reproduce

The POSIX module's security property is: **act on the inode we validated, not on a name re-resolved
at use time**, under a trust boundary on every directory component. Concretely, Windows cleanup must:

1. Resolve the socket's directory without following a reparse point (symlink/junction) in any
   component, and validate a trust boundary on each component.
2. Identify the exact node the server bound (a stable file identity captured at bind).
3. Remove that identified node, not a pathname re-resolved during teardown.
4. Fail closed on an untrusted parent, an identity mismatch, or a non-socket node.

## 2. Why pathname `DeleteFileA` is not an equivalent

The current Windows teardown calls `DeleteFileA(path)` (and the same at bind-time reclaim). This is
**name-based**: the object manager re-resolves the whole path at the call, so an actor who can write
an ancestor directory can substitute the entry between any check and the delete. It carries no file
identity and no per-component no-follow guarantee. It is the exact time-of-check to time-of-use hole
the POSIX side removed, so it is rejected as the Windows solution regardless of any surrounding
type/attribute checks. `DeleteFileA` also has nuanced link semantics (it does not uniformly "follow a
reparse point to delete its target"), so reasoning about it as a POSIX-`unlink` equivalent is unsafe.

## 3. Windows primitives for an identity-anchored delete

The building blocks that could express the section 1 guarantee:

- **No-follow open of the final node.** `CreateFileW`/`CreateFile2` with
  `FILE_FLAG_OPEN_REPARSE_POINT` opens the reparse point itself rather than its target, plus
  `FILE_FLAG_BACKUP_SEMANTICS` (required to obtain a handle to a directory, and useful for metadata),
  requesting `DELETE` access and a permissive `dwShareMode`.
- **Per-component no-follow, directory-anchored walk.** Win32 `CreateFile` takes a path, not a parent
  handle, so it re-resolves intermediate components on each call. The NT layer
  (`NtCreateFile`/`NtOpenFile` via `ntdll`) accepts `OBJECT_ATTRIBUTES.RootDirectory` = a directory
  handle plus a leaf name and `OBJ_DONT_REPARSE` (fail if the target is a reparse point) or
  `FILE_OPEN_REPARSE_POINT`. That is the true `openat`-style anchor: walk each component relative to
  the validated parent handle. Cost: an `ntdll` dependency and NT-status handling.
- **Stable file identity.** `GetFileInformationByHandleEx(FileIdInfo)` returns `FILE_ID_INFO`
  (`VolumeSerialNumber` + 128-bit `FileId`), stable on NTFS/ReFS. The legacy
  `GetFileInformationByHandle` (`dwVolumeSerialNumber` + 64-bit `nFileIndex`) is weaker and can be
  reused/unstable. Capture the identity of the bound node at bind, re-verify it at teardown through a
  freshly opened no-follow handle before deleting.
- **Delete through the verified handle.** `SetFileInformationByHandle(FileDispositionInfoEx,
  FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS)` deletes the entry the handle
  refers to (identity-anchored, no name re-resolution). Where POSIX semantics is unavailable, the
  older `FileDispositionInfo` marks delete-on-close (removed when the last handle closes). Either
  targets the opened object, not a re-resolved name.

Sketch of a conforming teardown: NT-walk the parent components no-follow, validating the trust
boundary at each; open the leaf no-follow with `DELETE`; `GetFileInformationByHandleEx(FileIdInfo)`
and compare to the captured identity; verify it is the AF_UNIX node type; then set the POSIX
disposition to delete and close. No pathname is re-resolved after the walk.

## 4. The Windows trust boundary

Windows has no uid/mode model, so the parent-directory trust check is an **ACL** decision, not a
`geteuid()` + mode-bit check. A "trusted" parent directory is one whose DACL grants create/delete-
child only to principals inside the trust domain (typically the server's own account SID, plus
SYSTEM and Administrators), with no write for other/authenticated users. Reading it: `GetSecurityInfo`
on the directory handle (owner + DACL). A robust ACL trust evaluation (inheritance, well-known SIDs,
`CREATOR OWNER`, deny ACEs) is materially harder than the POSIX mode-bit rule and is itself an open
design point; a conservative first rule is "owner is the server SID or SYSTEM/Administrators, and no
ACE grants write/delete-child to any other non-well-known SID," else fail closed.

## 5. Open questions (EXPERIMENT: run on real Windows before implementing)

1. **Can an AF_UNIX socket node be opened as a file at all?** The node is a reparse point
   (`IO_REPARSE_TAG_AF_UNIX`). Probe: `CreateFileW(path, DELETE, share, NULL, OPEN_EXISTING,
   FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL)` on a live and on a stale socket
   node; record success/`GetLastError`. If sockets reject a file open, delete-by-handle is impossible
   and the design must fall back (section 6).
2. **Does delete-by-handle work on it?** If (1) succeeds, probe
   `SetFileInformationByHandle(FileDispositionInfoEx, POSIX|DELETE)` and, as fallback,
   `FileDispositionInfo`. Record whether the node is removed.
3. **Is `FILE_ID_INFO` stable and distinct for the node** across close/rebind on NTFS and ReFS, and
   what is the behavior on SMB/network paths and FAT/exFAT (where identity may be absent or reused)?
4. **Minimum-version availability.** `FileDispositionInfoEx` POSIX semantics requires Windows 10
   1709+ on NTFS; `FILE_ID_INFO` requires Windows 8+; Windows AF_UNIX requires Windows 10 1803+.
   Confirm the intersection against the project's supported Windows floor and the MinGW/IOCP CI image.
5. **NT-API dependency.** Confirm whether `NtCreateFile` dir-relative no-follow is acceptable (an
   `ntdll` import), or whether a Win32-only walk (full-path `CreateFile` + `OPEN_REPARSE_POINT` on
   each component, relying entirely on the section 4 ACL trust of every component to make intermediate
   substitution impossible) is sufficient. The ACL-of-every-component argument mirrors the POSIX
   every-component trust: if no untrusted actor can write any ancestor, name re-resolution cannot be
   diverted, so a Win32-only path may meet the guarantee without `ntdll`. This trade needs a decision.

## 6. Decision framework

- **If the experiment confirms** a no-follow open + `FILE_ID_INFO` identity + delete-by-handle on the
  AF_UNIX node across the supported targets: implement `src/unix_socket_node_win.c` behind the same
  `unix_socket_node.h` contract, with per-component ACL trust validation, identity capture at bind,
  and identity-verified delete-by-handle at teardown/reclaim. Prefer the Win32-only walk if section 5
  question 5 resolves that way; otherwise use the NT-relative walk.
- **If it cannot be met across supported targets** (sockets not openable by handle, unstable identity
  on a supported filesystem, or an unacceptably weak ACL check): do **not** ship a name-based
  `DeleteFileA`. Instead choose, and document as the Windows contract:
  - **Fail-closed automatic cleanup:** `unix_socket_unlink` on Windows validates the parent ACL and
    only removes a node it can open-and-identify; if it cannot, bind/teardown refuse to delete
    (an existing node makes bind fail with a clear error) rather than deleting an ambiguous entry.
  - **Caller-managed cleanup:** Windows performs no automatic unlink; the public precondition states
    that on Windows the caller owns socket-path lifecycle (create in a caller-controlled directory,
    remove stale nodes out of band). `unix_socket_unlink` becomes a no-op on Windows with a documented
    reason.
- Either fallback is preferable to the current unconditional `DeleteFileA`, which is removed in the
  same increment that lands the chosen policy.

## 7. Contract and gate implications

- The Windows implementation, whichever path is chosen, lands as `src/unix_socket_node_win.c`
  implementing the existing `unix_socket_node.h` contract; `UNIX_NODE_SRC` becomes that file on
  Windows (currently empty). The HTTP adapter is already platform-neutral, so no protocol-layer change
  is needed, and `check-no-fsnode-in-protocols` continues to hold.
- If a fallback (fail-closed / caller-managed) is chosen, the neutral status set may need a
  `KL_UNIX_NODE_ERR_UNSUPPORTED` (or a documented no-op teardown) so the HTTP adapter can surface the
  Windows policy without a POSIX-specific assumption.
- The public `unix_socket_unlink` / owner / group / mode doc-comments gain a Windows note reflecting
  the chosen policy.

## 8. Recommendation

Run the section 5 EXPERIMENT probes on the supported Windows floor before writing any implementation.
The identity-anchored delete-by-handle approach (section 3) is the target if the probes pass; the
Win32-only every-component-ACL walk is the preferred shape if it removes the `ntdll` dependency
without weakening the guarantee. If the probes fail on any supported target, ship **fail-closed**
automatic cleanup as the default (least surprising for existing `unix_socket_unlink` users) and
document caller-managed cleanup as the escape hatch. Under no outcome is pathname `DeleteFileA`
retained as the mechanism.

Implementation is deferred until these findings are reviewed and the EXPERIMENT is run.

## 9. Recorded EXPERIMENT results (windows-latest, one run)

The section 5 probes were run once on a GitHub `windows-latest` runner via a temporary probe-only
workflow (`.github/workflows/windows-probe.yml` + `.github/windows-probe/win_unix_node_probe.c`; that
machinery is removed before the security PR is finalized). Environment: Windows `10.0 build 26100`,
filesystem **NTFS**, volume flags include `OPEN_BY_FILE_ID` and `REPARSE_POINTS`; the process token
held none of SeBackup/SeRestore/SeTakeOwnership/SeSecurity.

Results (all on the runner's local NTFS; no ReFS/SMB/FAT was probed, so no conclusion is drawn for
those):

- The AF_UNIX node is a reparse point (attributes include `FILE_ATTRIBUTE_REPARSE_POINT`).
- It can be opened no-follow with `DELETE` + delete sharing (`FILE_FLAG_OPEN_REPARSE_POINT`), even
  while the socket is live: OK.
- `GetFileInformationByHandleEx(FileIdInfo)` returns a `FILE_ID_INFO`: OK; stable across two opens and
  distinct after a rebind.
- Delete through the verified handle via `FileDispositionInfoEx` (POSIX semantics) removes the node:
  OK.
- Open after external deletion fails cleanly.
- A full-path open with `FILE_FLAG_OPEN_REPARSE_POINT` **traverses an intermediate junction**,
  confirming that flag guards only the final component, so a per-component walk that rejects
  intermediate reparse points is required.
- The temp-directory ACL (SYSTEM, Administrators, and the process user, all full control, inherited)
  showed why the trust check must weigh `WRITE_DAC`/`WRITE_OWNER`/`FILE_DELETE_CHILD`, not just
  present write access.

Conclusion: the identity-anchored delete is feasible on modern Windows / local NTFS. The
implementation supports exactly that verified capability set and fails closed elsewhere
(`KL_UNIX_NODE_ERR_UNSUPPORTED` for non-NTFS or missing FileIdInfo / reparse / POSIX disposition;
`UNTRUSTED_PARENT` for an intermediate reparse point or an untrusted-writable component). It never
falls back to `DeleteFileA`. Behavior on ReFS, SMB, FAT, and Windows versions below the probed build
remains unverified and is therefore treated as unsupported until separately validated.
