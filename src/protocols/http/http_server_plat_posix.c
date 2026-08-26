#if defined(__linux__)
/* Needed for struct ucred / SO_PEERCRED. Must precede any libc header. */
#define _GNU_SOURCE
#endif

/*
 * http_server_plat_posix.c: POSIX implementation of the server platform services
 * (http_server_plat.h). The AF_UNIX node lifecycle, peer credentials, and signal
 * handling live here so http_server.c stays platform-#ifdef-free.
 *
 * This is a POSIX TU: it keeps intra-POSIX (__linux__ vs __APPLE__/BSD)
 * variance behind #ifdef, exactly like socket_posix.c does for sendfile.
 */

#include "http_server_plat.h"
#include "http_internal.h"        /* kl_http_server_log / kl_http_server_log_errno + KlHttpServer */
#include "socket.h"
#include "sockaddr_native.h" /* KlSockAddr <-> sockaddr (bind currency) */
#include <keel/allocator.h>

#include <string.h>
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>   /* unsetenv */
#include <unistd.h>
#include <pwd.h>
#include <grp.h>
#include <sys/stat.h>
#include <sys/un.h>
#include "unix_socket_node.h"   /* substrate AF_UNIX filesystem-node lifecycle (transport axis) */

/* The opaque KlHttpServer.unix_node storage must be large enough for the substrate node state
 * (the concrete type is private to the module). Verified here, where both are in scope, so the
 * public header's byte count can never silently fall short of KL_UNIX_NODE_STORAGE. */
_Static_assert(sizeof(((KlHttpServer *)0)->unix_node) >= KL_UNIX_NODE_STORAGE,
               "KlHttpServer.unix_node is too small for the AF_UNIX node-cleanup state");

#if !defined(KL_NO_SIGNAL)
#include <signal.h>
#include <stdatomic.h>
#endif

/* ── Signal handling ─────────────────────────────────────────────────── */

#if !defined(KL_NO_SIGNAL)
static _Atomic(KlHttpServer *) kl_signal_server = NULL;
static struct sigaction    kl_old_term, kl_old_int;

static void kl_signal_handler(int sig) {
    (void)sig;
    KlHttpServer *s = atomic_load(&kl_signal_server);
    if (s) kl_http_server_stop(s);
}
#endif

void kl_http_server_plat_signals_install(KlHttpServer *s) {
#if !defined(KL_NO_SIGNAL)
    signal(SIGPIPE, SIG_IGN);
    if (s->config.install_signal_handlers) {
        atomic_store(&kl_signal_server, s);
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = kl_signal_handler;
        sa.sa_flags = 0;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGTERM, &sa, &kl_old_term);
        sigaction(SIGINT, &sa, &kl_old_int);
    }
#else
    (void)s;
#endif
}

/* cppcheck-suppress constParameterPointer ; symmetric with kl_http_server_plat_signals_install */
void kl_http_server_plat_signals_restore(KlHttpServer *s) {
#if !defined(KL_NO_SIGNAL)
    if (s->config.install_signal_handlers) {
        sigaction(SIGTERM, &kl_old_term, NULL);
        sigaction(SIGINT, &kl_old_int, NULL);
        atomic_store(&kl_signal_server, (KlHttpServer *)NULL);
    }
#else
    (void)s;
#endif
}

/* ── AF_UNIX node lifecycle: thin adapter over the substrate node module ─────────────────────
 *
 * The filesystem-node lifecycle (trusted-directory walk, stale reclamation, ownership/mode
 * mutation, teardown) is transport/socket-axis work and lives in the substrate module
 * (unix_socket_node.h). This adapter only translates KlHttpServerConfig into a neutral policy, maps
 * the module's status into KlError + server logging, and owns the KlHttpServer bookkeeping. It
 * performs no filesystem or socket-path syscalls itself. */

static KlError unix_node_status_to_error(KlUnixNodeStatus st) {
    switch (st) {
        case KL_UNIX_NODE_ERR_INVALID_PATH:
        case KL_UNIX_NODE_ERR_UNKNOWN_OWNER:    return KL_ERR_INVALID_ARG;
        case KL_UNIX_NODE_ERR_SOCKET:           return KL_ERR_SOCKET;
        case KL_UNIX_NODE_ERR_NOMEM:            return KL_ERR_ALLOC;
        case KL_UNIX_NODE_ERR_UNTRUSTED_PARENT:
        case KL_UNIX_NODE_ERR_FOREIGN_NODE:
        case KL_UNIX_NODE_ERR_BIND:
        default:                                return KL_ERR_BIND;
    }
}

/* Translate a fail-closed status into a useful server log line, without leaking unrelated
 * filesystem detail. Syscall failures carry the module-reported errno. */
