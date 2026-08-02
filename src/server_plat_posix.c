#if defined(__linux__)
/* Needed for struct ucred / SO_PEERCRED. Must precede any libc header. */
#define _GNU_SOURCE
#endif

/*
 * server_plat_posix.c — POSIX implementation of the server platform services
 * (server_plat.h). The AF_UNIX node lifecycle, peer credentials, and signal
 * handling extracted from server.c so that TU stays platform-#ifdef-free.
 *
 * This is a POSIX TU: it keeps intra-POSIX (__linux__ vs __APPLE__/BSD)
 * variance behind #ifdef, exactly like socket_posix.c does for sendfile.
 */

#include "server_plat.h"
#include "internal.h"        /* kl_log / kl_log_errno + KlServer */
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
#if !defined(KL_NO_SIGNAL)
#include <signal.h>
#include <stdatomic.h>
#endif

/* ── Signal handling ─────────────────────────────────────────────────── */

#if !defined(KL_NO_SIGNAL)
static _Atomic(KlServer *) kl_signal_server = NULL;
static struct sigaction    kl_old_term, kl_old_int;

static void kl_signal_handler(int sig) {
    (void)sig;
    KlServer *s = atomic_load(&kl_signal_server);
    if (s) kl_server_stop(s);
}
#endif

