#include "utest.h"
#include <keel/keel.h>
#include "mock_tls.h"   /* shared completion-capable identity TLS mock */
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <grp.h>

/* Peer-credential capture (set inside the handler). */
static KlPeerCred g_captured_cred;
static int        g_cred_rc = -2;
static char       g_captured_label[256];
static int        g_label_rc = -2;
static int        g_addr_rc = -2;

static void unix_handle_whoami(KlRequest *req, KlResponse *res, void *ctx) {
    (void)ctx;
    char ip[64];
    uint16_t port = 0;
    g_cred_rc = kl_request_peer_cred(req, &g_captured_cred);
    g_label_rc = kl_request_peer_label(req, g_captured_label,
                                       sizeof(g_captured_label));
    g_addr_rc = kl_request_peer_addr(req, ip, sizeof(ip), &port);
    kl_response_json(res, 200, "{\"ok\":true}", 11);
}

/* WS server-side peer-cred capture (set in on_open, post-upgrade). */
static KlPeerCred g_ws_srv_cred;
static int        g_ws_srv_cred_rc = -2;

static void ws_srv_on_open(KlWsServerConn *ws, void *ctx) {
    (void)ctx;
    g_ws_srv_cred_rc = kl_ws_server_peer_cred(ws, &g_ws_srv_cred);
}

/* Percent-encode a socket path for an http+unix:// URL (encode everything
 * that is not an RFC 3986 unreserved character). */
static void pct_encode_path(const char *src, char *dst, size_t dstsz) {
    static const char *hex = "0123456789ABCDEF";
    size_t di = 0;
    for (size_t i = 0; src[i] && di + 4 < dstsz; i++) {
        unsigned char c = (unsigned char)src[i];
        int unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                         (c >= '0' && c <= '9') ||
                         c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved) {
            dst[di++] = (char)c;
        } else {
            dst[di++] = '%';
            dst[di++] = hex[c >> 4];
            dst[di++] = hex[c & 0xF];
        }
    }
    dst[di] = '\0';
}

/* Minimal async-client harness. */
typedef struct {
    int    done;
    int    error;
    int    status;
    char   body[256];
    size_t body_len;
} UnixAsyncCtx;

static void unix_async_done(KlClient *client, void *user_data) {
    UnixAsyncCtx *ctx = user_data;
    ctx->error = kl_client_error(client);
    if (ctx->error == 0) {
        const KlClientResponse *resp = kl_client_response(client);
        if (resp) {
            ctx->status = resp->status;
            if (resp->body && resp->body_len > 0) {
                size_t n = resp->body_len < sizeof(ctx->body) - 1
                         ? resp->body_len : sizeof(ctx->body) - 1;
                memcpy(ctx->body, resp->body, n);
                ctx->body_len = n;
            }
        }
    }
    ctx->done = 1;
}

static int unix_run_until_done(KlEventCtx *ev, UnixAsyncCtx *ctx, int timeout_ms) {
    int elapsed = 0;
    while (!ctx->done && elapsed < timeout_ms) {
        if (kl_event_ctx_run(ev, 16, 50) < 0) return -1;
        elapsed += 50;
    }
    return ctx->done ? 0 : -1;
}

static void unix_handle_hello(KlRequest *req, KlResponse *res, void *ctx) {
    (void)req;
    (void)ctx;
    kl_response_json(res, 200, "{\"ok\":true}", 11);
}

static void *unix_server_thread(void *arg) {
    kl_server_run((KlServer *)arg);
    return NULL;
}

/* 302 → relative /hello (redirect-over-unix). */
static void unix_handle_redirect(KlRequest *req, KlResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_response_status(res, 302);
    kl_response_header(res, "Location", "/hello");
    kl_response_body_borrow(res, "moved", 5);
}

/* WebSocket echo callbacks (server side). */
static void ws_srv_on_message(KlWsServerConn *ws, const char *data, size_t len,
                              int is_binary, void *ctx) {
    (void)is_binary; (void)ctx;
    kl_ws_server_send_text(ws, data, len);
}

/* WebSocket client context. */
typedef struct {
    int  open;
    int  got_msg;
    int  closed;
    char msg[128];
    size_t msg_len;
} WsCliCtx;