static void log_unix_node_failure(KlHttpServer *s, KlUnixNodeStatus st, int err,
                                  const KlUnixNodePolicy *policy) {
    switch (st) {
        case KL_UNIX_NODE_ERR_INVALID_PATH:
            kl_http_server_log(s, KL_HTTP_SERVER_LOG_ERROR, "invalid unix socket path");
            break;
        case KL_UNIX_NODE_ERR_UNKNOWN_OWNER:
            kl_http_server_log(s, KL_HTTP_SERVER_LOG_ERROR,
                   "unknown unix socket owner '%s' or group '%s'",
                   policy->owner ? policy->owner : "", policy->group ? policy->group : "");
            break;
        case KL_UNIX_NODE_ERR_UNTRUSTED_PARENT:
            kl_http_server_log(s, KL_HTTP_SERVER_LOG_ERROR,
                   "refusing unix socket cleanup: parent directory outside the trust boundary");
            break;
        case KL_UNIX_NODE_ERR_FOREIGN_NODE:
            kl_http_server_log(s, KL_HTTP_SERVER_LOG_ERROR,
                   "refusing unix socket cleanup: existing node is not a socket the server owns");
            break;
        case KL_UNIX_NODE_ERR_NOMEM:
            kl_http_server_log(s, KL_HTTP_SERVER_LOG_ERROR,
                   "out of memory resolving unix socket owner/group");
            break;
        case KL_UNIX_NODE_ERR_SOCKET:
        case KL_UNIX_NODE_ERR_BIND:
        default:
            kl_http_server_log(s, KL_HTTP_SERVER_LOG_ERROR, "unix socket bind failed: %s", strerror(err));
            break;
    }
}

int kl_http_server_plat_bind_unix(KlHttpServer *s) {
    KlUnixNodePolicy policy = {
        .path         = s->config.unix_socket_path,
        .unlink_stale = s->config.unix_socket_unlink,
        .owner        = s->config.unix_socket_owner,
        .group        = s->config.unix_socket_group,
        .mode         = s->config.unix_socket_mode,
        .set_mode     = s->config.unix_socket_mode != 0,
    };
    int err = 0;
    KlUnixNodeStatus st = kl_unix_socket_node_bind(&policy, s->ev.sockets, &s->alloc_storage,
                                                   &s->unix_node, &s->listen_fd, &err);
    if (st != KL_UNIX_NODE_OK) {
        log_unix_node_failure(s, st, err, &policy);
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

/* ── Peer credentials ────────────────────────────────────────────────── */

/* cppcheck-suppress constParameterPointer  ; 'out' is written on the
   platform branches below (invisible to cppcheck's default config). */
int kl_peer_cred_fd(KlSocketHandle fd, KlPeerCred *out) {
    if (!kl_handle_valid(fd) || !out)
        return -1;

#if defined(__linux__) && defined(SO_PEERCRED)
    struct ucred cr;
    socklen_t len = sizeof(cr);
    if (getsockopt((int)fd, SOL_SOCKET, SO_PEERCRED, &cr, &len) != 0)
        return -1;
    out->uid = (long)cr.uid;
    out->gid = (long)cr.gid;
    out->pid = (long)cr.pid;
    out->has_pid = 1;
    return 0;
#elif defined(__APPLE__) || defined(__FreeBSD__) || \
      defined(__OpenBSD__) || defined(__NetBSD__)
    uid_t uid;
    gid_t gid;
    if (getpeereid((int)fd, &uid, &gid) != 0)
        return -1;
    out->uid = (long)uid;
    out->gid = (long)gid;
    out->pid = 0;
    out->has_pid = 0;
    /* macOS/BSD getpeereid gives no pid; LOCAL_PEERPID supplies it on macOS. */
#if defined(__APPLE__) && defined(LOCAL_PEERPID)
    {
        pid_t pid = 0;
        socklen_t pl = sizeof(pid);
        if (getsockopt((int)fd, SOL_LOCAL, LOCAL_PEERPID, &pid, &pl) == 0) {
            out->pid = (long)pid;
            out->has_pid = 1;
        }
    }
#endif
    return 0;
#else
    (void)fd;
    return -1;  /* peer credentials not supported on this platform */
#endif
}

unsigned kl_platform_caps(void) {
    /* Mirrors kl_peer_cred_fd's per-dialect support (POSIX dialects are the
     * PAL TU's own business). systemd socket activation is a Linux concept. */
    unsigned caps = 0;
#if defined(__linux__) && defined(SO_PEERCRED)
    caps |= KL_PLATCAP_PEER_CRED | KL_PLATCAP_PEER_CRED_PID | KL_PLATCAP_SYSTEMD_ACTIVATION;
#elif defined(__APPLE__)
    caps |= KL_PLATCAP_PEER_CRED | KL_PLATCAP_PEER_CRED_PID;   /* getpeereid + LOCAL_PEERPID */
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    caps |= KL_PLATCAP_PEER_CRED;                              /* getpeereid: uid/gid only */
#endif
    return caps;
}

void kl_http_server_plat_unsetenv(const char *name) {
    unsetenv(name);
}

int kl_http_server_plat_peer_label_fd(KlSocketHandle fd, char *buf, size_t buflen) {
#if defined(__linux__) && defined(SO_PEERSEC)
    socklen_t len = (socklen_t)buflen;
    if (getsockopt((int)fd, SOL_SOCKET, SO_PEERSEC, buf, &len) != 0)
        return -1;
    /* Ensure NUL-termination regardless of what the kernel returned. */
    if ((size_t)len >= buflen)
        len = (socklen_t)(buflen - 1);
    buf[len] = '\0';
    return 0;
#else
    (void)fd; (void)buf; (void)buflen;
    return -1;  /* SO_PEERSEC (SELinux/AppArmor label) is Linux-only */
#endif
}