void kl_srv_signals_install(KlServer *s) {
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

/* cppcheck-suppress constParameterPointer ; symmetric with kl_srv_signals_install */
void kl_srv_signals_restore(KlServer *s) {
#if !defined(KL_NO_SIGNAL)
    if (s->config.install_signal_handlers) {
        sigaction(SIGTERM, &kl_old_term, NULL);
        sigaction(SIGINT, &kl_old_int, NULL);
        atomic_store(&kl_signal_server, (KlServer *)NULL);
    }
#else
    (void)s;
#endif
}

/* ── AF_UNIX node lifecycle ──────────────────────────────────────────── */

static int unlink_stale_unix_socket(KlServer *s, const char *path) {
    struct stat st;
    if (lstat(path, &st) < 0) {
        if (errno == ENOENT)
            return 0;
        kl_log_errno(s, KL_LOG_ERROR, "lstat unix socket");
        s->last_error = KL_ERR_BIND;
        return -1;
    }

    if (!S_ISSOCK(st.st_mode)) {
        kl_log(s, KL_LOG_ERROR,
               "refusing to unlink non-socket unix path '%s'", path);
        s->last_error = KL_ERR_BIND;
        return -1;
    }

    if (unlink(path) < 0) {
        kl_log_errno(s, KL_LOG_ERROR, "unlink unix socket");
        s->last_error = KL_ERR_BIND;
        return -1;
    }

    return 0;
}

/* Resolve a username to a uid via getpwnam_r. Returns 0 on success, -1 if the
 * user is unknown or on allocation failure (sets last_error accordingly). */
static int resolve_uid(KlServer *s, const char *name, uid_t *out) {
    long hint = sysconf(_SC_GETPW_R_SIZE_MAX);
    size_t bufsz = (hint > 0) ? (size_t)hint : 4096;
    for (;;) {
        char *buf = kl_malloc(&s->alloc_storage, bufsz);
        if (!buf) { s->last_error = KL_ERR_ALLOC; return -1; }
        struct passwd pw, *res = NULL;
        int rc = getpwnam_r(name, &pw, buf, bufsz, &res);
        if (rc == 0) {
            int ok = res ? 0 : -1;
            if (ok == 0) *out = res->pw_uid;
            kl_free(&s->alloc_storage, buf, bufsz);
            return ok;
        }
        kl_free(&s->alloc_storage, buf, bufsz);
        if (rc == ERANGE && bufsz < (1u << 20)) { bufsz *= 2; continue; }
        return -1;
    }
}

/* Resolve a group name to a gid via getgrnam_r. Same contract as above. */
static int resolve_gid(KlServer *s, const char *name, gid_t *out) {
    long hint = sysconf(_SC_GETGR_R_SIZE_MAX);
    size_t bufsz = (hint > 0) ? (size_t)hint : 4096;
    for (;;) {
        char *buf = kl_malloc(&s->alloc_storage, bufsz);
        if (!buf) { s->last_error = KL_ERR_ALLOC; return -1; }
        struct group gr, *res = NULL;
        int rc = getgrnam_r(name, &gr, buf, bufsz, &res);
        if (rc == 0) {
            int ok = res ? 0 : -1;
            if (ok == 0) *out = res->gr_gid;
            kl_free(&s->alloc_storage, buf, bufsz);
            return ok;
        }
        kl_free(&s->alloc_storage, buf, bufsz);
        if (rc == ERANGE && bufsz < (1u << 20)) { bufsz *= 2; continue; }
        return -1;
    }
}

int kl_srv_bind_unix(KlServer *s) {
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

    if (s->config.unix_socket_unlink &&
        unlink_stale_unix_socket(s, path) < 0) {
        kl_sock_close(s->ev.sockets, s->listen_fd);
        s->listen_fd = KL_INVALID_SOCKET;
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, path_len + 1);

    socklen_t addr_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
                                     path_len + 1);

    /* Create the socket node with the requested mode atomically: bind()
     * applies (0777 & ~umask), so a temporary umask closes the window in
     * which the socket would otherwise be reachable with default (looser)
     * permissions before the chmod below.  Restored immediately after bind. */
    mode_t old_umask = 0;
    int umask_set = 0;
    if (s->config.unix_socket_mode != 0) {
        old_umask = umask(0777 & ~(mode_t)s->config.unix_socket_mode);
        umask_set = 1;
    }
    KlSockAddr bind_sa;
    kl_sockaddr_from_native(&bind_sa, (struct sockaddr *)&addr, addr_len);
    int bind_rc = kl_sock_bind(s->ev.sockets, s->listen_fd, &bind_sa);
    if (umask_set)
        umask(old_umask);
    if (bind_rc < 0) {
        kl_log_errno(s, KL_LOG_ERROR, "bind");
        s->last_error = KL_ERR_BIND;
        kl_sock_close(s->ev.sockets, s->listen_fd);
        s->listen_fd = KL_INVALID_SOCKET;
        return -1;
    }

    s->unix_socket_owned = 1;

    /* Ownership: resolve owner/group names to ids and chown the socket node.
     * Done before listen() (where connections first become possible), so the
     * socket is never reachable with the wrong owner/group.  chown to a
     * different user needs privilege (CAP_CHOWN/root); setting group to one
     * the process belongs to is generally allowed for the file owner. */
    if (s->config.unix_socket_owner || s->config.unix_socket_group) {
        uid_t uid = (uid_t)-1;   /* -1 = leave unchanged (chown semantics) */
        gid_t gid = (gid_t)-1;
        if (s->config.unix_socket_owner &&
            resolve_uid(s, s->config.unix_socket_owner, &uid) < 0) {
            if (s->last_error != KL_ERR_ALLOC) {
                kl_log(s, KL_LOG_ERROR, "unknown unix socket owner '%s'",
                       s->config.unix_socket_owner);
                s->last_error = KL_ERR_INVALID_ARG;
            }
            goto fail;
        }
        if (s->config.unix_socket_group &&
            resolve_gid(s, s->config.unix_socket_group, &gid) < 0) {
            if (s->last_error != KL_ERR_ALLOC) {
                kl_log(s, KL_LOG_ERROR, "unknown unix socket group '%s'",
                       s->config.unix_socket_group);
                s->last_error = KL_ERR_INVALID_ARG;
            }
            goto fail;
        }
        if (chown(path, uid, gid) < 0) {
            kl_log_errno(s, KL_LOG_ERROR, "chown unix socket");
            s->last_error = KL_ERR_BIND;
            goto fail;
        }
    }

    /* Enforce the exact mode regardless of the platform's socket-creation
     * base (some create nodes 0666, not 0777).  The umask above already
     * closed the permissions window; this guarantees the precise bits, and
     * re-asserts them after any chown (which clears set-gid on some OSes). */
    if (s->config.unix_socket_mode != 0 &&
        chmod(path, (mode_t)s->config.unix_socket_mode) < 0) {
        kl_log_errno(s, KL_LOG_ERROR, "chmod unix socket");
        s->last_error = KL_ERR_BIND;
        goto fail;
    }

    s->bound_port = 0;
    return 0;

fail:
    kl_sock_close(s->ev.sockets, s->listen_fd);
    s->listen_fd = KL_INVALID_SOCKET;
    unlink(path);
    s->unix_socket_owned = 0;
    return -1;
}

void kl_srv_unlink_owned_unix(KlServer *s) {
    if (s->unix_socket_owned && s->config.unix_socket_unlink &&
        s->config.unix_socket_path) {
        /* Re-check that the path is still a socket before unlinking, so a
         * regular file (or a socket from a different process) that replaced
         * our path is not removed.  lstat (not stat) avoids following a
         * symlink.  Best-effort teardown — no error reporting. */
        struct stat st;
        if (lstat(s->config.unix_socket_path, &st) == 0 && S_ISSOCK(st.st_mode))
            unlink(s->config.unix_socket_path);
    }
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
    /* Mirrors kl_peer_cred_fd's per-dialect support (F5: POSIX dialects are the
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

void kl_srv_unsetenv(const char *name) {
    unsetenv(name);
}

int kl_srv_peer_label_fd(KlSocketHandle fd, char *buf, size_t buflen) {
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