static void ws_cli_on_open(KlWsClientConn *ws, void *ud) {
    WsCliCtx *c = ud;
    c->open = 1;
    kl_ws_client_send_text(ws, "ping", 4);
}
static void ws_cli_on_message(KlWsClientConn *ws, const char *data, size_t len,
                              int is_binary, void *ud) {
    (void)ws; (void)is_binary;
    WsCliCtx *c = ud;
    size_t n = len < sizeof(c->msg) - 1 ? len : sizeof(c->msg) - 1;
    memcpy(c->msg, data, n);
    c->msg[n] = '\0';
    c->msg_len = n;
    c->got_msg = 1;
}
static void ws_cli_on_close(KlWsClientConn *ws, uint16_t code, const char *r,
                            size_t rlen, void *ud) {
    (void)ws; (void)code; (void)r; (void)rlen;
    ((WsCliCtx *)ud)->closed = 1;
}

/* Passthrough TLS: routes I/O through the KlTls vtable without encryption, so the full
 * https+unix code path (handshake state, SNI, TLS read/write) is exercised without a real
 * crypto backend. The mock is the shared tests/mock_tls.h (mock_tls_create), which implements
 * the completion-mode ops (feed_input/drain_output) too, so tls_over_https_unix runs over the
 * completion backend as well as readiness. */

static void test_sock_path(char *buf, size_t buflen, const char *name) {
    snprintf(buf, buflen, "./keel-%ld-%s.sock", (long)getpid(), name);
}

static socklen_t unix_addr(struct sockaddr_un *addr, const char *path) {
    memset(addr, 0, sizeof(*addr));
    addr->sun_family = AF_UNIX;
    size_t path_len = strlen(path);
    if (path_len >= sizeof(addr->sun_path))
        return 0;
    memcpy(addr->sun_path, path, path_len + 1);
    return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + path_len + 1);
}

static int create_bound_unix_socket(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_un addr;
    socklen_t addr_len = unix_addr(&addr, path);
    if (addr_len == 0 || bind(fd, (struct sockaddr *)&addr, addr_len) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static int connect_unix_retry(const char *path, int attempts) {
    for (int i = 0; i < attempts; i++) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0)
            return -1;

        struct sockaddr_un addr;
        socklen_t addr_len = unix_addr(&addr, path);
        if (addr_len == 0) {
            close(fd);
            return -1;
        }

        if (connect(fd, (struct sockaddr *)&addr, addr_len) == 0)
            return fd;

        close(fd);
        usleep(10000);
    }
    return -1;
}

static ssize_t read_unix_response(int fd, char *buf, size_t buflen) {
    ssize_t total = 0;
    struct pollfd pfd = { .fd = fd, .events = POLLIN };

    while (total < (ssize_t)buflen - 1) {
        int pr = poll(&pfd, 1, 2000);
        if (pr <= 0)
            break;
        ssize_t n = read(fd, buf + total, buflen - (size_t)total - 1);
        if (n <= 0)
            break;
        total += n;
    }
    buf[total] = '\0';
    return total;
}

UTEST(unix_socket, serves_http_over_unix_stream_socket) {
    char path[108];
    test_sock_path(path, sizeof(path), "serve");
    unlink(path);

    KlServer srv;
    KlConfig cfg = {
        .unix_socket_path = path,
        .unix_socket_unlink = 1,
        .max_connections = 8,
    };
    ASSERT_EQ(0, kl_server_init(&srv, &cfg));
    ASSERT_EQ(srv.config.transport, KL_TRANSPORT_UNIX);
    kl_server_route(&srv, "GET", "/hello", unix_handle_hello, NULL, NULL);

    pthread_t tid;
    ASSERT_EQ(0, pthread_create(&tid, NULL, unix_server_thread, &srv));

    int fd = connect_unix_retry(path, 200);
    ASSERT_TRUE(fd >= 0);

    const char *req =
        "GET /hello HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n"
        "\r\n";
    ASSERT_EQ((ssize_t)strlen(req), write(fd, req, strlen(req)));

    char buf[1024];
    ssize_t n = read_unix_response(fd, buf, sizeof(buf));
    ASSERT_TRUE(n > 0);
    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);
    ASSERT_TRUE(strstr(buf, "{\"ok\":true}") != NULL);
    close(fd);

    kl_server_stop(&srv);
    pthread_join(tid, NULL);
    kl_server_free(&srv);
    ASSERT_NE(0, access(path, F_OK));
}

