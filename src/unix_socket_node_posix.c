#if defined(__linux__)
/* getpwnam_r / getgrnam_r reentrant forms + AT_* on some libcs. Must precede any libc header. */
#define _GNU_SOURCE
#endif

/*
 * unix_socket_node_posix.c: POSIX implementation of the AF_UNIX filesystem-node lifecycle contract
 * (unix_socket_node.h). This is substrate transport code: it names no protocol type and includes no
 * protocol header. The security model is the trust boundary on EVERY directory component of the
 * socket path; see docs/unix_socket_cleanup_security_design.md.
 *
 * Path anchoring: POSIX has no bindat(), so bind() re-resolves the textual pathname. The walk below
 * therefore validates the trust policy on every component from a trusted root to the parent, so no
 * attacker can substitute any component the textual bind() re-resolves. Relative paths are anchored
 * to the current working directory (opened once); because bind() resolves a relative sun_path against
 * the process cwd, a relative path is only safe if the process does not chdir() between bind and
 * teardown (KEEL's single-threaded server model makes this a caller precondition, not a race).
 */

#include "unix_socket_node.h"
#include "socket.h"            /* KlSocketProvider seam: socket / bind / cloexec / close */
#include "sockaddr_native.h"   /* KlSockAddr <-> sockaddr */

#include <string.h>
#include <errno.h>
#include <stddef.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>
#include <sys/types.h>         /* uid_t, gid_t, dev_t, ino_t */
#include <sys/stat.h>
#include <sys/un.h>
#include <fcntl.h>

/* Compile coverage: these POSIX.1-2008 flags must exist on every hosted POSIX target, and the
 * leaf-copy buffer must bound sun_path. The openat / fstatat / unlinkat / fchownat / fchmodat CALLS
 * are likewise -Werror-guarded against an implicit declaration if a target lacks them. */
#if !defined(O_DIRECTORY) || !defined(O_NOFOLLOW) || !defined(O_CLOEXEC) || !defined(AT_SYMLINK_NOFOLLOW)
#error "AF_UNIX node-cleanup hardening requires O_DIRECTORY, O_NOFOLLOW, O_CLOEXEC, AT_SYMLINK_NOFOLLOW"
#endif

/* Upper bound on the copied leaf; sun_path is 108 (Linux) / 104 (BSD/macOS). */
#define KL_UNIX_NODE_PATH_MAX 108

/* Per-bind lifecycle state. Owned by the caller's reserved KL_UNIX_NODE_STORAGE bytes; managed only
 * here. Fixed storage, no allocation. Native POSIX identity types (no truncation). A zeroed buffer
 * (dir_open == 0) is a valid "not open" state, so memset alone is a valid init. */
typedef struct {
    int   dir_open;                     /* 1 = dirfd holds a live descriptor */
    int   dirfd;                        /* lifetime-held, trust-validated parent dir (when dir_open) */
    int   node_captured;                /* 1 = node_dev/node_ino are valid (we bound the node) */
    int   owner_set;                    /* 1 = owner_uid is a resolved policy owner */
    uid_t owner_uid;                    /* resolved owner uid (accepted-owner set) */
    dev_t node_dev;                     /* bound-node identity (teardown check) */
    ino_t node_ino;
    char  base[KL_UNIX_NODE_PATH_MAX];
} KlUnixNodeState;

_Static_assert(sizeof(KlUnixNodeState) <= KL_UNIX_NODE_STORAGE,
               "KL_UNIX_NODE_STORAGE must bound KlUnixNodeState");
_Static_assert(sizeof(((struct sockaddr_un *)0)->sun_path) <= KL_UNIX_NODE_PATH_MAX,
               "KL_UNIX_NODE_PATH_MAX must bound sockaddr_un.sun_path");

/* ── Name resolution (kept in substrate so it never leaks into the protocol layer) ─────────── */

