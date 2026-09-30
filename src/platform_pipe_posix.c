/*
 * platform_pipe_posix.c: the pipe PAL seam (platform_pipe.h) on POSIX.
 *
 * NAMED PIPES: none. POSIX local bidirectional connection-oriented IPC is AF_UNIX, which Keel already
 * serves through the socket provider. A POSIX FIFO is not the counterpart (it is one-way and has no
 * per-client instance), so nothing here maps a pipe name onto one.
 *
 * ANONYMOUS PAIRS: an ordinary pipe. Every native pipe call lives in this TU:
 *
 *   - CREATION. pipe2(O_CLOEXEC), or pipe + FD_CLOEXEC on macOS (no pipe2; the window where a
 *     concurrent fork in another thread inherits both ends is the spawner's to close with its own
 *     spawn lock). Only the PARENT end is O_NONBLOCK: a child's runtime expects a blocking descriptor.
 *   - I/O. read / write, EINTR-retried, on the event loop only when readiness says so. A pipe end is
 *     a file descriptor, not a socket: nothing here goes through KlSocketProvider.
 *   - SIGPIPE, LOCALLY. A write to a pipe whose reader has gone raises SIGPIPE, whose default action
 *     kills the process, and write() has no per-call flag to stop it. Keel never changes the process's
 *     signal disposition. Instead:
 *       - where the platform has F_SETNOSIGPIPE (macOS), the parent write end is created with it, and
 *         the kernel reports EPIPE without a signal;
 *       - elsewhere each write blocks SIGPIPE in the CALLING THREAD only (pthread_sigmask). If the
 *         write fails with EPIPE, the SIGPIPE it raised is consumed with a zero-timeout sigtimedwait,
 *         but only if none was already pending before the write, so an embedder's own pending SIGPIPE
 *         is left alone. Then the thread's previous mask is restored.
 *
 * HANDLE ENCODING. KlPipeHandle is an opaque pointer. Here it carries a descriptor as fd + 1, so NULL
 * is "no handle" and fd 0 is representable. The encoding lives in pipe_h / pipe_fd below and nowhere
 * else (check-pipe-seam exempts this TU, and only this TU, from its no-integer-cast rule for that).
 */
#if !defined(_GNU_SOURCE) && !defined(__APPLE__)
#define _GNU_SOURCE   /* pipe2 */
#endif
#include "platform_pipe.h"
#include <keel/anon_pipe_native.h>   /* kl_anon_pipe_end_fd */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

static KlPipeHandle *pipe_h(int fd) { return (KlPipeHandle *)(uintptr_t)((unsigned)fd + 1u); }
static int pipe_fd(const KlPipeHandle *h) { return h ? (int)((uintptr_t)h - 1u) : -1; }

/* ── Named pipes: not on POSIX ─────────────────────────────────────────────────────────────── */

KlPipeOpenStatus kl_plat_pipe_open_client(const char *path, KlPipeHandle **out) {
    (void)path; (void)out;
    return KL_PIPE_OPEN_UNSUPPORTED;
}

KlPipeOpenStatus kl_plat_pipe_create_instance(const char *path, int first, KlPipeHandle **out) {
    (void)path; (void)first; (void)out;
    return KL_PIPE_OPEN_UNSUPPORTED;
}

/* ── Anonymous pair ────────────────────────────────────────────────────────────────────────── */

#if defined(__APPLE__)
static int set_cloexec(int fd) {
    int f = fcntl(fd, F_GETFD, 0);
    return (f < 0 || fcntl(fd, F_SETFD, f | FD_CLOEXEC) < 0) ? -1 : 0;
}
#endif