UTEST(unix_socket, existing_path_fails_without_unlink) {
    char path[108];
    test_sock_path(path, sizeof(path), "stale");
    unlink(path);

    FILE *f = fopen(path, "w");
    ASSERT_TRUE(f != NULL);
    fclose(f);

    KlServer srv;
    KlConfig cfg = {
        .transport = KL_TRANSPORT_UNIX,
        .unix_socket_path = path,
        .unix_socket_unlink = 0,
    };
    ASSERT_EQ(0, kl_server_init(&srv, &cfg));
    ASSERT_EQ(-1, kl_server_run(&srv));
    ASSERT_EQ(srv.last_error, KL_ERR_BIND);
    kl_server_free(&srv);

    unlink(path);
}

UTEST(unix_socket, unlink_option_rejects_non_socket_path) {
    char path[108];
    test_sock_path(path, sizeof(path), "regular");
    unlink(path);

    FILE *f = fopen(path, "w");
    ASSERT_TRUE(f != NULL);
    fclose(f);

    KlServer srv;
    KlConfig cfg = {
        .transport = KL_TRANSPORT_UNIX,
        .unix_socket_path = path,
        .unix_socket_unlink = 1,
    };
    ASSERT_EQ(0, kl_server_init(&srv, &cfg));
    ASSERT_EQ(-1, kl_server_run(&srv));
    ASSERT_EQ(srv.last_error, KL_ERR_BIND);
    kl_server_free(&srv);
    ASSERT_EQ(0, access(path, F_OK));
    unlink(path);
}

UTEST(unix_socket, unlink_option_replaces_stale_socket_path) {
    char path[108];
    test_sock_path(path, sizeof(path), "replace");
    unlink(path);

    int stale_fd = create_bound_unix_socket(path);
    ASSERT_TRUE(stale_fd >= 0);
    close(stale_fd);

    KlServer srv;
    KlConfig cfg = {
        .transport = KL_TRANSPORT_UNIX,
        .unix_socket_path = path,
        .unix_socket_unlink = 1,
    };
    ASSERT_EQ(0, kl_server_init(&srv, &cfg));
    kl_server_route(&srv, "GET", "/hello", unix_handle_hello, NULL, NULL);

    pthread_t tid;
    ASSERT_EQ(0, pthread_create(&tid, NULL, unix_server_thread, &srv));

    int fd = connect_unix_retry(path, 200);
    ASSERT_TRUE(fd >= 0);
    close(fd);

    kl_server_stop(&srv);
    pthread_join(tid, NULL);
    kl_server_free(&srv);
    ASSERT_NE(0, access(path, F_OK));
}

UTEST(unix_socket, too_long_path_is_invalid) {
    char path[200];
    memset(path, 'a', sizeof(path));
    path[0] = '/';
    path[sizeof(path) - 1] = '\0';

    KlServer srv;
    KlConfig cfg = {
        .transport = KL_TRANSPORT_UNIX,
        .unix_socket_path = path,
    };
    ASSERT_EQ(0, kl_server_init(&srv, &cfg));
    ASSERT_EQ(-1, kl_server_run(&srv));
    ASSERT_EQ(srv.last_error, KL_ERR_INVALID_ARG);
    kl_server_free(&srv);
}

UTEST(unix_socket, socket_mode_is_applied) {
    char path[108];
    test_sock_path(path, sizeof(path), "mode");
    unlink(path);

    KlServer srv;
    KlConfig cfg = {
        .unix_socket_path = path,
        .unix_socket_unlink = 1,
        .unix_socket_mode = 0660,
        .max_connections = 4,
    };
    ASSERT_EQ(0, kl_server_init(&srv, &cfg));

    pthread_t tid;
    ASSERT_EQ(0, pthread_create(&tid, NULL, unix_server_thread, &srv));

    int fd = connect_unix_retry(path, 200);
    ASSERT_TRUE(fd >= 0);
    close(fd);

    struct stat st;
    ASSERT_EQ(0, stat(path, &st));
    ASSERT_EQ((mode_t)0660, (mode_t)(st.st_mode & 0777));

    kl_server_stop(&srv);
    pthread_join(tid, NULL);
    kl_server_free(&srv);
}

