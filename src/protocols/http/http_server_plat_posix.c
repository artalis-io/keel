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
#include <fcntl.h>        /* openat, O_DIRECTORY / O_NOFOLLOW / O_CLOEXEC, AT_SYMLINK_NOFOLLOW */

/* Compile coverage for the hardened AF_UNIX lifecycle (see
 * docs/unix_socket_cleanup_security_design.md): these POSIX.1-2008 flags must exist on every
 * hosted POSIX target, and the leaf-copy buffer must bound sun_path. The openat / fstatat /
 * unlinkat / fchownat / fchmodat CALLS are likewise -Werror-guarded against an implicit
 * declaration if a target lacks them. */
#if !defined(O_DIRECTORY) || !defined(O_NOFOLLOW) || !defined(O_CLOEXEC) || !defined(AT_SYMLINK_NOFOLLOW)
#error "AF_UNIX node-cleanup hardening requires O_DIRECTORY, O_NOFOLLOW, O_CLOEXEC, AT_SYMLINK_NOFOLLOW"
#endif
_Static_assert(sizeof(((struct sockaddr_un *)0)->sun_path) <= KL_HTTP_UNIX_PATH_MAX,
               "KL_HTTP_UNIX_PATH_MAX must bound sockaddr_un.sun_path");

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

/* ── AF_UNIX node lifecycle (hardened; docs/unix_socket_cleanup_security_design.md) ──────
 *
 * The security boundary is the socket's PARENT DIRECTORY. A component-walked,
 * O_NOFOLLOW-opened, trust-validated parent dirfd is held for the socket's lifetime and every
 * reclaim / owner / mode / teardown mutation is directory-relative (fstatat / unlinkat /
 * fchownat / fchmodat) with identity revalidation. There is no atomic "act iff inode X" on a
 * name, so the trusted-directory guarantee (not a per-call trick) is what actually protects the
 * lifecycle; the helpers below minimize the residual final-component window and fail closed. */

/* Resolve a username to a uid via getpwnam_r. Returns 0 on success, -1 if the
 * user is unknown or on allocation failure (sets last_error accordingly). */
