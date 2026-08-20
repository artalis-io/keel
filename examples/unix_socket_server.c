/*
 * unix_socket_server.c — Serving HTTP over a UNIX domain socket
 *
 * Concepts: KL_TRANSPORT_UNIX, socket activation (inherited fd via
 * kl_systemd_listen_fd), and peer-credential access (kl_http_request_peer_cred).
 *
 * Build:  make examples
 *
 * Run (bind a socket path):
 *     ./examples/unix_socket_server /tmp/keel.sock
 *     curl --unix-socket /tmp/keel.sock http://localhost/whoami
 *
 * Run (socket activation — systemd/launchd/inherited fd 3):
 *     systemd-socket-activate -l /tmp/keel.sock ./examples/unix_socket_server
 *     curl --unix-socket /tmp/keel.sock http://localhost/whoami
 */

#include <keel/keel.h>
#include <stdio.h>
#include <string.h>

/* Report the connecting peer's credentials (UNIX-socket only). */
static void handle_whoami(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)ctx;
    KlPeerCred cred;
    char body[160];
    int n;

    if (kl_http_request_peer_cred(req, &cred) == 0) {
        if (cred.has_pid)
            n = snprintf(body, sizeof(body),
                         "{\"uid\":%ld,\"gid\":%ld,\"pid\":%ld}",
                         cred.uid, cred.gid, cred.pid);
        else
            n = snprintf(body, sizeof(body),
                         "{\"uid\":%ld,\"gid\":%ld}", cred.uid, cred.gid);
    } else {
        n = snprintf(body, sizeof(body),
                     "{\"error\":\"peer credentials unavailable\"}");
    }
    kl_http_response_json(res, 200, body, (size_t)n);
}

int main(int argc, char **argv) {
    KlServer s;
    KlConfig cfg = {
        .max_connections = 64,
        .install_signal_handlers = 1,
    };

    /* Prefer an inherited listening fd (systemd/launchd socket activation);
     * otherwise bind the socket path given on the command line. */
    int activated_fd = kl_systemd_listen_fd();
    if (activated_fd > 0) {
        cfg.listen_fd = activated_fd;
    } else if (argc > 1) {
        cfg.unix_socket_path = argv[1];
        cfg.unix_socket_unlink = 1;   /* replace a stale socket on restart */
        cfg.unix_socket_mode = 0660;  /* owner+group only */
    } else {
        fprintf(stderr,
                "usage: %s <socket-path>   (or run under socket activation)\n",
                argv[0]);
        return 1;
    }

    if (kl_server_init(&s, &cfg) < 0) {
        fprintf(stderr, "init failed: %s\n", kl_strerror(s.last_error));
        return 1;
    }
    kl_server_route(&s, "GET", "/whoami", handle_whoami, NULL, NULL);

    if (activated_fd > 0)
        printf("unix_socket_server listening on inherited fd %d\n", activated_fd);
    else
        printf("unix_socket_server listening on unix:%s\n  curl --unix-socket %s http://localhost/whoami\n",
               argv[1], argv[1]);

    kl_server_run(&s);
    kl_server_free(&s);
    return 0;
}
