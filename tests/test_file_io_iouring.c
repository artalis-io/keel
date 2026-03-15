#include "utest.h"
#include <keel/file_io.h>
#include <keel/event.h>
#include <keel/allocator.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>

/* ═══════════════════════════════════════════════════════════════════
 * io_uring file I/O integration tests
 *
 * Only compiled on BACKEND=iouring builds. Exercises the real
 * io_uring backend: IORING_OP_READ submissions, CQE routing through
 * kl_event_wait, and tick() completion delivery.
 * ═══════════════════════════════════════════════════════════════════ */

/* Helper: create a temp file with known content, return fd (unlinked) */
static int make_temp_file(const char *content, size_t len) {
    char path[] = "/tmp/keel_fio_test_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0)
        return -1;
    unlink(path);
    ssize_t nw = write(fd, content, len);
    if (nw != (ssize_t)len) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Helper: submit a read, wait for CQE, tick, return completion count */
static int submit_wait_tick(KlFileIO *fio, KlEventLoop *loop,
                            int file_fd, void *buf, size_t len,
                            off_t offset, int sock_fd, void *udata,
                            KlFileIOResult *out, int max) {
    if (fio->submit(fio, file_fd, buf, len, offset, sock_fd, udata) < 0)
        return -1;

    KlEvent events[4];
    int n = kl_event_wait(loop, events, 4, 2000);
    if (n < 0)
        return -1;
    /* n=0 is expected: file CQEs are buffered, not reported as events */

    return fio->tick(fio, out, max);
}

UTEST(file_io_iouring, create_non_null) {
    KlAllocator a = kl_allocator_default();
    KlEventLoop loop;
    memset(&loop, 0, sizeof(loop));
    loop.alloc = &a;
    ASSERT_EQ(kl_event_init(&loop), 0);

    KlFileIO *fio = kl_file_io_create(&loop, &a);
    ASSERT_TRUE(fio != NULL);

    fio->destroy(fio);
    kl_event_close(&loop);
}

UTEST(file_io_iouring, read_small_file) {
    /* End-to-end: submit → io_uring_submit → CQE → tick → verify */
    KlAllocator a = kl_allocator_default();
    KlEventLoop loop;
    memset(&loop, 0, sizeof(loop));
    loop.alloc = &a;
    ASSERT_EQ(kl_event_init(&loop), 0);

    KlFileIO *fio = kl_file_io_create(&loop, &a);
    ASSERT_TRUE(fio != NULL);

    const char *content = "Hello from io_uring file I/O!";
    size_t content_len = strlen(content);
    int file_fd = make_temp_file(content, content_len);
    ASSERT_GE(file_fd, 0);

    char buf[256];
    memset(buf, 0, sizeof(buf));
    int marker = 42;

    KlFileIOResult results[4];
    int nf = submit_wait_tick(fio, &loop, file_fd, buf, sizeof(buf),
                              0, 10, &marker, results, 4);
    ASSERT_EQ(nf, 1);
    ASSERT_EQ(results[0].udata, &marker);
    ASSERT_EQ(results[0].result, (ssize_t)content_len);
    ASSERT_EQ(memcmp(buf, content, content_len), 0);

    close(file_fd);
    fio->destroy(fio);
    kl_event_close(&loop);
}

UTEST(file_io_iouring, read_with_offset) {
    /* Read from the middle of a file */
    KlAllocator a = kl_allocator_default();
    KlEventLoop loop;
    memset(&loop, 0, sizeof(loop));
    loop.alloc = &a;
    ASSERT_EQ(kl_event_init(&loop), 0);

    KlFileIO *fio = kl_file_io_create(&loop, &a);
    ASSERT_TRUE(fio != NULL);

    const char *content = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    int file_fd = make_temp_file(content, 26);
    ASSERT_GE(file_fd, 0);

    char buf[64];
    memset(buf, 0, sizeof(buf));
    int marker = 1;

    KlFileIOResult results[4];
    int nf = submit_wait_tick(fio, &loop, file_fd, buf, 16,
                              10, 11, &marker, results, 4);
    ASSERT_EQ(nf, 1);
    ASSERT_EQ(results[0].result, (ssize_t)16);
    ASSERT_EQ(memcmp(buf, "KLMNOPQRSTUVWXYZ", 16), 0);

    close(file_fd);
    fio->destroy(fio);
    kl_event_close(&loop);
}

UTEST(file_io_iouring, multi_chunk) {
    /* Read a 1000-byte file in 300-byte chunks (simulates connection loop) */
    KlAllocator a = kl_allocator_default();
    KlEventLoop loop;
    memset(&loop, 0, sizeof(loop));
    loop.alloc = &a;
    ASSERT_EQ(kl_event_init(&loop), 0);

    KlFileIO *fio = kl_file_io_create(&loop, &a);
    ASSERT_TRUE(fio != NULL);

    char content[1000];
    for (int i = 0; i < 1000; i++)
        content[i] = (char)('A' + (i % 26));
    int file_fd = make_temp_file(content, 1000);
    ASSERT_GE(file_fd, 0);

    char buf[300];
    char received[1000];
    size_t total = 0;
    off_t offset = 0;
    int chunk = 0;
    int marker = 77;

    while (total < 1000) {
        size_t remaining = 1000 - total;
        size_t to_read = remaining < sizeof(buf) ? remaining : sizeof(buf);
        memset(buf, 0, sizeof(buf));

        KlFileIOResult results[4];
        int nf = submit_wait_tick(fio, &loop, file_fd, buf, to_read,
                                  offset, 20 + chunk, &marker, results, 4);
        ASSERT_EQ(nf, 1);
        ASSERT_GT(results[0].result, (ssize_t)0);

        size_t got = (size_t)results[0].result;
        memcpy(received + total, buf, got);
        total += got;
        offset += (off_t)got;
        chunk++;
    }

    ASSERT_EQ(total, (size_t)1000);
    ASSERT_EQ(memcmp(received, content, 1000), 0);
    /* 300+300+300+100 = 4 chunks */
    ASSERT_EQ(chunk, 4);

    close(file_fd);
    fio->destroy(fio);
    kl_event_close(&loop);
}

UTEST(file_io_iouring, read_past_eof) {
    /* Requesting more bytes than file size returns actual file size */
    KlAllocator a = kl_allocator_default();
    KlEventLoop loop;
    memset(&loop, 0, sizeof(loop));
    loop.alloc = &a;
    ASSERT_EQ(kl_event_init(&loop), 0);

    KlFileIO *fio = kl_file_io_create(&loop, &a);
    ASSERT_TRUE(fio != NULL);

    const char *content = "short";
    int file_fd = make_temp_file(content, 5);
    ASSERT_GE(file_fd, 0);

    char buf[4096];
    memset(buf, 0, sizeof(buf));
    int marker = 1;

    KlFileIOResult results[4];
    int nf = submit_wait_tick(fio, &loop, file_fd, buf, sizeof(buf),
                              0, 12, &marker, results, 4);
    ASSERT_EQ(nf, 1);
    ASSERT_EQ(results[0].result, (ssize_t)5);
    ASSERT_EQ(memcmp(buf, "short", 5), 0);

    close(file_fd);
    fio->destroy(fio);
    kl_event_close(&loop);
}

UTEST(file_io_iouring, read_at_eof_returns_zero) {
    /* Reading at offset == file_size returns 0 bytes */
    KlAllocator a = kl_allocator_default();
    KlEventLoop loop;
    memset(&loop, 0, sizeof(loop));
    loop.alloc = &a;
    ASSERT_EQ(kl_event_init(&loop), 0);

    KlFileIO *fio = kl_file_io_create(&loop, &a);
    ASSERT_TRUE(fio != NULL);

    const char *content = "data";
    int file_fd = make_temp_file(content, 4);
    ASSERT_GE(file_fd, 0);

    char buf[64];
    memset(buf, 0, sizeof(buf));
    int marker = 1;

    KlFileIOResult results[4];
    int nf = submit_wait_tick(fio, &loop, file_fd, buf, sizeof(buf),
                              4, 13, &marker, results, 4);
    ASSERT_EQ(nf, 1);
    ASSERT_EQ(results[0].result, (ssize_t)0);

    close(file_fd);
    fio->destroy(fio);
    kl_event_close(&loop);
}

UTEST(file_io_iouring, destroy_no_leak) {
    /* Create and immediately destroy — verify no leaks under ASan */
    KlAllocator a = kl_allocator_default();
    KlEventLoop loop;
    memset(&loop, 0, sizeof(loop));
    loop.alloc = &a;
    ASSERT_EQ(kl_event_init(&loop), 0);

    KlFileIO *fio = kl_file_io_create(&loop, &a);
    ASSERT_TRUE(fio != NULL);

    fio->destroy(fio);
    kl_event_close(&loop);
}

UTEST_MAIN();
