/*
 * keel/anon_pipe_native.h: the native value of an anonymous pipe's child end, for the spawner only.
 *
 * Explicitly platform-specific, and deliberately separate from <keel/anon_pipe.h> so portable code
 * never names a native type. No platform header is included: a Windows HANDLE is returned as void *.
 *
 * OWNERSHIP: BORROWED. Reading the native value transfers nothing. The KlAnonPipeEnd still owns it, and
 * the only way to release it is kl_anon_pipe_end_close; never CloseHandle / close() it directly. The
 * value is valid until that close. A spawner that needs a copy it owns (e.g. to dup2 onto a standard
 * descriptor in the child) makes that copy itself; the original is still closed through the end.
 *
 * INHERITANCE IS THE SPAWNER'S. Keel creates the end non-inheritable / close-on-exec and never changes
 * that. The intended sequence:
 *
 *   kl_anon_pipe_create                  child end non-inheritable, parent end never inheritable
 *   native = kl_anon_pipe_end_handle()   (or _fd); borrowed
 *   make ONLY that value inheritable     Windows: SetHandleInformation(HANDLE_FLAG_INHERIT) and,
 *                                        ideally, PROC_THREAD_ATTRIBUTE_HANDLE_LIST naming just it;
 *                                        POSIX: dup2 it onto 0/1/2 in the child (the copy is not
 *                                        close-on-exec; the original is)
 *   spawn
 *   kl_anon_pipe_end_close               in the parent, on success or failure, so the parent holds no
 *                                        copy of the child's end (otherwise a reader never sees EOF)
 */
#ifndef KEEL_ANON_PIPE_NATIVE_H
#define KEEL_ANON_PIPE_NATIVE_H

#include <keel/anon_pipe.h>
#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
/** The child end's HANDLE (as void *), or NULL if `peer` is empty. Created non-inheritable and
 *  synchronous (not overlapped). */
void *kl_anon_pipe_end_handle(const KlAnonPipeEnd *peer);
#else
/** The child end's file descriptor, or -1 if `peer` is empty. Created close-on-exec and blocking.
 *  macOS has no pipe2, so both ends are made close-on-exec just after creation; a concurrent fork in
 *  another thread in that window inherits them, which the spawner closes with its own spawn lock. */
int   kl_anon_pipe_end_fd(const KlAnonPipeEnd *peer);
#endif

#ifdef __cplusplus
}
#endif

#endif /* KEEL_ANON_PIPE_NATIVE_H */
