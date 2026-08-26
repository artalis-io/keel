/* Windows unit tests for the identity-anchored AF_UNIX node module (src/unix_socket_node_win.c).
 * Built and run only on Windows (excluded from the POSIX wildcard; enrolled in WIN_TEST_SUITES).
 * Uses the winsock socket provider + the neutral module contract, with Win32 / mklink / icacls for
 * directory + reparse + ACL setup. Deterministic (no timing races). */
#include "utest.h"
#include "unix_socket_node.h"
#include "socket.h"            /* kl_sock_close (internal seam, via -Isrc) */
#include <keel/allocator.h>
#include <keel/socket.h>       /* kl_socket_provider_winsock */
#include <keel/handle.h>

#include <winsock2.h>
#include <windows.h>
#include <afunix.h>
#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>            /* SHGetFolderPathA + CSIDL_LOCAL_APPDATA (native per-user base) */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifndef IO_REPARSE_TAG_AF_UNIX
#define IO_REPARSE_TAG_AF_UNIX (0x80000023L)
#endif

/* The command helpers below always receive a literal format string; silence the (false) warning. */
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
/* Path builders compose bounded native paths (well under MAX_PATH) into MAX_PATH buffers; GCC -O2
 * cannot prove the source bound and warns about truncation. Deterministic test scaffolding. */
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif

typedef union { unsigned char b[KL_UNIX_NODE_STORAGE]; double _a; void *_p; } NodeStore;

static const KlSocketProvider *sk(void) { return kl_socket_provider_winsock(); }

static void run_cmd(const char *fmt, const char *a) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), fmt, a);
    system(cmd);
}
static void run_cmd2(const char *fmt, const char *a, const char *b) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), fmt, a, b);
    system(cmd);
}

/* The current process token user's SID as a string (for the protective grant on the test dir). */
static int current_user_sid_str(char *out, size_t n) {
    HANDLE tok = NULL; int ok = 0;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        DWORD sz = 0; GetTokenInformation(tok, TokenUser, NULL, 0, &sz);
        TOKEN_USER *tu = sz ? (TOKEN_USER *)malloc(sz) : NULL;
        if (tu && GetTokenInformation(tok, TokenUser, tu, sz, &sz)) {
            char *s = NULL;
            if (ConvertSidToStringSidA(tu->User.Sid, &s) && s) {
                snprintf(out, n, "%s", s);
                LocalFree(s);
                ok = 1;
            }
        }
        free(tu);
        CloseHandle(tok);
    }
    return ok;
}

/* The native per-user base directory (FOLDERID_LocalAppData), NOT MSYS2 %TEMP% (which can be a
 * world-writable tree the hardened walk correctly refuses). Returns 1 on success. */
static int local_appdata(char *out, size_t n) {
    char buf[MAX_PATH];
    if (SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, SHGFP_TYPE_CURRENT, buf) != S_OK)
        return 0;
    snprintf(out, n, "%s", buf);
    return 1;
}

/* Dump an open handle's owner + every ACE (type/flags/mask/SID). Used to fail LOUDLY with an ACL
 * diagnostic if the runner's native per-user chain is unexpectedly unsafe (never a silent skip). */
static void dump_acl(const char *label, HANDLE h) {
    PSECURITY_DESCRIPTOR sd = NULL; PSID owner = NULL; PACL dacl = NULL;
    DWORD r = GetSecurityInfo(h, SE_FILE_OBJECT,
                              OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                              &owner, NULL, &dacl, NULL, &sd);
    if (r != ERROR_SUCCESS) { printf("  [%s] GetSecurityInfo FAILED %lu\n", label, r); return; }
    char *os = NULL; if (owner) ConvertSidToStringSidA(owner, &os);
    printf("  [%s] owner=%s aces=%u\n", label, os ? os : "(null)", dacl ? dacl->AceCount : 0u);
    if (os) LocalFree(os);
    if (dacl) for (WORD i = 0; i < dacl->AceCount; i++) {
        void *raw = NULL; if (!GetAce(dacl, i, &raw)) continue;
        ACE_HEADER *hd = (ACE_HEADER *)raw;
        if (hd->AceType == ACCESS_ALLOWED_ACE_TYPE || hd->AceType == ACCESS_DENIED_ACE_TYPE) {
            ACCESS_ALLOWED_ACE *a = (ACCESS_ALLOWED_ACE *)raw;   /* DENY shares the layout */
            char *ss = NULL; ConvertSidToStringSidA((PSID)&a->SidStart, &ss);
            printf("    ACE[%u] %s flags=0x%02x mask=0x%08lx sid=%s\n", i,
                   hd->AceType == ACCESS_ALLOWED_ACE_TYPE ? "ALLOW" : "DENY ",
                   (unsigned)hd->AceFlags, (unsigned long)a->Mask, ss ? ss : "?");
            if (ss) LocalFree(ss);
        } else {
            printf("    ACE[%u] type=%u flags=0x%02x (non-basic)\n",
                   i, (unsigned)hd->AceType, (unsigned)hd->AceFlags);
        }
    }
    if (sd) LocalFree(sd);
}