UTEST(unix_socket, peer_credentials_available) {
    char path[108];
    test_sock_path(path, sizeof(path), "cred");
    unlink(path);
    g_cred_rc = -2;

    KlServer srv;
    KlConfig cfg = {
        .unix_socket_path = path,
        .unix_socket_unlink = 1,
        .max_connections = 4,
    };
    ASSERT_EQ(0, kl_server_init(&srv, &cfg));
    kl_server_route(&srv, "GET", "/whoami", unix_handle_whoami, NULL, NULL);

    pthread_t tid;
    ASSERT_EQ(0, pthread_create(&tid, NULL, unix_server_thread, &srv));

    int fd = connect_unix_retry(path, 200);
    ASSERT_TRUE(fd >= 0);
    const char *req =
        "GET /whoami HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_EQ((ssize_t)strlen(req), write(fd, req, strlen(req)));
    char buf[1024];
    ASSERT_TRUE(read_unix_response(fd, buf, sizeof(buf)) > 0);
    close(fd);

    kl_server_stop(&srv);
    pthread_join(tid, NULL);
    kl_server_free(&srv);

    ASSERT_EQ(0, g_cred_rc);
    ASSERT_EQ((long)getuid(), g_captured_cred.uid);
    ASSERT_EQ((long)getgid(), g_captured_cred.gid);
    /* A UNIX socket has no IP address — peer_addr reports unavailable. */
    ASSERT_EQ(-1, g_addr_rc);
    /* Peer is this same test process; when a pid is available (Linux
     * SO_PEERCRED, macOS LOCAL_PEERPID) it must be ours. */
    if (g_captured_cred.has_pid)
        ASSERT_EQ((long)getpid(), g_captured_cred.pid);
    /* Peer label is Linux/SELinux-only; smoke-check it doesn't misbehave. */
    ASSERT_TRUE(g_label_rc == 0 || g_label_rc == -1);
}

UTEST(unix_socket, adopts_inherited_listen_fd) {
    char path[108];
    test_sock_path(path, sizeof(path), "adopt");
    unlink(path);

    /* Simulate socket activation: pre-bind + listen, then hand the fd off. */
    int lfd = create_bound_unix_socket(path);
    ASSERT_TRUE(lfd >= 0);
    ASSERT_EQ(0, listen(lfd, 16));

    KlServer srv;
    KlConfig cfg = {
        .listen_fd = lfd,
        .max_connections = 4,
    };
    ASSERT_EQ(0, kl_server_init(&srv, &cfg));
    kl_server_route(&srv, "GET", "/hello", unix_handle_hello, NULL, NULL);

    pthread_t tid;
    ASSERT_EQ(0, pthread_create(&tid, NULL, unix_server_thread, &srv));

    int fd = connect_unix_retry(path, 200);
    ASSERT_TRUE(fd >= 0);
    const char *req =
        "GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_EQ((ssize_t)strlen(req), write(fd, req, strlen(req)));
    char buf[1024];
    ssize_t n = read_unix_response(fd, buf, sizeof(buf));
    ASSERT_TRUE(n > 0);
    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);
    close(fd);

    /* A completed round-trip means kl_server_run finished adopting the fd,
     * so reading the (now-stable) transport here is race-free. */
    ASSERT_EQ(KL_TRANSPORT_UNIX, srv.config.transport);

    kl_server_stop(&srv);
    pthread_join(tid, NULL);
    kl_server_free(&srv);
    /* Adopted socket path is not unlinked by KEEL (supervisor owns it). */
    unlink(path);
}

