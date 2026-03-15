/*
 * proxy_client.c — HTTP proxy client (all 4 modes)
 *
 * Self-contained: starts a local target server and proxy relay,
 * then demonstrates sync/async × HTTP/HTTPS proxy modes.
 *
 * Build:  make examples
 * Run:    ./examples/proxy_client
 */

#include <keel/keel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <errno.h>

/* ── Passthrough TLS mock ────────────────────────────────────────── */

/*
 * Proves the CONNECT tunnel works without requiring mbedTLS.
 * handshake → OK immediately, read/write → raw fd I/O.
 */

typedef struct {
    KlTls base;
} PassthroughTls;

static KlTlsResult pt_handshake(KlTls *self, int fd)
    { (void)self; (void)fd; return KL_TLS_OK; }
static ssize_t pt_read(KlTls *self, int fd, void *buf, size_t len)
    { (void)self; return read(fd, buf, len); }
static ssize_t pt_write(KlTls *self, int fd, const void *buf, size_t len)
    { (void)self; return write(fd, buf, len); }
static KlTlsResult pt_shutdown(KlTls *self, int fd)
    { (void)self; (void)fd; return KL_TLS_OK; }
static size_t pt_pending(KlTls *self)
    { (void)self; return 0; }
static void pt_reset(KlTls *self) { (void)self; }
static void pt_destroy(KlTls *self) { (void)self; }
static int pt_set_hostname(KlTls *self, const char *h)
    { (void)self; (void)h; return 0; }

static PassthroughTls pt_instance = {{
    .handshake = pt_handshake, .read = pt_read, .write = pt_write,
    .shutdown = pt_shutdown, .pending = pt_pending, .reset = pt_reset,
    .destroy = pt_destroy, .alpn_protocol = NULL,
    .set_hostname = pt_set_hostname,
}};

static KlTls *pt_factory(KlTlsCtx *ctx, KlAllocator *alloc)
    { (void)ctx; (void)alloc; return &pt_instance.base; }

/* ── Target server (runs in background thread) ───────────────────── */

static KlServer target_srv;
static int target_port;

static void handle_hello(KlRequest *req, KlResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_response_json(res, 200,
        "{\"message\":\"hello via proxy\"}", 28);
}

static void *target_thread(void *arg) {
    kl_server_run((KlServer *)arg);
    return NULL;
}

/* ── Mini proxy relay thread ─────────────────────────────────────── */

static int proxy_port;
static int proxy_listen_fd;

static int proxy_make_listener(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd); return -1;
    }
    socklen_t alen = sizeof(addr);
    getsockname(fd, (struct sockaddr *)&addr, &alen);
    proxy_port = ntohs(addr.sin_port);
    if (listen(fd, 8) < 0) { close(fd); return -1; }
    return fd;
}

/* Connect to 127.0.0.1:port, return fd or -1 */
static int proxy_connect_target(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd); return -1;
    }
    return fd;
}

/* Bidirectional relay until one side closes */
static void proxy_relay(int a, int b) {
    struct pollfd fds[2];
    fds[0].fd = a; fds[0].events = POLLIN;
    fds[1].fd = b; fds[1].events = POLLIN;
    char buf[4096];
    for (;;) {
        int rc = poll(fds, 2, 5000);
        if (rc <= 0) break;
        for (int i = 0; i < 2; i++) {
            if (fds[i].revents & POLLIN) {
                ssize_t n = read(fds[i].fd, buf, sizeof(buf));
                if (n <= 0) return;
                int dst = (i == 0) ? b : a;
                ssize_t sent = 0;
                while (sent < n) {
                    ssize_t w = write(dst, buf + sent, (size_t)(n - sent));
                    if (w <= 0) return;
                    sent += w;
                }
            }
            if (fds[i].revents & (POLLERR | POLLHUP)) return;
        }
    }
}

/*
 * Handle one proxy connection:
 * - HTTP forwarding: absolute-form URL → connect to target → relay
 * - CONNECT: respond 200 → bidirectional relay
 */
