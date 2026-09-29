/*
 * test_stream_close_live.c: graceful close after a terminal write failure, over a REAL socket.
 *
 * test_stream_close.c pins the rule with mock hooks. This suite proves it is a property of the
 * generic stream, not of one adapter: a bare KlStream over a real loopback TCP socket (the readiness
 * writer path, so it runs on every engine and needs no event loop) queues bytes the kernel will not
 * take, begins a GRACEFUL close, then its peer resets the connection. Before the fix the queue could
 * never drain and the close never detached. Now the failed write makes delivery impossible, the
 * undeliverable queue stops holding the close, and on_close fires exactly once. The named-pipe (IOCP)
 * counterpart lives in test_pipe_stream.c.
 */
#include "utest.h"
#include <keel/keel.h>
#include <keel/stream.h>
#include <keel/stream_detail.h>   /* bare KlStream storage */
#include <keel/sockaddr.h>
#include <keel/handle.h>
#include "../src/socket.h"        /* kl_sockdef_*: the default socket seam, full-width handles */
#include "net_compat.h"           /* AF_INET / SOCK_STREAM, kl_test_sleep_ms */
#include <errno.h>
#include <string.h>
#include <stdlib.h>

static int make_pair(KlSocketHandle *end, KlSocketHandle *peer) {
    KlSocketHandle lis = kl_sockdef_socket(AF_INET, SOCK_STREAM, 0);
    if (!kl_handle_valid(lis)) return -1;
    uint8_t lo[4] = { 127, 0, 0, 1 };
    KlSockAddr addr; kl_sockaddr_from_ipv4(&addr, lo, 0);
    KlSockAddr bound;
    KlSocketHandle cli = KL_INVALID_SOCKET, srv = KL_INVALID_SOCKET;
    if (kl_sockdef_bind(lis, &addr) < 0 || kl_sockdef_listen(lis, 1) < 0) goto fail;
    if (kl_sockdef_get_local_addr(lis, &bound) < 0) goto fail;
    cli = kl_sockdef_socket(AF_INET, SOCK_STREAM, 0);
    if (!kl_handle_valid(cli)) goto fail;
    if (kl_sockdef_connect(cli, &bound) < 0) goto fail;
    srv = kl_sockdef_accept(lis, NULL);
    if (!kl_handle_valid(srv)) goto fail;
    kl_sockdef_close(lis);
    *end = cli; *peer = srv;
    return 0;
fail:
    if (kl_handle_valid(lis)) kl_sockdef_close(lis);
    if (kl_handle_valid(cli)) kl_sockdef_close(cli);
    return -1;
}

/* The stream's readiness writer: bytes sent, 0 = would-block, -1 = fatal (the peer is gone). */
static kl_ssize_t sock_writer(const char *data, size_t len, void *ctx) {
    KlSocketHandle fd = *(KlSocketHandle *)ctx;
    kl_ssize_t n = kl_sockdef_send(fd, data, len);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    return n;
}

static int g_closes;
static void on_close(void *ctx) { (void)ctx; g_closes++; }

UTEST(stream_close_live, socket_peer_reset_with_queue_behind_detaches) {
    KlAllocator a = kl_allocator_default();
    KlSocketHandle end, peer;
    ASSERT_EQ(make_pair(&end, &peer), 0);
    ASSERT_EQ(kl_sockdef_set_nonblocking(end), 0);

    KlStream s;
    char rbuf[256];
    ASSERT_EQ(kl_stream_init(&s, rbuf, sizeof rbuf), 0);
    ASSERT_EQ(kl_stream_write_init(&s, &a, 16u << 20), 0);
    ASSERT_EQ(kl_stream_set_writer(&s, sock_writer, &end), 0);
    g_closes = 0;
    ASSERT_EQ(kl_stream_close_init(&s, on_close, NULL), 0);

    /* The peer never reads: write until the kernel refuses and bytes stay queued in the stream. */
    static char chunk[64 * 1024];
    memset(chunk, 'k', sizeof chunk);
    for (int i = 0; i < 200 && kl_stream_write_pending(&s) == 0; i++)
        ASSERT_EQ((int)kl_stream_write(&s, chunk, sizeof chunk), (int)KL_STREAM_ACCEPTED);
    ASSERT_GT((int)(kl_stream_write_pending(&s) > 0), 0);

    ASSERT_EQ(kl_stream_close_begin(&s), 0);          /* GRACEFUL: wants to drain the queue */
    ASSERT_EQ(g_closes, 0);

    kl_sockdef_close(peer);                           /* unread data in its buffer → RST */

    /* Drive the writable side as an adapter would, until the failed send ends the close. */
    int saw_failure = 0;
    for (int i = 0; i < 500 && g_closes == 0; i++) {
        if (kl_stream_flush(&s) < 0) saw_failure = 1;
        if (g_closes == 0) kl_test_sleep_ms(10);
    }
    ASSERT_EQ(saw_failure, 1);                        /* the reset really surfaced as a write error */
    ASSERT_EQ(g_closes, 1);                           /* ... and the close progressed, exactly once */
    ASSERT_EQ(kl_stream_is_detached(&s), 1);
    ASSERT_EQ(kl_stream_flush(&s), -1);               /* still failed ... */
    ASSERT_EQ(g_closes, 1);                           /* ... and on_close never fires again */

    ASSERT_EQ(kl_stream_write_free(&s), 0);           /* the abandoned queue is the owner's to free */
    kl_sockdef_close(end);
}

UTEST_MAIN();