UTEST(unix_socket, systemd_listen_fd_protocol) {
    char pidbuf[32];
    snprintf(pidbuf, sizeof(pidbuf), "%ld", (long)getpid());

    /* Correct handshake → first fd (SD_LISTEN_FDS_START == 3), count reported. */
    setenv("LISTEN_PID", pidbuf, 1);
    setenv("LISTEN_FDS", "3", 1);
    int n = -1;
    ASSERT_EQ(3, kl_systemd_listen_fds(&n));
    ASSERT_EQ(3, n);

    /* Env is cleared after the call, so a second call reports "no activation". */
    ASSERT_TRUE(getenv("LISTEN_PID") == NULL);
    n = -1;
    ASSERT_EQ(-1, kl_systemd_listen_fds(&n));
    ASSERT_EQ(0, n);

    /* The single-fd convenience wrapper still works. */
    setenv("LISTEN_PID", pidbuf, 1);
    setenv("LISTEN_FDS", "1", 1);
    ASSERT_EQ(3, kl_systemd_listen_fd());

    /* Wrong PID → not for us. */
    setenv("LISTEN_PID", "1", 1);
    setenv("LISTEN_FDS", "1", 1);
    ASSERT_EQ(-1, kl_systemd_listen_fd());
}

UTEST(unix_socket, systemd_listen_fd_by_name) {
    char pidbuf[32];
    snprintf(pidbuf, sizeof(pidbuf), "%ld", (long)getpid());

    /* Three passed sockets named http:https:admin → fds 3,4,5. */
    setenv("LISTEN_PID", pidbuf, 1);
    setenv("LISTEN_FDS", "3", 1);
    setenv("LISTEN_FDNAMES", "http:https:admin", 1);
    ASSERT_EQ(4, kl_systemd_listen_fd_by_name("https"));

    /* Env is consumed, so a repeat reports nothing. */
    ASSERT_EQ(-1, kl_systemd_listen_fd_by_name("http"));

    /* Unknown name → -1. */
    setenv("LISTEN_PID", pidbuf, 1);
    setenv("LISTEN_FDS", "3", 1);
    setenv("LISTEN_FDNAMES", "http:https:admin", 1);
    ASSERT_EQ(-1, kl_systemd_listen_fd_by_name("nope"));
}

UTEST(unix_socket, client_connects_over_http_unix) {
    char path[108];
    test_sock_path(path, sizeof(path), "clientunix");
    unlink(path);

    KlServer srv;
    KlConfig cfg = {
        .unix_socket_path = path,
        .unix_socket_unlink = 1,
        .max_connections = 4,
    };
    ASSERT_EQ(0, kl_server_init(&srv, &cfg));
    kl_server_route(&srv, "GET", "/hello", unix_handle_hello, NULL, NULL);

    pthread_t tid;
    ASSERT_EQ(0, pthread_create(&tid, NULL, unix_server_thread, &srv));

    int probe = connect_unix_retry(path, 200);
    ASSERT_TRUE(probe >= 0);
    close(probe);

    char enc[220];
    pct_encode_path(path, enc, sizeof(enc));
    char url[300];
    snprintf(url, sizeof(url), "http+unix://%s/hello", enc);

    KlAllocator a = kl_allocator_default();
    KlClientResponse resp;
    int rc = kl_client_request(&a, NULL, "GET", url,
                               NULL, 0, NULL, 0, &resp);
    ASSERT_EQ(0, rc);
    ASSERT_EQ(200, resp.status);
    ASSERT_TRUE(resp.body != NULL &&
                strstr(resp.body, "\"ok\":true") != NULL);
    kl_client_response_free(&resp);

    /* Pooled path bypasses the pool for UNIX and still serves the request. */
    KlClientResponse resp2;
    KlClientPool pool;
    ASSERT_EQ(0, kl_cpool_init(&pool, NULL, &a, NULL));
    ASSERT_EQ(0, kl_client_request_pooled(&pool, &a, NULL, "GET", url,
                                          NULL, 0, NULL, 0, &resp2));
    ASSERT_EQ(200, resp2.status);
    ASSERT_TRUE(resp2.body != NULL && strstr(resp2.body, "\"ok\":true") != NULL);
    kl_client_response_free(&resp2);
    kl_cpool_free(&pool);

    kl_server_stop(&srv);
    pthread_join(tid, NULL);
    kl_server_free(&srv);
}

