#include "utest.h"
#include <keel/keel.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static char     g_ip[INET6_ADDRSTRLEN];
static uint16_t g_port;
static int      g_rc = -2;

static void handle(KlRequest *req, KlResponse *res, void *ctx) {
    (void)ctx;
    g_ip[0] = '\0';
    g_port = 0;
    g_rc = kl_request_peer_addr(req, g_ip, sizeof(g_ip), &g_port);
    kl_response_json(res, 200, "{\"ok\":true}", 11);
}

static void *srv_thread(void *a) { kl_server_run((KlServer *)a); return NULL; }

static void drain_response(int fd) {
    char buf[512];
    struct pollfd p = { .fd = fd, .events = POLLIN };
    while (poll(&p, 1, 2000) > 0) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) break;
    }
}

UTEST(peer_addr, tcp_ipv4) {
    KlServer srv;
    KlConfig cfg = { .port = 0, .bind_addr = "127.0.0.1", .max_connections = 4 };
    ASSERT_EQ(0, kl_server_init(&srv, &cfg));
    kl_server_route(&srv, "GET", "/x", handle, NULL, NULL);

    pthread_t t;
    ASSERT_EQ(0, pthread_create(&t, NULL, srv_thread, &srv));
    for (int i = 0; i < 200 && srv.bound_port == 0; i++) usleep(10000);
    ASSERT_TRUE(srv.bound_port > 0);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_TRUE(fd >= 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)srv.bound_port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    ASSERT_EQ(0, connect(fd, (struct sockaddr *)&a, sizeof(a)));

    const char *req = "GET /x HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    ASSERT_EQ((ssize_t)strlen(req), write(fd, req, strlen(req)));
    drain_response(fd);
    close(fd);

    kl_server_stop(&srv);
    pthread_join(t, NULL);
    kl_server_free(&srv);

    ASSERT_EQ(0, g_rc);
    ASSERT_STREQ(g_ip, "127.0.0.1");
    ASSERT_TRUE(g_port > 0);   /* ephemeral client port */
}

UTEST(peer_addr, tcp_ipv6) {
    KlServer srv;
    KlConfig cfg = { .port = 0, .bind_addr = "::1", .max_connections = 4 };
    if (kl_server_init(&srv, &cfg) != 0)
        UTEST_SKIP("IPv6 unavailable");

    kl_server_route(&srv, "GET", "/x", handle, NULL, NULL);
    pthread_t t;
    ASSERT_EQ(0, pthread_create(&t, NULL, srv_thread, &srv));
    for (int i = 0; i < 200 && srv.bound_port == 0; i++) usleep(10000);
    if (srv.bound_port <= 0) {
        kl_server_stop(&srv); pthread_join(t, NULL); kl_server_free(&srv);
        UTEST_SKIP("IPv6 bind failed");
    }

    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    struct sockaddr_in6 a6;
    memset(&a6, 0, sizeof(a6));
    a6.sin6_family = AF_INET6;
    a6.sin6_port = htons((uint16_t)srv.bound_port);
    inet_pton(AF_INET6, "::1", &a6.sin6_addr);
    int crc = (fd >= 0) ? connect(fd, (struct sockaddr *)&a6, sizeof(a6)) : -1;
    if (crc == 0) {
        const char *req = "GET /x HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
        write(fd, req, strlen(req));
        drain_response(fd);
    }
    if (fd >= 0) close(fd);

    kl_server_stop(&srv);
    pthread_join(t, NULL);
    kl_server_free(&srv);

    if (crc != 0)
        UTEST_SKIP("IPv6 connect failed");
    ASSERT_EQ(0, g_rc);
    ASSERT_STREQ(g_ip, "::1");
}

UTEST_MAIN();
