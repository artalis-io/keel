/*
 * unix_socket_node_win.c: Windows implementation of the AF_UNIX filesystem-node lifecycle contract
 * (unix_socket_node.h). Substrate transport code: no protocol type, no protocol header.
 *
 * Security model (see docs/unix_socket_cleanup_security_design.md +
 * docs/unix_socket_cleanup_windows_spike.md): an IDENTITY-ANCHORED delete. The spike verified, on
 * Windows 10 build 26100 / local NTFS, that an AF_UNIX socket node can be opened with a no-follow
 * handle, identified by a stable FILE_ID_INFO (volume serial + 128-bit file id), and deleted through
 * that verified handle (FileDispositionInfoEx POSIX). This module supports ONLY that verified
 * capability set: modern Windows on local NTFS with FileIdInfo + reparse handling + POSIX-semantics
 * handle disposition. Everything else fails closed:
 *   - non-NTFS / SMB / ReFS / FAT / unknown volumes           -> KL_UNIX_NODE_ERR_UNSUPPORTED
 *   - missing FileIdInfo / POSIX disposition / reparse support -> KL_UNIX_NODE_ERR_UNSUPPORTED
 *   - an untrusted SID that can delete/replace or re-permission any path component -> UNTRUSTED_PARENT
 *   - an intermediate reparse point (junction/symlink/mount)   -> UNTRUSTED_PARENT
 * It NEVER falls back to DeleteFileA or any pathname deletion.
 *
 * Trust proof for the Win32 (non-NT-relative) walk: every directory component is opened no-follow
 * (FILE_FLAG_OPEN_REPARSE_POINT), rejected if it is a reparse point, and ACL-checked default-deny;
 * the final parent handle is then HELD open without delete sharing (pinning it against rename/delete)
 * for the socket lifetime. Because no untrusted principal can delete/rename/re-permission any
 * component, and no component is a reparse point, later textual re-resolution of the leaf resolves to
 * the same chain; the FILE_ID_INFO check on the leaf is the belt-and-suspenders identity anchor.
 */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00   /* Windows 10: FILE_ID_INFO, FileDispositionInfoEx, AF_UNIX */
#endif

#include "unix_socket_node.h"
#include "socket.h"            /* KlSocketProvider seam: socket / bind / cloexec / close */
#include "sockaddr_native.h"

#include <winsock2.h>
#include <windows.h>
#include <afunix.h>
#include <aclapi.h>
#include <sddl.h>          /* ConvertStringSidToSidW (TrustedInstaller SID) */
#include <stdint.h>
#include <string.h>
#include <wchar.h>

/* FileDispositionInfoEx (FILE_INFO_BY_HANDLE_CLASS value 21) + flags postdate some toolchain
 * headers (mingw-w64 lacks them; the Windows SDK has them). Local names + the raw class value keep
 * the module compiling on both; the runtime behavior is identical. */
enum { KL_FileDispositionInfoEx = 21 };
#define KL_FILE_DISPOSITION_FLAG_DELETE          0x00000001u
#define KL_FILE_DISPOSITION_FLAG_POSIX_SEMANTICS 0x00000002u
typedef struct { DWORD Flags; } KL_FILE_DISPOSITION_INFO_EX;

#ifndef IO_REPARSE_TAG_AF_UNIX
#define IO_REPARSE_TAG_AF_UNIX (0x80000023L)
#endif

#define KL_UNIX_NODE_PATH_MAX 108   /* sun_path bound */

/* Per-bind state (opaque KL_UNIX_NODE_STORAGE bytes). A zeroed buffer (dir_open == 0) is a valid
 * "not open" state. Fixed storage, no allocation. */
typedef struct {
    int          dir_open;                  /* 1 = parent holds a live handle */
    HANDLE       parent;                    /* held, trust-validated, pinned parent directory */
    int          node_captured;             /* 1 = volser/file_id are valid */
    ULONGLONG    volser;                    /* FILE_ID_INFO.VolumeSerialNumber of the bound node */
    BYTE         file_id[16];               /* FILE_ID_INFO.FileId (128-bit) */
    char         path[KL_UNIX_NODE_PATH_MAX]; /* full socket path (bounded by sun_path) */
} KlUnixNodeState;

_Static_assert(sizeof(KlUnixNodeState) <= KL_UNIX_NODE_STORAGE,
               "KL_UNIX_NODE_STORAGE must bound KlUnixNodeState");

