/*
 * platform_pipe.h: INTERNAL platform-services interface for Windows Named Pipes, part of the PAL.
 * No ABI commitment.
 *
 * One concern: creating, opening and closing the NATIVE handle of a local pipe endpoint (a named-pipe
 * client or server instance, or the two ends of an anonymous pair). The byte
 * I/O on that handle is not here; it rides the completion engine (completion_pipe.h), because on
 * Windows it is overlapped ReadFile/WriteFile on the IOCP port.
 *
 * WHY A PIPE IS NOT A SOCKET HERE. A named pipe is a Win32 HANDLE, not a Winsock SOCKET, so it
 * never travels through KlSocketProvider or KlSocketHandle. It has its own handle type below: an
 * INCOMPLETE struct pointer. That keeps it pointer-width (a HANDLE is a pointer) while making it a
 * distinct type the compiler will not implicitly convert to KlSocketHandle (intptr_t) or int, so
 * narrowing it or passing it to a socket op is a compile error rather than a silent bug.
 *
 * WHY A SEPARATE PAL HEADER: the same reason platform_socket.h and platform_thread.h are separate. It
 * is a per-concern seam with its own per-OS TU pair:
 *
 *   platform_pipe_win.c     CreateFileW on \\.\pipe\..., byte read mode, identification-only SQOS;
 *                           the anonymous pair as a private single-instance named pipe
 *   platform_pipe_posix.c   KL_PIPE_OPEN_UNSUPPORTED (POSIX local IPC is AF_UNIX, not this)
 *
 * HANDLE, SECURITY_ATTRIBUTES and every Win32 call stay confined to the Windows TU; this header pulls
 * in no platform header at all.
 */
#ifndef KEEL_SRC_PLATFORM_PIPE_H
#define KEEL_SRC_PLATFORM_PIPE_H

/* The native pipe handle. Never defined: the Windows TU casts it to and from HANDLE. */
typedef struct KlPipeHandle KlPipeHandle;

/* Outcome of opening a client end. Mirrors the public KlPipeStatus one-for-one (pipe_stream.c maps
 * it) so the public header need not include this internal one. */
typedef enum {
    KL_PIPE_OPEN_OK = 0,
    KL_PIPE_OPEN_ABSENT,        /* no server has created the pipe (ERROR_FILE_NOT_FOUND) */
    KL_PIPE_OPEN_BUSY,          /* every instance is connected (ERROR_PIPE_BUSY); retry is the caller's */
    KL_PIPE_OPEN_DENIED,        /* the pipe's DACL refused the requested access */
    KL_PIPE_OPEN_INVALID,       /* not a local \\.\pipe\ name, too long, or not valid UTF-8 */
    KL_PIPE_OPEN_UNSUPPORTED,   /* this platform has no named pipes */
    KL_PIPE_OPEN_ERROR,         /* any other failure */
    KL_PIPE_OPEN_IN_USE         /* listen: the name already exists (another server, or a squatter) */
} KlPipeOpenStatus;

/* Open the client end of the LOCAL named pipe `path` (UTF-8, "\\.\pipe\<name>") for overlapped
 * duplex byte I/O, WITHOUT waiting: an absent or busy pipe fails at once (WaitNamedPipe blocks and is
 * never used). On KL_PIPE_OPEN_OK, *out is the handle; otherwise *out is untouched. A remote server
 * name (\\host\pipe\...) is rejected as INVALID: this is local IPC. */
KlPipeOpenStatus kl_plat_pipe_open_client(const char *path, KlPipeHandle **out);

/* Create one SERVER instance of the local named pipe `path` for overlapped duplex byte I/O, ready for
 * an overlapped ConnectNamedPipe, the completion_pipe.h KL_PIPE_OP_ACCEPT op. `first` = 1 for a listener's
 * first instance: it claims the name with FILE_FLAG_FIRST_PIPE_INSTANCE, so a name some other process
 * already created (a squatter) fails with KL_PIPE_OPEN_IN_USE instead of being shared. Security is
 * fixed, not configurable: remote clients are rejected, and the DACL grants the current user and
 * LocalSystem only (the default pipe DACL would also grant Everyone read). */
KlPipeOpenStatus kl_plat_pipe_create_instance(const char *path, int first, KlPipeHandle **out);

/* Create an anonymous, one-directional pipe pair. `*parent` is overlapped (for the completion engine)
 * and `*child` synchronous (for a child process's C runtime); neither is inheritable. `parent_reads` =
 * 1: the parent end reads and the child end writes; 0: the reverse. Each end has access for its own
 * direction only. On KL_PIPE_OPEN_OK both are set; on any other status neither is, and nothing is
 * left open. Windows builds the pair as a private named pipe: a random name, a single instance, the
 * current-user + LocalSystem DACL, remote clients rejected, and the connected client verified to be
 * this process. */
KlPipeOpenStatus kl_plat_pipe_create_pair(int parent_reads, KlPipeHandle **parent, KlPipeHandle **child);

/* Close a handle from kl_plat_pipe_open_client, kl_plat_pipe_create_instance or kl_plat_pipe_create_pair. Precondition: no overlapped op on it is outstanding
 * (every op physically retired), so no completion can target it after this returns. NULL-safe. */
void kl_plat_pipe_close(KlPipeHandle *h);

#endif /* KEEL_SRC_PLATFORM_PIPE_H */
