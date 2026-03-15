#include "utest.h"
#include <keel/file_io.h>
#include <keel/connection.h>
#include <keel/allocator.h>
#include <keel/response.h>
#include <keel/event.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>

/* File I/O phase constants (mirror connection.c) */
#define FILE_IO_IDLE       0
#define FILE_IO_READING    1
#define FILE_IO_WRITING    2
#define FILE_IO_CANCELLING 3

/* ── Mock file I/O backend ───────────────────────────────────────── */

typedef struct {
    KlFileIO base;
    /* Last submit args */
    int submitted;
    int last_file_fd;
    void *last_buf;
    size_t last_len;
    off_t last_offset;
    int last_sock_fd;
    void *last_udata;
    /* Completion to return from tick */
    int has_completion;
    ssize_t completion_result;
    void *completion_udata;
    /* Cancel tracking */
    int cancelled;
    int cancel_sock_fd;
} MockFileIO;

static int mock_submit(KlFileIO *fio, int file_fd, void *buf,
                        size_t len, off_t offset,
                        int sock_fd, void *udata) {
    MockFileIO *m = (MockFileIO *)fio;
    m->submitted++;
    m->last_file_fd = file_fd;
    m->last_buf = buf;
    m->last_len = len;
    m->last_offset = offset;
    m->last_sock_fd = sock_fd;
    m->last_udata = udata;
    return 0;
}

static int mock_cancel(KlFileIO *fio, int sock_fd) {
    MockFileIO *m = (MockFileIO *)fio;
    m->cancelled++;
    m->cancel_sock_fd = sock_fd;
    return 0;
}

static int mock_tick(KlFileIO *fio, KlFileIOResult *out, int max) {
    MockFileIO *m = (MockFileIO *)fio;
    if (!m->has_completion || max <= 0) return 0;
    out[0].udata = m->completion_udata;
    out[0].result = m->completion_result;
    out[0].zero_copy = 0;
    m->has_completion = 0;
    return 1;
}

static void mock_destroy(KlFileIO *fio) {
    (void)fio;
}

static void mock_file_io_init(MockFileIO *m) {
    memset(m, 0, sizeof(*m));
    m->base.submit = mock_submit;
    m->base.cancel = mock_cancel;
    m->base.tick = mock_tick;
    m->base.destroy = mock_destroy;
}

/* ── Tests ───────────────────────────────────────────────────────── */

UTEST(file_io, vtable_null_on_non_iouring) {
    /* On non-io_uring backends, kl_file_io_create returns NULL */
    KlAllocator a = kl_allocator_default();
    KlEventLoop loop;
    memset(&loop, 0, sizeof(loop));
    loop.alloc = &a;

    /* Don't call kl_event_init — just test with a zeroed loop */
    KlFileIO *fio = kl_file_io_create(&loop, &a);
    ASSERT_TRUE(fio == NULL);
}

UTEST(file_io, submit_called) {
    /* Connection with mock file_io calls submit for file body.
     * Non-TLS path tries splice first (buf=NULL). */
    MockFileIO mock;
    mock_file_io_init(&mock);

    KlAllocator a = kl_allocator_default();
    KlConn c;
    memset(&c, 0, sizeof(c));
    c.alloc = &a;
    c.fd = 42;
    c.state = KL_CONN_SENDING;
    c.file_io = &mock.base;
    c.file_io_phase = FILE_IO_IDLE;

    /* Set up read buffer */
    c.read_buf = kl_malloc(&a, 8192);
    c.read_cap = 8192;

    /* Set up response as file body with headers already sent */
    memset(&c.res, 0, sizeof(c.res));
    c.res.body_mode = KL_BODY_FILE;
    c.res.file_fd = 10;
    c.res.file_size = 1000;
    c.res.file_offset = 0;
    c.res.headers_sent = 1;
    c.res.head_request = 0;
    c.res.file_fd = 10;

    KlConnState state = kl_conn_on_writable(&c);
    ASSERT_EQ(state, KL_CONN_SENDING);
    ASSERT_EQ(mock.submitted, 1);
    ASSERT_EQ(mock.last_file_fd, 10);
    ASSERT_EQ(mock.last_sock_fd, 42);
    ASSERT_EQ(mock.last_buf, (void *)NULL);  /* splice path: buf=NULL */
    ASSERT_EQ(mock.last_offset, (off_t)0);
    ASSERT_EQ(c.file_io_phase, FILE_IO_READING);

    kl_free(&a, c.read_buf, 8192);
}

