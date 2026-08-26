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
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* The command helpers below always receive a literal format string; silence the (false) warning. */
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
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

UTEST_MAIN();
