/*
 * completion_pipe_absent.c: the named-pipe completion seam on every engine that has none.
 *
 * Linked on every build whose event backend is not IOCP (epoll / kqueue / poll / WSAPoll / io_uring /
 * pollcomp). kl_comp_pipe_available() is 0, and the pipe transport checks it before it opens a
 * handle, so the remaining functions are unreachable in a correct program; they fail closed rather
 * than abort, because a caller that skipped the check should get an error, not a crash.
 *
 * No readiness emulation lives here on purpose: a named pipe is completion-native, and WSAPoll cannot
 * watch a HANDLE. See docs/architecture/windows_named_pipes.md.
 */
#include "completion_pipe.h"

int kl_comp_pipe_available(const struct KlEventCtx *ctx) { (void)ctx; return 0; }

int kl_comp_pipe_attach(struct KlEventCtx *ctx, KlPipeHandle *h) {
    (void)ctx; (void)h;
    return -1;
}

int kl_comp_pipe_post(struct KlEventCtx *ctx, const KlPipeIoOp *op) {
    (void)ctx; (void)op;
    return -1;
}

void kl_comp_pipe_cancel(struct KlEventCtx *ctx, const struct KlDgramLife *life, KlPipeOpKind kind) {
    (void)ctx; (void)life; (void)kind;
}