UTEST(file_io, headers_first) {
    /* Headers must be sent before file read is submitted.
     * When headers_sent is 0, on_writable should call kl_response_send
     * first. Since we don't have a real fd, we just verify the logic path. */
    MockFileIO mock;
    mock_file_io_init(&mock);

    KlAllocator a = kl_allocator_default();
    KlConn c;
    memset(&c, 0, sizeof(c));
    c.alloc = &a;
    c.fd = -1;  /* invalid fd — send will fail */
    c.state = KL_CONN_SENDING;
    c.file_io = &mock.base;
    c.file_io_phase = FILE_IO_IDLE;

    c.read_buf = kl_malloc(&a, 8192);
    c.read_cap = 8192;

    memset(&c.res, 0, sizeof(c.res));
    c.res.body_mode = KL_BODY_FILE;
    c.res.file_fd = 10;
    c.res.file_size = 100;
    c.res.headers_sent = 0;  /* headers not sent yet */
    c.res.conn_fd = -1;

    /* response_send will fail on invalid fd — that's expected */
    KlConnState state = kl_conn_on_writable(&c);
    /* Should close due to send failure, no file I/O submitted */
    ASSERT_EQ(state, KL_CONN_CLOSED);
    ASSERT_EQ(mock.submitted, 0);

    kl_free(&a, c.read_buf, 8192);
}

UTEST(file_io, complete_writes) {
    /* On completion, data written to socket, offset advanced */
    MockFileIO mock;
    mock_file_io_init(&mock);

    KlAllocator a = kl_allocator_default();

    /* Use a socketpair to test real writes */
    int fds[2];
    int rc = socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    ASSERT_EQ(rc, 0);

    /* Make write end non-blocking */
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);

    KlConn c;
    memset(&c, 0, sizeof(c));
    c.alloc = &a;
    c.fd = fds[0];
    c.state = KL_CONN_SENDING;
    c.file_io = &mock.base;
    c.file_io_phase = FILE_IO_READING;

    c.read_buf = kl_malloc(&a, 8192);
    c.read_cap = 8192;

    /* Simulate file data in read_buf */
    memcpy(c.read_buf, "hello world", 11);

    memset(&c.res, 0, sizeof(c.res));
    c.res.body_mode = KL_BODY_FILE;
    c.res.file_fd = 10;
    c.res.file_size = 11;
    c.res.file_offset = 0;
    c.res.headers_sent = 1;

    /* Deliver completion: 11 bytes read */
    KlConnState state = kl_conn_on_file_complete(&c, 11, 0);

    /* File fully sent — should complete (CLOSED since keep_alive=0) */
    ASSERT_EQ(state, KL_CONN_CLOSED);
    ASSERT_EQ(c.res.file_offset, (off_t)11);

    /* Read from the other end to verify */
    char buf[64];
    ssize_t nr = read(fds[1], buf, sizeof(buf));
    ASSERT_EQ(nr, 11);
    ASSERT_EQ(memcmp(buf, "hello world", 11), 0);

    kl_free(&a, c.read_buf, 8192);
    close(fds[0]);
    close(fds[1]);
}

UTEST(file_io, partial_write) {
    /* EAGAIN during write → stay in SENDING with FILE_IO_WRITING */
    MockFileIO mock;
    mock_file_io_init(&mock);
    KlAllocator a = kl_allocator_default();

    /* Use socketpair, fill the write buffer to trigger EAGAIN */
    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);

    /* Fill the socket buffer */
    char fill[65536];
    memset(fill, 'X', sizeof(fill));
    while (write(fds[0], fill, sizeof(fill)) > 0) {}

    KlConn c;
    memset(&c, 0, sizeof(c));
    c.alloc = &a;
    c.fd = fds[0];
    c.state = KL_CONN_SENDING;
    c.file_io = &mock.base;
    c.file_io_phase = FILE_IO_READING;

    c.read_buf = kl_malloc(&a, 8192);
    c.read_cap = 8192;
    memcpy(c.read_buf, "test data here", 14);

    memset(&c.res, 0, sizeof(c.res));
    c.res.body_mode = KL_BODY_FILE;
    c.res.file_fd = 10;
    c.res.file_size = 14;
    c.res.file_offset = 0;
    c.res.headers_sent = 1;

    KlConnState state = kl_conn_on_file_complete(&c, 14, 0);
    /* Socket full → EAGAIN → stays SENDING in WRITING phase */
    ASSERT_EQ(state, KL_CONN_SENDING);
    ASSERT_EQ(c.file_io_phase, FILE_IO_WRITING);

    kl_free(&a, c.read_buf, 8192);
    close(fds[0]);
    close(fds[1]);
}

