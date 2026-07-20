#if defined(__linux__)
/* Needed for struct ucred / SO_PEERCRED (kl_request_peer_cred). Must precede
 * any libc header. server.c uses no function whose signature changes under
 * _GNU_SOURCE, so this is safe. */
#define _GNU_SOURCE
#endif

#include <keel/server.h>
#include <keel/async.h>
#include <keel/timer.h>
#include <keel/tls.h>
#include <keel/websocket_server.h>
#include <keel/h2_server.h>
#include <string.h>
#include <unistd.h>
#if !defined(KL_NO_SIGNAL)
#include <signal.h>
#endif
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <fcntl.h>
#include <pwd.h>
#include <grp.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include "internal.h"
#include "socket.h"

#define KL_LISTEN_BACKLOG  128
#define KL_EVENTS_PER_TICK 64
#define KL_POLL_TIMEOUT_MS 1000

/* ── Signal handling ─────────────────────────────────────────────── */

#if !defined(KL_NO_SIGNAL)
static _Atomic(KlServer *) kl_signal_server = NULL;

static void kl_signal_handler(int sig) {
    (void)sig;
    KlServer *s = atomic_load(&kl_signal_server);
    if (s) kl_server_stop(s);
}
#endif

static const char kl_408_response[] =
    "HTTP/1.1 408 Request Timeout\r\n"
    "Content-Length: 0\r\n"
    "Connection: close\r\n"
    "\r\n";

__attribute__((format(printf, 3, 4)))
static void kl_log(KlServer *s, int level, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (s->config.log_fn) {
        s->config.log_fn(level, fmt, ap, s->config.log_user_data);
    } else {
        fprintf(stderr, "keel: ");
        vfprintf(stderr, fmt, ap);
        fputc('\n', stderr);
    }
    va_end(ap);
}

static void kl_log_errno(KlServer *s, int level, const char *msg) {
    kl_log(s, level, "%s: %s", msg, strerror(errno));
}

