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
#include <sys/socket.h>
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
#include <pthread.h>
#include <sched.h>   /* sched_yield */
#include <time.h>    /* nanosleep */
#endif

/* ── Signal handling ─────────────────────────────────────────────────── */

#if !defined(KL_NO_SIGNAL)
/* A process-directed SIGTERM/SIGINT runs the handler on any thread that does not block it, so the
 * loop thread's restore cannot tell whether a handler elsewhere has already loaded the server.
 * Every run of the handler counts itself in kl_signal_inflight BEFORE it loads the pointer, and
 * leaves after its kl_http_server_stop. The restore clears the pointer first and then waits for the
 * count to reach zero. All of these are sequentially consistent, so a handler that loaded the
 * server had counted itself before the pointer was cleared, and the restore's wait sees it: it
 * returns only after that handler is done with the server, and the caller may then free it. A
 * handler that loads after the clear sees NULL (or a later server's pointer) and touches nothing.
 * The handler never waits, so a signal that interrupts the restoring thread itself runs to
 * completion before the wait resumes. The count is process-wide: a restore also waits out handlers
 * stopping another server, each a few microseconds. A process that forks while a handler is running
 * on another thread leaves the child a count that never drains; fork from a thread that is not in a
 * handler (as POSIX already requires for the mutex below).
 *
 * Both objects must be lock-free for the handler to use them: 0 fails the build here; 1 (object
 * dependent) is asked at runtime in the install, which then leaves the handlers uninstalled. */
#if ATOMIC_INT_LOCK_FREE == 0 || ATOMIC_POINTER_LOCK_FREE == 0
#error "KEEL's signal handler needs lock-free int and pointer atomics"
#endif
static _Atomic(KlHttpServer *) kl_signal_server   = NULL;
static atomic_int              kl_signal_inflight = 0;

/* Install and restore run on loop threads, possibly two servers' at once; the handler never takes
 * this lock. The dispositions in place before the first server installed are saved once and put
 * back when the server currently receiving signals (the last to install) restores. Only that server
 * receives signals: if it returns while an earlier one still runs, the earlier one gets none. */
static pthread_mutex_t     kl_signal_lock = PTHREAD_MUTEX_INITIALIZER;
static int                 kl_signal_saved = 0;
static struct sigaction    kl_old_term, kl_old_int;

#ifdef KEEL_HTTP_SERVER_PLAT_TEST_HOOKS
/* Test-only: called by the handler after it has loaded the server, before it stops it. */
void (*kl_http_server_test_signal_loaded)(void) = NULL;
#endif

static void kl_signal_handler(int sig) {
    (void)sig;
    int saved_errno = errno;   /* the wakeup write may set it (EAGAIN on a full pipe): keep the
                                * interrupted code's errno intact */
    atomic_fetch_add(&kl_signal_inflight, 1);
    KlHttpServer *s = atomic_load(&kl_signal_server);
#ifdef KEEL_HTTP_SERVER_PLAT_TEST_HOOKS
    if (s && kl_http_server_test_signal_loaded) kl_http_server_test_signal_loaded();
#endif
    if (s) kl_http_server_stop(s);
    atomic_fetch_sub(&kl_signal_inflight, 1);
    errno = saved_errno;
}

#ifdef KEEL_HTTP_SERVER_PLAT_TEST_HOOKS
/* Test-only: the handler itself, to run on a chosen thread without a signal. */
void kl_http_server_test_signal_handler(int sig) { kl_signal_handler(sig); }
#endif
#endif

void kl_http_server_plat_signals_install(KlHttpServer *s) {
#if !defined(KL_NO_SIGNAL)
    signal(SIGPIPE, SIG_IGN);
    if (s->config.install_signal_handlers) {
        if (!atomic_is_lock_free(&kl_signal_server) || !atomic_is_lock_free(&kl_signal_inflight)) {
            kl_http_server_log(s, KL_HTTP_SERVER_LOG_ERROR,
                               "signal handlers not installed: atomics are not lock-free here");
            return;
        }
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = kl_signal_handler;
        sa.sa_flags = 0;
        sigemptyset(&sa.sa_mask);
        pthread_mutex_lock(&kl_signal_lock);
        atomic_store(&kl_signal_server, s);
        if (!kl_signal_saved) {
            sigaction(SIGTERM, &sa, &kl_old_term);
            sigaction(SIGINT, &sa, &kl_old_int);
            kl_signal_saved = 1;
        } else {
            sigaction(SIGTERM, &sa, NULL);   /* still the first server's saved dispositions */
            sigaction(SIGINT, &sa, NULL);
        }
        pthread_mutex_unlock(&kl_signal_lock);
    }
#else
    (void)s;
#endif
}

/* cppcheck-suppress constParameterPointer ; symmetric with kl_http_server_plat_signals_install */
void kl_http_server_plat_signals_restore(KlHttpServer *s) {
#if !defined(KL_NO_SIGNAL)
    if (s->config.install_signal_handlers) {
        pthread_mutex_lock(&kl_signal_lock);
        /* Only the server receiving signals puts the old dispositions back: one that installed
         * earlier and was superseded leaves the later server its signals. */
        if (atomic_load(&kl_signal_server) == s) {
            sigaction(SIGTERM, &kl_old_term, NULL);
            sigaction(SIGINT, &kl_old_int, NULL);
            kl_signal_saved = 0;
            atomic_store(&kl_signal_server, (KlHttpServer *)NULL);
        }
        pthread_mutex_unlock(&kl_signal_lock);
        /* Whether or not it was still installed, a handler may have loaded this server before the
         * pointer moved on: wait for every run in progress. */
        /* Yield first; then sleep, so a real-time restoring thread cannot starve a preempted
         * handler on its CPU. A handler runs for microseconds, so the wait is short. */
        for (int spins = 0; atomic_load(&kl_signal_inflight) != 0; spins++) {
            if (spins < 64) { sched_yield(); continue; }
            struct timespec ts = { 0, 100000 };   /* 100 us */
            nanosleep(&ts, NULL);
        }
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

/* Peer credentials exist only for a UNIX-domain socket. Querying any other socket is not an error
 * the OS reports reliably: on Linux SO_PEERCRED of a TCP socket succeeds with the kernel's
 * "no credentials" values, and on macOS getpeereid() of a TCP socket succeeds with uid 0 and gid 0
 * (LOCAL_PEERCRED is read at level SOL_LOCAL, which equals IPPROTO_IP). Check the family first, so a
 * remote peer is never reported as a local identity. */
static int peer_is_unix(int fd) {
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    memset(&ss, 0, sizeof(ss));
    if (getsockname(fd, (struct sockaddr *)&ss, &len) != 0)
        return 0;
    return ss.ss_family == AF_UNIX;
}

int kl_http_server_plat_peer_cred_readable(const KlSocketProvider *p) {
    /* Identity of the ops table, not the NATIVE_FD capability: lwIP's BSD provider advertises
     * NATIVE_FD for its own (lwIP-numbered) sockets, and a decorator may route I/O elsewhere. */
    return !p || (p->ops == kl_socket_provider_posix()->ops &&
                  kl_socket_provider_has_cap(p, KL_SOCK_CAP_NATIVE_FD));
}

/* cppcheck-suppress constParameterPointer  ; 'out' is written on the
   platform branches below (invisible to cppcheck's default config). */
int kl_peer_cred_fd(KlSocketHandle fd, KlPeerCred *out) {
    if (!kl_handle_valid(fd) || !out)
        return -1;
    if (!peer_is_unix((int)fd))
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
