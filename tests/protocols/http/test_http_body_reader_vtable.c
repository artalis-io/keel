/*
 * test_http_body_reader_vtable.c: a per-route body-reader factory that returns a table missing a
 * REQUIRED op (on_data/on_complete/on_error/destroy) must be rejected WITHOUT crashing. Core drives
 * those four unconditionally over a request body, so a malformed reader would fault later.
 *
 * Public boundary: a real loopback HTTP/1.1 server on a worker thread, driven with a POST that has a
 * body. Response-code contract:
 *   - factory returns NULL           -> 415 (documented "unsupported media"), unchanged.
 *   - factory returns a valid reader -> handler runs (200).
 *   - factory returns a malformed reader (a required op is NULL) -> 500 (an implementation fault,
 *     not unsupported media) + close.
 * destroy is called only when present; the reader here is a shared static with stateless ops, so no
 * case leaks even when destroy is the omitted op.
 */
#include "utest.h"
#include "../../../src/protocols/http/http_conn_internal.h"
#include <keel/keel.h>
#include "net_compat.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

/* ── configurable stub body reader (one required op omitted per mode) ──────── */
static int  stub_on_data(KlHttpBodyReader *self, const char *d, size_t n) { (void)self;(void)d;(void)n; return 0; }
static void stub_on_complete(KlHttpBodyReader *self) { (void)self; }
static void stub_on_error(KlHttpBodyReader *self) { (void)self; }
static void stub_destroy(KlHttpBodyReader *self) { (void)self; }

enum { MODE_VALID = 0, MODE_NULL, MODE_OMIT_ON_DATA, MODE_OMIT_ON_COMPLETE,
       MODE_OMIT_ON_ERROR, MODE_OMIT_DESTROY };
static int              g_mode;
static KlHttpBodyReader g_reader;   /* shared; ops stateless (no heap => no leak) */

static KlHttpBodyReader *reader_factory(KlAllocator *alloc, const KlHttpRequest *req, void *ud) {
    (void)alloc; (void)req; (void)ud;
    if (g_mode == MODE_NULL) return NULL;
    memset(&g_reader, 0, sizeof(g_reader));
    g_reader.on_data     = stub_on_data;
    g_reader.on_complete = stub_on_complete;
    g_reader.on_error    = stub_on_error;
    g_reader.destroy     = stub_destroy;
    switch (g_mode) {
        case MODE_OMIT_ON_DATA:     g_reader.on_data     = NULL; break;
        case MODE_OMIT_ON_COMPLETE: g_reader.on_complete = NULL; break;
        case MODE_OMIT_ON_ERROR:    g_reader.on_error    = NULL; break;
        case MODE_OMIT_DESTROY:     g_reader.destroy     = NULL; break;
        default: break;
    }
    return &g_reader;
}

static void handle_upload(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_http_response_json(res, 200, "{\"ok\":true}", 11);
}

static void *server_thread_fn(void *arg) { kl_http_server_run((KlHttpServer *)arg); return NULL; }

static void wait_for_bind(KlHttpServer *s) {
    for (int i = 0; i < 200 && s->bound_port == 0; i++) usleep(10000);
}

static int connect_to(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { kl_test_closesock(fd); return -1; }
    return fd;
}

/* POST /upload with a 5-byte body; return the parsed status code (or -1). */
static int post_upload_status(int port) {
    int fd = connect_to(port);
    if (fd < 0) return -1;
    const char *req =
        "POST /upload HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Content-Length: 5\r\n"
        "Connection: close\r\n"
        "\r\n"
        "hello";
    kl_test_sockwrite(fd, req, strlen(req));

    char buf[2048]; size_t total = 0;
    while (total < sizeof(buf) - 1) {
        if (kl_test_poll1(fd, 0, 2000) <= 0) break;
        long n = kl_test_sockread(fd, buf + total, sizeof(buf) - total - 1);
        if (n <= 0) break;
        total += (size_t)n;
        buf[total] = '\0';
        if (strstr(buf, "\r\n\r\n")) break;   /* headers complete; status is in the first line */
    }
    kl_test_closesock(fd);
    buf[total] = '\0';
    int status = -1;
    if (sscanf(buf, "HTTP/1.1 %d", &status) != 1) return -1;
    return status;
}

/* Register POST /upload with the configurable factory; run a POST; return the status. */
static int run_one(int mode) {
    g_mode = mode;
    KlHttpServer srv;
    KlHttpServerConfig cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.port = 0;
    cfg.max_connections = 2;
    if (kl_http_server_init(&srv, &cfg) != 0) return -2;
    kl_http_server_route(&srv, "POST", "/upload", handle_upload, NULL, reader_factory);
    pthread_t tid;
    pthread_create(&tid, NULL, server_thread_fn, &srv);
    wait_for_bind(&srv);
    int status = post_upload_status(srv.bound_port);
    kl_http_server_stop(&srv);
    pthread_join(tid, NULL);
    kl_http_server_free(&srv);
    return status;
}

/* A valid reader lets the handler run (200). */
UTEST(body_reader_vtable, valid_runs_handler) {
    ASSERT_EQ(run_one(MODE_VALID), 200);
}

/* A NULL factory return keeps the documented 415. */
UTEST(body_reader_vtable, null_is_415) {
    ASSERT_EQ(run_one(MODE_NULL), 415);
}

/* A malformed non-NULL reader (each required op omitted) is a 500, not a 415. */
UTEST(body_reader_vtable, malformed_is_500) {
    int modes[] = { MODE_OMIT_ON_DATA, MODE_OMIT_ON_COMPLETE, MODE_OMIT_ON_ERROR, MODE_OMIT_DESTROY };
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++)
        ASSERT_EQ(run_one(modes[i]), 500);
}

UTEST_MAIN();