/* A fresh, TRUSTED work directory under the native per-user base (LocalAppData): inheritance is
 * removed and a protected DACL grants ONLY the current user, SYSTEM, and Administrators. This gives
 * a directory whose whole ancestor chain is trusted (LocalAppData's chain is per-user), which the
 * hardened bind requires. mk_trusted_base_or_fail() below verifies that chain against the production
 * trust check before the happy-path tests rely on it. */
static void mk_workdir(char *buf, size_t n, const char *name) {
    char base[MAX_PATH];
    if (!local_appdata(base, sizeof(base))) { buf[0] = '\0'; return; }
    snprintf(buf, n, "%s\\keel-nodetest-%lu-%s", base, (unsigned long)GetCurrentProcessId(), name);
    run_cmd("cmd /c rmdir /s /q \"%s\" >nul 2>&1", buf);
    CreateDirectoryA(buf, NULL);
    char usid[128];
    if (current_user_sid_str(usid, sizeof(usid))) {
        run_cmd2("cmd /c icacls \"%s\" /inheritance:r /grant:r *%s:(OI)(CI)(F) "
                 "/grant:r *S-1-5-18:(OI)(CI)(F) /grant:r *S-1-5-32-544:(OI)(CI)(F) >nul 2>&1",
                 buf, usid);
    }
}
static void rm_workdir(const char *dir) { run_cmd("cmd /c rmdir /s /q \"%s\" >nul 2>&1", dir); }

/* Verify the native per-user base's ancestor chain passes the PRODUCTION trust check before the
 * happy-path tests rely on it. If it does not (an unexpectedly unsafe runner), fail LOUDLY with a
 * per-component ACL diagnostic rather than silently skipping the security tests. */
