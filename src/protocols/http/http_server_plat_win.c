/*
 * http_server_plat_win.c: Windows implementation of the server platform services
 * (http_server_plat.h). The Windows sibling of http_server_plat_posix.c; compiled only on
 * Windows (Makefile SERVER_PLAT_SRC), so it uses Win32 directly with no
 * internal #ifdef.
 *
 * AF_UNIX exists on Windows 10+ (via <afunix.h>, pulled by sockcompat.h) with
 * working bind/connect, but has no filesystem-permission or ownership model and
 * no peer-credential mechanism, so the node perms (chmod/chown/getpwnam) and
 * peer creds are simply absent here. SIGPIPE does not exist; graceful stop uses
 * a console control handler instead of SIGTERM/SIGINT.
 */

#include "http_server_plat.h"
#include "http_internal.h"   /* kl_http_server_log_errno + KlHttpServer; pulls socket.h -> winsock2/afunix */
#include "socket.h"
#include "sockaddr_native.h" /* KlSockAddr <-> sockaddr (bind currency) */
#include "unix_socket_node.h" /* substrate AF_UNIX filesystem-node lifecycle (transport axis) */

#include <windows.h>
#include <string.h>
#include <stddef.h>

_Static_assert(sizeof(((KlHttpServer *)0)->unix_node) >= KL_UNIX_NODE_STORAGE,
               "KlHttpServer.unix_node is too small for the AF_UNIX node-cleanup state");

/* ── Signal handling (console control handler) ───────────────────────── */

/* The handler runs on a thread the system creates for each console event, not on the loop thread.
 * Windows does not document that unregistering it waits for a run already in progress (on the
 * builds tested, SetConsoleCtrlHandler(FALSE) did wait for a running handler), so the server's
 * lifetime does not rest on it. Each run holds the lock (shared) across its load of the server and its
 * kl_http_server_stop; the restore clears the pointer under the lock (exclusive), so it returns only
 * after any run that saw the server has finished with it, and the caller may then free the server.
 * kl_http_server_stop only publishes a request and wakes the loop, so holding the lock across it
 * costs the restore no more than that.
 *
 * For a close, logoff or shutdown event Windows ends the process once the handler returns, so only
 * Ctrl-C and Ctrl-Break leave the server time to drain (see install_signal_handlers). */
static SRWLOCK       kl_signal_lock   = SRWLOCK_INIT;
static KlHttpServer *kl_signal_server = NULL;

#ifdef KEEL_HTTP_SERVER_PLAT_TEST_HOOKS
/* Test-only: called by the handler after it has loaded the server, while it holds the lock. */
void (*kl_http_server_test_ctrl_loaded)(void) = NULL;
#endif

static BOOL WINAPI kl_ctrl_handler(DWORD type) {
    switch (type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT: {
            AcquireSRWLockShared(&kl_signal_lock);
            KlHttpServer *s = kl_signal_server;
#ifdef KEEL_HTTP_SERVER_PLAT_TEST_HOOKS
            if (s && kl_http_server_test_ctrl_loaded) kl_http_server_test_ctrl_loaded();
#endif
            if (s) kl_http_server_stop(s);
            ReleaseSRWLockShared(&kl_signal_lock);
            return s ? TRUE : FALSE;
        }
        default:
            return FALSE;
    }
}

#ifdef KEEL_HTTP_SERVER_PLAT_TEST_HOOKS
/* Test-only: run the handler as the console's event thread would. */
BOOL kl_http_server_test_ctrl_handler(DWORD type) { return kl_ctrl_handler(type); }
#endif

void kl_http_server_plat_signals_install(KlHttpServer *s) {
    /* No SIGPIPE on Windows; send() never raises it. */
    if (s->config.install_signal_handlers) {
        AcquireSRWLockExclusive(&kl_signal_lock);
        kl_signal_server = s;
        ReleaseSRWLockExclusive(&kl_signal_lock);
        SetConsoleCtrlHandler(kl_ctrl_handler, TRUE);
    }
}

/* cppcheck-suppress constParameterPointer ; symmetric with kl_http_server_plat_signals_install */
void kl_http_server_plat_signals_restore(KlHttpServer *s) {
    if (s->config.install_signal_handlers) {
        /* Unregister first so no new event is routed here, then wait out any run already holding
         * the server (see kl_signal_lock). Never call SetConsoleCtrlHandler with the lock held: the
         * console may hold its own lock while it runs a handler, which then takes this one. */
        SetConsoleCtrlHandler(kl_ctrl_handler, FALSE);
        AcquireSRWLockExclusive(&kl_signal_lock);
        /* Only its own: a server that installed later keeps receiving events, through the
         * registration it added (registrations are counted, one per install). */
        if (kl_signal_server == s) kl_signal_server = NULL;
        ReleaseSRWLockExclusive(&kl_signal_lock);
    }
}

