/*
 * win_unix_node_probe.c - TEMPORARY diagnostic for the Windows AF_UNIX-cleanup spike (issue #250).
 *
 * NOT production code and NOT a permanent test. It records whether Windows can perform an
 * identity-anchored delete of an AF_UNIX socket node (no-follow handle + stable file identity +
 * delete-by-handle), under the acceptance conditions in
 * docs/unix_socket_cleanup_windows_spike.md. It ALWAYS exits 0: unsupported behavior is a recorded
 * result, not a job failure. It touches only its own temp directory on the runner's filesystem
 * (expected NTFS) and makes no claims about ReFS/SMB/FAT. Removed before the security PR is
 * made mergeable.
 */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00   /* Windows 10: FILE_ID_INFO, FileDispositionInfoEx, AF_UNIX */
#endif

#include <winsock2.h>
#include <windows.h>
#include <afunix.h>
#include <aclapi.h>
#include <sddl.h>
#include <winternl.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "advapi32.lib")

/* FileDispositionInfoEx (FILE_INFO_BY_HANDLE_CLASS value 21) and its struct/flags postdate some
 * toolchain headers (mingw-w64 lacks them; the Windows SDK has them). Use local names + the raw
 * class value so the probe compiles on both; the runtime behavior is identical (the OS interprets
 * class 21 + the flags). */
enum { KL_FileDispositionInfoEx = 21 };
#define KL_FILE_DISPOSITION_FLAG_DELETE          0x00000001u
#define KL_FILE_DISPOSITION_FLAG_POSIX_SEMANTICS 0x00000002u
typedef struct { DWORD Flags; } KL_FILE_DISPOSITION_INFO_EX;

static void line(void) { printf("----------------------------------------------------------------\n"); }
static void rec(const char *probe, const char *result, const char *detail) {
    printf("PROBE %-34s : %-10s %s\n", probe, result, detail ? detail : "");
    fflush(stdout);
}
static const char *werr(DWORD e) {
    static char b[256];
    DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, e,
                             0, b, sizeof(b) - 1, NULL);
    while (n && (b[n-1] == '\n' || b[n-1] == '\r')) b[--n] = 0;
    if (!n) snprintf(b, sizeof(b), "error %lu", (unsigned long)e);
    return b;
}

/* ── environment ─────────────────────────────────────────────────────────────────────────── */
static void report_environment(const char *workdir) {
    line();
    printf("== Environment ==\n");

    /* OS version via RtlGetVersion (not subject to manifest shimming). */
    typedef LONG (WINAPI *RtlGetVersion_t)(PRTL_OSVERSIONINFOW);
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    RtlGetVersion_t rgv = nt ? (RtlGetVersion_t)GetProcAddress(nt, "RtlGetVersion") : NULL;
    if (rgv) {
        RTL_OSVERSIONINFOW vi; memset(&vi, 0, sizeof(vi)); vi.dwOSVersionInfoSize = sizeof(vi);
        rgv(&vi);
        printf("  Windows version   : %lu.%lu build %lu\n",
               (unsigned long)vi.dwMajorVersion, (unsigned long)vi.dwMinorVersion,
               (unsigned long)vi.dwBuildNumber);
    } else {
        printf("  Windows version   : (RtlGetVersion unavailable)\n");
    }
    printf("  Compiled _WIN32_WINNT: 0x%04X\n", (unsigned)_WIN32_WINNT);

    /* Filesystem + volume capabilities of the work directory's volume. */
    char root[MAX_PATH]; char volname[MAX_PATH]; char fsname[MAX_PATH];
    DWORD serial = 0, maxcomp = 0, flags = 0;
    if (GetVolumePathNameA(workdir, root, sizeof(root)) &&
        GetVolumeInformationA(root, volname, sizeof(volname), &serial, &maxcomp, &flags,
                              fsname, sizeof(fsname))) {
        printf("  Volume root        : %s\n", root);
        printf("  Filesystem         : %s (serial %08lX)\n", fsname, (unsigned long)serial);
        printf("  Volume flags       : 0x%08lX%s%s%s\n", (unsigned long)flags,
               (flags & FILE_SUPPORTS_OPEN_BY_FILE_ID) ? " OPEN_BY_FILE_ID" : "",
               (flags & FILE_SUPPORTS_REPARSE_POINTS)  ? " REPARSE_POINTS"  : "",
               (flags & FILE_SUPPORTS_HARD_LINKS)      ? " HARD_LINKS"      : "");
        printf("  NOTE: only this filesystem is probed; no ReFS/SMB/FAT conclusions are drawn.\n");
    } else {
        printf("  Volume info        : unavailable (%s)\n", werr(GetLastError()));
    }
}