static KlUnixNodeStatus resolve_uid(KlAllocator *alloc, const char *name, uid_t *out) {
    long hint = sysconf(_SC_GETPW_R_SIZE_MAX);
    size_t bufsz = (hint > 0) ? (size_t)hint : 4096;
    for (;;) {
        char *buf = kl_malloc(alloc, bufsz);
        if (!buf) return KL_UNIX_NODE_ERR_NOMEM;
        struct passwd pw, *res = NULL;
        int rc = getpwnam_r(name, &pw, buf, bufsz, &res);
        if (rc == 0) {
            KlUnixNodeStatus st = res ? KL_UNIX_NODE_OK : KL_UNIX_NODE_ERR_UNKNOWN_OWNER;
            if (res) *out = res->pw_uid;
            kl_free(alloc, buf, bufsz);
            return st;
        }
        kl_free(alloc, buf, bufsz);
        if (rc == ERANGE && bufsz < (1u << 20)) { bufsz *= 2; continue; }
        return KL_UNIX_NODE_ERR_UNKNOWN_OWNER;
    }
}

static KlUnixNodeStatus resolve_gid(KlAllocator *alloc, const char *name, gid_t *out) {
    long hint = sysconf(_SC_GETGR_R_SIZE_MAX);
    size_t bufsz = (hint > 0) ? (size_t)hint : 4096;
    for (;;) {
        char *buf = kl_malloc(alloc, bufsz);
        if (!buf) return KL_UNIX_NODE_ERR_NOMEM;
        struct group gr, *res = NULL;
        int rc = getgrnam_r(name, &gr, buf, bufsz, &res);
        if (rc == 0) {
            KlUnixNodeStatus st = res ? KL_UNIX_NODE_OK : KL_UNIX_NODE_ERR_UNKNOWN_OWNER;
            if (res) *out = res->gr_gid;
            kl_free(alloc, buf, bufsz);
            return st;
        }
        kl_free(alloc, buf, bufsz);
        if (rc == ERANGE && bufsz < (1u << 20)) { bufsz *= 2; continue; }
        return KL_UNIX_NODE_ERR_UNKNOWN_OWNER;
    }
}

/* ── Trust-boundary primitives ─────────────────────────────────────────────────────────────── */

/* Reject unusable paths and split off the leaf. dirbuf receives a mutable copy of the path; sets
 * *base_out to the final component. Returns the index of the last '/' (>=0), or -1 when there is no
 * '/'. On rejection returns -1 AND leaves *base_out NULL. */
static int split_and_check_path(const char *path, char *dirbuf, size_t dircap, const char **base_out) {
    *base_out = NULL;
    size_t len = strlen(path);
    if (len == 0 || len >= dircap) return -1;
    if (path[len - 1] == '/') return -1;                 /* no final component */
    memcpy(dirbuf, path, len + 1);
    char *slash = strrchr(dirbuf, '/');
    const char *base = slash ? slash + 1 : dirbuf;
    if (base[0] == '\0' || strcmp(base, ".") == 0 || strcmp(base, "..") == 0)
        return -1;
    *base_out = base;
    return slash ? (int)(slash - dirbuf) : -1;
}

/* Enforce the trust tier on an opened directory. tier_b = ownership transfers to a foreign uid,
 * which forbids ANY group/other write (a sticky shared dir is not sufficient once the node is owned
 * by that uid, since directory-entry substitution needs directory write). Returns 0 or -1. */
static int check_dir_trust(int dirfd, int tier_b) {
    struct stat ds;
    if (fstat(dirfd, &ds) < 0) return -1;
    if (ds.st_uid != geteuid() && ds.st_uid != 0) return -1;
    if (ds.st_mode & (S_IWGRP | S_IWOTH)) {
        if (tier_b) return -1;
        if (!(ds.st_mode & S_ISVTX)) return -1;
    }
    return 0;
}

/* Open the parent directory, walking EVERY component with O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC from a
 * trusted root and enforcing the trust tier on EACH component (start dir + every intermediate),
 * so no component the textual bind() re-resolves can be substituted by an untrusted actor. @dir is
 * the mutable parent portion ("" = the start dir); it is destructively tokenized. On success sets
 * *out_dirfd and returns KL_UNIX_NODE_OK; otherwise returns a status (*out_errno set for syscall
 * failures) and leaves *out_dirfd = -1. */
