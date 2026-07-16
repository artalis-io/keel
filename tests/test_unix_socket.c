#include "utest.h"
#include <keel/keel.h>
#include <errno.h>
#include <stddef.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static void unix_handle_hello(KlRequest *req, KlResponse *res, void *ctx) {
    (void)req;
    (void)ctx;
    kl_response_json(res, 200, "{\"ok\":true}", 11);
}

static void *unix_server_thread(void *arg) {
    kl_server_run((KlServer *)arg);
    return NULL;
}

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

UTEST_MAIN();