/* ── token privileges that can bypass ACLs ───────────────────────────────────────────────── */
static void report_token_privileges(void) {
    line();
    printf("== Process token privileges (ACL-bypassing) ==\n");
    HANDLE tok = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        printf("  (OpenProcessToken failed: %s)\n", werr(GetLastError()));
        return;
    }
    static const char *names[] = { "SeBackupPrivilege", "SeRestorePrivilege",
                                   "SeTakeOwnershipPrivilege", "SeSecurityPrivilege" };
    for (size_t i = 0; i < sizeof(names)/sizeof(names[0]); i++) {
        LUID luid; PRIVILEGE_SET ps; BOOL has = FALSE;
        if (LookupPrivilegeValueA(NULL, names[i], &luid)) {
            ps.PrivilegeCount = 1; ps.Control = PRIVILEGE_SET_ALL_NECESSARY;
            ps.Privilege[0].Luid = luid; ps.Privilege[0].Attributes = 0;
            PrivilegeCheck(tok, &ps, &has);
            printf("  %-26s : %s\n", names[i], has ? "PRESENT+ENABLED" : "absent-or-disabled");
        }
    }
    CloseHandle(tok);
    printf("  NOTE: a token holding SeRestore/SeBackup/SeTakeOwnership can override the DACL trust check.\n");
}

/* ── ACL analysis of a directory (owner + who can write/delete-child/change-DACL) ─────────── */
static void report_dir_acl(const char *dir) {
    line();
    printf("== Parent-directory ACL analysis: %s ==\n", dir);
    PSECURITY_DESCRIPTOR sd = NULL; PSID owner = NULL; PACL dacl = NULL;
    DWORD rc = GetNamedSecurityInfoA(dir, SE_FILE_OBJECT,
                    OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                    &owner, NULL, &dacl, NULL, &sd);
    if (rc != ERROR_SUCCESS) { printf("  (GetNamedSecurityInfo failed: %s)\n", werr(rc)); return; }

    char *ostr = NULL;
    if (owner && ConvertSidToStringSidA(owner, &ostr)) { printf("  Owner SID : %s\n", ostr); LocalFree(ostr); }

    if (!dacl) { printf("  DACL      : NULL (no protection)\n"); LocalFree(sd); return; }
    printf("  DACL ACEs : %u\n", (unsigned)dacl->AceCount);
    for (WORD i = 0; i < dacl->AceCount; i++) {
        ACCESS_ALLOWED_ACE *ace = NULL;
        if (!GetAce(dacl, i, (void **)&ace)) continue;
        PSID sid = (PSID)&ace->SidStart;
        char *ss = NULL; ConvertSidToStringSidA(sid, &ss);
        ACCESS_MASK m = ace->Mask;
        const char *kind = (ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE) ? "ALLOW"
                         : (ace->Header.AceType == ACCESS_DENIED_ACE_TYPE)  ? "DENY " : "OTHER";
        int inherited = (ace->Header.AceFlags & INHERITED_ACE) ? 1 : 0;
        printf("    [%2u] %s %-46s mask 0x%08lX%s%s%s%s%s\n", (unsigned)i, kind, ss ? ss : "(sid?)",
               (unsigned long)m,
               (m & WRITE_DAC)         ? " WRITE_DAC" : "",
               (m & WRITE_OWNER)       ? " WRITE_OWNER" : "",
               (m & DELETE)            ? " DELETE" : "",
               (m & FILE_DELETE_CHILD) ? " DELETE_CHILD" : "",
               inherited ? " (inherited)" : "");
        if (ss) LocalFree(ss);
    }
    printf("  NOTE: a non-trust-domain SID with WRITE_DAC/WRITE_OWNER can grant itself write later,\n");
    printf("        so 'lacks write now' is NOT sufficient; the trust check must weigh these rights.\n");
    LocalFree(sd);
}