UTEST(file_io, multi_chunk) {
    /* Large file: multiple read→write cycles */
    MockFileIO mock;
    mock_file_io_init(&mock);
    KlAllocator a = kl_allocator_default();

    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);

    KlConn c;
    memset(&c, 0, sizeof(c));
    c.alloc = &a;
    c.fd = fds[0];
    c.state = KL_CONN_SENDING;
    c.file_io = &mock.base;

    size_t bufcap = 100;
    c.read_buf = kl_malloc(&a, bufcap);
    c.read_cap = bufcap;

    memset(&c.res, 0, sizeof(c.res));
    c.res.body_mode = KL_BODY_FILE;
    c.res.file_fd = 10;
    c.res.file_size = 250;  /* larger than buffer */
    c.res.file_offset = 0;
    c.res.headers_sent = 1;

    /* First: submit read */
    KlConnState state = kl_conn_on_writable(&c);
    ASSERT_EQ(state, KL_CONN_SENDING);
    ASSERT_EQ(c.file_io_phase, FILE_IO_READING);
    ASSERT_EQ(mock.submitted, 1);
    ASSERT_EQ(mock.last_len, (size_t)100);  /* capped to read_cap */
    ASSERT_EQ(mock.last_offset, (off_t)0);

    /* Complete first chunk */
    memset(c.read_buf, 'A', 100);
    state = kl_conn_on_file_complete(&c, 100, 0);

    /* After writing 100 bytes, offset advances, submits next read */
    ASSERT_EQ(c.res.file_offset, (off_t)100);
    ASSERT_EQ(mock.submitted, 2);
    ASSERT_EQ(mock.last_offset, (off_t)100);
    ASSERT_EQ(mock.last_len, (size_t)100);

    /* Complete second chunk */
    memset(c.read_buf, 'B', 100);
    state = kl_conn_on_file_complete(&c, 100, 0);
    ASSERT_EQ(c.res.file_offset, (off_t)200);
    ASSERT_EQ(mock.submitted, 3);
    ASSERT_EQ(mock.last_len, (size_t)50);  /* remaining = 50 */

    /* Complete final chunk */
    memset(c.read_buf, 'C', 50);
    state = kl_conn_on_file_complete(&c, 50, 0);
    ASSERT_EQ(c.res.file_offset, (off_t)250);

    /* Drain the data from the read end (non-blocking) */
    fcntl(fds[1], F_SETFL, fcntl(fds[1], F_GETFL) | O_NONBLOCK);
    char drain_buf[4096];
    size_t total = 0;
    ssize_t nr;
    while ((nr = read(fds[1], drain_buf, sizeof(drain_buf))) > 0)
        total += (size_t)nr;

    ASSERT_EQ(total, (size_t)250);

    kl_free(&a, c.read_buf, bufcap);
    close(fds[0]);
    close(fds[1]);
}

UTEST(file_io, cancel_on_timeout) {
    /* Pending read cancelled on connection timeout */
    MockFileIO mock;
    mock_file_io_init(&mock);
    KlAllocator a = kl_allocator_default();

    KlConn c;
    memset(&c, 0, sizeof(c));
    c.alloc = &a;
    c.fd = 99;
    c.state = KL_CONN_SENDING;
    c.file_io = &mock.base;
    c.file_io_phase = FILE_IO_READING;

    /* Simulate what the server timeout sweep does */
    mock.base.cancel(&mock.base, c.fd);
    c.file_io_phase = FILE_IO_CANCELLING;

    ASSERT_EQ(mock.cancelled, 1);
    ASSERT_EQ(mock.cancel_sock_fd, 99);
    ASSERT_EQ(c.file_io_phase, FILE_IO_CANCELLING);
}

UTEST(file_io, cancel_cqe) {
    /* Cancel completion handled, connection released */
    MockFileIO mock;
    mock_file_io_init(&mock);
    KlAllocator a = kl_allocator_default();

    KlConn c;
    memset(&c, 0, sizeof(c));
    c.alloc = &a;
    c.fd = 99;
    c.state = KL_CONN_SENDING;
    c.file_io = &mock.base;
    c.file_io_phase = FILE_IO_CANCELLING;

    c.read_buf = kl_malloc(&a, 8192);
    c.read_cap = 8192;

    /* Cancel CQE arrives with -ECANCELED */
    KlConnState state = kl_conn_on_file_complete(&c, -125, 0);  /* -ECANCELED */
    ASSERT_EQ(state, KL_CONN_CLOSED);
    ASSERT_EQ(c.file_io_phase, FILE_IO_IDLE);

    kl_free(&a, c.read_buf, 8192);
}