UTEST(unix_socket, async_client_connects_over_http_unix) {
    char path[108];
    test_sock_path(path, sizeof(path), "asyncunix");
    unlink(path);

    KlServer srv;
    KlConfig cfg = {
        .unix_socket_path = path,
        .unix_socket_unlink = 1,
        .max_connections = 4,
    };
    ASSERT_EQ(0, kl_server_init(&srv, &cfg));
    kl_server_route(&srv, "GET", "/hello", unix_handle_hello, NULL, NULL);

    pthread_t tid;
    ASSERT_EQ(0, pthread_create(&tid, NULL, unix_server_thread, &srv));
    int probe = connect_unix_retry(path, 200);
    ASSERT_TRUE(probe >= 0);
    close(probe);

    char enc[220];
    pct_encode_path(path, enc, sizeof(enc));
    char url[300];
    snprintf(url, sizeof(url), "http+unix://%s/hello", enc);

    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(0, kl_event_ctx_init(&ev, &a));

    /* Async, non-pooled. */
    UnixAsyncCtx actx;
    memset(&actx, 0, sizeof(actx));
    KlClient *c = kl_client_start(&ev, &a, NULL, "GET", url,
                                  NULL, 0, NULL, 0, unix_async_done, &actx);
    ASSERT_TRUE(c != NULL);
    ASSERT_EQ(0, unix_run_until_done(&ev, &actx, 3000));
    ASSERT_EQ(0, actx.error);
    ASSERT_EQ(200, actx.status);
    ASSERT_TRUE(strstr(actx.body, "\"ok\":true") != NULL);
    kl_client_free(c);

    /* Async pooled path bypasses the pool for UNIX too. */
    KlClientPool pool;
    ASSERT_EQ(0, kl_cpool_init(&pool, NULL, &a, &ev));
    UnixAsyncCtx pctx;
    memset(&pctx, 0, sizeof(pctx));
    KlClient *pc = kl_client_start_pooled(&pool, &ev, &a, NULL, "GET", url,
                                          NULL, 0, NULL, 0, unix_async_done, &pctx);
    ASSERT_TRUE(pc != NULL);
    ASSERT_EQ(0, unix_run_until_done(&ev, &pctx, 3000));
    ASSERT_EQ(200, pctx.status);
    kl_client_free(pc);
    kl_cpool_free(&pool);

    kl_event_ctx_free(&ev);
    kl_server_stop(&srv);
    pthread_join(tid, NULL);
    kl_server_free(&srv);
}

UTEST(unix_socket, redirect_follows_over_http_unix) {
    char path[108];
    test_sock_path(path, sizeof(path), "redir");
    unlink(path);

    KlServer srv;
    KlConfig cfg = {
        .unix_socket_path = path,
        .unix_socket_unlink = 1,
        .max_connections = 4,
    };
    ASSERT_EQ(0, kl_server_init(&srv, &cfg));
    kl_server_route(&srv, "GET", "/hello", unix_handle_hello, NULL, NULL);
    kl_server_route(&srv, "GET", "/go", unix_handle_redirect, NULL, NULL);

    pthread_t tid;
    ASSERT_EQ(0, pthread_create(&tid, NULL, unix_server_thread, &srv));
    int probe = connect_unix_retry(path, 200);
    ASSERT_TRUE(probe >= 0);
    close(probe);

    char enc[220];
    pct_encode_path(path, enc, sizeof(enc));
    char url[300];
    snprintf(url, sizeof(url), "http+unix://%s/go", enc);

    KlAllocator a = kl_allocator_default();
    KlClientResponse resp;
    /* /go → 302 → relative /hello → 200; redirect resolution keeps the
     * http+unix authority. */
    int rc = kl_redirect_request(&a, NULL, NULL, "GET", url,
                                 NULL, 0, NULL, 0, &resp);
    ASSERT_EQ(0, rc);
    ASSERT_EQ(200, resp.status);
    ASSERT_TRUE(resp.body != NULL && strstr(resp.body, "\"ok\":true") != NULL);
    kl_client_response_free(&resp);

    kl_server_stop(&srv);
    pthread_join(tid, NULL);
    kl_server_free(&srv);
}

