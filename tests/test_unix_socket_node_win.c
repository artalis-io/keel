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
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifndef IO_REPARSE_TAG_AF_UNIX
#define IO_REPARSE_TAG_AF_UNIX (0x80000023L)
#endif

/* The command helpers below always receive a literal format string; silence the (false) warning. */
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
/* Path builders compose bounded %TEMP% paths (well under MAX_PATH) into MAX_PATH buffers; GCC -O2
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

/* A fresh work directory under %TEMP% (inherits the trusted temp ACL). */
static void mk_workdir(char *buf, size_t n, const char *name) {
    char tmp[MAX_PATH];
    GetTempPathA(sizeof(tmp), tmp);
    snprintf(buf, n, "%skeel-wnode-%lu-%s", tmp, (unsigned long)GetCurrentProcessId(), name);
    run_cmd("cmd /c rmdir /s /q \"%s\" >nul 2>&1", buf);
    CreateDirectoryA(buf, NULL);
}
static void rm_workdir(const char *dir) { run_cmd("cmd /c rmdir /s /q \"%s\" >nul 2>&1", dir); }

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

/* The held parent directory is pinned (no delete sharing): it cannot be renamed/removed while bound. */
UTEST(unix_node_win, parent_pinned_while_bound) {
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
    ASSERT_FALSE(MoveFileA(parent, moved));   /* pinned: rename must fail */

    kl_sock_close(sk(), fd);
    kl_unix_socket_node_teardown(&st, 1);
    rm_workdir(base);
    WSACleanup();
}

/* ── TEMPORARY runner diagnostic (removed once the happy-path failure is understood) ──────────── */

static void diag_dump_acl(const char *label, HANDLE h) {
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
        const char *ty = hd->AceType == ACCESS_ALLOWED_ACE_TYPE ? "ALLOW"
                       : hd->AceType == ACCESS_DENIED_ACE_TYPE  ? "DENY " : "OTHER";
        if (hd->AceType == ACCESS_ALLOWED_ACE_TYPE || hd->AceType == ACCESS_DENIED_ACE_TYPE) {
            ACCESS_ALLOWED_ACE *a = (ACCESS_ALLOWED_ACE *)raw;   /* DENY shares the layout */
            char *ss = NULL; ConvertSidToStringSidA((PSID)&a->SidStart, &ss);
            printf("    ACE[%u] %s flags=0x%02x mask=0x%08lx sid=%s\n",
                   i, ty, (unsigned)hd->AceFlags, (unsigned long)a->Mask, ss ? ss : "?");
            if (ss) LocalFree(ss);
        } else {
            printf("    ACE[%u] type=%u flags=0x%02x (non-basic)\n",
                   i, (unsigned)hd->AceType, (unsigned)hd->AceFlags);
        }
    }
    if (sd) LocalFree(sd);
}