static KlUnixNodeStatus open_parent_walk(int tier_b, char *dir, int start_absolute,
                                         int *out_dirfd, int *out_errno) {
    *out_dirfd = -1;
    int dirfd = open(start_absolute ? "/" : ".", O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dirfd < 0) { if (out_errno) *out_errno = errno; return KL_UNIX_NODE_ERR_BIND; }
    if (check_dir_trust(dirfd, tier_b) < 0) { close(dirfd); return KL_UNIX_NODE_ERR_UNTRUSTED_PARENT; }
    char *p = dir;
    while (*p == '/') p++;
    while (*p) {
        char *seg = p;
        char *next = strchr(p, '/');
        if (next) { *next = '\0'; p = next + 1; while (*p == '/') p++; }
        else p += strlen(p);
        if (seg[0] == '\0' || strcmp(seg, ".") == 0) continue;
        if (strcmp(seg, "..") == 0) { close(dirfd); return KL_UNIX_NODE_ERR_INVALID_PATH; }
        int nfd = openat(dirfd, seg, O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (nfd < 0) { int e = errno; close(dirfd); if (out_errno) *out_errno = e; return KL_UNIX_NODE_ERR_BIND; }
        close(dirfd);
        dirfd = nfd;
        if (check_dir_trust(dirfd, tier_b) < 0) { close(dirfd); return KL_UNIX_NODE_ERR_UNTRUSTED_PARENT; }
    }
    *out_dirfd = dirfd;
    return KL_UNIX_NODE_OK;
}

/* Directory-relative identity check for a leaf we may remove/mutate. Returns 0 (present: a socket
 * owned by an accepted uid, *st filled), 1 (absent: ENOENT), or -1 (unsafe/foreign/non-socket/error).
 * Accepted owners: euid, plus owner_uid when owner_set. */
static int stat_accepted_socket(int dirfd, const char *base, uid_t owner_uid, int owner_set,
                                struct stat *st) {
    if (fstatat(dirfd, base, st, AT_SYMLINK_NOFOLLOW) < 0)
        return (errno == ENOENT) ? 1 : -1;
    if (!S_ISSOCK(st->st_mode)) return -1;
    if (st->st_uid != geteuid() && !(owner_set && st->st_uid == owner_uid)) return -1;
    return 0;
}

/* Close the held parent dir exactly once, clearing the open flag and per-bind identity. */
static void close_dir(KlUnixNodeState *ns) {
    if (ns->dir_open) { close(ns->dirfd); ns->dir_open = 0; }
    ns->dirfd = -1;
    ns->base[0] = '\0';
    ns->node_captured = 0;
}

/* True iff the leaf is still exactly the node we bound (socket, dev+ino match the captured identity). */
static int leaf_is_bound_node(const KlUnixNodeState *ns) {
    struct stat st;
    if (!ns->node_captured) return 0;
    if (fstatat(ns->dirfd, ns->base, &st, AT_SYMLINK_NOFOLLOW) < 0) return 0;
    return S_ISSOCK(st.st_mode) && st.st_dev == ns->node_dev && st.st_ino == ns->node_ino;
}

/* Create the AF_UNIX socket + bind it at @path through @sockets (umask-guarded when a mode is set).
 * Sets *out_fd on success; closes it and returns -1 on failure (errno preserved). */
static int create_and_bind(const KlSocketProvider *sockets, const KlUnixNodePolicy *policy,
                           const char *path, size_t path_len, KlSocketHandle *out_fd) {
    KlSocketHandle fd = kl_sock_socket(sockets, AF_UNIX, SOCK_STREAM, 0);
    if (!kl_handle_valid(fd)) return -1;
    kl_sock_set_cloexec(sockets, fd);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, path_len + 1);
    socklen_t addr_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + path_len + 1);

    mode_t old_umask = 0; int umask_set = 0;
    if (policy->set_mode) { old_umask = umask(0777 & ~(mode_t)policy->mode); umask_set = 1; }
    KlSockAddr bind_sa;
    kl_sockaddr_from_native(&bind_sa, (struct sockaddr *)&addr, addr_len);
    int rc = kl_sock_bind(sockets, fd, &bind_sa);
    int saved = errno;
    if (umask_set) umask(old_umask);
    if (rc < 0) { kl_sock_close(sockets, fd); errno = saved; return -1; }
    *out_fd = fd;
    return 0;
}