UTEST(unix_socket, websocket_connects_over_ws_unix) {
    char path[108];
    test_sock_path(path, sizeof(path), "wsunix");
    unlink(path);

    KlServer srv;
    KlConfig cfg = {
        .unix_socket_path = path,
        .unix_socket_unlink = 1,
        .max_connections = 4,
    };
    ASSERT_EQ(0, kl_server_init(&srv, &cfg));

    g_ws_srv_cred_rc = -2;
    KlWsServerConfig ws_cfg;
    kl_ws_server_config_init(&ws_cfg);
    ws_cfg.callbacks.on_open = ws_srv_on_open;
    ws_cfg.callbacks.on_message = ws_srv_on_message;
    ASSERT_EQ(0, kl_server_ws(&srv, "/ws", &ws_cfg));

    pthread_t tid;
    ASSERT_EQ(0, pthread_create(&tid, NULL, unix_server_thread, &srv));
    int probe = connect_unix_retry(path, 200);
    ASSERT_TRUE(probe >= 0);
    close(probe);

    char enc[220];
    pct_encode_path(path, enc, sizeof(enc));
    char url[300];
    snprintf(url, sizeof(url), "ws+unix://%s/ws", enc);

    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(0, kl_event_ctx_init(&ev, &a));

    WsCliCtx wc;
    memset(&wc, 0, sizeof(wc));
    KlWsClientCallbacks cbs = {
        .on_open = ws_cli_on_open,
        .on_message = ws_cli_on_message,
        .on_close = ws_cli_on_close,
    };
    KlWsClientConn *ws = kl_ws_client_connect(&ev, &a, NULL, url, &cbs, &wc);
    ASSERT_TRUE(ws != NULL);

    int elapsed = 0;
    while (!wc.got_msg && elapsed < 3000) {
        if (kl_event_ctx_run(&ev, 16, 50) < 0) break;
        elapsed += 50;
    }
    ASSERT_TRUE(wc.open);
    ASSERT_TRUE(wc.got_msg);
    ASSERT_STREQ(wc.msg, "ping");   /* echoed back */
    /* Server read the peer's creds on the live (post-upgrade) WS connection. */
    ASSERT_EQ(0, g_ws_srv_cred_rc);
    ASSERT_EQ((long)getuid(), g_ws_srv_cred.uid);

    kl_ws_client_free(ws);
    kl_event_ctx_free(&ev);
    kl_server_stop(&srv);
    pthread_join(tid, NULL);
    kl_server_free(&srv);
}

UTEST(unix_socket, tls_over_https_unix) {
    char path[108];
    test_sock_path(path, sizeof(path), "tlsunix");
    unlink(path);

    KlTlsConfig srv_tls = { .factory = mock_tls_create };
    KlServer srv;
    KlConfig cfg = {
        .unix_socket_path = path,
        .unix_socket_unlink = 1,
        .max_connections = 4,
        .tls = &srv_tls,
    };
    ASSERT_EQ(0, kl_server_init(&srv, &cfg));
    kl_server_route(&srv, "GET", "/hello", unix_handle_hello, NULL, NULL);

    pthread_t tid;
    ASSERT_EQ(0, pthread_create(&tid, NULL, unix_server_thread, &srv));
    int probe = connect_unix_retry(path, 200);
    ASSERT_TRUE(probe >= 0);
    close(probe);

    char enc[220];
    pct_encode_path(path, enc, sizeof(enc));
    char url[300];
    snprintf(url, sizeof(url), "https+unix://%s/hello", enc);

    /* Client speaks TLS (passthrough) over the UNIX socket; SNI defaults to
     * "localhost". Exercises the https+unix connect + handshake path. */
    KlTlsConfig cli_tls = { .factory = mock_tls_create };
    KlClientConfig ccfg;
    memset(&ccfg, 0, sizeof(ccfg));
    ccfg.tls = &cli_tls;

    KlAllocator a = kl_allocator_default();
    KlClientResponse resp;
    int rc = kl_client_request(&a, &ccfg, "GET", url, NULL, 0, NULL, 0, &resp);
    ASSERT_EQ(0, rc);
    ASSERT_EQ(200, resp.status);
    ASSERT_TRUE(resp.body != NULL && strstr(resp.body, "\"ok\":true") != NULL);
    kl_client_response_free(&resp);

    kl_server_stop(&srv);
    pthread_join(tid, NULL);
    kl_server_free(&srv);
}

