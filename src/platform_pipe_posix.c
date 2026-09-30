/*
 * platform_pipe_posix.c: the named-pipe PAL seam on POSIX, where there are no Windows Named Pipes.
 *
 * POSIX local bidirectional connection-oriented IPC is AF_UNIX, which Keel already serves through the
 * socket provider. A POSIX FIFO is not the counterpart (it is one-way and has no per-client instance),
 * so nothing here tries to map a pipe name onto one.
 */
#include "platform_pipe.h"

KlPipeOpenStatus kl_plat_pipe_open_client(const char *path, KlPipeHandle **out) {
    (void)path; (void)out;
    return KL_PIPE_OPEN_UNSUPPORTED;
}

KlPipeOpenStatus kl_plat_pipe_create_instance(const char *path, int first, KlPipeHandle **out) {
    (void)path; (void)first; (void)out;
    return KL_PIPE_OPEN_UNSUPPORTED;
}

/* The POSIX anonymous pair is a pipe2 pair on the readiness engines, a different adapter from this
 * completion-mode seam; this seam has none to offer. */
KlPipeOpenStatus kl_plat_pipe_create_pair(int parent_reads, KlPipeHandle **parent, KlPipeHandle **child) {
    (void)parent_reads; (void)parent; (void)child;
    return KL_PIPE_OPEN_UNSUPPORTED;
}

void kl_plat_pipe_close(KlPipeHandle *h) { (void)h; }
