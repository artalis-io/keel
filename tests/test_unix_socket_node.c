/* Direct unit tests for the substrate AF_UNIX node module (src/unix_socket_node.h), exercising the
 * neutral error contract that is hard to reach through the HTTP server: repeated bind, deterministic
 * diagnostics, teardown-failure reporting, out_fd invalidation, and zero-init safety. Built with
 * -Isrc so it can include the internal contract header + socket seam. */
#include "utest.h"
#include "unix_socket_node.h"    /* module under test */
#include "socket.h"              /* kl_sock_close (internal seam, via -Isrc) */
#include <keel/allocator.h>
#include <keel/socket.h>         /* kl_socket_provider_posix */
#include <keel/handle.h>

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Suitably aligned opaque storage for the module state. */
typedef union { unsigned char b[KL_UNIX_NODE_STORAGE]; max_align_t a; } NodeStore;

static void node_path(char *buf, size_t n, const char *name) {
    snprintf(buf, n, "./keel-node-%ld-%s.sock", (long)getpid(), name);
}

UTEST(unix_node, repeated_bind_rejected_without_clobbering_fd) {
    NodeStore st;
    kl_unix_socket_node_init(&st);
    KlAllocator alloc = kl_allocator_default();
    const KlSocketProvider *sk = kl_socket_provider_posix();
    char path[108];
    node_path(path, sizeof(path), "repeat");
    unlink(path);

    KlUnixNodePolicy pol = { .path = path, .unlink_stale = 1 };
    KlSocketHandle fd = KL_INVALID_SOCKET;
    int err = -1;
    ASSERT_EQ(KL_UNIX_NODE_OK, kl_unix_socket_node_bind(&pol, sk, &alloc, &st, &fd, &err));
    ASSERT_TRUE(kl_handle_valid(fd));
    ASSERT_EQ(0, err);

    /* A second bind on the still-open state is rejected with a deterministic errno and does NOT
     * touch the caller's live handle (no leak, no clobber). */
    KlSocketHandle fd2 = fd;
    int err2 = 0;
    ASSERT_EQ(KL_UNIX_NODE_ERR_BIND, kl_unix_socket_node_bind(&pol, sk, &alloc, &st, &fd2, &err2));
    ASSERT_EQ(EEXIST, err2);            /* deterministic, never strerror(0) = "Success" */
    ASSERT_TRUE(kl_handle_valid(fd2));  /* untouched: still the first handle */

    kl_sock_close(sk, fd);
    ASSERT_EQ(KL_UNIX_NODE_OK, kl_unix_socket_node_teardown(&st, 1));
    unlink(path);
}

UTEST(unix_node, out_fd_invalid_and_state_closed_on_failure) {
    NodeStore st;
    kl_unix_socket_node_init(&st);
    KlAllocator alloc = kl_allocator_default();
    const KlSocketProvider *sk = kl_socket_provider_posix();

    KlUnixNodePolicy pol = { .path = "", .unlink_stale = 1 };  /* invalid path */
    KlSocketHandle fd = (KlSocketHandle)0x7fff;                /* junk sentinel to prove it is reset */
    int err = 999;
    ASSERT_EQ(KL_UNIX_NODE_ERR_INVALID_PATH, kl_unix_socket_node_bind(&pol, sk, &alloc, &st, &fd, &err));
    ASSERT_FALSE(kl_handle_valid(fd));   /* set invalid on the normal-path failure */
    /* State never opened -> teardown is a clean no-op. */
    ASSERT_EQ(KL_UNIX_NODE_OK, kl_unix_socket_node_teardown(&st, 1));
}

UTEST(unix_node, teardown_reports_unlink_failure) {
    if (geteuid() == 0) {
        UTEST_SKIP("root bypasses directory write permission, so unlinkat cannot be forced to fail");
        return;
    }
    NodeStore st;
    kl_unix_socket_node_init(&st);
    KlAllocator alloc = kl_allocator_default();
    const KlSocketProvider *sk = kl_socket_provider_posix();

    char dir[96];
    snprintf(dir, sizeof(dir), "./keel-node-%ld-tdir", (long)getpid());
    rmdir(dir);
    ASSERT_EQ(0, mkdir(dir, 0700));
    char path[160];
    snprintf(path, sizeof(path), "%s/x.sock", dir);

    KlUnixNodePolicy pol = { .path = path, .unlink_stale = 1 };
    KlSocketHandle fd = KL_INVALID_SOCKET;
    int err = -1;
    ASSERT_EQ(KL_UNIX_NODE_OK, kl_unix_socket_node_bind(&pol, sk, &alloc, &st, &fd, &err));

    /* Drop write on the parent so the teardown unlinkat fails; the trust re-check still passes
     * (owner + no group/other write), so the module reaches unlinkat and must REPORT its failure. */
    ASSERT_EQ(0, chmod(dir, 0500));
    ASSERT_EQ(KL_UNIX_NODE_ERR_BIND, kl_unix_socket_node_teardown(&st, 1));

    kl_sock_close(sk, fd);
    chmod(dir, 0700);
    unlink(path);
    rmdir(dir);
}

UTEST(unix_node, zero_initialized_storage_is_a_safe_not_open_state) {
    NodeStore st;
    memset(&st, 0, sizeof(st));   /* no kl_unix_socket_node_init(): a zeroed buffer must be "not open" */
    ASSERT_EQ(KL_UNIX_NODE_OK, kl_unix_socket_node_teardown(&st, 1));  /* no close(0), no crash */
}

UTEST_MAIN();
