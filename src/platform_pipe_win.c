/*
 * platform_pipe_win.c: the Windows implementation of the named-pipe PAL seam (platform_pipe.h).
 *
 * Opens the CLIENT end of a local named pipe. Everything Win32 about it stays in this TU: UTF-8 to
 * UTF-16, CreateFileW, the security-quality-of-service flags, the read mode, and CloseHandle.
 *
 * Choices, each for a reason:
 *
 *   - LOCAL ONLY. The path must be \\.\pipe\<name>. A \\server\pipe\ name would open a remote pipe
 *     over SMB, which is not local IPC and would authenticate as the user to another machine.
 *   - NEVER WAITS. CreateFileW on a local pipe returns immediately: a handle, ERROR_PIPE_BUSY (every
 *     instance is connected), or ERROR_FILE_NOT_FOUND (no server). WaitNamedPipe blocks the calling
 *     thread, which here is the event loop, so it is not used; a busy pipe is reported and the retry
 *     policy belongs to the caller.
 *   - IDENTIFICATION, NOT IMPERSONATION. SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION lets the
 *     server learn who connected but not act as them. The default (SecurityImpersonation) would let a
 *     server that squatted the name impersonate the connecting user.
 *   - BYTE READ MODE, explicitly. Keel's contract is an ordered byte stream, and byte read mode also
 *     rules out ERROR_MORE_DATA (a message-mode partial read), whose completion semantics differ.
 *   - FILE_FLAG_OVERLAPPED, and NO FILE_SKIP_COMPLETION_PORT_ON_SUCCESS, so every I/O that is
 *     accepted queues exactly one completion packet (single-shot, invariant I5).
 *   - IT IS REALLY A PIPE. GetFileType must say FILE_TYPE_PIPE; the prefix check alone is textual.
 */
#include "platform_pipe.h"

#include <windows.h>
#include <string.h>

/* A pipe name is at most 256 characters; the \\.\pipe\ prefix is 9 more. Generous, bounded. */
#define KL_PIPE_WPATH_MAX 512

/* Is `p` exactly "\\.\pipe\" (case-insensitive on "pipe") followed by a non-empty name? */
static int pipe_path_is_local(const char *p) {
    static const char pre[] = "\\\\.\\pipe\\";
    size_t n = sizeof(pre) - 1;
    for (size_t i = 0; i < n; i++) {
        char c = p[i];
        if (c == '\0') return 0;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != pre[i]) return 0;
    }
    return p[n] != '\0';
}

KlPipeOpenStatus kl_plat_pipe_open_client(const char *path, KlPipeHandle **out) {
    if (!path || !out || !pipe_path_is_local(path)) return KL_PIPE_OPEN_INVALID;

    wchar_t wpath[KL_PIPE_WPATH_MAX];
    int wn = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wpath, KL_PIPE_WPATH_MAX);
    if (wn <= 0) return KL_PIPE_OPEN_INVALID;   /* invalid UTF-8, or longer than any pipe name */

    HANDLE h = CreateFileW(wpath, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                           NULL);
    if (h == INVALID_HANDLE_VALUE) {
        switch (GetLastError()) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:  return KL_PIPE_OPEN_ABSENT;
        case ERROR_PIPE_BUSY:       return KL_PIPE_OPEN_BUSY;
        case ERROR_ACCESS_DENIED:   return KL_PIPE_OPEN_DENIED;
        case ERROR_INVALID_NAME:
        case ERROR_BAD_PATHNAME:    return KL_PIPE_OPEN_INVALID;
        default:                    return KL_PIPE_OPEN_ERROR;
        }
    }
    if (GetFileType(h) != FILE_TYPE_PIPE) {
        CloseHandle(h);
        return KL_PIPE_OPEN_INVALID;
    }
    DWORD mode = PIPE_READMODE_BYTE;
    if (!SetNamedPipeHandleState(h, &mode, NULL, NULL)) {
        CloseHandle(h);
        return KL_PIPE_OPEN_ERROR;
    }
    *out = (KlPipeHandle *)h;
    return KL_PIPE_OPEN_OK;
}

void kl_plat_pipe_close(KlPipeHandle *h) {
    if (h) CloseHandle((HANDLE)h);
}