/* ── small helpers ─────────────────────────────────────────────────────────────────────────── */

static int widen(const char *s, wchar_t *out, int outcap) {
    int n = MultiByteToWideChar(CP_ACP, 0, s, -1, out, outcap);
    return n > 0;
}

/* Trust-domain SIDs: the process token user, LocalSystem, Administrators, TrustedInstaller. */
typedef struct TrustSids_ {
    PSID self; PSID system; PSID admins; PSID ti; void *self_buf;
} TrustSids;

/* Is a SID in the trust domain: the process token user, LocalSystem, Administrators, or
 * TrustedInstaller (system directories such as C:\ / C:\Windows are owned by TrustedInstaller). */
static int sid_is_trusted(PSID sid, PSID self, PSID system, PSID admins, PSID ti) {
    if (self && EqualSid(sid, self)) return 1;
    if (system && EqualSid(sid, system)) return 1;
    if (admins && EqualSid(sid, admins)) return 1;
    if (ti && EqualSid(sid, ti)) return 1;
    return 0;
}

/* Rights that let a principal SUBSTITUTE an existing entry (delete/rename it, or re-permission it so
 * they later can). Directory ADD rights (FILE_ADD_FILE == FILE_WRITE_DATA, FILE_ADD_SUBDIRECTORY ==
 * FILE_APPEND_DATA, and the GENERIC_WRITE that maps to them) are deliberately NOT here: creating a
 * new sibling cannot replace an already-validated component, and treating them as unsafe would fail
 * closed on nearly every real directory (C:\ grants Users create-files/create-folders). The
 * substitution vectors are: delete the entry (DELETE, or FILE_DELETE_CHILD on its parent) or take
 * over its ACL (WRITE_DAC / WRITE_OWNER); GENERIC_ALL subsumes all of these. */
#define KL_SUBSTITUTE_RIGHTS \
    (DELETE | WRITE_DAC | WRITE_OWNER | FILE_DELETE_CHILD | GENERIC_ALL)

/* Default-deny ACL check on an open handle (directory or the node). Returns 0 (trusted) or -1
 * (reject). Rejects: a NULL/absent DACL, an owner outside the trust domain, any ALLOW ace granting a
 * substitution right to an untrusted SID, and any ACE form we cannot evaluate (non-basic types are
 * treated as unsafe). */
static int acl_default_deny(HANDLE h, const struct TrustSids_ *t) {
    PSECURITY_DESCRIPTOR sd = NULL; PSID owner = NULL; PACL dacl = NULL;
    if (GetSecurityInfo(h, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                        &owner, NULL, &dacl, NULL, &sd) != ERROR_SUCCESS)
        return -1;
    int ok = 0;
    if (!owner || !sid_is_trusted(owner, t->self, t->system, t->admins, t->ti)) goto out;  /* owner has WRITE_DAC */
    if (!dacl) goto out;                                                     /* NULL DACL = everyone full */
    for (WORD i = 0; i < dacl->AceCount; i++) {
        void *raw = NULL;
        if (!GetAce(dacl, i, &raw)) goto out;
        const ACE_HEADER *hdr = (const ACE_HEADER *)raw;
        /* An INHERIT_ONLY ace confers NO access on this object; it only seeds what children inherit.
         * Skip it: the question here is exactly who can substitute THIS component. (The default C:\
         * DACL, for one, carries an inherit-only Modify for Authenticated Users, which does not grant
         * them any right on C:\ itself.) An effective (non-inherit-only) inherited ace is still
         * evaluated normally, so a directory that actually inherited a hostile grant is still caught. */
        if (hdr->AceFlags & INHERIT_ONLY_ACE) continue;
        if (hdr->AceType == ACCESS_ALLOWED_ACE_TYPE) {
            const ACCESS_ALLOWED_ACE *ace = (const ACCESS_ALLOWED_ACE *)raw;
            PSID sid = (PSID)&ace->SidStart;
            if (!IsValidSid(sid)) goto out;
            if ((ace->Mask & KL_SUBSTITUTE_RIGHTS) &&
                !sid_is_trusted(sid, t->self, t->system, t->admins, t->ti))
                goto out;   /* an untrusted principal can substitute here */
        } else if (hdr->AceType == ACCESS_DENIED_ACE_TYPE) {
            /* a DENY only reduces access; safe to ignore for a "who can substitute" analysis */
        } else {
            goto out;       /* object/conditional/compound ACE: cannot evaluate -> unsafe */
        }
    }
    ok = 1;
out:
    if (sd) LocalFree(sd);
    return ok ? 0 : -1;
}