/* ── core: bind an AF_UNIX socket at a path, returning the SOCKET (INVALID on failure) ─────── */
static SOCKET bind_unix(const char *path) {
    SOCKET s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) { rec("create AF_UNIX socket", "NO", werr(WSAGetLastError())); return INVALID_SOCKET; }
    SOCKADDR_UN a; memset(&a, 0, sizeof(a)); a.sun_family = AF_UNIX;
    strncpy(a.sun_path, path, sizeof(a.sun_path) - 1);
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) == SOCKET_ERROR) {
        rec("bind AF_UNIX socket", "NO", werr(WSAGetLastError())); closesocket(s); return INVALID_SOCKET;
    }
    return s;
}

/* Open a node no-follow with DELETE + full sharing (incl. delete). */
static HANDLE open_node_nofollow_delete(const char *path) {
    return CreateFileA(path, DELETE | READ_CONTROL | FILE_READ_ATTRIBUTES,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                       FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
}

typedef struct { unsigned long long volser; unsigned char id[16]; int ok; } NodeId;

static NodeId get_file_id(HANDLE h) {
    NodeId r; memset(&r, 0, sizeof(r));
    FILE_ID_INFO fi;
    if (GetFileInformationByHandleEx(h, FileIdInfo, &fi, sizeof(fi))) {
        r.volser = fi.VolumeSerialNumber; memcpy(r.id, &fi.FileId, 16); r.ok = 1;
    }
    return r;
}

static int delete_by_handle_posix(HANDLE h, const char **how) {
    /* Preferred: POSIX-semantics immediate delete (Win10 1709+ / NTFS). */
    KL_FILE_DISPOSITION_INFO_EX ex; memset(&ex, 0, sizeof(ex));
    ex.Flags = KL_FILE_DISPOSITION_FLAG_DELETE | KL_FILE_DISPOSITION_FLAG_POSIX_SEMANTICS;
    if (SetFileInformationByHandle(h, (FILE_INFO_BY_HANDLE_CLASS)KL_FileDispositionInfoEx, &ex, sizeof(ex))) { *how = "FileDispositionInfoEx(POSIX)"; return 1; }
    DWORD e1 = GetLastError();
    /* Fallback: legacy delete-on-close. */
    FILE_DISPOSITION_INFO di; di.DeleteFile = TRUE;
    if (SetFileInformationByHandle(h, FileDispositionInfo, &di, sizeof(di))) { *how = "FileDispositionInfo(on-close)"; return 2; }
    static char b[300]; snprintf(b, sizeof(b), "Ex=%s ; legacy=%s", werr(e1), werr(GetLastError()));
    *how = b; return 0;
}

int main(void) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) { printf("WSAStartup failed\n"); return 0; }

    char tmp[MAX_PATH]; GetTempPathA(sizeof(tmp), tmp);
    char work[MAX_PATH]; snprintf(work, sizeof(work), "%skeel-winprobe-%lu", tmp, (unsigned long)GetCurrentProcessId());
    CreateDirectoryA(work, NULL);

    report_environment(work);
    report_token_privileges();
    report_dir_acl(work);

    line();
    printf("== Identity-anchored delete probes ==\n");

    char sockpath[MAX_PATH]; snprintf(sockpath, sizeof(sockpath), "%s\\x.sock", work);
    DeleteFileA(sockpath);

    /* Bind a real AF_UNIX node. */
    SOCKET s = bind_unix(sockpath);
    if (s == INVALID_SOCKET) { printf("cannot bind AF_UNIX; remaining node probes skipped\n"); goto done; }
    rec("bind AF_UNIX node", "OK", sockpath);

    /* Is the node a reparse point, and what tag? */
    {
        DWORD attr = GetFileAttributesA(sockpath);
        char d[96];
        snprintf(d, sizeof(d), "attrs=0x%08lX reparse=%s", (unsigned long)attr,
                 (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT)) ? "yes" : "no");
        rec("node file attributes", "INFO", d);
    }

    /* Sharing conflict while the socket is live. */
    {
        HANDLE h = open_node_nofollow_delete(sockpath);
        if (h != INVALID_HANDLE_VALUE) { rec("open node while socket live", "OK", "handle acquired"); CloseHandle(h); }
        else rec("open node while socket live", "NO", werr(GetLastError()));
    }

    /* Open no-follow, capture identity, delete by handle. Close the socket first so it is
     * a stale node (the real teardown case), but keep it a valid AF_UNIX filesystem entry. */
    closesocket(s); s = INVALID_SOCKET;
    NodeId id_bind; memset(&id_bind, 0, sizeof(id_bind));
    {
        HANDLE h = open_node_nofollow_delete(sockpath);
        if (h == INVALID_HANDLE_VALUE) {
            rec("open stale node no-follow+DELETE", "NO", werr(GetLastError()));
        } else {
            rec("open stale node no-follow+DELETE", "OK", NULL);
            id_bind = get_file_id(h);
            rec("GetFileInformationByHandleEx FileIdInfo", id_bind.ok ? "OK" : "NO", NULL);
            const char *how = NULL;
            int d = delete_by_handle_posix(h, &how);
            rec("delete by verified handle", d ? "OK" : "NO", how);
            CloseHandle(h);
            char d2[64]; snprintf(d2, sizeof(d2), "%s", (GetFileAttributesA(sockpath) == INVALID_FILE_ATTRIBUTES) ? "node removed" : "node still present");
            rec("node removed after handle delete", (GetFileAttributesA(sockpath) == INVALID_FILE_ATTRIBUTES) ? "OK" : "NO", d2);
        }
    }

    /* FILE_ID stability - rebind and confirm the id differs from the deleted node (distinctness),
     * and that a re-open of the same live node yields a stable id across two opens. */
    DeleteFileA(sockpath);
    s = bind_unix(sockpath);
    if (s != INVALID_SOCKET) {
        HANDLE h1 = open_node_nofollow_delete(sockpath);
        HANDLE h2 = open_node_nofollow_delete(sockpath);
        if (h1 != INVALID_HANDLE_VALUE && h2 != INVALID_HANDLE_VALUE) {
            NodeId a = get_file_id(h1), b = get_file_id(h2);
            int stable = a.ok && b.ok && a.volser == b.volser && memcmp(a.id, b.id, 16) == 0;
            int distinct = id_bind.ok && a.ok && !(a.volser == id_bind.volser && memcmp(a.id, id_bind.id, 16) == 0);
            rec("FILE_ID stable across two opens", stable ? "OK" : "NO", NULL);
            rec("FILE_ID distinct from prior node", distinct ? "OK" : "INFO", NULL);
        }
        if (h1 != INVALID_HANDLE_VALUE) CloseHandle(h1);
        if (h2 != INVALID_HANDLE_VALUE) CloseHandle(h2);
        closesocket(s); s = INVALID_SOCKET;
    }

    /* Teardown after external deletion - the node is gone; a no-follow open must fail cleanly. */
    DeleteFileA(sockpath);
    {
        HANDLE h = open_node_nofollow_delete(sockpath);
        if (h == INVALID_HANDLE_VALUE) rec("open after external deletion", "OK", werr(GetLastError()));
        else { rec("open after external deletion", "NO", "unexpectedly opened"); CloseHandle(h); }
    }

    /* Intermediate junction - create dir real/, junction link/ -> real/, bind under link/sub, and
     * report whether a full-path open with OPEN_REPARSE_POINT traverses the intermediate junction
     * (it does: OPEN_REPARSE_POINT affects only the FINAL component), motivating per-component handling. */
    {
        char real[MAX_PATH], link[MAX_PATH], sub[MAX_PATH], cmd[MAX_PATH*3];
        snprintf(real, sizeof(real), "%s\\real", work);
        snprintf(link, sizeof(link), "%s\\link", work);
        CreateDirectoryA(real, NULL);
        snprintf(sub, sizeof(sub), "%s\\real\\sub", work); CreateDirectoryA(sub, NULL);
        snprintf(cmd, sizeof(cmd), "cmd /c mklink /J \"%s\" \"%s\" >nul 2>&1", link, real);
        int made = (system(cmd) == 0);
        if (made) {
            char via[MAX_PATH]; snprintf(via, sizeof(via), "%s\\link\\sub", work);
            HANDLE h = CreateFileA(via, READ_CONTROL, FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                                   NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS, NULL);
            rec("full-path open traverses intermediate junction",
                (h != INVALID_HANDLE_VALUE) ? "YES" : "NO",
                "OPEN_REPARSE_POINT guards only the final component; per-component walk required");
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
            snprintf(cmd, sizeof(cmd), "cmd /c rmdir \"%s\" >nul 2>&1", link); system(cmd);
        } else {
            rec("create intermediate junction", "SKIP", "mklink /J unavailable");
        }
        RemoveDirectoryA(sub); RemoveDirectoryA(real);
    }

done:
    DeleteFileA(sockpath);
    RemoveDirectoryA(work);
    line();
    printf("== Probe complete (exit 0 regardless of individual results) ==\n");
    WSACleanup();
    return 0;
}
