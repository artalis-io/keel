#include "utest.h"
#include <keel/keel.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "net_compat.h"

static char     g_ip[INET6_ADDRSTRLEN];
static uint16_t g_port;
static int      g_rc = -2;

static void handle(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)ctx;
    g_ip[0] = '\0';
    g_port = 0;
    g_rc = kl_http_request_peer_addr(req, g_ip, sizeof(g_ip), &g_port);
    kl_http_response_json(res, 200, "{\"ok\":true}", 11);
}

static void *srv_thread(void *a) { kl_server_run((KlServer *)a); return NULL; }

static void drain_response(int fd) {
    char buf[512];
    while (kl_test_poll1(fd, 0, 2000) > 0) {
        long n = kl_test_sockread(fd, buf, sizeof(buf));
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
    ASSERT_EQ((long)strlen(req), kl_test_sockwrite(fd, req, strlen(req)));
    drain_response(fd);
    kl_test_closesock(fd);

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
        kl_test_sockwrite(fd, req, strlen(req));
        drain_response(fd);
    }
    if (fd >= 0) kl_test_closesock(fd);

    kl_server_stop(&srv);
    pthread_join(t, NULL);
    kl_server_free(&srv);

    if (crc != 0)
        UTEST_SKIP("IPv6 connect failed");
    ASSERT_EQ(0, g_rc);
    ASSERT_STREQ(g_ip, "::1");
}

/* ── PROXY protocol integration ──────────────────────────────────────── */

static int connect_local(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) { kl_test_closesock(fd); return -1; }
    return fd;
}

UTEST(peer_addr, proxy_v1_trusted) {
    g_rc = -2; g_ip[0] = '\0'; g_port = 0;
    KlServer srv;
    KlConfig cfg = { .port = 0, .bind_addr = "127.0.0.1", .max_connections = 4,
                     .proxy_trusted_cidrs = "127.0.0.1/32" };
    ASSERT_EQ(0, kl_server_init(&srv, &cfg));
    kl_server_route(&srv, "GET", "/x", handle, NULL, NULL);
    pthread_t t;
    ASSERT_EQ(0, pthread_create(&t, NULL, srv_thread, &srv));
    for (int i = 0; i < 200 && srv.bound_port == 0; i++) usleep(10000);
    ASSERT_TRUE(srv.bound_port > 0);

    int fd = connect_local(srv.bound_port);
    ASSERT_TRUE(fd >= 0);
    const char *msg =
        "PROXY TCP4 203.0.113.7 10.0.0.1 5000 443\r\n"
        "GET /x HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    ASSERT_EQ((long)strlen(msg), kl_test_sockwrite(fd, msg, strlen(msg)));
    drain_response(fd);
    kl_test_closesock(fd);

    kl_server_stop(&srv); pthread_join(t, NULL); kl_server_free(&srv);
    ASSERT_EQ(0, g_rc);
    ASSERT_STREQ("203.0.113.7", g_ip);   /* header address, not 127.0.0.1 */
    ASSERT_EQ(5000, g_port);
}

UTEST(peer_addr, proxy_v2_trusted) {
    g_rc = -2; g_ip[0] = '\0'; g_port = 0;
    KlServer srv;
    KlConfig cfg = { .port = 0, .bind_addr = "127.0.0.1", .max_connections = 4,
                     .proxy_trusted_cidrs = "127.0.0.0/8" };
    ASSERT_EQ(0, kl_server_init(&srv, &cfg));
    kl_server_route(&srv, "GET", "/x", handle, NULL, NULL);
    pthread_t t;
    ASSERT_EQ(0, pthread_create(&t, NULL, srv_thread, &srv));
    for (int i = 0; i < 200 && srv.bound_port == 0; i++) usleep(10000);
    ASSERT_TRUE(srv.bound_port > 0);

    int fd = connect_local(srv.bound_port);
    ASSERT_TRUE(fd >= 0);
    uint8_t hdr[28];
    static const uint8_t sig[12] = {0x0D,0x0A,0x0D,0x0A,0x00,0x0D,0x0A,0x51,0x55,0x49,0x54,0x0A};
    memcpy(hdr, sig, 12);
    hdr[12] = 0x21;              /* v2 | PROXY */
    hdr[13] = 0x11;             /* AF_INET | STREAM */
    hdr[14] = 0x00; hdr[15] = 0x0C;
    struct in_addr s, d; memset(&d, 0, sizeof(d));
    inet_pton(AF_INET, "198.51.100.9", &s);
    memcpy(hdr + 16, &s, 4); memcpy(hdr + 20, &d, 4);
    uint16_t sp = htons(40000), dp = htons(443);
    memcpy(hdr + 24, &sp, 2); memcpy(hdr + 26, &dp, 2);
    ASSERT_EQ((long)28, kl_test_sockwrite(fd, hdr, 28));
    const char *req = "GET /x HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    ASSERT_EQ((long)strlen(req), kl_test_sockwrite(fd, req, strlen(req)));
    drain_response(fd);
    kl_test_closesock(fd);

    kl_server_stop(&srv); pthread_join(t, NULL); kl_server_free(&srv);
    ASSERT_EQ(0, g_rc);
    ASSERT_STREQ("198.51.100.9", g_ip);
    ASSERT_EQ(40000, g_port);
}

UTEST(peer_addr, proxy_trusted_no_header) {
    /* Trusted source, but a direct request (no PROXY header) → socket addr. */
    g_rc = -2; g_ip[0] = '\0'; g_port = 0;
    KlServer srv;
    KlConfig cfg = { .port = 0, .bind_addr = "127.0.0.1", .max_connections = 4,
                     .proxy_trusted_cidrs = "127.0.0.1/32" };
    ASSERT_EQ(0, kl_server_init(&srv, &cfg));
    kl_server_route(&srv, "GET", "/x", handle, NULL, NULL);
    pthread_t t;
    ASSERT_EQ(0, pthread_create(&t, NULL, srv_thread, &srv));
    for (int i = 0; i < 200 && srv.bound_port == 0; i++) usleep(10000);
    ASSERT_TRUE(srv.bound_port > 0);

    int fd = connect_local(srv.bound_port);
    ASSERT_TRUE(fd >= 0);
    const char *req = "GET /x HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    ASSERT_EQ((long)strlen(req), kl_test_sockwrite(fd, req, strlen(req)));
    drain_response(fd);
    kl_test_closesock(fd);

    kl_server_stop(&srv); pthread_join(t, NULL); kl_server_free(&srv);
    ASSERT_EQ(0, g_rc);
    ASSERT_STREQ("127.0.0.1", g_ip);
}

UTEST(peer_addr, proxy_bad_cidr_rejected) {
    KlServer srv;
    KlConfig cfg = { .port = 0, .proxy_trusted_cidrs = "not-a-cidr" };
    ASSERT_EQ(-1, kl_server_init(&srv, &cfg));
    ASSERT_EQ(srv.last_error, KL_ERR_INVALID_ARG);
}

UTEST_MAIN();