/* Open a path no-follow (reparse point itself), backup semantics (directories), never following a
 * final reparse. Returns INVALID_HANDLE_VALUE on failure. */
static HANDLE open_nofollow(const wchar_t *wpath, DWORD access, DWORD share) {
    return CreateFileW(wpath, access, share, NULL, OPEN_EXISTING,
                       FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
}

static int is_reparse(HANDLE h) {
    BY_HANDLE_FILE_INFORMATION bi;
    if (!GetFileInformationByHandle(h, &bi)) return 1;   /* cannot tell -> treat as unsafe */
    return (bi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ? 1 : 0;
}
static int is_directory(HANDLE h) {
    BY_HANDLE_FILE_INFORMATION bi;
    if (!GetFileInformationByHandle(h, &bi)) return 0;
    return (bi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
}

/* NTFS + required volume capabilities on the volume of an open handle. Returns 0 or -1 (unsupported). */
static int volume_is_supported(HANDLE h) {
    wchar_t fsname[32]; DWORD serial = 0, maxcomp = 0, flags = 0;
    if (!GetVolumeInformationByHandleW(h, NULL, 0, &serial, &maxcomp, &flags, fsname, 32)) return -1;
    if (_wcsicmp(fsname, L"NTFS") != 0) return -1;
    if (!(flags & FILE_SUPPORTS_OPEN_BY_FILE_ID)) return -1;
    if (!(flags & FILE_SUPPORTS_REPARSE_POINTS)) return -1;
    return 0;
}

static int get_file_id(HANDLE h, ULONGLONG *volser, BYTE id[16]) {
    FILE_ID_INFO fi;
    if (!GetFileInformationByHandleEx(h, FileIdInfo, &fi, sizeof(fi))) return -1;
    *volser = fi.VolumeSerialNumber;
    memcpy(id, &fi.FileId, 16);
    return 0;
}

/* Delete strictly through the verified handle, POSIX semantics only (no legacy fallback, no name
 * deletion). Returns 0 or -1. */
static int delete_by_handle(HANDLE h) {
    KL_FILE_DISPOSITION_INFO_EX ex;
    ex.Flags = KL_FILE_DISPOSITION_FLAG_DELETE | KL_FILE_DISPOSITION_FLAG_POSIX_SEMANTICS;
    return SetFileInformationByHandle(h, (FILE_INFO_BY_HANDLE_CLASS)KL_FileDispositionInfoEx,
                                      &ex, sizeof(ex)) ? 0 : -1;
}

static int reparse_tag_is_afunix(HANDLE h, int *is_reparse_out) {
    BYTE buf[MAXIMUM_REPARSE_DATA_BUFFER_SIZE];
    DWORD got = 0;
    if (!DeviceIoControl(h, FSCTL_GET_REPARSE_POINT, NULL, 0, buf, sizeof(buf), &got, NULL)) {
        *is_reparse_out = 0;
        return 0;   /* not a reparse point (or cannot read): caller decides */
    }
    *is_reparse_out = 1;
    return (*(DWORD *)buf == (DWORD)IO_REPARSE_TAG_AF_UNIX) ? 1 : 0;
}

/* Build the trust-domain SIDs. Returns 1 if the token user resolved (required). */
static int trust_sids_init(TrustSids *t) {
    memset(t, 0, sizeof(*t));
    HANDLE tok = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        DWORD n = 0; GetTokenInformation(tok, TokenUser, NULL, 0, &n);
        if (n) {
            t->self_buf = LocalAlloc(LPTR, n);
            if (t->self_buf && GetTokenInformation(tok, TokenUser, t->self_buf, n, &n))
                t->self = ((TOKEN_USER *)t->self_buf)->User.Sid;
        }
        CloseHandle(tok);
    }
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    AllocateAndInitializeSid(&nt, 1, SECURITY_LOCAL_SYSTEM_RID, 0,0,0,0,0,0,0, &t->system);
    AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
                             0,0,0,0,0,0, &t->admins);
    /* TrustedInstaller owns C:\ / C:\Windows etc.; ConvertStringSidToSid allocates via LocalAlloc. */
    ConvertStringSidToSidW(L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464", &t->ti);
    return t->self != NULL;
}
static void trust_sids_free(TrustSids *t) {
    if (t->self_buf) LocalFree(t->self_buf);
    if (t->system) FreeSid(t->system);
    if (t->admins) FreeSid(t->admins);
    if (t->ti) LocalFree(t->ti);
}

/* Walk every component of the parent directory of @path, no-follow, rejecting reparse points and
 * ACL default-denying each; open + verify NTFS/caps on the final parent and HOLD it (no delete
 * sharing, pinning it). Returns OK (sets *out_parent) or a fail-closed status. */
static KlUnixNodeStatus walk_hold_parent(const char *path, const TrustSids *t, HANDLE *out_parent) {
    *out_parent = INVALID_HANDLE_VALUE;
    size_t len = strlen(path);
    if (len == 0 || len >= KL_UNIX_NODE_PATH_MAX) return KL_UNIX_NODE_ERR_INVALID_PATH;

    /* Find the parent portion (strip the final component). Accept both separators. */
    size_t cut = 0; int have = 0;
    for (size_t i = 0; i < len; i++) if (path[i] == '\\' || path[i] == '/') { cut = i; have = 1; }
    if (!have) return KL_UNIX_NODE_ERR_INVALID_PATH;                 /* need an anchored directory */
    if (cut + 1 >= len) return KL_UNIX_NODE_ERR_INVALID_PATH;        /* trailing separator: no leaf */

    /* Walk each directory prefix "root", "root\a", "root\a\b" ... up to the parent. */
    HANDLE held = INVALID_HANDLE_VALUE;
    size_t i = 0;
    while (i <= cut) {
        /* advance to the next separator (or the parent end) */
        size_t j = i;
        while (j <= cut && path[j] != '\\' && path[j] != '/') j++;
        /* prefix = path[0..j) ; for a drive root "C:" append a separator */
        char prefix[KL_UNIX_NODE_PATH_MAX + 2];
        size_t plen = j;
        if (plen == 0) { i = j + 1; continue; }
        memcpy(prefix, path, plen); prefix[plen] = '\0';
        if (plen == 2 && prefix[1] == ':') { prefix[2] = '\\'; prefix[3] = '\0'; }  /* "C:" -> "C:\" */
        if (strcmp(prefix, ".") == 0 || strcmp(prefix, "..") == 0) {
            if (held != INVALID_HANDLE_VALUE) CloseHandle(held);
            return KL_UNIX_NODE_ERR_INVALID_PATH;
        }
        wchar_t wprefix[KL_UNIX_NODE_PATH_MAX + 4];
        if (!widen(prefix, wprefix, (int)(sizeof(wprefix)/sizeof(wprefix[0])))) {
            if (held != INVALID_HANDLE_VALUE) CloseHandle(held);
            return KL_UNIX_NODE_ERR_INVALID_PATH;
        }
        /* Hold the FINAL parent without delete sharing (pin it); intermediates may allow delete
         * sharing (we only validate + close them). */
        int is_final = (j >= cut);
        DWORD share = is_final ? (FILE_SHARE_READ | FILE_SHARE_WRITE)
                               : (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
        HANDLE d = open_nofollow(wprefix, READ_CONTROL | FILE_READ_ATTRIBUTES, share);
        if (d == INVALID_HANDLE_VALUE) {
            if (held != INVALID_HANDLE_VALUE) CloseHandle(held);
            return KL_UNIX_NODE_ERR_UNTRUSTED_PARENT;
        }
        if (!is_directory(d) || is_reparse(d)) {   /* reject a non-dir or an intermediate reparse point */
            CloseHandle(d);
            if (held != INVALID_HANDLE_VALUE) CloseHandle(held);
            return KL_UNIX_NODE_ERR_UNTRUSTED_PARENT;
        }
        if (acl_default_deny(d, t) < 0) {
            CloseHandle(d);
            if (held != INVALID_HANDLE_VALUE) CloseHandle(held);
            return KL_UNIX_NODE_ERR_UNTRUSTED_PARENT;
        }
        if (held != INVALID_HANDLE_VALUE) CloseHandle(held);
        held = d;
        i = j + 1;
        if (j >= cut) break;
    }
    if (held == INVALID_HANDLE_VALUE) return KL_UNIX_NODE_ERR_INVALID_PATH;
    if (volume_is_supported(held) < 0) { CloseHandle(held); return KL_UNIX_NODE_ERR_UNSUPPORTED; }
    *out_parent = held;
    return KL_UNIX_NODE_OK;
}

static void close_dir(KlUnixNodeState *ns) {
    if (ns->dir_open && ns->parent != INVALID_HANDLE_VALUE) CloseHandle(ns->parent);
    ns->parent = INVALID_HANDLE_VALUE;
    ns->dir_open = 0;
    ns->node_captured = 0;
    ns->path[0] = '\0';
}

/* Open the leaf no-follow (with DELETE) via its full path. Safe under the walk's trust proof; the
 * caller verifies identity before acting. */
static HANDLE open_leaf(const char *path, DWORD access) {
    wchar_t wp[KL_UNIX_NODE_PATH_MAX + 4];
    if (!widen(path, wp, (int)(sizeof(wp)/sizeof(wp[0])))) return INVALID_HANDLE_VALUE;
    return open_nofollow(wp, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
}

/* Create + bind the AF_UNIX socket at @path through the provider (creates the node). */
static int create_and_bind(const KlSocketProvider *sockets, const char *path, size_t path_len,
                           KlSocketHandle *out_fd) {
    KlSocketHandle fd = kl_sock_socket(sockets, AF_UNIX, SOCK_STREAM, 0);
    if (!kl_handle_valid(fd)) return -1;
    kl_sock_set_cloexec(sockets, fd);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, path_len + 1);
    socklen_t addr_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + path_len + 1);
    KlSockAddr sa;
    kl_sockaddr_from_native(&sa, (struct sockaddr *)&addr, addr_len);
    if (kl_sock_bind(sockets, fd, &sa) < 0) { kl_sock_close(sockets, fd); return -1; }
    *out_fd = fd;
    return 0;
}

/* ── public contract ───────────────────────────────────────────────────────────────────────── */

void kl_unix_socket_node_init(void *state) {
    KlUnixNodeState *ns = state;
    memset(ns, 0, sizeof(*ns));
    ns->parent = INVALID_HANDLE_VALUE;
}

KlUnixNodeStatus kl_unix_socket_node_bind(const KlUnixNodePolicy *policy,
                                          const KlSocketProvider *sockets,
                                          KlAllocator *alloc, void *state,
                                          KlSocketHandle *out_fd, int *out_errno) {
    KlUnixNodeState *ns = state;
    (void)alloc;   /* Windows AF_UNIX has no uid/gid/mode model: owner/group/mode are ignored. */
    if (out_errno) *out_errno = 0;
    if (ns->dir_open) { if (out_errno) *out_errno = ERROR_ALREADY_EXISTS; return KL_UNIX_NODE_ERR_BIND; }
    if (out_fd) *out_fd = KL_INVALID_SOCKET;

    const char *path = policy->path;
    if (!path || path[0] == '\0') return KL_UNIX_NODE_ERR_INVALID_PATH;
    size_t path_len = strlen(path);
    if (path_len >= sizeof(((struct sockaddr_un *)0)->sun_path)) return KL_UNIX_NODE_ERR_INVALID_PATH;

    TrustSids t;
    if (!trust_sids_init(&t)) { trust_sids_free(&t); return KL_UNIX_NODE_ERR_UNTRUSTED_PARENT; }

    HANDLE parent = INVALID_HANDLE_VALUE;
    KlUnixNodeStatus w = walk_hold_parent(path, &t, &parent);
    if (w != KL_UNIX_NODE_OK) { trust_sids_free(&t); return w; }

    ns->parent = parent;
    ns->dir_open = 1;
    ns->node_captured = 0;
    memcpy(ns->path, path, path_len + 1);

    /* Reclaim a stale node (opt-in): it must be an AF_UNIX reparse node (not an arbitrary file), and
     * removable only through its own verified handle. Fail closed on anything else. */
    if (policy->unlink_stale) {
        HANDLE leaf = open_leaf(path, DELETE | FILE_READ_ATTRIBUTES);
        if (leaf == INVALID_HANDLE_VALUE) {
            DWORD e = GetLastError();
            if (e != ERROR_FILE_NOT_FOUND && e != ERROR_PATH_NOT_FOUND) {
                if (out_errno) *out_errno = (int)e;
                trust_sids_free(&t);
                close_dir(ns);
                return KL_UNIX_NODE_ERR_FOREIGN_NODE;
            }
            /* absent: nothing to reclaim */
        } else {
            int is_rp = 0;
            int afunix = reparse_tag_is_afunix(leaf, &is_rp);
            if (!is_rp || !afunix) {   /* present but not an AF_UNIX node: refuse */
                CloseHandle(leaf); trust_sids_free(&t); close_dir(ns);
                return KL_UNIX_NODE_ERR_FOREIGN_NODE;
            }
            int drc = delete_by_handle(leaf);
            DWORD de = GetLastError();
            CloseHandle(leaf);
            if (drc < 0) {
                if (out_errno) *out_errno = (int)de;
                trust_sids_free(&t);
                close_dir(ns);
                return KL_UNIX_NODE_ERR_BIND;
            }
        }
    }

    if (create_and_bind(sockets, path, path_len, out_fd) < 0) {
        int e = WSAGetLastError();
        trust_sids_free(&t); close_dir(ns);
        if (out_errno) *out_errno = e;
        return KL_UNIX_NODE_ERR_BIND;
    }

    /* Capture the bound node identity immediately, via a no-follow handle. */
    {
        HANDLE leaf = open_leaf(path, FILE_READ_ATTRIBUTES);
        int okid = 0;
        if (leaf != INVALID_HANDLE_VALUE) {
            int is_rp = 0, afunix = reparse_tag_is_afunix(leaf, &is_rp);
            if (is_rp && afunix && get_file_id(leaf, &ns->volser, ns->file_id) == 0) okid = 1;
            CloseHandle(leaf);
        }
        if (!okid) {
            if (out_errno) *out_errno = (int)GetLastError();
            /* remove the node we just bound (by verified... we have no id; best-effort via a fresh
             * no-follow handle that we do NOT trust for identity: only close the socket + leave the
             * node for the fail-closed teardown). Simpler: refuse and let teardown skip (no id). */
            kl_sock_close(sockets, *out_fd); *out_fd = KL_INVALID_SOCKET;
            trust_sids_free(&t); close_dir(ns);
            return KL_UNIX_NODE_ERR_BIND;
        }
        ns->node_captured = 1;
    }

    trust_sids_free(&t);
    return KL_UNIX_NODE_OK;
}

KlUnixNodeStatus kl_unix_socket_node_teardown(void *state, int unlink_stale) {
    KlUnixNodeState *ns = state;
    KlUnixNodeStatus rc = KL_UNIX_NODE_OK;
    if (unlink_stale && ns->dir_open && ns->node_captured && ns->path[0]) {
        TrustSids t;
        if (!trust_sids_init(&t)) { trust_sids_free(&t); rc = KL_UNIX_NODE_ERR_UNTRUSTED_PARENT; goto done; }
        /* Re-verify the held parent still passes trust (it is pinned, but re-check defends against a
         * re-permissioning we did not gate). */
        if (acl_default_deny(ns->parent, &t) < 0 ||
            volume_is_supported(ns->parent) < 0) {
            trust_sids_free(&t); rc = KL_UNIX_NODE_ERR_UNTRUSTED_PARENT; goto done;
        }
        HANDLE leaf = open_leaf(ns->path, DELETE | FILE_READ_ATTRIBUTES);
        if (leaf == INVALID_HANDLE_VALUE) {
            trust_sids_free(&t); rc = KL_UNIX_NODE_ERR_FOREIGN_NODE; goto done;   /* gone or unopenable */
        }
        ULONGLONG vs = 0; BYTE id[16];
        int is_rp = 0, afunix = reparse_tag_is_afunix(leaf, &is_rp);
        if (!is_rp || !afunix || get_file_id(leaf, &vs, id) < 0 ||
            vs != ns->volser || memcmp(id, ns->file_id, 16) != 0) {
            CloseHandle(leaf); trust_sids_free(&t); rc = KL_UNIX_NODE_ERR_FOREIGN_NODE; goto done;
        }
        if (delete_by_handle(leaf) < 0) rc = KL_UNIX_NODE_ERR_BIND;
        CloseHandle(leaf);
        trust_sids_free(&t);
    }
done:
    close_dir(ns);
    return rc;
}