static void proxy_handle_conn(int client_fd) {
    char req[4096];
    size_t total = 0;

    /* Read request headers */
    while (total < sizeof(req) - 1) {
        struct pollfd pfd = { .fd = client_fd, .events = POLLIN };
        if (poll(&pfd, 1, 3000) <= 0) break;
        ssize_t r = read(client_fd, req + total, sizeof(req) - 1 - total);
        if (r <= 0) break;
        total += (size_t)r;
        req[total] = '\0';
        if (strstr(req, "\r\n\r\n")) break;
    }
    if (total == 0) { close(client_fd); return; }

    if (strncmp(req, "CONNECT ", 8) == 0) {
        /* HTTPS CONNECT tunnel */
        /* Parse port from "CONNECT host:port HTTP/1.1" */
        char *colon = strchr(req + 8, ':');
        if (!colon) { close(client_fd); return; }
        int tgt_port = (int)strtol(colon + 1, NULL, 10);

        /* Respond 200 */
        const char *ok = "HTTP/1.1 200 Connection Established\r\n\r\n";
        if (write(client_fd, ok, strlen(ok)) < 0) { close(client_fd); return; }

        /* Connect to target and relay */
        int tgt_fd = proxy_connect_target(tgt_port);
        if (tgt_fd < 0) { close(client_fd); return; }
        proxy_relay(client_fd, tgt_fd);
        close(tgt_fd);
    } else {
        /* HTTP forwarding: parse absolute URL for target port */
        /* Request looks like "GET http://127.0.0.1:PORT/hello HTTP/1.1\r\n..." */
        char *url_start = strchr(req, ' ');
        if (!url_start) { close(client_fd); return; }
        url_start++;

        /* Extract port from URL: http://127.0.0.1:PORT/path */
        char *port_start = strstr(url_start, "127.0.0.1:");
        if (!port_start) { close(client_fd); return; }
        port_start += 10; /* skip "127.0.0.1:" */
        int tgt_port = (int)strtol(port_start, NULL, 10);

        /* Extract path from URL */
        char *path = strchr(port_start, '/');
        if (!path) path = "/";

        /* Build request for target: convert absolute URL to relative */
        char fwd[4096];
        /* Find method */
        size_t method_len = (size_t)(url_start - 1 - req);
        /* Find end of request line */
        char *eol = strstr(req, "\r\n");
        if (!eol) { close(client_fd); return; }

        /* Find rest of headers (after first line) */
        char *headers_start = eol + 2;
        int n = snprintf(fwd, sizeof(fwd), "%.*s %s HTTP/1.1\r\n%s",
                         (int)method_len, req, path, headers_start);

        /* Connect to target */
        int tgt_fd = proxy_connect_target(tgt_port);
        if (tgt_fd < 0) { close(client_fd); return; }

        /* Send rewritten request */
        if (write(tgt_fd, fwd, (size_t)n) < 0) { close(tgt_fd); close(client_fd); return; }

        /* Read response from target and relay back */
        char resp[8192];
        size_t resp_total = 0;
        for (;;) {
            struct pollfd pfd = { .fd = tgt_fd, .events = POLLIN };
            if (poll(&pfd, 1, 3000) <= 0) break;
            ssize_t r = read(tgt_fd, resp + resp_total,
                             sizeof(resp) - 1 - resp_total);
            if (r <= 0) break;
            resp_total += (size_t)r;
        }
        if (resp_total > 0) {
            ssize_t wr = write(client_fd, resp, resp_total);
            (void)wr;
        }
        close(tgt_fd);
    }
    close(client_fd);
}

static void *proxy_thread(void *arg) {
    int listen_fd = *(int *)arg;
    /* Handle 4 connections (one per demo) */
    for (int i = 0; i < 4; i++) {
        struct pollfd pfd = { .fd = listen_fd, .events = POLLIN };
        if (poll(&pfd, 1, 10000) <= 0) break;
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) continue;
        proxy_handle_conn(fd);
    }
    return NULL;
}

/* ── Sync HTTP demo ──────────────────────────────────────────────── */

static int sync_http_demo(void) {
    printf("--- Sync HTTP (forwarding) ---\n");
    KlAllocator alloc = kl_allocator_default();
    KlProxyConfig proxy = {
        .host = "127.0.0.1", .port = (uint16_t)proxy_port, .auth = NULL
    };
    KlClientConfig cfg = { .timeout_ms = 5000, .proxy = &proxy };
    KlClientResponse resp;
    char url[256];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/hello", target_port);

    if (kl_client_request(&alloc, &cfg, "GET", url,
                          NULL, 0, NULL, 0, &resp) != 0) {
        printf("  ERROR: %s\n", kl_strerror(resp.error));
        return -1;
    }
    printf("  status: %d\n", resp.status);
    printf("  body:   %.*s\n", (int)resp.body_len, resp.body);
    int ok = (resp.status == 200 &&
              strstr(resp.body, "hello via proxy") != NULL);
    kl_client_response_free(&resp);
    return ok ? 0 : -1;
}

/* ── Async HTTP demo ─────────────────────────────────────────────── */

static int async_result;
static int async_done;

static void on_async_done(KlClient *client, void *user_data) {
    (void)user_data;
    if (kl_client_error(client) == 0) {
        const KlClientResponse *r = kl_client_response(client);
        printf("  status: %d\n", r->status);
        printf("  body:   %.*s\n", (int)r->body_len, r->body);
        async_result = (r->status == 200 &&
                        strstr(r->body, "hello via proxy") != NULL) ? 0 : -1;
    } else {
        printf("  ERROR: %s\n", kl_strerror(kl_client_last_error(client)));
        async_result = -1;
    }
    kl_client_free(client);
    async_done = 1;
}