/* ── AF_UNIX node lifecycle: thin adapter over the substrate node module ─────────────────────
 *
 * Identical shape to the POSIX adapter: build a neutral policy, delegate the identity-anchored
 * lifecycle (unix_socket_node_win.c: no-follow open + FILE_ID_INFO verify + delete-by-handle on
 * local NTFS, fail closed elsewhere), and map the module status into KlError + server logging. This
 * TU performs no filesystem or socket-path syscalls itself. Windows AF_UNIX has no owner/group/mode
 * model, so those policy fields are ignored by the module. */

static KlError unix_node_status_to_error(KlUnixNodeStatus st) {
    switch (st) {
        case KL_UNIX_NODE_ERR_INVALID_PATH:
        case KL_UNIX_NODE_ERR_UNKNOWN_OWNER:    return KL_ERR_INVALID_ARG;
        case KL_UNIX_NODE_ERR_SOCKET:           return KL_ERR_SOCKET;
        case KL_UNIX_NODE_ERR_NOMEM:            return KL_ERR_ALLOC;
        case KL_UNIX_NODE_ERR_UNTRUSTED_PARENT:
        case KL_UNIX_NODE_ERR_FOREIGN_NODE:
        case KL_UNIX_NODE_ERR_UNSUPPORTED:
        case KL_UNIX_NODE_ERR_BIND:
        default:                                return KL_ERR_BIND;
    }
}

static void log_unix_node_failure(KlHttpServer *s, KlUnixNodeStatus st, int err) {
    switch (st) {
        case KL_UNIX_NODE_ERR_INVALID_PATH:
            kl_http_server_log(s, KL_HTTP_SERVER_LOG_ERROR, "invalid unix socket path");
            break;
        case KL_UNIX_NODE_ERR_UNTRUSTED_PARENT:
            kl_http_server_log(s, KL_HTTP_SERVER_LOG_ERROR,
                   "refusing unix socket cleanup: a path component is untrusted or a reparse point");
            break;
        case KL_UNIX_NODE_ERR_FOREIGN_NODE:
            kl_http_server_log(s, KL_HTTP_SERVER_LOG_ERROR,
                   "refusing unix socket cleanup: node is not the AF_UNIX node the server bound");
            break;
        case KL_UNIX_NODE_ERR_UNSUPPORTED:
            kl_http_server_log(s, KL_HTTP_SERVER_LOG_ERROR,
                   "unix socket cleanup unsupported here: identity-anchored delete requires local NTFS "
                   "with FileIdInfo + reparse + POSIX handle disposition");
            break;
        case KL_UNIX_NODE_ERR_SOCKET:
        case KL_UNIX_NODE_ERR_BIND:
        default:
            kl_http_server_log(s, KL_HTTP_SERVER_LOG_ERROR, "unix socket bind failed (win32 error %d)", err);
            break;
    }
}

int kl_http_server_plat_bind_unix(KlHttpServer *s) {
    KlUnixNodePolicy policy = {
        .path         = s->config.unix_socket_path,
        .unlink_stale = s->config.unix_socket_unlink,
        .owner        = s->config.unix_socket_owner,   /* ignored on Windows */
        .group        = s->config.unix_socket_group,   /* ignored on Windows */
        .mode         = s->config.unix_socket_mode,    /* ignored on Windows */
        .set_mode     = s->config.unix_socket_mode != 0,
    };
    int err = 0;
    KlUnixNodeStatus st = kl_unix_socket_node_bind(&policy, s->ev.sockets, &s->alloc_storage,
                                                   &s->unix_node, &s->listen_fd, &err);
    if (st != KL_UNIX_NODE_OK) {
        log_unix_node_failure(s, st, err);
        s->last_error = unix_node_status_to_error(st);
        return -1;
    }
    s->unix_socket_owned = 1;
    s->bound_port = 0;
    return 0;
}

void kl_http_server_plat_unlink_owned_unix(KlHttpServer *s) {
    if (s->unix_socket_owned)
        (void)kl_unix_socket_node_teardown(&s->unix_node, s->config.unix_socket_unlink);
    s->unix_socket_owned = 0;
}

/* ── Peer credentials (unsupported on Windows AF_UNIX) ───────────────── */

void kl_http_server_plat_unsetenv(const char *name) {
    (void)SetEnvironmentVariableA(name, NULL);   /* NULL value removes it */
}

int kl_peer_cred_fd(KlSocketHandle fd, KlPeerCred *out) {
    (void)fd; (void)out;
    return -1;
}

int kl_http_server_plat_peer_cred_readable(const KlSocketProvider *p) {
    (void)p;
    return 0;   /* no peer-credential or peer-label query on Windows */
}

unsigned kl_platform_caps(void) {
    return 0;   /* no SO_PEERCRED, no systemd socket activation on Windows */
}

int kl_http_server_plat_peer_label_fd(KlSocketHandle fd, char *buf, size_t buflen) {
    (void)fd; (void)buf; (void)buflen;
    return -1;
}