static int kl_server_bind_tcp(KlServer *s) {
    /* Resolve bind address (supports IPv4, IPv6, and hostnames) */
    struct addrinfo hints, *ai = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICHOST;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", s->config.port);

    int gai_rc = getaddrinfo(s->config.bind_addr, port_str, &hints, &ai);
    if (gai_rc != 0 || !ai) {
        kl_log(s, KL_LOG_ERROR, "invalid bind address '%s': %s",
               s->config.bind_addr, gai_strerror(gai_rc));
        s->last_error = KL_ERR_DNS;
        return -1;
    }

    s->listen_fd = kl_sock_socket(s->ev.sockets, ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (s->listen_fd < 0) {
        kl_log_errno(s, KL_LOG_ERROR, "socket");
        s->last_error = KL_ERR_SOCKET;
        freeaddrinfo(ai);
        return -1;
    }
    kl_sock_set_cloexec(s->ev.sockets, s->listen_fd);

    int opt = 1;
    setsockopt(s->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
    (void)setsockopt(s->listen_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif

    /* IPv6 dual-stack: accept both IPv4 and IPv6 on :: */
    if (ai->ai_family == AF_INET6) {
        int off = 0;
        setsockopt(s->listen_fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
    }

    if (kl_sock_bind(s->ev.sockets, s->listen_fd, ai->ai_addr, ai->ai_addrlen) < 0) {
        kl_log_errno(s, KL_LOG_ERROR, "bind");
        s->last_error = KL_ERR_BIND;
        close(s->listen_fd);
        s->listen_fd = -1;
        freeaddrinfo(ai);
        return -1;
    }
    freeaddrinfo(ai);

    /* Retrieve OS-assigned port (useful when config.port == 0) */
    {
        struct sockaddr_storage sa;
        memset(&sa, 0, sizeof(sa));
        socklen_t sa_len = sizeof(sa);
        if (getsockname(s->listen_fd, (struct sockaddr *)&sa, &sa_len) == 0) {
            if (sa.ss_family == AF_INET)
                s->bound_port = ntohs(((struct sockaddr_in *)&sa)->sin_port);
            else if (sa.ss_family == AF_INET6)
                s->bound_port = ntohs(((struct sockaddr_in6 *)&sa)->sin6_port);
        }
    }

    return 0;
}

static int kl_server_unlink_stale_unix_socket(KlServer *s, const char *path) {
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
static int kl_resolve_uid(KlServer *s, const char *name, uid_t *out) {
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
static int kl_resolve_gid(KlServer *s, const char *name, gid_t *out) {
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

static int kl_server_bind_unix(KlServer *s) {
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
    if (s->listen_fd < 0) {
        kl_log_errno(s, KL_LOG_ERROR, "socket");
        s->last_error = KL_ERR_SOCKET;
        return -1;
    }
    kl_sock_set_cloexec(s->ev.sockets, s->listen_fd);

    if (s->config.unix_socket_unlink &&
        kl_server_unlink_stale_unix_socket(s, path) < 0) {
        close(s->listen_fd);
        s->listen_fd = -1;
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
    int bind_rc = kl_sock_bind(s->ev.sockets, s->listen_fd, (struct sockaddr *)&addr, addr_len);
    if (umask_set)
        umask(old_umask);
    if (bind_rc < 0) {
        kl_log_errno(s, KL_LOG_ERROR, "bind");
        s->last_error = KL_ERR_BIND;
        close(s->listen_fd);
        s->listen_fd = -1;
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
            kl_resolve_uid(s, s->config.unix_socket_owner, &uid) < 0) {
            if (s->last_error != KL_ERR_ALLOC) {
                kl_log(s, KL_LOG_ERROR, "unknown unix socket owner '%s'",
                       s->config.unix_socket_owner);
                s->last_error = KL_ERR_INVALID_ARG;
            }
            goto fail;
        }
        if (s->config.unix_socket_group &&
            kl_resolve_gid(s, s->config.unix_socket_group, &gid) < 0) {
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
    close(s->listen_fd);
    s->listen_fd = -1;
    unlink(path);
    s->unix_socket_owned = 0;
    return -1;
}

/*
 * Adopt a pre-bound, already-listening fd (socket activation). The fd's
 * family determines the transport, so TCP_NODELAY and unlink policy stay
 * correct. KEEL never owns (never unlinks) an adopted UNIX socket.
 */
static int kl_server_adopt_fd(KlServer *s) {
    s->listen_fd = s->config.listen_fd;

    struct sockaddr_storage sa;
    memset(&sa, 0, sizeof(sa));
    socklen_t sa_len = sizeof(sa);
    if (getsockname(s->listen_fd, (struct sockaddr *)&sa, &sa_len) != 0) {
        kl_log_errno(s, KL_LOG_ERROR, "getsockname on adopted fd");
        s->last_error = KL_ERR_SOCKET;
        s->listen_fd = -1;
        return -1;
    }

    if (sa.ss_family == AF_UNIX) {
        s->config.transport = KL_TRANSPORT_UNIX;
        s->bound_port = 0;
    } else {
        s->config.transport = KL_TRANSPORT_TCP;
        if (sa.ss_family == AF_INET)
            s->bound_port = ntohs(((struct sockaddr_in *)&sa)->sin_port);
        else if (sa.ss_family == AF_INET6)
            s->bound_port = ntohs(((struct sockaddr_in6 *)&sa)->sin6_port);
    }
    /* Adopted fd is never unlinked — the supervisor owns the socket path. */
    s->unix_socket_owned = 0;
    /* CLOEXEC so the inherited listener doesn't re-leak to our own children. */
    kl_sock_set_cloexec(s->ev.sockets, s->listen_fd);
    return 0;
}

static int kl_server_bind_listener(KlServer *s) {
    if (s->config.listen_fd > 0)
        return kl_server_adopt_fd(s);
    if (s->config.transport == KL_TRANSPORT_UNIX)
        return kl_server_bind_unix(s);
    return kl_server_bind_tcp(s);
}

/* cppcheck-suppress constParameterPointer  ; 'out' is written on the
   platform branches below (invisible to cppcheck's default config). */
int kl_peer_cred_fd(KlSocketHandle fd, KlPeerCred *out) {
    if (fd < 0 || !out)
        return -1;

#if defined(__linux__) && defined(SO_PEERCRED)
    struct ucred cr;
    socklen_t len = sizeof(cr);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &len) != 0)
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
    if (getpeereid(fd, &uid, &gid) != 0)
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
        if (getsockopt(fd, SOL_LOCAL, LOCAL_PEERPID, &pid, &pl) == 0) {
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

int kl_request_peer_cred(const KlRequest *req, KlPeerCred *out) {
    if (!req)
        return -1;
    const KlConn *conn = kl_request_conn(req);
    if (!conn)
        return -1;
    return kl_peer_cred_fd(conn->fd, out);
}

int kl_request_peer_label(const KlRequest *req, char *buf, size_t buflen) {
    if (!req || !buf || buflen == 0)
        return -1;
    const KlConn *conn = kl_request_conn(req);
    if (!conn || conn->fd < 0)
        return -1;

#if defined(__linux__) && defined(SO_PEERSEC)
    socklen_t len = (socklen_t)buflen;
    if (getsockopt(conn->fd, SOL_SOCKET, SO_PEERSEC, buf, &len) != 0)
        return -1;
    /* Ensure NUL-termination regardless of what the kernel returned. */
    if ((size_t)len >= buflen)
        len = (socklen_t)(buflen - 1);
    buf[len] = '\0';
    return 0;
#else
    (void)buf; (void)buflen;
    return -1;  /* SO_PEERSEC (SELinux/AppArmor label) is Linux-only */
#endif
}

const struct sockaddr *kl_request_peer_sockaddr(const KlRequest *req,
                                                socklen_t *len) {
    if (!req)
        return NULL;
    const KlConn *conn = kl_request_conn(req);
    if (!conn || conn->peer_addr_len == 0)
        return NULL;
    if (len)
        *len = conn->peer_addr_len;
    return (const struct sockaddr *)&conn->peer_addr;
}

int kl_request_peer_addr(const KlRequest *req, char *ip, size_t iplen,
                         uint16_t *port) {
    if (!req || !ip || iplen == 0)
        return -1;
    const KlConn *conn = kl_request_conn(req);
    if (!conn || conn->peer_addr_len == 0)
        return -1;

    const struct sockaddr *sa = (const struct sockaddr *)&conn->peer_addr;
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *s4 = (const struct sockaddr_in *)sa;
        if (!inet_ntop(AF_INET, &s4->sin_addr, ip, (socklen_t)iplen))
            return -1;
        if (port)
            *port = ntohs(s4->sin_port);
        return 0;
    }
    if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *s6 = (const struct sockaddr_in6 *)sa;
        if (!inet_ntop(AF_INET6, &s6->sin6_addr, ip, (socklen_t)iplen))
            return -1;
        if (port)
            *port = ntohs(s6->sin6_port);
        return 0;
    }
    return -1;  /* AF_UNIX or other — no IP address (use peer credentials) */
}

int kl_request_peer_cert(const KlRequest *req, KlPeerCert *out) {
    if (!req || !out)
        return -1;
    const KlConn *conn = kl_request_conn(req);
    if (!conn || !conn->tls || !conn->tls->peer_cert)
        return -1;   /* plaintext connection, or backend lacks mTLS support */
    memset(out, 0, sizeof(*out));
    return conn->tls->peer_cert(conn->tls, out);
}

int kl_systemd_listen_fds(int *count) {
    const char *pid_s = getenv("LISTEN_PID");
    const char *fds_s = getenv("LISTEN_FDS");
    int first = -1;
    int n = 0;

    if (pid_s && fds_s) {
        char *end;
        long lpid = strtol(pid_s, &end, 10);
        if (end != pid_s && *end == '\0' && (long)getpid() == lpid) {
            long nfds = strtol(fds_s, &end, 10);
            if (end != fds_s && *end == '\0' && nfds >= 1 && nfds <= 4096) {
                n = (int)nfds;
                first = 3;  /* SD_LISTEN_FDS_START; the fds are 3 .. 3+n-1 */
            }
        }
    }

    /* Clear so the variables are not inherited by child processes. */
    unsetenv("LISTEN_PID");
    unsetenv("LISTEN_FDS");
    unsetenv("LISTEN_FDNAMES");
    if (count)
        *count = n;
    return first;
}

int kl_systemd_listen_fd(void) {
    return kl_systemd_listen_fds(NULL);
}

int kl_systemd_listen_fd_by_name(const char *name) {
    if (!name)
        return -1;
    const char *pid_s = getenv("LISTEN_PID");
    const char *fds_s = getenv("LISTEN_FDS");
    const char *names = getenv("LISTEN_FDNAMES");
    int result = -1;

    if (pid_s && fds_s && names) {
        char *end;
        long lpid = strtol(pid_s, &end, 10);
        long nfds = 0;
        if (end != pid_s && *end == '\0' && (long)getpid() == lpid) {
            nfds = strtol(fds_s, &end, 10);
            if (!(end != fds_s && *end == '\0' && nfds >= 1 && nfds <= 4096))
                nfds = 0;
        }
        /* LISTEN_FDNAMES is a colon-separated list, one name per passed fd in
         * fd order starting at SD_LISTEN_FDS_START (3). */
        size_t namelen = strlen(name);
        const char *p = names;
        for (long idx = 0; idx < nfds && p; idx++) {
            const char *colon = strchr(p, ':');
            size_t seglen = colon ? (size_t)(colon - p) : strlen(p);
            if (seglen == namelen && memcmp(p, name, namelen) == 0) {
                result = 3 + (int)idx;
                break;
            }
            p = colon ? colon + 1 : NULL;
        }
    }

    unsetenv("LISTEN_PID");
    unsetenv("LISTEN_FDS");
    unsetenv("LISTEN_FDNAMES");
    return result;
}

static void kl_server_close_listener(KlServer *s) {
    if (s->listen_fd >= 0) {
        close(s->listen_fd);
        s->listen_fd = -1;
    }
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

/*
 * Release a connection and resume the listen socket if it was paused
 * due to pool exhaustion.  Called from the event loop, timeout sweep,
 * and async completion.
 */
void kl_server_conn_release(KlServer *s, KlConn *c) {
    kl_conn_release(&s->pool, c);
    if (s->listen_paused && s->pool.free_list) {
        kl_event_add(&s->ev.loop, s->listen_fd, KL_EVENT_READ, NULL);
        s->listen_paused = 0;
    }
}

int kl_server_init(KlServer *s, const KlConfig *config) {
    if (!s || !config) {
        if (s) s->last_error = KL_ERR_INVALID_ARG;
        return -1;
    }
    memset(s, 0, sizeof(*s));
    s->listen_fd = -1;

    /* Apply defaults */
    s->config = *config;
    if (s->config.transport != KL_TRANSPORT_TCP &&
        s->config.transport != KL_TRANSPORT_UNIX) {
        s->last_error = KL_ERR_INVALID_ARG;
        return -1;
    }
    if (s->config.unix_socket_path &&
        s->config.transport == KL_TRANSPORT_TCP) {
        s->config.transport = KL_TRANSPORT_UNIX;
    }
    /* listen_fd: 0 = disabled, > 0 = adopt (fds 0-2 are stdio, not listeners).
     * Reject a negative value rather than silently ignoring it. */
    if (s->config.listen_fd < 0) {
        s->last_error = KL_ERR_INVALID_ARG;
        return -1;
    }
    if (s->config.bind_addr == NULL)
        s->config.bind_addr = "0.0.0.0";
    if (s->config.max_connections <= 0)
        s->config.max_connections = KL_DEFAULT_MAX_CONNS;
    if (s->config.read_timeout_ms <= 0)
        s->config.read_timeout_ms = KL_DEFAULT_READ_TIMEOUT;
    if (s->config.parser == NULL)
        s->config.parser = kl_parser_llhttp;
    if (s->config.max_body_size == 0)
        s->config.max_body_size = KL_DEFAULT_MAX_BODY_SIZE;
    if (s->config.max_header_size == 0)
        s->config.max_header_size = KL_READ_BUF_SIZE;
    if (s->config.max_header_size > SIZE_MAX / 2) {
        s->last_error = KL_ERR_OVERFLOW;
        return -1;
    }

    /* Set up allocator */
    if (s->config.alloc) {
        s->alloc_storage = *s->config.alloc;
    } else {
        s->alloc_storage = kl_allocator_default();
    }
    KlAllocator *alloc = &s->alloc_storage;

    /* Init subsystems */
    if (kl_router_init(&s->router, alloc) < 0) {
        s->last_error = KL_ERR_ALLOC;
        return -1;
    }
    if (kl_conn_pool_init(&s->pool, s->config.max_connections, alloc) < 0) {
        s->last_error = KL_ERR_ALLOC;
        kl_router_free(&s->router);
        return -1;
    }

    /* Copy TLS config if provided */
    if (s->config.tls) {
        s->tls_storage = *s->config.tls;
        s->config.tls = &s->tls_storage;
    }

    /* Copy HTTP/2 config if provided */
    if (s->config.h2) {
        s->h2_storage = *s->config.h2;
        s->config.h2 = &s->h2_storage;
    }

    /* Copy compress config if provided */
    if (s->config.compress) {
        s->compress_storage = *s->config.compress;
        s->config.compress = &s->compress_storage;
    }

    /* Parse the PROXY protocol trusted-source CIDR allowlist (if any). */
    s->proxy_cidrs = NULL;
    s->proxy_cidr_count = 0;
    if (s->config.proxy_trusted_cidrs) {
        KlCidr tmp[64];
        int cn = kl_cidr_parse_list(s->config.proxy_trusted_cidrs, tmp,
                                    (int)(sizeof(tmp) / sizeof(tmp[0])));
        if (cn < 0) {
            s->last_error = KL_ERR_INVALID_ARG;
            kl_conn_pool_free(&s->pool);
            kl_router_free(&s->router);
            return -1;
        }
        if (cn > 0) {
            s->proxy_cidrs = kl_malloc(alloc, (size_t)cn * sizeof(KlCidr));
            if (!s->proxy_cidrs) {
                s->last_error = KL_ERR_ALLOC;
                kl_conn_pool_free(&s->pool);
                kl_router_free(&s->router);
                return -1;
            }
            memcpy(s->proxy_cidrs, tmp, (size_t)cn * sizeof(KlCidr));
            s->proxy_cidr_count = cn;
        }
    }

    /* Create parsers and propagate config for each connection slot */
    for (int i = 0; i < s->pool.capacity; i++) {
        s->pool.conns[i].parser = s->config.parser(alloc);
        if (!s->pool.conns[i].parser) {
            s->last_error = KL_ERR_ALLOC;
            kl_conn_pool_free(&s->pool);
            kl_router_free(&s->router);
            return -1;
        }
        s->pool.conns[i].access_log = s->config.access_log;
        s->pool.conns[i].access_log_data = s->config.access_log_data;
        s->pool.conns[i].h2_config = s->config.h2;  /* NULL if disabled */
        s->pool.conns[i].router = &s->router;
        s->pool.conns[i].ctx = &s->ev;   /* for the socket provider (ctx->sockets) */
        s->pool.conns[i].max_body_size = s->config.max_body_size;
        s->pool.conns[i].max_header_size = s->config.max_header_size;
    }

    /* Pre-allocate TLS sessions (one per connection slot) */
    if (s->config.tls) {
        for (int i = 0; i < s->pool.capacity; i++) {
            s->pool.conns[i].tls = s->config.tls->factory(
                s->config.tls->ctx, alloc);
            if (!s->pool.conns[i].tls) {
                s->last_error = KL_ERR_TLS_INIT;
                kl_conn_pool_free(&s->pool);
                kl_router_free(&s->router);
                return -1;
            }
            /* Validate vtable — all 7 required pointers must be set
             * (alpn_protocol is optional, NULL if not supported) */
            const KlTls *t = s->pool.conns[i].tls;
            if (!t->handshake || !t->read || !t->write ||
                !t->shutdown || !t->pending || !t->reset || !t->destroy) {
                s->last_error = KL_ERR_TLS_VTABLE;
                kl_conn_pool_free(&s->pool);
                kl_router_free(&s->router);
                return -1;
            }
        }
    }

    /* Init event context — must happen before thread pool / watcher registration */
    if (kl_event_ctx_init(&s->ev, alloc) < 0) {
        s->last_error = KL_ERR_EVENT_INIT;
        kl_conn_pool_free(&s->pool);
        kl_router_free(&s->router);
        return -1;
    }

    /* Create async file I/O backend (NULL if backend doesn't support it) */
    s->file_io = kl_file_io_create(&s->ev.loop, alloc);
    for (int i = 0; i < s->pool.capacity; i++)
        s->pool.conns[i].file_io = s->file_io;

    return 0;
}

int kl_server_route(KlServer *s, const char *method, const char *pattern,
                    KlHandler handler, void *user_data,
                    KlBodyReaderFactory body_reader) {
    return kl_router_add(&s->router, method, pattern, handler, user_data,
                         body_reader);
}

int kl_server_route_streaming(KlServer *s, const char *method, const char *pattern,
                               KlHandler handler, void *user_data,
                               KlBodyReaderFactory body_reader) {
    return kl_router_add_streaming(&s->router, method, pattern, handler,
                                    user_data, body_reader);
}

int kl_server_route_streaming_async(KlServer *s, const char *method,
                                       const char *pattern,
                                       KlHandler handler, void *user_data,
                                       KlBodyReaderFactory body_reader) {
    return kl_router_add_streaming_async(&s->router, method, pattern, handler,
                                            user_data, body_reader);
}

int kl_server_use(KlServer *s, const char *method, const char *pattern,
                  KlMiddleware fn, void *user_data) {
    return kl_router_use(&s->router, method, pattern, fn, user_data);
}

int kl_server_use_post(KlServer *s, const char *method, const char *pattern,
                       KlMiddleware fn, void *user_data) {
    return kl_router_use_post(&s->router, method, pattern, fn, user_data);
}

int kl_server_ws(KlServer *s, const char *pattern, KlWsServerConfig *config) {
    /* Register as a GET route with no handler — ws_config triggers upgrade */
    if (kl_router_add(&s->router, "GET", pattern, NULL, NULL, NULL) < 0)
        return -1;
    s->router.routes[s->router.count - 1].ws_config = config;
    return 0;
}

int kl_server_run(KlServer *s) {
    KlAllocator *alloc = &s->alloc_storage;

#if !defined(KL_NO_SIGNAL)
    signal(SIGPIPE, SIG_IGN);
#endif

    if (kl_server_bind_listener(s) < 0)
        return -1;

    /* An adopted fd (socket activation) is already listening — don't re-listen. */
    if (s->config.listen_fd <= 0 && kl_sock_listen(s->ev.sockets, s->listen_fd, KL_LISTEN_BACKLOG) < 0) {
        kl_log_errno(s, KL_LOG_ERROR, "listen");
        s->last_error = KL_ERR_LISTEN;
        kl_server_close_listener(s);
        return -1;
    }

    if (kl_sock_set_nonblocking(s->ev.sockets, s->listen_fd) < 0) {
        kl_log_errno(s, KL_LOG_ERROR, "fcntl");
        s->last_error = KL_ERR_SOCKET;
        kl_server_close_listener(s);
        return -1;
    }

    /* Register listen socket for read events */
    if (kl_event_add(&s->ev.loop, s->listen_fd, KL_EVENT_READ, NULL) < 0) {
        kl_log_errno(s, KL_LOG_ERROR, "event_add listen");
        s->last_error = KL_ERR_EVENT_ADD;
        kl_server_close_listener(s);
        return -1;
    }

    if (s->config.transport == KL_TRANSPORT_UNIX) {
        /* An adopted fd (socket activation) has no path we own. */
        kl_log(s, KL_LOG_INFO, "listening on unix:%s",
               s->config.unix_socket_path ? s->config.unix_socket_path
                                          : "(inherited fd)");
    } else if (s->config.listen_fd > 0) {
        kl_log(s, KL_LOG_INFO, "listening on inherited fd %d (port %d)",
               (int)s->listen_fd, s->bound_port);
    } else {
        kl_log(s, KL_LOG_INFO, "listening on %s:%d",
               s->config.bind_addr, s->bound_port);
    }

#if !defined(KL_NO_SIGNAL)
    /* Install signal handlers if requested */
    struct sigaction old_term, old_int;
    if (s->config.install_signal_handlers) {
        atomic_store(&kl_signal_server, s);
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = kl_signal_handler;
        sa.sa_flags = 0;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGTERM, &sa, &old_term);
        sigaction(SIGINT, &sa, &old_int);
    }
#endif

    atomic_store(&s->running, 1);
    atomic_store(&s->draining, 0);
    KlEvent events[KL_EVENTS_PER_TICK];

    while (atomic_load(&s->running)) {
        /* Compute dynamic timeout based on nearest async op deadline */
        uint64_t now = kl_monotonic_ms();
        int wait_timeout = KL_POLL_TIMEOUT_MS;
        for (KlAsyncOp *aop = s->async_ops; aop; aop = aop->next) {
            if (aop->deadline_ms > 0) {
                if (now >= aop->deadline_ms) {
                    wait_timeout = 0;
                    break;
                }
                uint64_t rem = aop->deadline_ms - now;
                if (rem < (uint64_t)wait_timeout)
                    wait_timeout = (int)rem;
            }
        }

        wait_timeout = kl_timer_next_timeout(&s->ev, wait_timeout);

        int n = kl_event_wait(&s->ev.loop, events, KL_EVENTS_PER_TICK,
                              wait_timeout);
        if (n < 0) {
            if (errno == EINTR) continue;
            kl_log_errno(s, KL_LOG_ERROR, "event_wait");
            break;
        }

        /* Dispatch async file I/O completions */
        if (s->file_io) {
            KlFileIOResult fio_results[KL_EVENTS_PER_TICK];
            int nf = s->file_io->tick(s->file_io, fio_results,
                                       KL_EVENTS_PER_TICK);
            for (int fi = 0; fi < nf; fi++) {
                KlConn *fc = fio_results[fi].udata;
                KlConnState fstate = kl_conn_on_file_complete(
                    fc, fio_results[fi].result, fio_results[fi].zero_copy);
                if (fstate == KL_CONN_SENDING) {
                    if (fc->file_io_phase == 1) {
                        /* FILE_IO_READING — async read pending, no WRITE reg */
                    } else {
                        kl_event_mod(&s->ev.loop, fc->fd,
                                     KL_EVENT_WRITE, fc);
                    }
                } else if (fstate == KL_CONN_READING) {
                    kl_event_mod(&s->ev.loop, fc->fd, KL_EVENT_READ, fc);
                } else if (fstate == KL_CONN_CLOSED) {
                    kl_event_del(&s->ev.loop, fc->fd);
                    kl_server_conn_release(s, fc);
                }
            }
        }

        for (int i = 0; i < n; i++) {
            /* Watcher dispatch (tagged pointer, LSB=1) */
            if (kl_event_dispatch(&s->ev, &events[i]))
                continue;

            KlConn *c = (KlConn *)events[i].udata;

            if (c == NULL) {
                /* Listen socket — accept new connections */
                if (atomic_load(&s->draining)) goto rearm_listen;
                while (1) {
                    struct sockaddr_storage peer;
                    socklen_t peer_len = sizeof(peer);
                    int client_fd = kl_sock_accept(s->ev.sockets, s->listen_fd,
                                           (struct sockaddr *)&peer, &peer_len);
                    if (client_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        kl_log_errno(s, KL_LOG_ERROR, "accept");
                        break;
                    }

                    if (kl_sock_set_nonblocking(s->ev.sockets, client_fd) < 0) {
                        close(client_fd);
                        continue;
                    }
                    /* Don't leak client connections into child processes. */
                    kl_sock_set_cloexec(s->ev.sockets, client_fd);
                    if (s->config.transport == KL_TRANSPORT_TCP) {
                        int nodelay = 1;
                        (void)setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY,
                                         &nodelay, sizeof(nodelay));
                    }

                    KlConn *nc = kl_conn_acquire(&s->pool, client_fd);
                    if (!nc) {
                        close(client_fd);
                        /* Pool full — stop accepting until a slot frees up.
                         * The kernel TCP backlog queues further connections. */
                        kl_event_del(&s->ev.loop, s->listen_fd);
                        s->listen_paused = 1;
                        break;
                    }

                    /* Record the client address for kl_request_peer_addr(). */
                    nc->peer_source = KL_PEER_SOCKET;
                    if (peer_len > 0 && peer_len <= sizeof(nc->peer_addr)) {
                        memcpy(&nc->peer_addr, &peer, peer_len);
                        nc->peer_addr_len = peer_len;
                    } else {
                        nc->peer_addr_len = 0;
                    }

                    /* Set allocator on connection's response.
                     * This is set once on accept; response reuses it across keep-alive. */
                    nc->res.alloc = alloc;

                    /* TLS: enter handshake state instead of reading */
                    if (s->config.tls) {
                        nc->state = KL_CONN_TLS_HANDSHAKE;
                        nc->tls_want = KL_EVENT_READ;
                    }

                    /* PROXY protocol: from a trusted source, read the header
                     * first (before TLS/HTTP). Overrides the state above. */
                    if (s->proxy_cidr_count > 0 && peer_len > 0 &&
                        kl_cidr_match(s->proxy_cidrs, s->proxy_cidr_count,
                                      (struct sockaddr *)&peer)) {
                        nc->state = KL_CONN_PROXY_HEADER;
                    }

                    if (kl_event_add(&s->ev.loop, client_fd,
                                     KL_EVENT_READ, nc) < 0) {
                        kl_server_conn_release(s,nc);
                        continue;
                    }
                }
                /* Re-arm listen socket (no-op for persistent backends;
                 * required for io_uring's one-shot POLL_ADD) */
rearm_listen:
                if (!s->listen_paused)
                    kl_event_mod(&s->ev.loop, s->listen_fd,
                                 KL_EVENT_READ, NULL);
                continue;
            }

            /* Client connection event */
            KlConnState new_state = c->state;

            /* PROXY protocol header — read before TLS/HTTP */
            if (c->state == KL_CONN_PROXY_HEADER) {
                int pr = kl_conn_read_proxy_header(c);
                if (pr < 0) {
                    new_state = KL_CONN_CLOSED;
                    goto transition;
                }
                if (pr == 0) {
                    new_state = KL_CONN_PROXY_HEADER;   /* need more bytes */
                    goto transition;
                }
                /* Done — advance to the real initial state. */
                if (s->config.tls) {
                    c->tls_want = KL_EVENT_READ;
                    c->state = KL_CONN_TLS_HANDSHAKE;
                } else {
                    c->state = KL_CONN_READING;
                }
                new_state = c->state;
                /* Process any bytes already buffered after the header this
                 * tick; if none are pending, wait for the next event (edge-
                 * triggered backends don't re-deliver the consumed readiness,
                 * but newly-arriving bytes always trigger). */
                {
                    uint8_t probe;
                    if (recv(c->fd, &probe, 1, MSG_PEEK) <= 0)
                        goto transition;
                }
            }

            /* TLS handshake — handle before normal read/write */
            if (c->state == KL_CONN_TLS_HANDSHAKE) {
                new_state = kl_conn_on_handshake(c);
                goto transition;
            }

            /* WebSocket — handle read/write events */
            if (c->state == KL_CONN_WEBSOCKET) {
                if (events[i].ready & KL_EVENT_READ)
                    new_state = (KlConnState)kl_ws_server_on_readable(c);
                if (new_state == KL_CONN_WEBSOCKET &&
                    (events[i].ready & KL_EVENT_WRITE))
                    new_state = (KlConnState)kl_ws_server_on_writable(c);
                goto transition;
            }

            /* HTTP/2 — handle read/write events */
            if (c->state == KL_CONN_HTTP2) {
                if (events[i].ready & KL_EVENT_READ)
                    new_state = (KlConnState)kl_h2_server_on_readable(c);
                if (new_state == KL_CONN_HTTP2 &&
                    (events[i].ready & KL_EVENT_WRITE))
                    new_state = (KlConnState)kl_h2_server_on_writable(c);
                goto transition;
            }

            if (events[i].ready & KL_EVENT_READ) {
                new_state = kl_conn_on_readable(c, &s->router);
            }

            if (events[i].ready & KL_EVENT_WRITE) {
                if (new_state == KL_CONN_SENDING || c->state == KL_CONN_SENDING)
                    new_state = kl_conn_on_writable(c);
            }

            /* Try immediate send after read→parse→handle */
            if (new_state == KL_CONN_SENDING) {
                new_state = kl_conn_on_writable(c);
            }

transition:
            /* Transition */
            if (new_state == KL_CONN_TLS_HANDSHAKE) {
                if (kl_event_mod(&s->ev.loop, c->fd,
                                 (KlEventMask)c->tls_want, c) < 0) {
                    kl_event_del(&s->ev.loop, c->fd);
                    kl_server_conn_release(s,c);
                }
            } else if (new_state == KL_CONN_SENDING) {
                if (c->file_io_phase == 1) {
                    /* FILE_IO_READING — async read pending, no WRITE event */
                } else if (kl_event_mod(&s->ev.loop, c->fd,
                                 KL_EVENT_WRITE, c) < 0) {
                    kl_event_del(&s->ev.loop, c->fd);
                    kl_server_conn_release(s,c);
                }
            } else if (new_state == KL_CONN_WEBSOCKET) {
                KlEventMask ws_mask = KL_EVENT_READ;
                if (kl_ws_server_drain_pending(c))
                    ws_mask = (KlEventMask)(KL_EVENT_READ | KL_EVENT_WRITE);
                if (kl_event_mod(&s->ev.loop, c->fd, ws_mask, c) < 0) {
                    kl_event_del(&s->ev.loop, c->fd);
                    kl_server_conn_release(s,c);
                }
            } else if (new_state == KL_CONN_HTTP2) {
                KlEventMask mask = KL_EVENT_READ;
                if (c->h2 && c->h2->session &&
                    c->h2->session->want_write(c->h2->session))
                    mask = (KlEventMask)(KL_EVENT_READ | KL_EVENT_WRITE);
                if (kl_event_mod(&s->ev.loop, c->fd, mask, c) < 0) {
                    kl_event_del(&s->ev.loop, c->fd);
                    kl_server_conn_release(s,c);
                }
            } else if (new_state == KL_CONN_READING ||
                       new_state == KL_CONN_READING_BODY ||
                       new_state == KL_CONN_PROXY_HEADER) {
                if (kl_event_mod(&s->ev.loop, c->fd,
                                 KL_EVENT_READ, c) < 0) {
                    kl_event_del(&s->ev.loop, c->fd);
                    kl_server_conn_release(s,c);
                }
            } else if (new_state == KL_CONN_SUSPENDED) {
                /* Handler suspended for async I/O — FD already removed
                 * from event loop by kl_async_suspend. */
            } else if (new_state == KL_CONN_CLOSED) {
                kl_event_del(&s->ev.loop, c->fd);
                kl_server_conn_release(s,c);
            }
        }

        /* Sweep for timed-out connections.
         * Single-threaded: no TOCTOU risk — all event processing above is
         * complete, so connection states are stable.  Newly acquired slots
         * have fresh last_active_ms and won't be timed out. */
        now = kl_monotonic_ms();
        uint64_t timeout = (uint64_t)s->config.read_timeout_ms;
        uint64_t body_timeout = s->config.body_timeout_ms > 0
                                ? (uint64_t)s->config.body_timeout_ms
                                : timeout;
        for (int i = 0; i < s->pool.capacity; i++) {
            KlConn *tc = &s->pool.conns[i];
            if (tc->state == KL_CONN_CLOSED || tc->state == KL_CONN_PROCESSING)
                continue;
            /* Suspended: exempt from idle timeout — has its own deadline */
            if (tc->state == KL_CONN_SUSPENDED)
                continue;
            /* WebSocket: exempt from HTTP idle timeout, check close deadline */
            if (tc->state == KL_CONN_WEBSOCKET) {
                kl_ws_server_auto_ping(tc, now);
                if (kl_ws_server_check_close_timeout(tc, now)) {
                    kl_event_del(&s->ev.loop, tc->fd);
                    kl_server_conn_release(s,tc);
                }
                continue;
            }
            /* HTTP/2: PING keepalive is session's responsibility */
            if (tc->state == KL_CONN_HTTP2) {
                continue;
            }
            /* TLS handshake time counts against read timeout */
            int timed_out = (now - tc->last_active_ms > timeout);
            /* Body deadline: absolute time from body start, not resettable.
             * Catches slow-chunk attacks where 1 byte resets idle timer. */
            if (!timed_out && tc->state == KL_CONN_READING_BODY &&
                tc->body_start_ms > 0 &&
                now - tc->body_start_ms > body_timeout) {
                timed_out = 1;
            }
            if (timed_out) {
                /* Cancel pending async file read before release */
                if (tc->file_io_phase == 1 && tc->file_io) {
                    tc->file_io->cancel(tc->file_io, tc->fd);
                    tc->file_io_phase = 3;  /* FILE_IO_CANCELLING */
                    continue;  /* wait for cancel CQE in next tick */
                }
                if (tc->file_io_phase == 3)
                    continue;  /* FILE_IO_CANCELLING — still waiting */
                /* Best-effort 408: small write to non-blocking socket
                 * will almost always succeed in one call.
                 * Skip for TLS handshake — no HTTP framing yet. */
                if (tc->state == KL_CONN_READING ||
                    tc->state == KL_CONN_READING_BODY)
                    best_effort_conn_write(tc, kl_408_response,
                                           sizeof(kl_408_response) - 1);
                kl_event_del(&s->ev.loop, tc->fd);
                kl_server_conn_release(s,tc);
            }
        }

        /* Sweep async op deadlines */
        {
            KlAsyncOp *aop = s->async_ops;
            while (aop) {
                KlAsyncOp *next_aop = aop->next;
                if (aop->deadline_ms > 0 && now >= aop->deadline_ms) {
                    if (aop->on_deadline)
                        aop->on_deadline(aop, aop->user_data);
                }
                aop = next_aop;
            }
        }

        /* Fire expired timers */
        kl_timer_fire(&s->ev);

        /* Graceful drain: stop when all connections are idle or deadline hit */
        if (atomic_load(&s->draining)) {
            /* Send close 1001 to active WebSocket connections */
            for (int j = 0; j < s->pool.capacity; j++) {
                if (s->pool.conns[j].state == KL_CONN_WEBSOCKET)
                    kl_ws_server_drain_close(&s->pool.conns[j]);
                if (s->pool.conns[j].state == KL_CONN_HTTP2)
                    kl_h2_server_drain_shutdown(&s->pool.conns[j]);
            }
            int active = 0;
            for (int j = 0; j < s->pool.capacity; j++) {
                if (s->pool.conns[j].state != KL_CONN_CLOSED)
                    active++;
            }
            if (active == 0 || now >= s->drain_deadline_ms) {
                atomic_store(&s->running, 0);
            }
        }
    }

#if !defined(KL_NO_SIGNAL)
    /* Restore signal handlers */
    if (s->config.install_signal_handlers) {
        sigaction(SIGTERM, &old_term, NULL);
        sigaction(SIGINT, &old_int, NULL);
        atomic_store(&kl_signal_server, (KlServer *)NULL);
    }
#endif

    return 0;
}

void kl_server_stop(KlServer *s) {
    if (s->config.drain_timeout_ms > 0 && !atomic_load(&s->draining)) {
        /* Enter drain mode: stop accepting, let in-flight finish */
        atomic_store(&s->draining, 1);
        s->drain_deadline_ms = kl_monotonic_ms() +
                               (uint64_t)s->config.drain_timeout_ms;
    } else {
        atomic_store(&s->running, 0);
    }
}

void kl_server_stats(const KlServer *s, KlServerStats *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!s) return;

    out->active_connections = s->pool.active_count;
    out->max_connections    = s->pool.capacity;
    out->listen_paused      = s->listen_paused;

    /* Count suspended connections by walking the async ops list */
    int suspended = 0;
    for (const KlAsyncOp *op = s->async_ops; op; op = op->next)
        suspended++;
    out->async_suspended = suspended;
}

void kl_server_free(KlServer *s) {
    /* Cancel all active async ops */
    while (s->async_ops) {
        KlAsyncOp *op = s->async_ops;
        s->async_ops = op->next;
        if (op->on_cancel)
            op->on_cancel(op, op->user_data);
        if (op->conn)
            op->conn->async_op = NULL;
    }

    if (s->file_io) {
        s->file_io->destroy(s->file_io);
        s->file_io = NULL;
    }
    kl_server_close_listener(s);
    kl_event_ctx_free(&s->ev);
    if (s->proxy_cidrs) {
        kl_free(&s->alloc_storage, s->proxy_cidrs,
                (size_t)s->proxy_cidr_count * sizeof(KlCidr));
        s->proxy_cidrs = NULL;
        s->proxy_cidr_count = 0;
    }
    kl_conn_pool_free(&s->pool);
    kl_router_free(&s->router);
    if (s->config.tls && s->config.tls->ctx_destroy) {
        s->config.tls->ctx_destroy(s->config.tls->ctx);
    }
    if (s->config.compress && s->config.compress->ctx_destroy) {
        s->config.compress->ctx_destroy(s->config.compress->ctx);
    }
}