static int async_http_demo(void) {
    printf("\n--- Async HTTP (forwarding) ---\n");
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ev;
    if (kl_event_ctx_init(&ev, &alloc) < 0) return -1;

    KlProxyConfig proxy = {
        .host = "127.0.0.1", .port = (uint16_t)proxy_port, .auth = NULL
    };
    KlClientConfig cfg = { .timeout_ms = 5000, .proxy = &proxy };
    char url[256];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/hello", target_port);

    async_done = 0;
    async_result = -1;
    KlClient *c = kl_client_start(&ev, &alloc, &cfg, "GET", url,
                                   NULL, 0, NULL, 0,
                                   on_async_done, NULL);
    if (!c) { kl_event_ctx_free(&ev); return -1; }

    while (!async_done) {
        if (kl_event_ctx_run(&ev, 8, 1000) < 0) break;
    }
    kl_event_ctx_free(&ev);
    return async_result;
}

/* ── Sync HTTPS CONNECT demo ─────────────────────────────────────── */

static int sync_https_demo(void) {
    printf("\n--- Sync HTTPS (CONNECT tunnel) ---\n");
    KlAllocator alloc = kl_allocator_default();
    KlProxyConfig proxy = {
        .host = "127.0.0.1", .port = (uint16_t)proxy_port, .auth = NULL
    };
    KlTlsConfig tls_cfg = { .factory = pt_factory };
    KlClientConfig cfg = {
        .timeout_ms = 5000, .tls = &tls_cfg, .proxy = &proxy
    };
    KlClientResponse resp;
    char url[256];
    snprintf(url, sizeof(url), "https://127.0.0.1:%d/hello", target_port);

    if (kl_client_request(&alloc, &cfg, "GET", url,
                          NULL, 0, NULL, 0, &resp) != 0) {
        printf("  ERROR: %s\n", kl_strerror(resp.error));
        return -1;
    }
    printf("  status: %d\n", resp.status);
    printf("  body:   %.*s\n", (int)resp.body_len, resp.body);
    int ok = (resp.status == 200 &&
              strstr(resp.body, "hello via proxy") != NULL);
    kl_client_response_free(&resp);
    return ok ? 0 : -1;
}

/* ── Async HTTPS CONNECT demo ────────────────────────────────────── */

static int async_https_demo(void) {
    printf("\n--- Async HTTPS (CONNECT tunnel) ---\n");
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ev;
    if (kl_event_ctx_init(&ev, &alloc) < 0) return -1;

    KlProxyConfig proxy = {
        .host = "127.0.0.1", .port = (uint16_t)proxy_port, .auth = NULL
    };
    KlTlsConfig tls_cfg = { .factory = pt_factory };
    KlClientConfig cfg = {
        .timeout_ms = 5000, .tls = &tls_cfg, .proxy = &proxy
    };
    char url[256];
    snprintf(url, sizeof(url), "https://127.0.0.1:%d/hello", target_port);

    async_done = 0;
    async_result = -1;
    KlClient *c = kl_client_start(&ev, &alloc, &cfg, "GET", url,
                                   NULL, 0, NULL, 0,
                                   on_async_done, NULL);
    if (!c) { kl_event_ctx_free(&ev); return -1; }

    while (!async_done) {
        if (kl_event_ctx_run(&ev, 8, 1000) < 0) break;
    }
    kl_event_ctx_free(&ev);
    return async_result;
}

/* ── Main ─────────────────────────────────────────────────────────── */

int main(void) {
    printf("HTTP proxy client example\n\n");

    /* Start target server */
    KlConfig cfg = { .port = 0, .max_connections = 8, .max_body_size = 4096 };
    kl_server_init(&target_srv, &cfg);
    kl_server_route(&target_srv, "GET", "/hello", handle_hello, NULL, NULL);

    pthread_t srv_tid;
    pthread_create(&srv_tid, NULL, target_thread, &target_srv);
    for (int i = 0; i < 200 && target_srv.bound_port == 0; i++)
        usleep(10000);
    target_port = target_srv.bound_port;

    /* Start mini proxy */
    proxy_listen_fd = proxy_make_listener();
    if (proxy_listen_fd < 0) {
        fprintf(stderr, "proxy listener failed\n");
        kl_server_stop(&target_srv);
        pthread_join(srv_tid, NULL);
        kl_server_free(&target_srv);
        return 1;
    }

    pthread_t proxy_tid;
    pthread_create(&proxy_tid, NULL, proxy_thread, &proxy_listen_fd);

    /* Run demos */
    int failures = 0;
    if (sync_http_demo() != 0) failures++;
    if (async_http_demo() != 0) failures++;
    if (sync_https_demo() != 0) failures++;
    if (async_https_demo() != 0) failures++;

    /* Cleanup */
    close(proxy_listen_fd);
    pthread_join(proxy_tid, NULL);
    kl_server_stop(&target_srv);
    pthread_join(srv_tid, NULL);
    kl_server_free(&target_srv);

    printf("\nDone. (%d/4 passed)\n", 4 - failures);
    return failures ? 1 : 0;
}
