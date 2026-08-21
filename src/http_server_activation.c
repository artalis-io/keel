/*
 * http_server_activation.c — socket-activation fd inheritance (systemd LISTEN_* protocol).
 *
 * Extracted from http_server.c (Finding 6: http_server.c bundled several unrelated responsibilities)
 * so the systemd socket-activation surface is its own TU, separate from the readiness run
 * loop, TCP listener construction, and peer accessors. Parses the LISTEN_PID / LISTEN_FDS /
 * LISTEN_FDNAMES environment the service manager sets, validates it belongs to THIS process,
 * and returns the inherited listen fd (SD_LISTEN_FDS_START = 3). The env vars are cleared so
 * they are not inherited by children. Hosted-only; getpid()/getenv() come via internal.h's
 * <unistd.h> (the same chain http_server.c used), so cross-platform behavior is unchanged.
 */

#include <keel/http_server.h>
#include "internal.h"      /* <unistd.h> (getpid) on a hosted build */
#include "http_server_plat.h"   /* kl_http_server_plat_unsetenv */
#include <stdlib.h>        /* getenv, strtol */
#include <string.h>        /* strchr, strlen, memcmp */

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
    kl_http_server_plat_unsetenv("LISTEN_PID");
    kl_http_server_plat_unsetenv("LISTEN_FDS");
    kl_http_server_plat_unsetenv("LISTEN_FDNAMES");
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
        /* LISTEN_FDNAMES is a colon-separated list, one name per passed fd in fd order
         * starting at SD_LISTEN_FDS_START (3). */
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

    kl_http_server_plat_unsetenv("LISTEN_PID");
    kl_http_server_plat_unsetenv("LISTEN_FDS");
    kl_http_server_plat_unsetenv("LISTEN_FDNAMES");
    return result;
}
