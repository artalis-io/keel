/*
 * server_plat_win.c — Windows implementation of the server platform services
 * (server_plat.h). The Windows sibling of server_plat_posix.c; compiled only on
 * Windows (Makefile SERVER_PLAT_SRC), so it uses Win32 directly with no
 * internal #ifdef.
 *
 * AF_UNIX exists on Windows 10+ (via <afunix.h>, pulled by sockcompat.h) with
 * working bind/connect, but has no filesystem-permission or ownership model and
 * no peer-credential mechanism — so the node perms (chmod/chown/getpwnam) and
 * peer creds are simply absent here. SIGPIPE does not exist; graceful stop uses
 * a console control handler instead of SIGTERM/SIGINT.
 */

#include "server_plat.h"
#include "internal.h"   /* kl_log_errno + KlServer; pulls socket.h -> winsock2/afunix */
#include "socket.h"
#include "sockaddr_native.h" /* KlSockAddr <-> sockaddr (bind currency) */

#include <windows.h>
#include <string.h>
#include <stddef.h>
#include <stdatomic.h>

/* ── Signal handling (console control handler) ───────────────────────── */

static _Atomic(KlServer *) kl_signal_server = NULL;

static BOOL WINAPI kl_ctrl_handler(DWORD type) {
    switch (type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT: {
            KlServer *s = atomic_load(&kl_signal_server);
            if (s) { kl_server_stop(s); return TRUE; }
            return FALSE;
        }
        default:
            return FALSE;
    }
}

void kl_server_plat_signals_install(KlServer *s) {
    /* No SIGPIPE on Windows — send() never raises it. */
    if (s->config.install_signal_handlers) {
        atomic_store(&kl_signal_server, s);
        SetConsoleCtrlHandler(kl_ctrl_handler, TRUE);
    }
}

/* cppcheck-suppress constParameterPointer ; symmetric with kl_server_plat_signals_install */
void kl_server_plat_signals_restore(KlServer *s) {
    if (s->config.install_signal_handlers) {
        SetConsoleCtrlHandler(kl_ctrl_handler, FALSE);
        atomic_store(&kl_signal_server, (KlServer *)NULL);
    }
}

/* ── AF_UNIX node lifecycle ──────────────────────────────────────────── */

int kl_server_plat_bind_unix(KlServer *s) {
    const char *path = s->config.unix_socket_path;
    if (!path || path[0] == '\0') {
        s->last_error = KL_ERR_INVALID_ARG;
        return -1;
    }

    size_t path_len = strlen(path);
    if (path_len >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        s->last_error = KL_ERR_INVALID_ARG;
        return -1;
    }

    s->listen_fd = kl_sock_socket(s->ev.sockets, AF_UNIX, SOCK_STREAM, 0);
    if (!kl_handle_valid(s->listen_fd)) {
        kl_log_errno(s, KL_LOG_ERROR, "socket");
        s->last_error = KL_ERR_SOCKET;
        return -1;
    }
    kl_sock_set_cloexec(s->ev.sockets, s->listen_fd);

    /* AF_UNIX on Windows creates a file at the path; bind fails if it exists.
     * Best-effort removal (no S_ISSOCK guard — Windows has no such test). */
    if (s->config.unix_socket_unlink)
        (void)DeleteFileA(path);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, path_len + 1);

    socklen_t addr_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
                                     path_len + 1);

    KlSockAddr bind_sa;
    kl_sockaddr_from_native(&bind_sa, (struct sockaddr *)&addr, addr_len);
    if (kl_sock_bind(s->ev.sockets, s->listen_fd, &bind_sa) < 0) {
        kl_log_errno(s, KL_LOG_ERROR, "bind");
        s->last_error = KL_ERR_BIND;
        kl_sock_close(s->ev.sockets, s->listen_fd);
        s->listen_fd = KL_INVALID_SOCKET;
        return -1;
    }

    /* No node perms/ownership on Windows AF_UNIX (chmod/chown/getpwnam N/A). */
    s->unix_socket_owned = 1;
    s->bound_port = 0;
    return 0;
}

void kl_server_plat_unlink_owned_unix(KlServer *s) {
    if (s->unix_socket_owned && s->config.unix_socket_unlink &&
        s->config.unix_socket_path) {
        (void)DeleteFileA(s->config.unix_socket_path);
    }
    s->unix_socket_owned = 0;
}

/* ── Peer credentials (unsupported on Windows AF_UNIX) ───────────────── */

void kl_server_plat_unsetenv(const char *name) {
    (void)SetEnvironmentVariableA(name, NULL);   /* NULL value removes it */
}

int kl_peer_cred_fd(KlSocketHandle fd, KlPeerCred *out) {
    (void)fd; (void)out;
    return -1;
}

unsigned kl_platform_caps(void) {
    return 0;   /* no SO_PEERCRED, no systemd socket activation on Windows */
}

int kl_server_plat_peer_label_fd(KlSocketHandle fd, char *buf, size_t buflen) {
    (void)fd; (void)buf; (void)buflen;
    return -1;
}