UTEST(unix_node_win, zzz_runner_diagnostic) {
    WSADATA w; WSAStartup(MAKEWORD(2,2), &w);
    NodeStore st; kl_unix_socket_node_init(&st);
    KlAllocator al = kl_allocator_default();
    char dir[MAX_PATH]; mk_workdir(dir, sizeof(dir), "diag");
    char path[MAX_PATH]; snprintf(path, sizeof(path), "%s\\x.sock", dir);
    printf("DIAG path=%s\n", path);

    KlUnixNodePolicy pol = { .path = path, .unlink_stale = 1 };
    KlSocketHandle fd = KL_INVALID_SOCKET; int err = 0;
    KlUnixNodeStatus s = kl_unix_socket_node_bind(&pol, sk(), &al, &st, &fd, &err);
    printf("DIAG bind status=%d err=%d gle=%lu fd_valid=%d\n",
           (int)s, err, (unsigned long)GetLastError(), kl_handle_valid(fd));
    if (kl_handle_valid(fd)) { kl_sock_close(sk(), fd); kl_unix_socket_node_teardown(&st, 1); }

    /* Per-component walk dump of the parent chain (owner + every ACE + volume caps on the leaf). */
    size_t len = strlen(path), cut = 0;
    for (size_t k = 0; k < len; k++) if (path[k] == '\\' || path[k] == '/') cut = k;
    size_t i = 0;
    while (i <= cut) {
        size_t j = i; while (j <= cut && path[j] != '\\' && path[j] != '/') j++;
        size_t plen = j;
        if (plen == 0) { i = j + 1; continue; }
        char prefix[300]; memcpy(prefix, path, plen); prefix[plen] = '\0';
        if (plen == 2 && prefix[1] == ':') { prefix[2] = '\\'; prefix[3] = '\0'; }
        wchar_t wp[300]; MultiByteToWideChar(CP_ACP, 0, prefix, -1, wp, 300);
        HANDLE d = CreateFileW(wp, READ_CONTROL | FILE_READ_ATTRIBUTES,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                               OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
        if (d == INVALID_HANDLE_VALUE) {
            printf("COMP '%s' open FAILED gle=%lu\n", prefix, (unsigned long)GetLastError());
            i = j + 1; if (j >= cut) break; continue;
        }
        BY_HANDLE_FILE_INFORMATION bi; memset(&bi, 0, sizeof(bi)); GetFileInformationByHandle(d, &bi);
        printf("COMP '%s' attrs=0x%lx dir=%d reparse=%d\n", prefix, (unsigned long)bi.dwFileAttributes,
               !!(bi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY),
               !!(bi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT));
        diag_dump_acl(prefix, d);
        if (j >= cut) {
            wchar_t fsname[32]; DWORD ser = 0, mc = 0, fl = 0;
            if (GetVolumeInformationByHandleW(d, NULL, 0, &ser, &mc, &fl, fsname, 32)) {
                char fsn[64]; WideCharToMultiByte(CP_ACP, 0, fsname, -1, fsn, 64, NULL, NULL);
                printf("VOL fsname=%s flags=0x%lx open_by_id=%d reparse=%d\n", fsn, (unsigned long)fl,
                       !!(fl & FILE_SUPPORTS_OPEN_BY_FILE_ID), !!(fl & FILE_SUPPORTS_REPARSE_POINTS));
            } else {
                printf("VOL GetVolumeInformationByHandleW FAILED gle=%lu\n", (unsigned long)GetLastError());
            }
        }
        CloseHandle(d);
        i = j + 1; if (j >= cut) break;
    }

    /* Post-bind node probe: raw AF_UNIX bind, then reparse-tag + FileIdInfo on the node handle. */
    {
        SOCKET s2 = socket(AF_UNIX, SOCK_STREAM, 0);
        SOCKADDR_UN a; memset(&a, 0, sizeof(a)); a.sun_family = AF_UNIX;
        snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
        int brc = bind(s2, (struct sockaddr *)&a, sizeof(a));
        printf("RAW bind rc=%d wsa=%d fileattr=0x%lx\n", brc, WSAGetLastError(),
               (unsigned long)GetFileAttributesA(path));
        wchar_t wp[300]; MultiByteToWideChar(CP_ACP, 0, path, -1, wp, 300);
        HANDLE ln = CreateFileW(wp, FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                                OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
        if (ln == INVALID_HANDLE_VALUE) {
            printf("NODE open FAILED gle=%lu\n", (unsigned long)GetLastError());
        } else {
            BYTE buf[MAXIMUM_REPARSE_DATA_BUFFER_SIZE]; DWORD got = 0;
            BOOL rok = DeviceIoControl(ln, FSCTL_GET_REPARSE_POINT, NULL, 0, buf, sizeof(buf), &got, NULL);
            printf("NODE fsctl_reparse ok=%d gle=%lu tag=0x%08lx (afunix=0x%08lx)\n",
                   rok, (unsigned long)(rok ? 0 : GetLastError()),
                   (unsigned long)(rok ? *(DWORD *)buf : 0), (unsigned long)IO_REPARSE_TAG_AF_UNIX);
            /* (FileIdInfo probe omitted: FILE_ID_INFO needs _WIN32_WINNT >= 0x0602, which the test
             * prelude does not set; the module sets it internally, so get_file_id compiles there.) */
            CloseHandle(ln);
        }
        closesocket(s2); DeleteFileA(path);
    }
    rm_workdir(dir);
    WSACleanup();
    ASSERT_TRUE(1);
}

UTEST_MAIN();
