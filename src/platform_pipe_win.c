/*
 * platform_pipe_win.c: the Windows implementation of the named-pipe PAL seam (platform_pipe.h).
 *
 * Opens the CLIENT end of a local named pipe, creates SERVER instances for the pipe listener, and
 * creates anonymous pairs (kl_plat_pipe_create_pair).
 * Everything Win32 about either stays in this TU: UTF-8 to UTF-16, CreateFileW / CreateNamedPipeW, the
 * security descriptor, the security-quality-of-service flags, the read mode, and CloseHandle.
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
#include <keel/anon_pipe_native.h>   /* kl_anon_pipe_end_handle */

#include <windows.h>
#include <bcrypt.h>    /* BCryptGenRandom: the anonymous pair's unguessable name */
#include <sddl.h>      /* ConvertSidToStringSidW / ConvertStringSecurityDescriptorToSecurityDescriptorW */
#include <stdio.h>
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

/* Validate a local pipe name and convert it to UTF-16. 0, or -1 (invalid / too long / bad UTF-8). */
static int pipe_wpath(const char *path, wchar_t *wpath) {
    if (!path || !pipe_path_is_local(path)) return -1;
    int wn = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wpath, KL_PIPE_WPATH_MAX);
    return wn > 0 ? 0 : -1;
}

KlPipeOpenStatus kl_plat_pipe_open_client(const char *path, KlPipeHandle **out) {
    wchar_t wpath[KL_PIPE_WPATH_MAX];
    if (!out || pipe_wpath(path, wpath) != 0) return KL_PIPE_OPEN_INVALID;

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

/* The server DACL: the current user and LocalSystem, full access; nobody else (protected, so nothing
 * is inherited). Built from the PROCESS token's user so an impersonating thread cannot widen it. The
 * caller frees *out with LocalFree. */
static int pipe_server_sd(PSECURITY_DESCRIPTOR *out) {
    HANDLE tok = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return -1;
    /* TOKEN_USER plus the SID it points into; aligned for the TOKEN_USER at its start. */
    _Alignas(TOKEN_USER) unsigned char tu_buf[256];
    DWORD len = 0;
    BOOL ok = GetTokenInformation(tok, TokenUser, tu_buf, (DWORD)sizeof(tu_buf), &len);
    CloseHandle(tok);
    if (!ok) return -1;
    const TOKEN_USER *tu = (const TOKEN_USER *)tu_buf;
    wchar_t *sid = NULL;
    if (!ConvertSidToStringSidW(tu->User.Sid, &sid)) return -1;
    wchar_t sddl[256];
    int n = (int)(sizeof(sddl) / sizeof(sddl[0]));
    int r = -1;
    if (lstrlenW(sid) < n - 40) {
        lstrcpyW(sddl, L"D:P(A;;GA;;;");
        lstrcatW(sddl, sid);
        lstrcatW(sddl, L")(A;;GA;;;SY)");
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, out, NULL)) r = 0;
    }
    LocalFree(sid);
    return r;
}