KlPipeOpenStatus kl_plat_pipe_create_pair(int parent_reads, KlPipeHandle **parent, KlPipeHandle **child) {
    if (!parent || !child) return KL_PIPE_OPEN_INVALID;
    int fds[2];
#if defined(__APPLE__)
    if (pipe(fds) < 0) return KL_PIPE_OPEN_ERROR;
    if (set_cloexec(fds[0]) < 0 || set_cloexec(fds[1]) < 0) goto fail;
#else
    if (pipe2(fds, O_CLOEXEC) < 0) return KL_PIPE_OPEN_ERROR;
#endif
    int pfd = parent_reads ? fds[0] : fds[1];
    int cfd = parent_reads ? fds[1] : fds[0];
    int fl = fcntl(pfd, F_GETFL, 0);
    if (fl < 0 || fcntl(pfd, F_SETFL, fl | O_NONBLOCK) < 0) goto fail;
#if defined(F_SETNOSIGPIPE)
    if (!parent_reads && fcntl(pfd, F_SETNOSIGPIPE, 1) < 0) goto fail;
#endif
    *parent = pipe_h(pfd);
    *child  = pipe_h(cfd);
    return KL_PIPE_OPEN_OK;
fail:
    close(fds[0]);
    close(fds[1]);
    return KL_PIPE_OPEN_ERROR;
}

void kl_plat_pipe_close(KlPipeHandle *h) {
    if (h) close(pipe_fd(h));
}

/* ── Readiness I/O ─────────────────────────────────────────────────────────────────────────── */

int kl_plat_pipe_readiness(void) { return 1; }

KlSocketHandle kl_plat_pipe_pollable(const KlPipeHandle *h) {
    return h ? (KlSocketHandle)pipe_fd(h) : KL_INVALID_SOCKET;
}

static int transient(int e) { return e == EAGAIN || e == EWOULDBLOCK; }

kl_ssize_t kl_plat_pipe_read(KlPipeHandle *h, char *buf, size_t len, int *would_block) {
    *would_block = 0;
    ssize_t n;
    do { n = read(pipe_fd(h), buf, len); } while (n < 0 && errno == EINTR);
    if (n < 0 && transient(errno)) *would_block = 1;
    return (kl_ssize_t)n;
}

#if defined(F_SETNOSIGPIPE)
/* The parent write end carries F_SETNOSIGPIPE: EPIPE comes back without a signal. */
static ssize_t write_quiet(int fd, const char *data, size_t len) {
    ssize_t n;
    do { n = write(fd, data, len); } while (n < 0 && errno == EINTR);
    return n;
}
#else
/* Block SIGPIPE in this thread for the write; consume only the SIGPIPE this write raised. */
static ssize_t write_quiet(int fd, const char *data, size_t len) {
    sigset_t pipe_only, old, pend;
    sigemptyset(&pipe_only);
    sigaddset(&pipe_only, SIGPIPE);
    int pending_before = (sigpending(&pend) == 0 && sigismember(&pend, SIGPIPE) == 1);
    pthread_sigmask(SIG_BLOCK, &pipe_only, &old);
    ssize_t n;
    do { n = write(fd, data, len); } while (n < 0 && errno == EINTR);
    int err = errno;
    if (n < 0 && err == EPIPE && !pending_before) {
        const struct timespec zero = { 0, 0 };
        while (sigtimedwait(&pipe_only, NULL, &zero) < 0 && errno == EINTR) { }
    }
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    errno = err;
    return n;
}
#endif

kl_ssize_t kl_plat_pipe_write(KlPipeHandle *h, const char *data, size_t len, int *would_block) {
    *would_block = 0;
    ssize_t n = write_quiet(pipe_fd(h), data, len);
    if (n < 0 && transient(errno)) *would_block = 1;
    return (kl_ssize_t)n;
}

/* ── The child's end ───────────────────────────────────────────────────────────────────────── */

void kl_plat_pipe_end_adopt(KlAnonPipeEnd *e, KlPipeHandle *child) {
    e->_handle = NULL;
    e->_fd1    = pipe_fd(child) + 1;
}

void kl_plat_pipe_end_release(KlAnonPipeEnd *e) {
    if (e->_fd1 > 0) close(e->_fd1 - 1);
    e->_handle = NULL;
    e->_fd1    = 0;
}

int kl_anon_pipe_end_fd(const KlAnonPipeEnd *peer) {
    return (peer && peer->_fd1 > 0) ? peer->_fd1 - 1 : -1;
}