/* ── Public contract ───────────────────────────────────────────────────────────────────────── */

void kl_unix_socket_node_init(void *state) {
    KlUnixNodeState *ns = state;
    memset(ns, 0, sizeof(*ns));   /* dir_open = 0 => not open; explicit init is optional */
    ns->dirfd = -1;
}

KlUnixNodeStatus kl_unix_socket_node_bind(const KlUnixNodePolicy *policy,
                                          const KlSocketProvider *sockets,
                                          KlAllocator *alloc, void *state,
                                          KlSocketHandle *out_fd, int *out_errno) {
    KlUnixNodeState *ns = state;
    if (out_errno) *out_errno = 0;

    /* Reject bind on an already-open state (the caller must teardown first). Do this BEFORE touching
     * *out_fd, so a misuse neither leaks the held dir nor clobbers the caller's live handle. */
    if (ns->dir_open) { if (out_errno) *out_errno = EEXIST; return KL_UNIX_NODE_ERR_BIND; }

    if (out_fd) *out_fd = KL_INVALID_SOCKET;   /* invalid until a bind actually succeeds */

    const char *path = policy->path;
    if (!path || path[0] == '\0') return KL_UNIX_NODE_ERR_INVALID_PATH;
    size_t path_len = strlen(path);
    if (path_len >= sizeof(((struct sockaddr_un *)0)->sun_path))
        return KL_UNIX_NODE_ERR_INVALID_PATH;

    /* Resolve owner/group ONCE, up front, and reuse for the trust-tier decision, the reclaim
     * accepted-owner set, and the post-bind chown (no duplicate/inconsistent lookups). */
    uid_t owner_uid = (uid_t)-1; gid_t owner_gid = (gid_t)-1;
    int owner_set = 0, group_set = 0;
    if (policy->owner) {
        KlUnixNodeStatus st = resolve_uid(alloc, policy->owner, &owner_uid);
        if (st != KL_UNIX_NODE_OK) return st;   /* state not open, out_fd invalid */
        owner_set = 1;
    }
    if (policy->group) {
        KlUnixNodeStatus st = resolve_gid(alloc, policy->group, &owner_gid);
        if (st != KL_UNIX_NODE_OK) return st;
        group_set = 1;
    }
    int tier_b = owner_set && owner_uid != geteuid();

    /* Component-walk + hold the trusted parent dir; copy the leaf into lifecycle-owned storage. */
    char dirbuf[KL_UNIX_NODE_PATH_MAX];
    const char *base = NULL;
    int slash_at = split_and_check_path(path, dirbuf, sizeof(dirbuf), &base);
    if (!base) return KL_UNIX_NODE_ERR_INVALID_PATH;
    if (strlen(base) >= sizeof(ns->base)) return KL_UNIX_NODE_ERR_INVALID_PATH;
    char base_copy[KL_UNIX_NODE_PATH_MAX];
    memcpy(base_copy, base, strlen(base) + 1);       /* copy BEFORE the walk NUL-splits dirbuf */
    if (slash_at < 0) dirbuf[0] = '\0';
    else dirbuf[slash_at] = '\0';
    int dirfd = -1;
    KlUnixNodeStatus walk = open_parent_walk(tier_b, dirbuf, path[0] == '/', &dirfd, out_errno);
    if (walk != KL_UNIX_NODE_OK) return walk;
    ns->dirfd = dirfd;
    ns->dir_open = 1;
    memcpy(ns->base, base_copy, strlen(base_copy) + 1);
    ns->owner_uid = owner_uid;
    ns->owner_set = owner_set;
    ns->node_captured = 0;

    /* Reclaim a stale, validated, accepted-owner socket node (opt-in); fail closed otherwise. */
    if (policy->unlink_stale) {
        struct stat st;
        int r = stat_accepted_socket(dirfd, ns->base, owner_uid, owner_set, &st);
        if (r < 0) {
            if (out_errno) *out_errno = (errno == ENOENT) ? ESTALE : errno;
            close_dir(ns);
            return KL_UNIX_NODE_ERR_FOREIGN_NODE;
        }
        if (r == 0 && unlinkat(dirfd, ns->base, 0) < 0) {
            if (out_errno) *out_errno = errno;
            close_dir(ns);
            return KL_UNIX_NODE_ERR_BIND;
        }
    }

    if (create_and_bind(sockets, policy, path, path_len, out_fd) < 0) {
        int saved = errno;
        close_dir(ns);
        if (out_errno) *out_errno = saved;
        return KL_UNIX_NODE_ERR_BIND;
    }

    /* Capture the bound FILESYSTEM node identity via fstatat (NOT the socket fd, which names the
     * kernel object). This is the chown/chmod + teardown identity. */
    struct stat bnode;
    if (fstatat(dirfd, ns->base, &bnode, AT_SYMLINK_NOFOLLOW) < 0) {
        if (out_errno) *out_errno = errno;
        goto fail_unlink;
    }
    if (!S_ISSOCK(bnode.st_mode)) {
        if (out_errno) *out_errno = ESTALE;   /* the node we bound is no longer a socket */
        goto fail_unlink;
    }
    ns->node_dev = bnode.st_dev;
    ns->node_ino = bnode.st_ino;
    ns->node_captured = 1;

    /* Owner/group via fchownat (identity revalidated), directory-relative, not fchown(fd). */
    if (owner_set || group_set) {
        if (!leaf_is_bound_node(ns)) { if (out_errno) *out_errno = ESTALE; goto fail_unlink; }
        uid_t uid = owner_set ? owner_uid : (uid_t)-1;
        gid_t gid = group_set ? owner_gid : (gid_t)-1;
        if (fchownat(dirfd, ns->base, uid, gid, AT_SYMLINK_NOFOLLOW) < 0) {
            if (out_errno) *out_errno = errno;
            goto fail_unlink;
        }
    }

    /* Exact mode via fchmodat, AFTER chown (chown can clear set-*id). Safe because tier B forbids a
     * foreign owner directory write, so it cannot substitute the entry between chown and chmod. */
    if (policy->set_mode) {
        if (!leaf_is_bound_node(ns)) { if (out_errno) *out_errno = ESTALE; goto fail_unlink; }
        if (fchmodat(dirfd, ns->base, (mode_t)policy->mode, 0) < 0) {
            if (out_errno) *out_errno = errno;
            goto fail_unlink;
        }
    }

    return KL_UNIX_NODE_OK;

fail_unlink:
    /* Failure-path cleanup: remove ONLY the node we bound (validated identity), never a replacement. */
    if (leaf_is_bound_node(ns))
        (void)unlinkat(ns->dirfd, ns->base, 0);
    kl_sock_close(sockets, *out_fd);
    *out_fd = KL_INVALID_SOCKET;
    close_dir(ns);
    return KL_UNIX_NODE_ERR_BIND;
}