UTEST(file_io, tls_fallback) {
    /* TLS connections use existing pread path, not file_io */
    MockFileIO mock;
    mock_file_io_init(&mock);
    KlAllocator a = kl_allocator_default();

    /* Create a fake TLS pointer (just non-NULL to trigger TLS path) */
    int fake_tls = 1;

    KlConn c;
    memset(&c, 0, sizeof(c));
    c.alloc = &a;
    c.fd = -1;
    c.state = KL_CONN_SENDING;
    c.file_io = &mock.base;
    c.file_io_phase = FILE_IO_IDLE;
    c.tls = (KlTls *)(void *)&fake_tls;  /* non-NULL = TLS active */

    c.read_buf = kl_malloc(&a, 8192);
    c.read_cap = 8192;

    memset(&c.res, 0, sizeof(c.res));
    c.res.body_mode = KL_BODY_FILE;
    c.res.file_fd = 10;
    c.res.file_size = 100;
    c.res.headers_sent = 1;
    c.res.conn_fd = -1;

    /* on_writable with TLS should NOT use file_io, should use response_send */
    (void)kl_conn_on_writable(&c);
    /* Will fail due to invalid fd, but should NOT have called submit */
    ASSERT_EQ(mock.submitted, 0);

    kl_free(&a, c.read_buf, 8192);
}

UTEST(file_io, head_request) {
    /* HEAD request sends headers only, no file read */
    MockFileIO mock;
    mock_file_io_init(&mock);
    KlAllocator a = kl_allocator_default();

    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);

    KlConn c;
    memset(&c, 0, sizeof(c));
    c.alloc = &a;
    c.fd = fds[0];
    c.state = KL_CONN_SENDING;
    c.file_io = &mock.base;
    c.file_io_phase = FILE_IO_IDLE;

    c.read_buf = kl_malloc(&a, 8192);
    c.read_cap = 8192;

    memset(&c.res, 0, sizeof(c.res));
    c.res.body_mode = KL_BODY_FILE;
    c.res.file_fd = 10;
    c.res.file_size = 100;
    c.res.headers_sent = 1;
    c.res.head_request = 1;  /* HEAD request */

    KlConnState state = kl_conn_on_writable(&c);
    /* HEAD: no file read submitted, should complete */
    ASSERT_EQ(mock.submitted, 0);
    /* Connection should be done (CLOSED since keep_alive=0) */
    ASSERT_EQ(state, KL_CONN_CLOSED);

    kl_free(&a, c.read_buf, 8192);
    close(fds[0]);
    close(fds[1]);
}

UTEST(file_io, read_error) {
    /* Negative result from file I/O → close connection */
    KlAllocator a = kl_allocator_default();

    KlConn c;
    memset(&c, 0, sizeof(c));
    c.alloc = &a;
    c.fd = -1;
    c.state = KL_CONN_SENDING;
    c.file_io_phase = FILE_IO_READING;

    c.read_buf = kl_malloc(&a, 8192);
    c.read_cap = 8192;

    KlConnState state = kl_conn_on_file_complete(&c, -5, 0);
    ASSERT_EQ(state, KL_CONN_CLOSED);
    ASSERT_EQ(c.file_io_phase, FILE_IO_IDLE);

    /* Zero-byte read also closes */
    c.state = KL_CONN_SENDING;
    c.file_io_phase = FILE_IO_READING;
    state = kl_conn_on_file_complete(&c, 0, 0);
    ASSERT_EQ(state, KL_CONN_CLOSED);

    kl_free(&a, c.read_buf, 8192);
}

UTEST(file_io, zero_copy_skips_write) {
    /* zero_copy=1: data already on socket, advance offset, no WRITING phase */
    MockFileIO mock;
    mock_file_io_init(&mock);
    KlAllocator a = kl_allocator_default();

    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);

    KlConn c;
    memset(&c, 0, sizeof(c));
    c.alloc = &a;
    c.fd = fds[0];
    c.state = KL_CONN_SENDING;
    c.file_io = &mock.base;
    c.file_io_phase = FILE_IO_READING;

    c.read_buf = kl_malloc(&a, 8192);
    c.read_cap = 8192;

    memset(&c.res, 0, sizeof(c.res));
    c.res.body_mode = KL_BODY_FILE;
    c.res.file_fd = 10;
    c.res.file_size = 500;
    c.res.file_offset = 0;
    c.res.headers_sent = 1;

    /* Simulate zero-copy completion of 500 bytes */
    KlConnState state = kl_conn_on_file_complete(&c, 500, 1);

    /* File fully sent — offset advanced, should complete */
    ASSERT_EQ(c.res.file_offset, (off_t)500);
    /* No WRITING phase — file_io_phase should NOT be FILE_IO_WRITING */
    ASSERT_NE(c.file_io_phase, FILE_IO_WRITING);
    /* Connection done (CLOSED since keep_alive=0) */
    ASSERT_EQ(state, KL_CONN_CLOSED);

    kl_free(&a, c.read_buf, 8192);
    close(fds[0]);
    close(fds[1]);
}

