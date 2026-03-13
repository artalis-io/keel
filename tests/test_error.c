#include "utest.h"
#include <keel/error.h>
#include <keel/server.h>
#include <keel/client.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

/* ── kl_strerror ─────────────────────────────────────────────────── */

UTEST(error, strerror_valid_codes) {
    for (int i = 0; i < KL_ERR__COUNT; i++) {
        const char *msg = kl_strerror((KlError)i);
        ASSERT_TRUE(msg != NULL);
        ASSERT_TRUE(strlen(msg) > 0);
    }
}

UTEST(error, strerror_none) {
    ASSERT_STREQ(kl_strerror(KL_ERR_NONE), "no error");
}

UTEST(error, strerror_specific) {
    ASSERT_STREQ(kl_strerror(KL_ERR_ALLOC), "memory allocation failed");
    ASSERT_STREQ(kl_strerror(KL_ERR_BIND), "bind failed");
    ASSERT_STREQ(kl_strerror(KL_ERR_DNS), "DNS resolution failed");
    ASSERT_STREQ(kl_strerror(KL_ERR_TLS_HANDSHAKE), "TLS handshake failed");
}

UTEST(error, strerror_out_of_range) {
    ASSERT_STREQ(kl_strerror((KlError)-1), "unknown error");
    ASSERT_STREQ(kl_strerror(KL_ERR__COUNT), "unknown error");
    ASSERT_STREQ(kl_strerror((KlError)9999), "unknown error");
}

/* ── KlServer.last_error ─────────────────────────────────────────── */

UTEST(error, server_init_success) {
    KlServer s;
    KlConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.port = 0;  /* unused — we won't run */
    ASSERT_EQ(kl_server_init(&s, &cfg), 0);
    ASSERT_EQ(s.last_error, KL_ERR_NONE);
    kl_server_free(&s);
}

UTEST(error, server_init_null) {
    KlServer s;
    memset(&s, 0, sizeof(s));
    ASSERT_EQ(kl_server_init(&s, NULL), -1);
    ASSERT_EQ(s.last_error, KL_ERR_INVALID_ARG);
}

UTEST(error, server_run_bind_in_use) {
    /* Occupy a port */
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_TRUE(sock >= 0);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;  /* kernel picks a free port */
    ASSERT_EQ(bind(sock, (struct sockaddr *)&addr, sizeof(addr)), 0);
    ASSERT_EQ(listen(sock, 1), 0);

    /* Find out which port the kernel picked */
    socklen_t len = sizeof(addr);
    ASSERT_EQ(getsockname(sock, (struct sockaddr *)&addr, &len), 0);
    int port = ntohs(addr.sin_port);

    /* Try to bind keel on the same port */
    KlServer s;
    KlConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.port = port;
    cfg.bind_addr = "127.0.0.1";
    ASSERT_EQ(kl_server_init(&s, &cfg), 0);
    ASSERT_EQ(kl_server_run(&s), -1);
    ASSERT_EQ(s.last_error, KL_ERR_BIND);

    kl_server_free(&s);
    close(sock);
}

/* ── KlClientResponse.error (sync) ──────────────────────────────── */

UTEST(error, sync_client_bad_url) {
    KlAllocator alloc = kl_allocator_default();
    KlClientResponse resp;
    int rc = kl_client_request(&alloc, NULL, "GET", "not-a-url",
                                NULL, 0, NULL, 0, &resp);
    ASSERT_EQ(rc, -1);
    ASSERT_EQ(resp.error, KL_ERR_URL);
}

UTEST(error, sync_client_https_no_tls) {
    KlAllocator alloc = kl_allocator_default();
    KlClientResponse resp;
    int rc = kl_client_request(&alloc, NULL, "GET", "https://example.com",
                                NULL, 0, NULL, 0, &resp);
    ASSERT_EQ(rc, -1);
    ASSERT_EQ(resp.error, KL_ERR_URL);
}

UTEST(error, sync_client_dns_fail) {
    KlAllocator alloc = kl_allocator_default();
    KlClientConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.timeout_ms = 1000;
    KlClientResponse resp;
    int rc = kl_client_request(&alloc, &cfg, "GET",
                                "http://this-host-does-not-exist.invalid/",
                                NULL, 0, NULL, 0, &resp);
    ASSERT_EQ(rc, -1);
    ASSERT_EQ(resp.error, KL_ERR_DNS);
}

UTEST(error, sync_client_connect_refused) {
    KlAllocator alloc = kl_allocator_default();
    KlClientConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.timeout_ms = 1000;
    KlClientResponse resp;
    /* Port 1 is unlikely to have anything listening */
    int rc = kl_client_request(&alloc, &cfg, "GET",
                                "http://127.0.0.1:1/",
                                NULL, 0, NULL, 0, &resp);
    ASSERT_EQ(rc, -1);
    ASSERT_TRUE(resp.error == KL_ERR_CONNECT || resp.error == KL_ERR_TIMEOUT);
}

UTEST_MAIN();