UTEST(unix_node_win, aa_native_trusted_chain_available) {
    WSADATA w; WSAStartup(MAKEWORD(2,2), &w);
    NodeStore st; kl_unix_socket_node_init(&st);
    KlAllocator al = kl_allocator_default();
    char dir[MAX_PATH]; mk_workdir(dir, sizeof(dir), "verify");
    ASSERT_NE(0, dir[0]);   /* LocalAppData resolved */
    char path[MAX_PATH]; snprintf(path, sizeof(path), "%s\\x.sock", dir);

    KlUnixNodePolicy pol = { .path = path, .unlink_stale = 1 };
    KlSocketHandle fd = KL_INVALID_SOCKET; int err = 0;
    KlUnixNodeStatus s = kl_unix_socket_node_bind(&pol, sk(), &al, &st, &fd, &err);
    if (s != KL_UNIX_NODE_OK) {
        printf("native per-user chain UNSAFE: bind status=%d err=%d path=%s\n", (int)s, err, path);
        size_t len = strlen(path), cut = 0;
        for (size_t k = 0; k < len; k++) if (path[k] == '\\' || path[k] == '/') cut = k;
        size_t i = 0;
        while (i <= cut) {
            size_t j = i; while (j <= cut && path[j] != '\\' && path[j] != '/') j++;
            size_t plen = j; if (plen == 0) { i = j + 1; continue; }
            char prefix[MAX_PATH]; memcpy(prefix, path, plen); prefix[plen] = '\0';
            if (plen == 2 && prefix[1] == ':') { prefix[2] = '\\'; prefix[3] = '\0'; }
            wchar_t wp[MAX_PATH]; MultiByteToWideChar(CP_ACP, 0, prefix, -1, wp, MAX_PATH);
            HANDLE d = CreateFileW(wp, READ_CONTROL | FILE_READ_ATTRIBUTES,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                                   OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
            if (d != INVALID_HANDLE_VALUE) { dump_acl(prefix, d); CloseHandle(d); }
            else printf("  [%s] open FAILED gle=%lu\n", prefix, (unsigned long)GetLastError());
            i = j + 1; if (j >= cut) break;
        }
    }
    ASSERT_EQ(KL_UNIX_NODE_OK, s);   /* the security tests must be exercised, never silently skipped */
    if (kl_handle_valid(fd)) { kl_sock_close(sk(), fd); kl_unix_socket_node_teardown(&st, 1); }
    rm_workdir(dir);
    WSACleanup();
}

/* Safe creation + owned-node removal (the verified happy path on local NTFS). */
UTEST(unix_node_win, safe_create_and_teardown) {
    WSADATA w; WSAStartup(MAKEWORD(2,2), &w);
    NodeStore st; kl_unix_socket_node_init(&st);
    KlAllocator al = kl_allocator_default();
    char dir[MAX_PATH]; mk_workdir(dir, sizeof(dir), "ok");
    char path[MAX_PATH]; snprintf(path, sizeof(path), "%s\\x.sock", dir);

    KlUnixNodePolicy pol = { .path = path, .unlink_stale = 1 };
    KlSocketHandle fd = KL_INVALID_SOCKET; int err = -1;
    ASSERT_EQ(KL_UNIX_NODE_OK, kl_unix_socket_node_bind(&pol, sk(), &al, &st, &fd, &err));
    ASSERT_TRUE(kl_handle_valid(fd));
    ASSERT_NE(INVALID_FILE_ATTRIBUTES, GetFileAttributesA(path));   /* node exists */

    kl_sock_close(sk(), fd);
    ASSERT_EQ(KL_UNIX_NODE_OK, kl_unix_socket_node_teardown(&st, 1));
    ASSERT_EQ(INVALID_FILE_ATTRIBUTES, GetFileAttributesA(path));   /* removed */
    rm_workdir(dir);
    WSACleanup();
}

/* Repeated bind on an open state is rejected with a deterministic error and does not clobber fd. */
UTEST(unix_node_win, repeated_bind_rejected) {
    WSADATA w; WSAStartup(MAKEWORD(2,2), &w);
    NodeStore st; kl_unix_socket_node_init(&st);
    KlAllocator al = kl_allocator_default();
    char dir[MAX_PATH]; mk_workdir(dir, sizeof(dir), "rep");
    char path[MAX_PATH]; snprintf(path, sizeof(path), "%s\\x.sock", dir);

    KlUnixNodePolicy pol = { .path = path, .unlink_stale = 1 };
    KlSocketHandle fd = KL_INVALID_SOCKET; int err = 0;
    ASSERT_EQ(KL_UNIX_NODE_OK, kl_unix_socket_node_bind(&pol, sk(), &al, &st, &fd, &err));
    KlSocketHandle fd2 = fd; int err2 = 0;
    ASSERT_EQ(KL_UNIX_NODE_ERR_BIND, kl_unix_socket_node_bind(&pol, sk(), &al, &st, &fd2, &err2));
    ASSERT_TRUE(kl_handle_valid(fd2));   /* untouched */

    kl_sock_close(sk(), fd);
    kl_unix_socket_node_teardown(&st, 1);
    rm_workdir(dir);
    WSACleanup();
}

UTEST(unix_node_win, zero_init_is_safe_not_open) {
    NodeStore st; memset(&st, 0, sizeof(st));   /* no init call */
    ASSERT_EQ(KL_UNIX_NODE_OK, kl_unix_socket_node_teardown(&st, 1));
}

/* An intermediate junction (reparse point) in the path is rejected. */
UTEST(unix_node_win, intermediate_reparse_rejected) {
    WSADATA w; WSAStartup(MAKEWORD(2,2), &w);
    NodeStore st; kl_unix_socket_node_init(&st);
    KlAllocator al = kl_allocator_default();
    char base[MAX_PATH]; mk_workdir(base, sizeof(base), "rp");
    char real[MAX_PATH], link[MAX_PATH], sub[MAX_PATH];
    snprintf(real, sizeof(real), "%s\\real", base);
    snprintf(link, sizeof(link), "%s\\link", base);
    CreateDirectoryA(real, NULL);
    snprintf(sub, sizeof(sub), "%s\\real\\sub", base);
    CreateDirectoryA(sub, NULL);
    run_cmd2("cmd /c mklink /J \"%s\" \"%s\" >nul 2>&1", link, real);

    char path[MAX_PATH]; snprintf(path, sizeof(path), "%s\\link\\sub\\x.sock", base);
    KlUnixNodePolicy pol = { .path = path, .unlink_stale = 1 };
    KlSocketHandle fd = KL_INVALID_SOCKET; int err = 0;
    KlUnixNodeStatus rc = kl_unix_socket_node_bind(&pol, sk(), &al, &st, &fd, &err);
    ASSERT_EQ(KL_UNIX_NODE_ERR_UNTRUSTED_PARENT, rc);   /* junction on the path -> fail closed */
    ASSERT_FALSE(kl_handle_valid(fd));
    rm_workdir(base);
    WSACleanup();
}

/* A directory whose ACL grants an untrusted SID (Everyone) full control is rejected. */
UTEST(unix_node_win, unsafe_acl_rejected) {
    WSADATA w; WSAStartup(MAKEWORD(2,2), &w);
    NodeStore st; kl_unix_socket_node_init(&st);
    KlAllocator al = kl_allocator_default();
    char dir[MAX_PATH]; mk_workdir(dir, sizeof(dir), "acl");
    /* grant the Everyone group full control on the socket's parent (an untrusted principal that can
     * then substitute the node) via its well-known SID in the icacls call below */
    run_cmd("cmd /c icacls \"%s\" /grant *S-1-1-0:(OI)(CI)F >nul 2>&1", dir);

    char path[MAX_PATH]; snprintf(path, sizeof(path), "%s\\x.sock", dir);
    KlUnixNodePolicy pol = { .path = path, .unlink_stale = 1 };
    KlSocketHandle fd = KL_INVALID_SOCKET; int err = 0;
    ASSERT_EQ(KL_UNIX_NODE_ERR_UNTRUSTED_PARENT,
              kl_unix_socket_node_bind(&pol, sk(), &al, &st, &fd, &err));
    ASSERT_FALSE(kl_handle_valid(fd));
    rm_workdir(dir);
    WSACleanup();
}

/* An INHERIT_ONLY grant to an untrusted SID confers no access on the directory itself, so it must
 * NOT reject (contrast unsafe_acl_rejected, which uses an effective (OI)(CI) grant). This mirrors the
 * default C:\ DACL, which carries an inherit-only Modify for Authenticated Users. */
UTEST(unix_node_win, inherit_only_ace_does_not_reject) {
    WSADATA w; WSAStartup(MAKEWORD(2,2), &w);
    NodeStore st; kl_unix_socket_node_init(&st);
    KlAllocator al = kl_allocator_default();
    char dir[MAX_PATH]; mk_workdir(dir, sizeof(dir), "io");
    /* (IO) = inherit-only: applies only to children created here, not to the directory object. */
    run_cmd("cmd /c icacls \"%s\" /grant *S-1-1-0:(OI)(CI)(IO)(M) >nul 2>&1", dir);

    char path[MAX_PATH]; snprintf(path, sizeof(path), "%s\\x.sock", dir);
    KlUnixNodePolicy pol = { .path = path, .unlink_stale = 1 };
    KlSocketHandle fd = KL_INVALID_SOCKET; int err = 0;
    ASSERT_EQ(KL_UNIX_NODE_OK, kl_unix_socket_node_bind(&pol, sk(), &al, &st, &fd, &err));
    ASSERT_TRUE(kl_handle_valid(fd));

    kl_sock_close(sk(), fd);
    ASSERT_EQ(KL_UNIX_NODE_OK, kl_unix_socket_node_teardown(&st, 1));
    rm_workdir(dir);
    WSACleanup();
}

/* Teardown refuses a node whose identity no longer matches (replaced after bind). */
UTEST(unix_node_win, teardown_refuses_identity_mismatch) {
    WSADATA w; WSAStartup(MAKEWORD(2,2), &w);
    NodeStore st; kl_unix_socket_node_init(&st);
    KlAllocator al = kl_allocator_default();
    char dir[MAX_PATH]; mk_workdir(dir, sizeof(dir), "mis");
    char path[MAX_PATH]; snprintf(path, sizeof(path), "%s\\x.sock", dir);

    KlUnixNodePolicy pol = { .path = path, .unlink_stale = 1 };
    KlSocketHandle fd = KL_INVALID_SOCKET; int err = 0;
    ASSERT_EQ(KL_UNIX_NODE_OK, kl_unix_socket_node_bind(&pol, sk(), &al, &st, &fd, &err));

    /* Replace the node with a DIFFERENT AF_UNIX socket (new file id). */
    kl_sock_close(sk(), fd);
    DeleteFileA(path);
    SOCKET s2 = socket(AF_UNIX, SOCK_STREAM, 0);
    SOCKADDR_UN a; memset(&a, 0, sizeof(a)); a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
    ASSERT_EQ(0, bind(s2, (struct sockaddr *)&a, sizeof(a)));

    ASSERT_EQ(KL_UNIX_NODE_ERR_FOREIGN_NODE, kl_unix_socket_node_teardown(&st, 1));
    ASSERT_NE(INVALID_FILE_ATTRIBUTES, GetFileAttributesA(path));   /* replacement survives */
    closesocket(s2);
    DeleteFileA(path);
    rm_workdir(dir);
    WSACleanup();
}

/* Teardown after external deletion refuses cleanly (nothing to identity-verify). */
UTEST(unix_node_win, teardown_after_external_deletion) {
    WSADATA w; WSAStartup(MAKEWORD(2,2), &w);
    NodeStore st; kl_unix_socket_node_init(&st);
    KlAllocator al = kl_allocator_default();
    char dir[MAX_PATH]; mk_workdir(dir, sizeof(dir), "ext");
    char path[MAX_PATH]; snprintf(path, sizeof(path), "%s\\x.sock", dir);

    KlUnixNodePolicy pol = { .path = path, .unlink_stale = 1 };
    KlSocketHandle fd = KL_INVALID_SOCKET; int err = 0;
    ASSERT_EQ(KL_UNIX_NODE_OK, kl_unix_socket_node_bind(&pol, sk(), &al, &st, &fd, &err));
    kl_sock_close(sk(), fd);
    DeleteFileA(path);   /* external removal */

    ASSERT_EQ(KL_UNIX_NODE_ERR_FOREIGN_NODE, kl_unix_socket_node_teardown(&st, 1));
    rm_workdir(dir);
    WSACleanup();
}

/* Sharing conflict / held-parent behavior. Holding the final parent WITHOUT delete sharing is a
 * defense-in-depth pin against an UNTRUSTED principal renaming/replacing a component; the ACTUAL
 * guarantee is the leaf identity anchor (teardown re-verifies the captured FILE_ID before deleting).
 * This test renames as the OWNER (which the pin is not meant to stop, and some environments permit),
 * so it asserts the security outcome portably: EITHER the pin blocks the rename, OR the identity
 * anchor keeps teardown safe (the original leaf path no longer resolves to our node, so teardown
 * refuses rather than acting on an ambiguous entry). Never a wrong deletion. */
UTEST(unix_node_win, parent_pin_or_identity_keeps_teardown_safe) {
    WSADATA w; WSAStartup(MAKEWORD(2,2), &w);
    NodeStore st; kl_unix_socket_node_init(&st);
    KlAllocator al = kl_allocator_default();
    char base[MAX_PATH]; mk_workdir(base, sizeof(base), "pin");
    char parent[MAX_PATH]; snprintf(parent, sizeof(parent), "%s\\p", base);
    CreateDirectoryA(parent, NULL);
    char path[MAX_PATH]; snprintf(path, sizeof(path), "%s\\x.sock", parent);

    KlUnixNodePolicy pol = { .path = path, .unlink_stale = 1 };
    KlSocketHandle fd = KL_INVALID_SOCKET; int err = 0;
    ASSERT_EQ(KL_UNIX_NODE_OK, kl_unix_socket_node_bind(&pol, sk(), &al, &st, &fd, &err));

    char moved[MAX_PATH]; snprintf(moved, sizeof(moved), "%s\\p2", base);
    if (MoveFileA(parent, moved)) {
        /* The environment permitted renaming the held parent: the identity anchor must still protect.
         * The original leaf path no longer resolves, so teardown refuses (never a mis-identified delete). */
        ASSERT_EQ(KL_UNIX_NODE_ERR_FOREIGN_NODE, kl_unix_socket_node_teardown(&st, 1));
        kl_sock_close(sk(), fd);
    } else {
        /* Pinned: the no-delete-sharing hold blocked the rename (the sharing conflict). A normal
         * teardown then removes exactly the node we bound. */
        kl_sock_close(sk(), fd);
        ASSERT_EQ(KL_UNIX_NODE_OK, kl_unix_socket_node_teardown(&st, 1));
        ASSERT_EQ(INVALID_FILE_ATTRIBUTES, GetFileAttributesA(path));
    }
    rm_workdir(base);
    WSACleanup();
}

UTEST_MAIN();