static int resolve_uid(KlHttpServer *s, const char *name, uid_t *out) {
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
static int resolve_gid(KlHttpServer *s, const char *name, gid_t *out) {
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

/* Reject unusable socket paths and split off the leaf. dirbuf receives a mutable copy of the
 * path; *base_out points at the final component within it. Returns the index of the last '/'
 * (>=0), or -1 when there is no '/'. On rejection returns -1 AND leaves *base_out NULL:
 * empty path, overlong path, trailing slash (no leaf), or a "."/".." leaf. */
static int split_and_check_path(KlHttpServer *s, const char *path, char *dirbuf, size_t dircap,
                                const char **base_out) {
    *base_out = NULL;
    size_t len = strlen(path);
    if (len == 0 || len >= dircap) { s->last_error = KL_ERR_INVALID_ARG; return -1; }
    if (path[len - 1] == '/') { s->last_error = KL_ERR_INVALID_ARG; return -1; }  /* no final component */
    memcpy(dirbuf, path, len + 1);
    char *slash = strrchr(dirbuf, '/');
    const char *base = slash ? slash + 1 : dirbuf;
    if (base[0] == '\0' || strcmp(base, ".") == 0 || strcmp(base, "..") == 0) {
        s->last_error = KL_ERR_INVALID_ARG; return -1;
    }
    *base_out = base;
    return slash ? (int)(slash - dirbuf) : -1;
}

/* Open the parent directory, walking EVERY component with O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC from a
 * trusted root, so no intermediate component may be a symlink (a single O_NOFOLLOW open guards only
 * the final component). @dir is the NUL-terminated, mutable parent portion ("" means the start dir
 * itself); it is destructively tokenized. Returns a dirfd (>=0) or -1 (sets last_error). */
static int open_parent_walk(KlHttpServer *s, char *dir, int start_absolute) {
    int dirfd = open(start_absolute ? "/" : ".", O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dirfd < 0) {
        kl_http_server_log_errno(s, KL_HTTP_SERVER_LOG_ERROR, "open unix socket dir");
        s->last_error = KL_ERR_BIND; return -1;
    }
    char *p = dir;
    while (*p == '/') p++;                       /* leading slash(es) already handled by the root open */
    while (*p) {
        char *seg = p;
        char *next = strchr(p, '/');
        if (next) { *next = '\0'; p = next + 1; while (*p == '/') p++; }
        else p += strlen(p);
        if (seg[0] == '\0' || strcmp(seg, ".") == 0) continue;   /* collapse // and skip . */
        if (strcmp(seg, "..") == 0) {            /* refuse .. so the trusted walk cannot escape upward */
            close(dirfd); s->last_error = KL_ERR_INVALID_ARG; return -1;
        }
        int nfd = openat(dirfd, seg, O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(dirfd);
        if (nfd < 0) {
            kl_http_server_log_errno(s, KL_HTTP_SERVER_LOG_ERROR, "walk unix socket dir");
            s->last_error = KL_ERR_BIND; return -1;
        }
        dirfd = nfd;
    }
    return dirfd;
}

/* Enforce the trust tier (design section 4.1) on an opened parent dir. @tier_b = ownership transfers
 * to a uid != euid, which forbids ANY group/other write (a sticky shared dir is not sufficient once
 * the node is owned by the foreign uid, per fact 4). Returns 0 (trusted) or -1 (fail closed). */
static int check_parent_trust(KlHttpServer *s, int dirfd, int tier_b) {
    struct stat ds;
    if (fstat(dirfd, &ds) < 0) { s->last_error = KL_ERR_BIND; return -1; }
    if (ds.st_uid != geteuid() && ds.st_uid != 0) {
        kl_http_server_log(s, KL_HTTP_SERVER_LOG_ERROR,
               "refusing unix socket cleanup: parent dir not owned by the server uid or root");
        s->last_error = KL_ERR_BIND; return -1;
    }
    if (ds.st_mode & (S_IWGRP | S_IWOTH)) {
        if (tier_b) {
            kl_http_server_log(s, KL_HTTP_SERVER_LOG_ERROR,
                   "refusing unix socket cleanup: parent dir is group/other-writable and ownership is transferred");
            s->last_error = KL_ERR_BIND; return -1;
        }
        if (!(ds.st_mode & S_ISVTX)) {
            kl_http_server_log(s, KL_HTTP_SERVER_LOG_ERROR,
                   "refusing unix socket cleanup: parent dir is group/other-writable without the sticky bit");
            s->last_error = KL_ERR_BIND; return -1;
        }
    }
    return 0;
}

/* Directory-relative identity check for a leaf we may remove/mutate. Returns 0 (present: a socket
 * owned by an accepted uid, *st filled), 1 (absent: ENOENT), or -1 (unsafe/foreign/non-socket/error;
 * fail closed). Accepted owners: euid, plus owner_uid when owner_set. */
static int stat_accepted_socket(KlHttpServer *s, int dirfd, const char *base,
                                uid_t owner_uid, int owner_set, struct stat *st) {
    if (fstatat(dirfd, base, st, AT_SYMLINK_NOFOLLOW) < 0) {
        if (errno == ENOENT) return 1;
        s->last_error = KL_ERR_BIND; return -1;
    }
    if (!S_ISSOCK(st->st_mode)) {
        kl_http_server_log(s, KL_HTTP_SERVER_LOG_ERROR, "refusing to act on a non-socket unix path");
        s->last_error = KL_ERR_BIND; return -1;
    }
    if (st->st_uid != geteuid() && !(owner_set && st->st_uid == owner_uid)) {
        kl_http_server_log(s, KL_HTTP_SERVER_LOG_ERROR,
               "refusing to act on a unix socket owned by an unexpected uid");
        s->last_error = KL_ERR_BIND; return -1;
    }
    return 0;
}

/* True iff the leaf is still exactly the node we bound: a socket whose dev+ino match the captured
 * identity. Used to revalidate before each owner/mode mutation and before any failure/teardown
 * unlink, so a substituted entry is never chowned/chmod-ed/removed. */
static int leaf_is_bound_node(KlHttpServer *s) {
    struct stat st;
    if (!s->unix_node.node_captured) return 0;
    if (fstatat(s->unix_node.dirfd, s->unix_node.base, &st, AT_SYMLINK_NOFOLLOW) < 0) return 0;
    return S_ISSOCK(st.st_mode) &&
           (unsigned long long)st.st_dev == s->unix_node.node_dev &&
           (unsigned long long)st.st_ino == s->unix_node.node_ino;
}

/* Create the AF_UNIX socket + bind it at @path (umask-guarded when a mode is configured). Sets
 * s->listen_fd on success; closes it and returns -1 on failure. bind() fails if the name already
 * exists, so it cannot clobber an entry an attacker re-created after a reclaim. */
static int create_and_bind_unix(KlHttpServer *s, const char *path, size_t path_len) {
    s->listen_fd = kl_sock_socket(s->ev.sockets, AF_UNIX, SOCK_STREAM, 0);
    if (!kl_handle_valid(s->listen_fd)) {
        kl_http_server_log_errno(s, KL_HTTP_SERVER_LOG_ERROR, "socket");
        s->last_error = KL_ERR_SOCKET; return -1;
    }
    kl_sock_set_cloexec(s->ev.sockets, s->listen_fd);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, path_len + 1);
    socklen_t addr_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + path_len + 1);

    mode_t old_umask = 0; int umask_set = 0;
    if (s->config.unix_socket_mode != 0) {
        old_umask = umask(0777 & ~(mode_t)s->config.unix_socket_mode); umask_set = 1;
    }
    KlSockAddr bind_sa;
    kl_sockaddr_from_native(&bind_sa, (struct sockaddr *)&addr, addr_len);
    int rc = kl_sock_bind(s->ev.sockets, s->listen_fd, &bind_sa);
    if (umask_set) umask(old_umask);
    if (rc < 0) {
        kl_http_server_log_errno(s, KL_HTTP_SERVER_LOG_ERROR, "bind");
        s->last_error = KL_ERR_BIND;
        kl_sock_close(s->ev.sockets, s->listen_fd);
        s->listen_fd = KL_INVALID_SOCKET;
        return -1;
    }
    return 0;
}

int kl_http_server_plat_bind_unix(KlHttpServer *s) {
    const char *path = s->config.unix_socket_path;
    if (!path || path[0] == '\0') { s->last_error = KL_ERR_INVALID_ARG; return -1; }
    size_t path_len = strlen(path);
    if (path_len >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        s->last_error = KL_ERR_INVALID_ARG; return -1;
    }

    /* Resolve owner/group ONCE, up front, and reuse the result for the trust-tier decision, the
     * reclaim accepted-owner set, and the post-bind chown (no duplicate/inconsistent lookups). */
    uid_t owner_uid = (uid_t)-1; gid_t owner_gid = (gid_t)-1;
    int owner_set = 0, group_set = 0;
    if (s->config.unix_socket_owner) {
        if (resolve_uid(s, s->config.unix_socket_owner, &owner_uid) < 0) {
            if (s->last_error != KL_ERR_ALLOC) {
                kl_http_server_log(s, KL_HTTP_SERVER_LOG_ERROR, "unknown unix socket owner '%s'",
                       s->config.unix_socket_owner);
                s->last_error = KL_ERR_INVALID_ARG;
            }
            return -1;
        }
        owner_set = 1;
    }
    if (s->config.unix_socket_group) {
        if (resolve_gid(s, s->config.unix_socket_group, &owner_gid) < 0) {
            if (s->last_error != KL_ERR_ALLOC) {
                kl_http_server_log(s, KL_HTTP_SERVER_LOG_ERROR, "unknown unix socket group '%s'",
                       s->config.unix_socket_group);
                s->last_error = KL_ERR_INVALID_ARG;
            }
            return -1;
        }
        group_set = 1;
    }
    /* Tier B whenever ownership transfers to a foreign uid, including startup reclaim of a socket
     * left by a prior chowned run. */
    int tier_b = owner_set && owner_uid != geteuid();

    /* The hardened directory machinery (component walk, trust tier, held dirfd, dev/ino) is only
     * needed when this server MUTATES the pathname: stale-unlink, chown, or chmod. A plain bind
     * performs no pathname mutation (bind fails safely if the name exists), so it takes the simple
     * path and does not impose a trusted-directory requirement on unrelated callers. */
    int will_mutate = s->config.unix_socket_unlink || owner_set || group_set ||
                      s->config.unix_socket_mode != 0;
    if (!will_mutate) {
        if (create_and_bind_unix(s, path, path_len) < 0) return -1;
        s->unix_socket_owned = 1;
        s->bound_port = 0;
        return 0;
    }

    /* Component-walk + hold the trusted parent dir; copy the leaf into lifecycle-owned storage. */
    char dirbuf[KL_HTTP_UNIX_PATH_MAX];
    const char *base = NULL;
    int slash_at = split_and_check_path(s, path, dirbuf, sizeof(dirbuf), &base);
    if (!base) return -1;
    if (strlen(base) >= sizeof(s->unix_node.base)) { s->last_error = KL_ERR_INVALID_ARG; return -1; }
    char base_copy[KL_HTTP_UNIX_PATH_MAX];
    memcpy(base_copy, base, strlen(base) + 1);      /* copy BEFORE the walk NUL-splits dirbuf */
    if (slash_at < 0) dirbuf[0] = '\0';             /* no '/': parent is the start dir */
    else dirbuf[slash_at] = '\0';                   /* terminate the parent portion */
    int dirfd = open_parent_walk(s, dirbuf, path[0] == '/');
    if (dirfd < 0) return -1;
    if (check_parent_trust(s, dirfd, tier_b) < 0) { close(dirfd); return -1; }
    s->unix_node.dirfd = dirfd;
    memcpy(s->unix_node.base, base_copy, strlen(base_copy) + 1);
    s->unix_node.owner_uid = (unsigned int)owner_uid;
    s->unix_node.owner_set = owner_set;
    s->unix_node.node_captured = 0;

    /* Reclaim a stale, validated, accepted-owner socket node (opt-in); fail closed otherwise. */
    if (s->config.unix_socket_unlink) {
        struct stat st;
        int r = stat_accepted_socket(s, dirfd, s->unix_node.base, owner_uid, owner_set, &st);
        if (r < 0) goto fail_close_dir;
        if (r == 0 && unlinkat(dirfd, s->unix_node.base, 0) < 0) {
            kl_http_server_log_errno(s, KL_HTTP_SERVER_LOG_ERROR, "unlink stale unix socket");
            s->last_error = KL_ERR_BIND; goto fail_close_dir;
        }
    }

    /* Create + bind (on its own failure it closes listen_fd, so we only unwind the dirfd). */
    if (create_and_bind_unix(s, path, path_len) < 0)
        goto fail_close_dir;
    s->unix_socket_owned = 1;

    /* Capture the bound FILESYSTEM node identity via fstatat (NOT fstat(listen_fd): the socket fd
     * names the kernel object, not the pathname inode). This is the chown/chmod + teardown identity. */
    {
        struct stat bnode;
        if (fstatat(dirfd, s->unix_node.base, &bnode, AT_SYMLINK_NOFOLLOW) < 0 ||
            !S_ISSOCK(bnode.st_mode)) {
            kl_http_server_log(s, KL_HTTP_SERVER_LOG_ERROR, "unix socket node not found after bind");
            s->last_error = KL_ERR_BIND; goto fail_unlink;
        }
        s->unix_node.node_dev = (unsigned long long)bnode.st_dev;
        s->unix_node.node_ino = (unsigned long long)bnode.st_ino;
        s->unix_node.node_captured = 1;
    }

    /* Owner/group via fchownat (identity revalidated), before listen() so the socket is never
     * reachable with the wrong owner. Directory-relative, not fchown(listen_fd) (fact 2). */
    if (owner_set || group_set) {
        if (!leaf_is_bound_node(s)) {
            kl_http_server_log(s, KL_HTTP_SERVER_LOG_ERROR, "unix socket identity changed before chown");
            s->last_error = KL_ERR_BIND; goto fail_unlink;
        }
        uid_t uid = owner_set ? owner_uid : (uid_t)-1;
        gid_t gid = group_set ? owner_gid : (gid_t)-1;
        if (fchownat(dirfd, s->unix_node.base, uid, gid, AT_SYMLINK_NOFOLLOW) < 0) {
            kl_http_server_log_errno(s, KL_HTTP_SERVER_LOG_ERROR, "chown unix socket");
            s->last_error = KL_ERR_BIND; goto fail_unlink;
        }
    }

    /* Exact mode via fchmodat, AFTER chown (chown can clear set-*id). Reasserting a privileged
     * name operation after ownership transfer is safe only because Tier B forbids the foreign owner
     * directory write, so it cannot substitute the entry between chown and chmod (fact 4). */
    if (s->config.unix_socket_mode != 0) {
        if (!leaf_is_bound_node(s)) {
            kl_http_server_log(s, KL_HTTP_SERVER_LOG_ERROR, "unix socket identity changed before chmod");
            s->last_error = KL_ERR_BIND; goto fail_unlink;
        }
        if (fchmodat(dirfd, s->unix_node.base, (mode_t)s->config.unix_socket_mode, 0) < 0) {
            kl_http_server_log_errno(s, KL_HTTP_SERVER_LOG_ERROR, "chmod unix socket");
            s->last_error = KL_ERR_BIND; goto fail_unlink;
        }
    }

    s->bound_port = 0;
    return 0;

fail_unlink:
    /* Failure-path cleanup: remove ONLY the node we bound (validated identity), never a replacement,
     * then fall through to close the listen fd (still open on a post-bind failure). */
    if (leaf_is_bound_node(s))
        (void)unlinkat(dirfd, s->unix_node.base, 0);
    s->unix_socket_owned = 0;
    if (kl_handle_valid(s->listen_fd)) {
        kl_sock_close(s->ev.sockets, s->listen_fd);
        s->listen_fd = KL_INVALID_SOCKET;
    }
fail_close_dir:
    close(s->unix_node.dirfd);
    s->unix_node.dirfd = -1;
    s->unix_node.base[0] = '\0';
    s->unix_node.node_captured = 0;
    return -1;
}

void kl_http_server_plat_unlink_owned_unix(KlHttpServer *s) {
    KlError saved = s->last_error;   /* best-effort teardown must not clobber the caller's error */
    if (s->unix_socket_owned && s->config.unix_socket_unlink &&
        s->unix_node.dirfd >= 0 && s->unix_node.base[0] != '\0' && s->unix_node.node_captured) {
        int tier_b = s->unix_node.owner_set && (uid_t)s->unix_node.owner_uid != geteuid();
        struct stat st;
        /* Fail closed unless the dir still passes its tier AND the leaf is still exactly our bound
         * node (socket, accepted owner, dev+ino match). Otherwise leave the ambiguous entry. */
        if (check_parent_trust(s, s->unix_node.dirfd, tier_b) == 0 &&
            stat_accepted_socket(s, s->unix_node.dirfd, s->unix_node.base,
                                 (uid_t)s->unix_node.owner_uid, s->unix_node.owner_set, &st) == 0 &&
            (unsigned long long)st.st_dev == s->unix_node.node_dev &&
            (unsigned long long)st.st_ino == s->unix_node.node_ino) {
            (void)unlinkat(s->unix_node.dirfd, s->unix_node.base, 0);
        }
    }
    s->unix_socket_owned = 0;
    if (s->unix_node.dirfd >= 0) { close(s->unix_node.dirfd); s->unix_node.dirfd = -1; }
    s->unix_node.base[0] = '\0';
    s->unix_node.node_captured = 0;
    s->last_error = saved;
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