UTEST(file_io, zero_copy_multi_chunk) {
    /* zero_copy: multiple chunks, offset advances, next read submitted */
    MockFileIO mock;
    mock_file_io_init(&mock);
    KlAllocator a = kl_allocator_default();

    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);

    KlConn c;
    memset(&c, 0, sizeof(c));
    c.alloc = &a;
    c.fd = fds[0];
    c.state = KL_CONN_SENDING;
    c.file_io = &mock.base;

    size_t bufcap = 100;
    c.read_buf = kl_malloc(&a, bufcap);
    c.read_cap = bufcap;

    memset(&c.res, 0, sizeof(c.res));
    c.res.body_mode = KL_BODY_FILE;
    c.res.file_fd = 10;
    c.res.file_size = 250;
    c.res.file_offset = 0;
    c.res.headers_sent = 1;

    /* First chunk: zero-copy 100 bytes */
    c.file_io_phase = FILE_IO_READING;
    KlConnState state = kl_conn_on_file_complete(&c, 100, 1);
    ASSERT_EQ(c.res.file_offset, (off_t)100);
    ASSERT_EQ(state, KL_CONN_SENDING);
    ASSERT_EQ(mock.submitted, 1);  /* next read submitted */

    /* Second chunk: zero-copy 100 bytes */
    c.file_io_phase = FILE_IO_READING;
    state = kl_conn_on_file_complete(&c, 100, 1);
    ASSERT_EQ(c.res.file_offset, (off_t)200);
    ASSERT_EQ(mock.submitted, 2);

    /* Final chunk: zero-copy 50 bytes */
    c.file_io_phase = FILE_IO_READING;
    state = kl_conn_on_file_complete(&c, 50, 1);
    ASSERT_EQ(c.res.file_offset, (off_t)250);
    /* File done */

    kl_free(&a, c.read_buf, bufcap);
    close(fds[0]);
    close(fds[1]);
}

/* Mock submit that rejects buf=NULL (no splice), accepts buf!=NULL */
static int mock_submit_no_splice(KlFileIO *fio, int file_fd, void *buf,
                                  size_t len, off_t offset,
                                  int sock_fd, void *udata) {
    MockFileIO *m = (MockFileIO *)fio;
    if (buf == NULL) return -1;  /* no splice support */
    m->submitted++;
    m->last_file_fd = file_fd;
    m->last_buf = buf;
    m->last_len = len;
    m->last_offset = offset;
    m->last_sock_fd = sock_fd;
    m->last_udata = udata;
    return 0;
}

UTEST(file_io, splice_fallback) {
    /* When splice submit (buf=NULL) returns -1, falls back to buffered read */
    MockFileIO mock;
    mock_file_io_init(&mock);
    mock.base.submit = mock_submit_no_splice;

    KlAllocator a = kl_allocator_default();

    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);

    KlConn c;
    memset(&c, 0, sizeof(c));
    c.alloc = &a;
    c.fd = fds[0];
    c.state = KL_CONN_SENDING;
    c.file_io = &mock.base;
    c.file_io_phase = FILE_IO_IDLE;

    c.read_buf = kl_malloc(&a, 8192);
    c.read_cap = 8192;

    memset(&c.res, 0, sizeof(c.res));
    c.res.body_mode = KL_BODY_FILE;
    c.res.file_fd = 10;
    c.res.file_size = 100;
    c.res.file_offset = 0;
    c.res.headers_sent = 1;

    /* on_writable triggers conn_file_submit_read:
     * splice (buf=NULL) fails → falls back to buffered read (buf=read_buf) */
    KlConnState state = kl_conn_on_writable(&c);
    ASSERT_EQ(state, KL_CONN_SENDING);
    ASSERT_EQ(mock.submitted, 1);
    ASSERT_EQ(mock.last_buf, c.read_buf);  /* buffered path used */
    ASSERT_EQ(c.file_io_phase, FILE_IO_READING);

    kl_free(&a, c.read_buf, 8192);
    close(fds[0]);
    close(fds[1]);
}

UTEST_MAIN();