KlUnixNodeStatus kl_unix_socket_node_teardown(void *state, int unlink_stale) {
    KlUnixNodeState *ns = state;
    KlUnixNodeStatus rc = KL_UNIX_NODE_OK;
    if (unlink_stale && ns->dir_open && ns->base[0] != '\0' && ns->node_captured) {
        int tier_b = ns->owner_set && ns->owner_uid != geteuid();
        struct stat st;
        /* Fail closed unless the parent still passes its tier AND the leaf is still exactly our bound
         * node (socket, accepted owner, dev+ino match). Otherwise leave the ambiguous entry. */
        if (check_dir_trust(ns->dirfd, tier_b) < 0) {
            rc = KL_UNIX_NODE_ERR_UNTRUSTED_PARENT;
        } else if (stat_accepted_socket(ns->dirfd, ns->base, ns->owner_uid, ns->owner_set, &st) != 0 ||
                   st.st_dev != ns->node_dev || st.st_ino != ns->node_ino) {
            rc = KL_UNIX_NODE_ERR_FOREIGN_NODE;
        } else if (unlinkat(ns->dirfd, ns->base, 0) < 0) {
            rc = KL_UNIX_NODE_ERR_BIND;   /* report the removal failure rather than claiming success */
        }
    }
    close_dir(ns);
    return rc;
}