KlPipeOpenStatus kl_plat_pipe_create_instance(const char *path, int first, KlPipeHandle **out) {
    wchar_t wpath[KL_PIPE_WPATH_MAX];
    if (!out || pipe_wpath(path, wpath) != 0) return KL_PIPE_OPEN_INVALID;
    PSECURITY_DESCRIPTOR sd = NULL;
    if (pipe_server_sd(&sd) != 0) return KL_PIPE_OPEN_ERROR;
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = sd;
    sa.bInheritHandle = FALSE;
    HANDLE h = CreateNamedPipeW(wpath,
                                PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
                                    (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                                    PIPE_REJECT_REMOTE_CLIENTS,
                                PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0, &sa);
    DWORD err = (h == INVALID_HANDLE_VALUE) ? GetLastError() : 0;
    LocalFree(sd);
    if (h == INVALID_HANDLE_VALUE) {
        switch (err) {
        case ERROR_ACCESS_DENIED:   return KL_PIPE_OPEN_IN_USE;   /* first instance: name taken */
        case ERROR_PIPE_BUSY:       return KL_PIPE_OPEN_BUSY;     /* instance limit reached */
        case ERROR_INVALID_NAME:
        case ERROR_BAD_PATHNAME:    return KL_PIPE_OPEN_INVALID;
        default:                    return KL_PIPE_OPEN_ERROR;
        }
    }
    *out = (KlPipeHandle *)h;
    return KL_PIPE_OPEN_OK;
}

/* ── Anonymous pair ──────────────────────────────────────────────────────────────────────────
 *
 * CreatePipe handles are never overlapped, so they cannot ride the IOCP port. The pair is instead a
 * private named pipe whose server end is the parent's (overlapped) and whose client end is the
 * child's (synchronous). Private because:
 *   - the name carries 128 bits from BCryptGenRandom (plus pid and a counter for uniqueness), and no
 *     random bits means no pair: the name is never guessable;
 *   - FILE_FLAG_FIRST_PIPE_INSTANCE and one instance: nobody can pre-create or share the name, and once
 *     the child end connects no one else can;
 *   - the current-user + LocalSystem DACL and PIPE_REJECT_REMOTE_CLIENTS, as for listener instances;
 *   - the connected client is verified to be this process (GetNamedPipeClientProcessId).
 * Each end gets the access CreatePipe would give it: data in its own direction, plus attribute access
 * so the child's runtime can query or set the pipe state. Neither end is inheritable. ConnectNamedPipe
 * is not needed: the client open connects the single instance. */

static volatile LONG g_anon_seq;

KlPipeOpenStatus kl_plat_pipe_create_pair(int parent_reads, KlPipeHandle **parent, KlPipeHandle **child) {
    if (!parent || !child) return KL_PIPE_OPEN_INVALID;
    unsigned char rnd[16];
    if (BCryptGenRandom(NULL, rnd, (ULONG)sizeof(rnd), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        return KL_PIPE_OPEN_ERROR;
    static const char hexd[] = "0123456789abcdef";
    char hex[2 * sizeof(rnd) + 1];
    for (size_t i = 0; i < sizeof(rnd); i++) {
        hex[2 * i]     = hexd[rnd[i] >> 4];
        hex[2 * i + 1] = hexd[rnd[i] & 0xF];
    }
    hex[2 * sizeof(rnd)] = '\0';
    char name[128];
    snprintf(name, sizeof(name), "\\\\.\\pipe\\keel-anon-%lu-%ld-%s",
             (unsigned long)GetCurrentProcessId(), (long)InterlockedIncrement(&g_anon_seq), hex);
    wchar_t wpath[KL_PIPE_WPATH_MAX];
    if (pipe_wpath(name, wpath) != 0) return KL_PIPE_OPEN_ERROR;

    PSECURITY_DESCRIPTOR sd = NULL;
    if (pipe_server_sd(&sd) != 0) return KL_PIPE_OPEN_ERROR;
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = sd;
    sa.bInheritHandle = FALSE;
    HANDLE ph = CreateNamedPipeW(wpath,
                                 (parent_reads ? PIPE_ACCESS_INBOUND : PIPE_ACCESS_OUTBOUND) |
                                     FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                 PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                                     PIPE_REJECT_REMOTE_CLIENTS,
                                 1, 64 * 1024, 64 * 1024, 0, &sa);
    LocalFree(sd);
    if (ph == INVALID_HANDLE_VALUE) return KL_PIPE_OPEN_ERROR;

    /* From here the parent end exists, and every failure leaves through `fail`, the one place that
     * closes whatever was created. */
    ULONG pid = 0;
    /* NULL security attributes: not inheritable. No FILE_FLAG_OVERLAPPED: the child does plain I/O. */
    HANDLE ch = CreateFileW(wpath,
                            parent_reads ? (GENERIC_WRITE | FILE_READ_ATTRIBUTES)
                                         : (GENERIC_READ | FILE_WRITE_ATTRIBUTES),
                            0, NULL, OPEN_EXISTING, SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                            NULL);
    if (ch == INVALID_HANDLE_VALUE) goto fail;
    if (!GetNamedPipeClientProcessId(ph, &pid) || pid != GetCurrentProcessId()) goto fail;
    *parent = (KlPipeHandle *)ph;
    *child  = (KlPipeHandle *)ch;
    return KL_PIPE_OPEN_OK;

fail:
    if (ch != INVALID_HANDLE_VALUE) CloseHandle(ch);
    CloseHandle(ph);
    return KL_PIPE_OPEN_ERROR;
}

void kl_plat_pipe_close(KlPipeHandle *h) {
    if (h) CloseHandle((HANDLE)h);
}

/* ── Readiness I/O: not on Windows (pipes ride the completion engine) ─────────────────────── */

int kl_plat_pipe_readiness(void) { return 0; }

KlSocketHandle kl_plat_pipe_pollable(const KlPipeHandle *h) {
    (void)h;
    return KL_INVALID_SOCKET;
}

kl_ssize_t kl_plat_pipe_read(KlPipeHandle *h, char *buf, size_t len, int *would_block) {
    (void)h; (void)buf; (void)len;
    *would_block = 0;
    return -1;
}

kl_ssize_t kl_plat_pipe_write(KlPipeHandle *h, const char *data, size_t len, int *would_block) {
    (void)h; (void)data; (void)len;
    *would_block = 0;
    return -1;
}

/* ── The child's end ───────────────────────────────────────────────────────────────────────── */

void kl_plat_pipe_end_adopt(KlAnonPipeEnd *e, KlPipeHandle *child) {
    e->_handle = child;
    e->_fd1    = 0;
}

void kl_plat_pipe_end_release(KlAnonPipeEnd *e) {
    if (e->_handle) CloseHandle((HANDLE)e->_handle);
    e->_handle = NULL;
    e->_fd1    = 0;
}

void *kl_anon_pipe_end_handle(const KlAnonPipeEnd *peer) {
    return peer ? peer->_handle : NULL;
}