UTEST(unix_socket, socket_group_is_applied) {
    struct group *g = getgrgid(getgid());
    if (!g || !g->gr_name)
        UTEST_SKIP("no group name for current gid");
    char grpname[128];
    snprintf(grpname, sizeof(grpname), "%s", g->gr_name);

    char path[108];
    test_sock_path(path, sizeof(path), "grp");
    unlink(path);

    KlServer srv;
    KlConfig cfg = {
        .unix_socket_path = path,
        .unix_socket_unlink = 1,
        .unix_socket_group = grpname,   /* our own group — allowed unprivileged */
        .unix_socket_mode = 0660,
        .max_connections = 4,
    };
    ASSERT_EQ(0, kl_server_init(&srv, &cfg));

    pthread_t tid;
    ASSERT_EQ(0, pthread_create(&tid, NULL, unix_server_thread, &srv));
    int probe = connect_unix_retry(path, 200);
    ASSERT_TRUE(probe >= 0);
    close(probe);

    struct stat st;
    ASSERT_EQ(0, stat(path, &st));
    ASSERT_EQ(getgid(), st.st_gid);
    ASSERT_EQ((mode_t)0660, (mode_t)(st.st_mode & 0777));

    kl_server_stop(&srv);
    pthread_join(tid, NULL);
    kl_server_free(&srv);
}

UTEST(unix_socket, unknown_owner_rejected) {
    char path[108];
    test_sock_path(path, sizeof(path), "badowner");
    unlink(path);

    KlServer srv;
    KlConfig cfg = {
        .unix_socket_path = path,
        .unix_socket_unlink = 1,
        .unix_socket_owner = "keel_no_such_user_zzz9",
        .max_connections = 4,
    };
    ASSERT_EQ(0, kl_server_init(&srv, &cfg));
    ASSERT_EQ(-1, kl_server_run(&srv));
    ASSERT_EQ(srv.last_error, KL_ERR_INVALID_ARG);
    kl_server_free(&srv);
    ASSERT_NE(0, access(path, F_OK));   /* socket unlinked on failure */
    unlink(path);
}

UTEST(unix_socket, unknown_group_rejected) {
    char path[108];
    test_sock_path(path, sizeof(path), "badgroup");
    unlink(path);

    KlServer srv;
    KlConfig cfg = {
        .unix_socket_path = path,
        .unix_socket_unlink = 1,
        .unix_socket_group = "keel_no_such_group_zzz9",
        .max_connections = 4,
    };
    ASSERT_EQ(0, kl_server_init(&srv, &cfg));
    ASSERT_EQ(-1, kl_server_run(&srv));
    ASSERT_EQ(srv.last_error, KL_ERR_INVALID_ARG);
    kl_server_free(&srv);
    unlink(path);
}

UTEST(unix_socket, listen_fd_is_cloexec) {
    char path[108];
    test_sock_path(path, sizeof(path), "cloexec");
    unlink(path);

    KlServer srv;
    KlConfig cfg = {
        .unix_socket_path = path,
        .unix_socket_unlink = 1,
        .max_connections = 4,
    };
    ASSERT_EQ(0, kl_server_init(&srv, &cfg));

    pthread_t tid;
    ASSERT_EQ(0, pthread_create(&tid, NULL, unix_server_thread, &srv));
    int probe = connect_unix_retry(path, 200);
    ASSERT_TRUE(probe >= 0);
    close(probe);

    int fdflags = fcntl(srv.listen_fd, F_GETFD);
    ASSERT_TRUE(fdflags >= 0);
    ASSERT_TRUE((fdflags & FD_CLOEXEC) != 0);

    kl_server_stop(&srv);
    pthread_join(tid, NULL);
    kl_server_free(&srv);
}

UTEST_MAIN();
