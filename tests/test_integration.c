#include "utest.h"
#include <keel/keel.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <signal.h>

static int test_port;  /* set by start_server / individual tests */

static void handle_hello(KlRequest *req, KlResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_response_json(res, 200, "{\"msg\":\"hello\"}", 15);
}

static void handle_param(KlRequest *req, KlResponse *res, void *ctx) {
    (void)ctx;
    size_t id_len;
    const char *id = kl_request_param(req, "id", &id_len);
    static char body[256]; /* static: body must outlive handler for async writev */
    int n;
    if (id) {
        n = snprintf(body, sizeof(body),
                     "{\"id\":\"%.*s\",\"num_params\":%d}",
                     (int)id_len, id, req->num_params);
    } else {
        n = snprintf(body, sizeof(body), "{\"id\":null,\"num_params\":0}");
    }
    if (n < 0) n = 0;
    kl_response_json(res, 200, body, (size_t)n);
}

static void handle_echo(KlRequest *req, KlResponse *res, void *ctx) {
    (void)ctx;
    KlBufReader *br = (KlBufReader *)req->body_reader;
    kl_response_status(res, 200);
    kl_response_header(res, "Content-Type", "text/plain");
    if (br && br->len > 0) {
        kl_response_body_borrow(res, br->data, br->len);
    } else {
        kl_response_body_borrow(res, "no body", 7);
    }
}

/* Handler that drives the streaming multipart iterator and reports a
 * "parts=N name0=... len0=..." summary line. The full body has been
 * received by the time the handler fires (in the non-streaming route),
 * so kl_multipart_next never returns NEED_DATA here.
 *
 * All allocations go through the keel allocator — no fixed-size static
 * buffers, no magic-constant part caps. The response body is sent via
 * kl_response_body_copy so Keel owns the bytes after this function
 * returns; the handler's scratch allocations are freed before return. */
typedef struct {
    char  *name;
    size_t name_len;
    char  *filename;
    size_t filename_len;
    int    has_filename;
    size_t size;
} HandleUploadPart;

static void handle_upload_free_parts(KlAllocator *a, HandleUploadPart *parts,
                                      size_t count, size_t cap) {
    if (!parts) return;
    for (size_t i = 0; i < count; i++) {
        if (parts[i].name)
            kl_free(a, parts[i].name, parts[i].name_len + 1);
        if (parts[i].filename)
            kl_free(a, parts[i].filename, parts[i].filename_len + 1);
    }
    kl_free(a, parts, cap * sizeof(*parts));
}

static char *handle_upload_dup(KlAllocator *a, const char *s, size_t n) {
    char *d = kl_malloc(a, n + 1);
    if (!d) return NULL;
    if (n > 0) memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

static void handle_upload(KlRequest *req, KlResponse *res, void *ctx) {
    (void)ctx;
    KlBodyReader *br = req->body_reader;
    if (!br) {
        kl_response_error(res, 400, "No reader");
        return;
    }
    KlAllocator alloc = kl_allocator_default();

    HandleUploadPart *parts = NULL;
    size_t parts_count = 0;
    size_t parts_cap = 0;

    KlMultipartPartMeta meta;
    const char *d = NULL;
    size_t      dn = 0;
    for (;;) {
        KlMultipartEvent e = kl_multipart_next(br, &meta, &d, &dn);
        if (e == KL_MP_EVT_PART_BEGIN) {
            if (parts_count == parts_cap) {
                size_t new_cap = parts_cap ? parts_cap * 2 : 4;
                size_t old_sz  = parts_cap * sizeof(*parts);
                size_t new_sz  = new_cap   * sizeof(*parts);
                HandleUploadPart *np = kl_realloc(&alloc, parts, old_sz, new_sz);
                if (!np) {
                    handle_upload_free_parts(&alloc, parts, parts_count, parts_cap);
                    kl_response_error(res, 500, "OOM");
                    return;
                }
                parts = np;
                parts_cap = new_cap;
            }
            HandleUploadPart *p = &parts[parts_count];
            memset(p, 0, sizeof(*p));
            p->name = handle_upload_dup(&alloc, meta.name, meta.name_len);
            if (!p->name) {
                /* L1 fix: don't promote a partially-initialised part
                 * (with NULL name) into the active count — later
                 * snprintf would deref it. Bail cleanly. */
                handle_upload_free_parts(&alloc, parts, parts_count, parts_cap);
                kl_response_error(res, 500, "OOM");
                return;
            }
            p->name_len = meta.name_len;
            if (meta.filename) {
                p->filename = handle_upload_dup(&alloc, meta.filename,
                                                meta.filename_len);
                if (!p->filename) {
                    kl_free(&alloc, p->name, p->name_len + 1);
                    handle_upload_free_parts(&alloc, parts, parts_count, parts_cap);
                    kl_response_error(res, 500, "OOM");
                    return;
                }
                p->filename_len = meta.filename_len;
                p->has_filename = 1;
            }
            parts_count++;
            continue;
        }
        if (e == KL_MP_EVT_PART_DATA) {
            if (parts_count > 0) parts[parts_count - 1].size += dn;
            continue;
        }
        if (e == KL_MP_EVT_PART_END) continue;
        if (e == KL_MP_EVT_DONE)     break;
        handle_upload_free_parts(&alloc, parts, parts_count, parts_cap);
        kl_response_error(res, 400, "Parse error");
        return;
    }
    if (parts_count == 0) {
        handle_upload_free_parts(&alloc, parts, parts_count, parts_cap);
        kl_response_error(res, 400, "No parts");
        return;
    }

    /* Two-pass: size the response body via snprintf(NULL,0,...), then
     * allocate exactly what's needed. L2 fix: validate each snprintf
     * return individually so a single failure can't poison the running
     * total. */
    size_t body_cap = 0;
    int n = snprintf(NULL, 0, "parts=%zu", parts_count);
    if (n < 0) {
        handle_upload_free_parts(&alloc, parts, parts_count, parts_cap);
        kl_response_error(res, 500, "snprintf");
        return;
    }
    body_cap += (size_t)n;
    for (size_t i = 0; i < parts_count; i++) {
        n = snprintf(NULL, 0, " name%zu=%s len%zu=%zu",
                     i, parts[i].name, i, parts[i].size);
        if (n < 0) {
            handle_upload_free_parts(&alloc, parts, parts_count, parts_cap);
            kl_response_error(res, 500, "snprintf");
            return;
        }
        body_cap += (size_t)n;
    }
    body_cap += 1;  /* trailing NUL */
    char *body = kl_malloc(&alloc, body_cap);
    if (!body) {
        handle_upload_free_parts(&alloc, parts, parts_count, parts_cap);
        kl_response_error(res, 500, "OOM");
        return;
    }
    size_t off = 0;
    n = snprintf(body + off, body_cap - off, "parts=%zu", parts_count);
    if (n < 0 || (size_t)n >= body_cap - off) {
        kl_free(&alloc, body, body_cap);
        handle_upload_free_parts(&alloc, parts, parts_count, parts_cap);
        kl_response_error(res, 500, "snprintf");
        return;
    }
    off += (size_t)n;
    for (size_t i = 0; i < parts_count; i++) {
        n = snprintf(body + off, body_cap - off,
                     " name%zu=%s len%zu=%zu",
                     i, parts[i].name, i, parts[i].size);
        if (n < 0 || (size_t)n >= body_cap - off) {
            kl_free(&alloc, body, body_cap);
            handle_upload_free_parts(&alloc, parts, parts_count, parts_cap);
            kl_response_error(res, 500, "snprintf");
            return;
        }
        off += (size_t)n;
    }

    kl_response_status(res, 200);
    kl_response_header(res, "Content-Type", "text/plain");
    /* body_copy: Keel takes its own copy, we free our scratch. */
    kl_response_body_copy(res, body, off);

    kl_free(&alloc, body, body_cap);
    handle_upload_free_parts(&alloc, parts, parts_count, parts_cap);
}

static void handle_no_reader(KlRequest *req, KlResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_response_json(res, 200, "{\"ok\":true}", 11);
}

/* ── Mid-stream early-exit on ERROR-return fixture ──────────────────
 *
 * Sibling of the streaming_mid_stream_early_exit test for the case
 * where the body reader REJECTS the body (returns -1 from on_data —
 * e.g. cap-exceeded). The handler stashes conn+res at dispatch time;
 * on_data writes the response, sets state = SENDING, then returns
 * -1 to simulate the reader rejecting the body bytes. The new branch
 * in kl_conn_on_readable's rc<0 / pr==KL_PARSE_ERROR paths must see
 * the SENDING state and honor it instead of overwriting with the
 * hardcoded 413 + CLOSED. */
static volatile int eer_err_handler_called = 0;

typedef struct {
    KlBodyReader  base;
    KlAllocator  *alloc;
    KlConn       *conn;
    KlResponse   *res;
    int           responded;
} EarlyExitErrReader;

static int  eer_err_on_data(KlBodyReader *self, const char *data, size_t len) {
    EarlyExitErrReader *r = (EarlyExitErrReader *)self;
    (void)data; (void)len;
    /* First call (handler hasn't stashed conn+res yet) or already
     * responded: succeed silently so the handler gets a chance to
     * run. Mirrors eer_on_data's null-conn handling. */
    if (r->responded || !r->conn || !r->res) return 0;
    /* Handler is alive — now pretend we just hit a cap. Write a
     * structured "handler" response BEFORE returning -1. The new
     * error-path branch in connection.c sees state == SENDING and
     * returns it without overwriting. */
    kl_response_status(r->res, 413);
    kl_response_header(r->res, "Content-Type", "text/plain");
    /* 31 bytes — em-dash is 3 bytes UTF-8 */
    kl_response_body_borrow(r->res, "cap exceeded — handler caught", 31);
    r->conn->state = KL_CONN_SENDING;
    r->responded = 1;
    return -1;   /* simulate body-reader rejection */
}
static void eer_err_on_complete(KlBodyReader *self) { (void)self; }
static void eer_err_on_error(KlBodyReader *self)    { (void)self; }
static void eer_err_destroy(KlBodyReader *self) {
    EarlyExitErrReader *r = (EarlyExitErrReader *)self;
    kl_free(r->alloc, r, sizeof(*r));
}

static KlBodyReader *eer_err_factory(KlAllocator *alloc, const KlRequest *req,
                                       void *user_data) {
    (void)req; (void)user_data;
    EarlyExitErrReader *r = kl_malloc(alloc, sizeof(*r));
    if (!r) return NULL;
    memset(r, 0, sizeof(*r));
    r->base.on_data     = eer_err_on_data;
    r->base.on_complete = eer_err_on_complete;
    r->base.on_error    = eer_err_on_error;
    r->base.destroy     = eer_err_destroy;
    r->alloc            = alloc;
    return &r->base;
}

static void handle_early_exit_err(KlRequest *req, KlResponse *res, void *ctx) {
    (void)ctx;
    EarlyExitErrReader *r = (EarlyExitErrReader *)req->body_reader;
    if (!r) { kl_response_error(res, 500, "no reader"); return; }
    r->conn = kl_request_conn(req);
    r->res  = res;
    r->conn->state = KL_CONN_READING_BODY;
    eer_err_handler_called = 1;
}

/* ── Streaming-async early-invoke fixture (v2.2.0) ───────────────────
 *
 * Same shape as handle_early_exit_err, but registered with
 * kl_server_route_streaming_async so the handler runs BEFORE leftover
 * is fed via on_data. That lets us close the single-read leftover-cap
 * gap: a body that arrives in the same kernel read as the header now
 * gets rejected via the parked handler's structured response, instead
 * of the body-reader's on_data silently succeeding because the handler
 * hadn't stashed conn+res yet.
 *
 * The fixture reuses eer_err_factory (stateless) but tracks invocation
 * with its own flag so tests stay independent.
 */
static volatile int eer_async_err_handler_called = 0;

static void handle_early_exit_async_err(KlRequest *req, KlResponse *res,
                                          void *ctx) {
    (void)ctx;
    EarlyExitErrReader *r = (EarlyExitErrReader *)req->body_reader;
    if (!r) { kl_response_error(res, 500, "no reader"); return; }
    r->conn = kl_request_conn(req);
    r->res  = res;
    r->conn->state = KL_CONN_READING_BODY;
    eer_async_err_handler_called = 1;
}

/* ── Mid-stream early-exit fixture ───────────────────────────────────
 *
 * Synthetic body reader + handler pair used by the
 * streaming_mid_stream_early_exit test. Together they simulate what a
 * Lua/JS coroutine-based handler would do: yield at dispatch time
 * (state = READING_BODY), then "resume" inside on_data, set up the
 * response, and transition state to SENDING. Without the early-exit
 * branch in kl_conn_on_readable's READING_BODY mid-stream return,
 * Keel would override that SENDING with READING_BODY and the response
 * would never be sent.
 */
/* Set by handle_early_exit when it runs. The test thread polls this
 * to know when the handler has run (and therefore when it's safe to
 * send the second body chunk), instead of guessing with a fixed
 * usleep. Reset to 0 at the start of each test. */
static volatile int eer_handler_called = 0;

typedef struct {
    KlBodyReader  base;
    KlAllocator  *alloc;
    KlConn       *conn;        /* stashed by handler at dispatch time */
    KlResponse   *res;         /* same */
    int           responded;   /* one-shot guard */
} EarlyExitReader;

static int  eer_on_data(KlBodyReader *self, const char *data, size_t len) {
    EarlyExitReader *r = (EarlyExitReader *)self;
    (void)data; (void)len;
    if (r->responded || !r->conn || !r->res) return 0;
    /* Mimic a coroutine waking on first on_data: set up the response,
     * transition state to SENDING. The new mid-stream early-exit
     * branch in connection.c sees the terminal state and returns it
     * instead of forcing READING_BODY. */
    kl_response_status(r->res, 200);
    kl_response_header(r->res, "Content-Type", "text/plain");
    kl_response_body_borrow(r->res, "early exit", 10);
    r->conn->state = KL_CONN_SENDING;
    r->responded = 1;
    return 0;
}
static void eer_on_complete(KlBodyReader *self) { (void)self; }
static void eer_on_error(KlBodyReader *self)    { (void)self; }
static void eer_destroy(KlBodyReader *self) {
    EarlyExitReader *r = (EarlyExitReader *)self;
    kl_free(r->alloc, r, sizeof(*r));
}

static KlBodyReader *eer_factory(KlAllocator *alloc, const KlRequest *req,
                                  void *user_data) {
    (void)req; (void)user_data;
    EarlyExitReader *r = kl_malloc(alloc, sizeof(*r));
    if (!r) return NULL;
    memset(r, 0, sizeof(*r));
    r->base.on_data     = eer_on_data;
    r->base.on_complete = eer_on_complete;
    r->base.on_error    = eer_on_error;
    r->base.destroy     = eer_destroy;
    r->alloc            = alloc;
    return &r->base;
}

static void handle_early_exit(KlRequest *req, KlResponse *res, void *ctx) {
    (void)ctx;
    /* Stash conn + res on the body reader so on_data can wake "us"
     * with the response. Then yield by leaving state as READING_BODY
     * so conn_invoke_streaming_handler returns READING_BODY and the
     * event loop keeps pumping on_data. */
    EarlyExitReader *r = (EarlyExitReader *)req->body_reader;
    if (!r) { kl_response_error(res, 500, "no reader"); return; }
    r->conn = kl_request_conn(req);
    r->res  = res;
    r->conn->state = KL_CONN_READING_BODY;
    /* Signal the test thread that we've run — it polls this before
     * sending the second body chunk so the test isn't timing-fragile. */
    eer_handler_called = 1;
}

static KlServer test_server;

static void *server_thread(void *arg) {
    (void)arg;
    kl_server_run(&test_server);
    return NULL;
}

static int connect_to(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)port),
    };
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Read full response (until EOF or read returns 0) */
static ssize_t read_response(int fd, char *buf, size_t buflen) {
    ssize_t total = 0;
    ssize_t n;
    while ((n = read(fd, buf + total,
                     buflen - (size_t)total - 1)) > 0) {
        total += n;
    }
    buf[total] = '\0';
    return total;
}

/* ── Server lifecycle ───────────────────────────────────────────────── */

static pthread_t server_tid;

/* Wait for server to bind (max 2s) */
static void wait_for_bind(KlServer *s) {
    for (int i = 0; i < 200 && s->bound_port == 0; i++) usleep(10000);
}

static void start_server(void) {
    KlConfig cfg = {.port = 0, .max_body_size = 4096};
    kl_server_init(&test_server, &cfg);
    kl_server_route(&test_server, "GET", "/hello", handle_hello, NULL, NULL);
    kl_server_route(&test_server, "POST", "/echo", handle_echo,
                    (void *)(size_t)(64 * 1024), /* 64KB max */
                    kl_body_reader_buffer);
    kl_server_route(&test_server, "POST", "/upload", handle_upload,
                    NULL, kl_body_reader_multipart);
    /* Streaming variant: same handler, same body reader, but registered
     * with kl_server_route_streaming so the dispatch path invokes the
     * handler after body-reader setup BEFORE on_complete. The handler
     * itself doesn't yield (kl_multipart_next never returns NEED_DATA
     * because the body arrives in one chunk in this test), so the
     * observable behaviour is the same response shape — the dispatch
     * path is exercised. */
    kl_server_route_streaming(&test_server, "POST", "/upload-stream",
                               handle_upload, NULL, kl_body_reader_multipart);
    /* Mid-stream early-exit test route (Keel v2.1.1). */
    kl_server_route_streaming(&test_server, "POST", "/early-exit",
                               handle_early_exit, NULL, eer_factory);
    /* Mid-stream early-exit on body-reader-ERROR-return route (v2.1.2). */
    kl_server_route_streaming(&test_server, "POST", "/early-exit-err",
                               handle_early_exit_err, NULL, eer_err_factory);
    /* Streaming-async early-invoke route (v2.2.0) — handler runs BEFORE
     * leftover is fed via on_data, closing the single-read leftover-
     * cap gap. */
    kl_server_route_streaming_async(&test_server, "POST", "/early-exit-async-err",
                                      handle_early_exit_async_err, NULL,
                                      eer_err_factory);
    kl_server_route(&test_server, "GET", "/users/:id", handle_param,
                    NULL, NULL);
    kl_server_route(&test_server, "POST", "/no-reader", handle_no_reader,
                    NULL, NULL);
    pthread_create(&server_tid, NULL, server_thread, NULL);
    wait_for_bind(&test_server);
    test_port = test_server.bound_port;
}

static void stop_server(void) {
    kl_server_stop(&test_server);
    pthread_join(server_tid, NULL);
    kl_server_free(&test_server);
}

/* ── Tests ──────────────────────────────────────────────────────────── */

UTEST(integration, hello_world) {
    start_server();

    int fd = connect_to(test_port);
    ASSERT_TRUE(fd >= 0);

    const char *req = "GET /hello HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Connection: close\r\n"
                      "\r\n";
    (void)write(fd, req, strlen(req));

    char buf[4096];
    read_response(fd, buf, sizeof(buf));
    close(fd);

    ASSERT_TRUE(strstr(buf, "HTTP/1.1 200 OK") != NULL);
    ASSERT_TRUE(strstr(buf, "{\"msg\":\"hello\"}") != NULL);

    stop_server();
}

UTEST(integration, post_large_body) {
    start_server();

    int fd = connect_to(test_port);
    ASSERT_TRUE(fd >= 0);

    /* Build a body > 8KB to exercise the READING_BODY sliding window */
    size_t body_len = 16384;
    char *body = malloc(body_len);
    ASSERT_TRUE(body != NULL);
    memset(body, 'X', body_len);

    char hdr[256];
    snprintf(hdr, sizeof(hdr),
             "POST /echo HTTP/1.1\r\n"
             "Host: localhost\r\n"
             "Content-Length: %zu\r\n"
             "Connection: close\r\n"
             "\r\n", body_len);
    (void)write(fd, hdr, strlen(hdr));
    (void)write(fd, body, body_len);

    char buf[32768];
    ssize_t total = read_response(fd, buf, sizeof(buf));
    close(fd);

    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);

    /* Find body start (after \r\n\r\n) */
    char *body_start = strstr(buf, "\r\n\r\n");
    ASSERT_TRUE(body_start != NULL);
    body_start += 4;
    size_t resp_body_len = (size_t)(total - (body_start - buf));
    ASSERT_EQ(resp_body_len, body_len);
    ASSERT_TRUE(memcmp(body_start, body, body_len) == 0);

    free(body);
    stop_server();
}

UTEST(integration, post_413) {
    start_server();

    int fd = connect_to(test_port);
    ASSERT_TRUE(fd >= 0);

    /* Send POST exceeding 64KB limit */
    const char *hdr = "POST /echo HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Content-Length: 100000\r\n"
                      "Connection: close\r\n"
                      "\r\n";
    (void)write(fd, hdr, strlen(hdr));

    /* Send some data to trigger the body reader */
    char chunk[8192];
    memset(chunk, 'A', sizeof(chunk));
    for (int i = 0; i < 15; i++)
        (void)write(fd, chunk, sizeof(chunk));

    char buf[4096];
    read_response(fd, buf, sizeof(buf));
    close(fd);

    ASSERT_TRUE(strstr(buf, "413") != NULL);

    stop_server();
}

UTEST(integration, keepalive_post) {
    start_server();

    int fd = connect_to(test_port);
    ASSERT_TRUE(fd >= 0);

    /* First POST on keep-alive connection */
    const char *req1 = "POST /echo HTTP/1.1\r\n"
                       "Host: localhost\r\n"
                       "Content-Length: 5\r\n"
                       "\r\n"
                       "first";
    (void)write(fd, req1, strlen(req1));

    /* Read first response — need to parse Content-Length to know when done */
    char buf[8192];
    ssize_t total = 0;
    ssize_t n;
    /* Read until we have the full first response */
    while ((n = read(fd, buf + total, sizeof(buf) - (size_t)total - 1)) > 0) {
        total += n;
        buf[total] = '\0';
        /* Check if we have a complete response */
        char *body_start = strstr(buf, "\r\n\r\n");
        if (body_start) {
            body_start += 4;
            char *cl = strstr(buf, "Content-Length:");
            if (cl) {
                size_t cl_val = (size_t)strtol(cl + 15, NULL, 10);
                size_t body_received = (size_t)(total - (body_start - buf));
                if (body_received >= cl_val) break;
            }
        }
    }

    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);
    ASSERT_TRUE(strstr(buf, "first") != NULL);

    /* Second POST on same connection */
    const char *req2 = "POST /echo HTTP/1.1\r\n"
                       "Host: localhost\r\n"
                       "Content-Length: 6\r\n"
                       "Connection: close\r\n"
                       "\r\n"
                       "second";
    (void)write(fd, req2, strlen(req2));

    char buf2[8192];
    read_response(fd, buf2, sizeof(buf2));
    close(fd);

    ASSERT_TRUE(strstr(buf2, "200 OK") != NULL);
    ASSERT_TRUE(strstr(buf2, "second") != NULL);

    stop_server();
}

UTEST(integration, empty_body) {
    start_server();

    int fd = connect_to(test_port);
    ASSERT_TRUE(fd >= 0);

    const char *req = "POST /echo HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Content-Length: 0\r\n"
                      "Connection: close\r\n"
                      "\r\n";
    (void)write(fd, req, strlen(req));

    char buf[4096];
    read_response(fd, buf, sizeof(buf));
    close(fd);

    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);
    ASSERT_TRUE(strstr(buf, "no body") != NULL);

    stop_server();
}

UTEST(integration, expect_100_continue) {
    start_server();

    int fd = connect_to(test_port);
    ASSERT_TRUE(fd >= 0);

    /* Send headers with Expect: 100-continue */
    const char *hdr = "POST /echo HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Content-Length: 11\r\n"
                      "Expect: 100-continue\r\n"
                      "Connection: close\r\n"
                      "\r\n";
    (void)write(fd, hdr, strlen(hdr));

    /* Read the 100 Continue interim response */
    char buf[4096];
    ssize_t n = 0;
    /* Wait for 100 Continue */
    for (int i = 0; i < 20 && n == 0; i++) {
        usleep(50000);
        ssize_t r = read(fd, buf + n, sizeof(buf) - (size_t)n - 1);
        if (r > 0) n += r;
    }
    buf[n] = '\0';
    ASSERT_TRUE(strstr(buf, "100 Continue") != NULL);

    /* Now send the body */
    (void)write(fd, "hello world", 11);

    /* Read the final response */
    char buf2[4096];
    ssize_t total = read_response(fd, buf2, sizeof(buf2));
    close(fd);
    (void)total;

    ASSERT_TRUE(strstr(buf2, "200 OK") != NULL);
    ASSERT_TRUE(strstr(buf2, "hello world") != NULL);

    stop_server();
}

UTEST(integration, head_request) {
    start_server();

    int fd = connect_to(test_port);
    ASSERT_TRUE(fd >= 0);

    const char *req = "HEAD /hello HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Connection: close\r\n"
                      "\r\n";
    (void)write(fd, req, strlen(req));

    char buf[4096];
    read_response(fd, buf, sizeof(buf));
    close(fd);

    /* Should get 200 with Content-Length but no body */
    ASSERT_TRUE(strstr(buf, "HTTP/1.1 200 OK") != NULL);
    ASSERT_TRUE(strstr(buf, "Content-Length: 15") != NULL);
    ASSERT_TRUE(strstr(buf, "application/json") != NULL);

    /* No body after headers */
    char *body_start = strstr(buf, "\r\n\r\n");
    ASSERT_TRUE(body_start != NULL);
    body_start += 4;
    ASSERT_EQ(strlen(body_start), (size_t)0);

    stop_server();
}

/* ── Access log test ────────────────────────────────────────────────── */

typedef struct {
    char method[16];
    char path[64];
    int status;
    size_t body_bytes;
    double duration_ms;
    int call_count;
} AccessLogCapture;

static void test_access_log_fn(const KlRequest *req, int status,
                                size_t body_bytes, double duration_ms,
                                void *user_data) {
    AccessLogCapture *cap = user_data;
    size_t mlen = req->method_len < sizeof(cap->method) - 1
                  ? req->method_len : sizeof(cap->method) - 1;
    memcpy(cap->method, req->method, mlen);
    cap->method[mlen] = '\0';
    size_t plen = req->path_len < sizeof(cap->path) - 1
                  ? req->path_len : sizeof(cap->path) - 1;
    memcpy(cap->path, req->path, plen);
    cap->path[plen] = '\0';
    cap->status = status;
    cap->body_bytes = body_bytes;
    cap->duration_ms = duration_ms;
    cap->call_count++;
}

static KlServer log_server;
static AccessLogCapture log_capture;

static void *log_server_thread(void *arg) {
    (void)arg;
    kl_server_run(&log_server);
    return NULL;
}

UTEST(integration, access_log) {
    memset(&log_capture, 0, sizeof(log_capture));
    KlConfig cfg = {
        .port = 0,
        .access_log = test_access_log_fn,
        .access_log_data = &log_capture,
    };
    kl_server_init(&log_server, &cfg);
    kl_server_route(&log_server, "GET", "/hello", handle_hello, NULL, NULL);

    pthread_t tid;
    pthread_create(&tid, NULL, log_server_thread, NULL);
    wait_for_bind(&log_server);
    int log_port = log_server.bound_port;

    int fd = connect_to(log_port);
    ASSERT_TRUE(fd >= 0);

    const char *req = "GET /hello HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Connection: close\r\n"
                      "\r\n";
    (void)write(fd, req, strlen(req));

    char buf[4096];
    read_response(fd, buf, sizeof(buf));
    close(fd);

    /* Wait briefly for the log callback to fire (it fires after send completes) */
    usleep(50000);

    kl_server_stop(&log_server);
    pthread_join(tid, NULL);
    kl_server_free(&log_server);

    ASSERT_EQ(1, log_capture.call_count);
    ASSERT_STREQ("GET", log_capture.method);
    ASSERT_STREQ("/hello", log_capture.path);
    ASSERT_EQ(200, log_capture.status);
    ASSERT_EQ((size_t)15, log_capture.body_bytes);
    ASSERT_TRUE(log_capture.duration_ms >= 0.0);
}

UTEST(integration, multipart_upload) {
    start_server();

    int fd = connect_to(test_port);
    ASSERT_TRUE(fd >= 0);

    const char *body =
        "--TESTBND\r\n"
        "Content-Disposition: form-data; name=\"field1\"\r\n"
        "\r\n"
        "value1"
        "\r\n--TESTBND\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"test.txt\"\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "file data here"
        "\r\n--TESTBND--\r\n";
    size_t body_len = strlen(body);

    char hdr[512];
    snprintf(hdr, sizeof(hdr),
             "POST /upload HTTP/1.1\r\n"
             "Host: localhost\r\n"
             "Content-Type: multipart/form-data; boundary=TESTBND\r\n"
             "Content-Length: %zu\r\n"
             "Connection: close\r\n"
             "\r\n", body_len);
    (void)write(fd, hdr, strlen(hdr));
    (void)write(fd, body, body_len);

    char buf[4096];
    read_response(fd, buf, sizeof(buf));
    close(fd);

    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);
    ASSERT_TRUE(strstr(buf, "parts=2") != NULL);
    ASSERT_TRUE(strstr(buf, "name0=field1") != NULL);
    ASSERT_TRUE(strstr(buf, "name1=file") != NULL);
    ASSERT_TRUE(strstr(buf, "len1=14") != NULL);

    stop_server();
}

UTEST(integration, streaming_mid_stream_early_exit) {
    /* Synthetic streaming handler that "yields" at dispatch time and
     * "wakes" inside on_data to send a 200 response BEFORE the body
     * is fully received. Without the v2.1.1 mid-stream early-exit
     * check in connection.c's READING_BODY, Keel would force the
     * state back to READING_BODY after on_data, ignoring the
     * handler's SENDING transition, and the response would never be
     * sent (the test would hang).
     *
     * Body is sent in TWO writes with a small sleep between so the
     * server processes them in two separate reads. The dispatcher
     * runs the handler after the leftover from write 1 has been
     * fed via on_data (NULL conn — no-op). Write 2 arrives later,
     * fires on_data with conn+res now set, triggers the response
     * mid-stream, and the early-exit branch returns SENDING. */
    start_server();

    int fd = connect_to(test_port);
    ASSERT_TRUE(fd >= 0);

    KlAllocator alloc = kl_allocator_default();

    /* Sizes scoped to the test, expressed at the call site. The
     * announced Content-Length is larger than the two chunks sum so
     * the body genuinely never completes — proves the early-exit
     * branch doesn't rely on end-of-body. */
    size_t chunk1_len       = 64;
    size_t chunk2_len       = 64;
    size_t announced_cl_len = chunk1_len + chunk2_len + 1024;
    size_t hdr_cap          = 256;
    size_t resp_cap         = 512;

    /* Reset the handler-ran flag so the poll below has clean state. */
    eer_handler_called = 0;

    char *hdr = kl_malloc(&alloc, hdr_cap);
    ASSERT_TRUE(hdr != NULL);
    int hdr_len = snprintf(hdr, hdr_cap,
        "POST /early-exit HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n", announced_cl_len);
    ASSERT_TRUE(hdr_len > 0);

    char *chunk1 = kl_malloc(&alloc, chunk1_len);
    char *chunk2 = kl_malloc(&alloc, chunk2_len);
    ASSERT_TRUE(chunk1 != NULL);
    ASSERT_TRUE(chunk2 != NULL);
    memset(chunk1, 'A', chunk1_len);
    memset(chunk2, 'B', chunk2_len);

    /* Write 1: header + chunk1. Server's first read consumes both;
     * leftover (chunk1) is fed via on_data with NULL conn → no-op.
     * Then handler runs, stashes conn+res, yields. */
    (void)write(fd, hdr, (size_t)hdr_len);
    (void)write(fd, chunk1, chunk1_len);

    /* Wait for the handler to have run before sending chunk2 — same
     * polling pattern as wait_for_bind() above. Avoids fixed-sleep
     * flakiness on loaded CI. ~2-second cap covers slow runners; the
     * normal-case wait is much shorter (a few ms). */
    for (int i = 0; i < 200 && !eer_handler_called; i++) usleep(10000);
    ASSERT_TRUE(eer_handler_called);

    /* Write 2: chunk2. Arrives in READING_BODY state, on_data fires
     * with conn+res NOW set → response goes out, state = SENDING.
     * The early-exit branch returns SENDING instead of forcing
     * READING_BODY. */
    (void)write(fd, chunk2, chunk2_len);

    char *buf = kl_malloc(&alloc, resp_cap);
    ASSERT_TRUE(buf != NULL);
    read_response(fd, buf, resp_cap);
    close(fd);

    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);
    ASSERT_TRUE(strstr(buf, "early exit") != NULL);

    kl_free(&alloc, hdr,    hdr_cap);
    kl_free(&alloc, chunk1, chunk1_len);
    kl_free(&alloc, chunk2, chunk2_len);
    kl_free(&alloc, buf,    resp_cap);

    stop_server();
}

UTEST(integration, streaming_mid_stream_early_exit_on_error) {
    /* v2.1.2 — sibling of streaming_mid_stream_early_exit for the
     * body-reader-returns-(-1) path. The fixture's on_data writes a
     * structured handler response (413 + "cap exceeded — handler
     * caught"), sets state = SENDING, and returns -1 to simulate a
     * cap-exceeded rejection. The new early-exit branch in
     * kl_conn_on_readable's rc<0 / pr==KL_PARSE_ERROR clauses must
     * see SENDING and return it without overwriting state with
     * KL_CONN_CLOSED + the hardcoded kl_413_response. The test
     * asserts on the handler-supplied body, not the hardcoded one. */
    start_server();

    int fd = connect_to(test_port);
    ASSERT_TRUE(fd >= 0);

    KlAllocator alloc = kl_allocator_default();
    size_t chunk1_len       = 64;
    size_t chunk2_len       = 64;
    size_t announced_cl_len = chunk1_len + chunk2_len + 1024;
    size_t hdr_cap          = 256;
    size_t resp_cap         = 512;

    eer_err_handler_called = 0;

    char *hdr = kl_malloc(&alloc, hdr_cap);
    ASSERT_TRUE(hdr != NULL);
    int hdr_len = snprintf(hdr, hdr_cap,
        "POST /early-exit-err HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n", announced_cl_len);
    ASSERT_TRUE(hdr_len > 0);

    char *chunk1 = kl_malloc(&alloc, chunk1_len);
    char *chunk2 = kl_malloc(&alloc, chunk2_len);
    ASSERT_TRUE(chunk1 != NULL);
    ASSERT_TRUE(chunk2 != NULL);
    memset(chunk1, 'A', chunk1_len);
    memset(chunk2, 'B', chunk2_len);

    /* Same two-write sync pattern as the success-path test — handler
     * runs after chunk1's leftover, stashes conn+res, yields. */
    (void)write(fd, hdr, (size_t)hdr_len);
    (void)write(fd, chunk1, chunk1_len);

    for (int i = 0; i < 200 && !eer_err_handler_called; i++) usleep(10000);
    ASSERT_TRUE(eer_err_handler_called);

    /* Chunk 2 arrives in READING_BODY. on_data fires with conn+res
     * set, writes the 413 response, sets state = SENDING, returns -1.
     * Without the fix Keel would write the hardcoded 413 + close;
     * with the fix the handler's "cap exceeded" body lands. */
    (void)write(fd, chunk2, chunk2_len);

    char *buf = kl_malloc(&alloc, resp_cap);
    ASSERT_TRUE(buf != NULL);
    read_response(fd, buf, resp_cap);
    close(fd);

    /* The handler's structured response landed — NOT the hardcoded
     * kl_413_response. Both share the "413 Payload Too Large" status
     * line (it's the standard reason phrase), so the discriminator is
     * the body: the hardcoded response has Content-Length: 0; ours
     * carries the "cap exceeded — handler caught" payload + custom
     * Content-Type: text/plain. */
    ASSERT_TRUE(strstr(buf, "413") != NULL);
    ASSERT_TRUE(strstr(buf, "Content-Type: text/plain") != NULL);
    ASSERT_TRUE(strstr(buf, "cap exceeded") != NULL);
    ASSERT_TRUE(strstr(buf, "Content-Length: 31") != NULL);

    kl_free(&alloc, hdr,    hdr_cap);
    kl_free(&alloc, chunk1, chunk1_len);
    kl_free(&alloc, chunk2, chunk2_len);
    kl_free(&alloc, buf,    resp_cap);

    stop_server();
}

UTEST(integration, streaming_async_single_read_leftover_cap) {
    /* v2.2.0 — closes the single-read leftover-rejection gap that
     * v2.1.2's fix left behind. With the legacy
     * kl_server_route_streaming, a body that arrives in the SAME
     * kernel read as the header gets processed as "leftover" inside
     * conn_dispatch_request BEFORE the handler runs — so on_data fires
     * with the body reader's conn+res still unstashed, the safety
     * guard kicks in (returns 0), and the cap goes silently undetected.
     *
     * The new kl_server_route_streaming_async dispatches the handler
     * FIRST. The handler stashes conn+res and parks (state ==
     * READING_BODY). Leftover processing then fires on_data with the
     * fixture's conn+res set, which writes the 413 + sets state =
     * SENDING, returns -1, and the v2.1.2 state-honoring branch in
     * the leftover path returns SENDING instead of clobbering with
     * the hardcoded kl_413_response. */
    start_server();

    int fd = connect_to(test_port);
    ASSERT_TRUE(fd >= 0);

    KlAllocator alloc = kl_allocator_default();
    size_t body_len         = 64;
    size_t announced_cl_len = body_len + 1024;
    size_t total_cap        = 1024;
    size_t resp_cap         = 512;

    eer_async_err_handler_called = 0;

    char *packet = kl_malloc(&alloc, total_cap);
    ASSERT_TRUE(packet != NULL);
    int hdr_len = snprintf(packet, total_cap,
        "POST /early-exit-async-err HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n", announced_cl_len);
    ASSERT_TRUE(hdr_len > 0);
    ASSERT_TRUE((size_t)hdr_len + body_len < total_cap);

    /* Body bytes immediately after the header — this is the critical
     * single-write/single-read shape. Without v2.2.0's reorder, these
     * bytes get fed to on_data BEFORE the handler runs and the cap
     * goes undetected. */
    memset(packet + hdr_len, 'A', body_len);

    /* Single write — packetised together by TCP, single read on the
     * server side (in the common case; the asserts below tolerate the
     * rare split). */
    (void)write(fd, packet, (size_t)hdr_len + body_len);

    char *buf = kl_malloc(&alloc, resp_cap);
    ASSERT_TRUE(buf != NULL);
    read_response(fd, buf, resp_cap);
    close(fd);

    /* Handler must have run (proves the reorder fired) AND the
     * structured 413 response must have landed (proves the state-
     * honoring branch picked it up over the hardcoded one). */
    ASSERT_TRUE(eer_async_err_handler_called);
    ASSERT_TRUE(strstr(buf, "413") != NULL);
    ASSERT_TRUE(strstr(buf, "Content-Type: text/plain") != NULL);
    ASSERT_TRUE(strstr(buf, "cap exceeded") != NULL);
    ASSERT_TRUE(strstr(buf, "Content-Length: 31") != NULL);

    kl_free(&alloc, packet, total_cap);
    kl_free(&alloc, buf,    resp_cap);

    stop_server();
}

UTEST(integration, streaming_route_no_body_runs_handler) {
    /* L3 regression: CL:0 + streaming route — handler must still be
     * invoked. Its body_reader is NULL (no factory call since
     * has_body=0), so handle_upload responds 400 "No reader". The
     * test verifies the handler RAN (we got 400, not 500 or hang). */
    start_server();

    int fd = connect_to(test_port);
    ASSERT_TRUE(fd >= 0);

    const char *req =
        "POST /upload-stream HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n"
        "\r\n";
    (void)write(fd, req, strlen(req));

    char buf[1024];
    read_response(fd, buf, sizeof(buf));
    close(fd);

    /* Handler ran (saw NULL body_reader) and emitted 400 "No reader". */
    ASSERT_TRUE(strstr(buf, "400 ") != NULL);
    ASSERT_TRUE(strstr(buf, "No reader") != NULL);

    stop_server();
}

UTEST(integration, multipart_upload_streaming_route) {
    /* Same payload + handler as the non-streaming multipart_upload
     * test, but routed through /upload-stream which is registered with
     * kl_server_route_streaming. Exercises the early-dispatch code
     * path in conn_dispatch_request and the on_complete-skip-post-
     * middleware branch.
     *
     * NOTE: handle_upload is a plain C handler with no yield mechanism.
     * If it gets KL_MP_EVT_NEED_DATA it errors out with 400 (since
     * yielding requires a coroutine, which lives in Hull, not Keel).
     * To exercise the happy path here, send headers + body in one
     * write so the entire body sits in the leftover buffer by the
     * time the dispatcher invokes the handler — kl_multipart_next
     * never has to return NEED_DATA. Hull's bindings will provide
     * the yield path. */
    start_server();

    int fd = connect_to(test_port);
    ASSERT_TRUE(fd >= 0);

    const char *body =
        "--TESTBND\r\n"
        "Content-Disposition: form-data; name=\"field1\"\r\n"
        "\r\n"
        "value1"
        "\r\n--TESTBND\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"test.txt\"\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "file data here"
        "\r\n--TESTBND--\r\n";
    size_t body_len = strlen(body);

    char hdr[512];
    int hdr_len = snprintf(hdr, sizeof(hdr),
             "POST /upload-stream HTTP/1.1\r\n"
             "Host: localhost\r\n"
             "Content-Type: multipart/form-data; boundary=TESTBND\r\n"
             "Content-Length: %zu\r\n"
             "Connection: close\r\n"
             "\r\n", body_len);
    ASSERT_TRUE(hdr_len > 0);

    /* Combined single write: headers + body in one packet so the body
     * is in the leftover buffer when the streaming dispatcher fires.
     * Allocate exactly what's needed — no magic-sized fixed buffer. */
    KlAllocator alloc = kl_allocator_default();
    size_t combined_len = (size_t)hdr_len + body_len;
    char *combined = kl_malloc(&alloc, combined_len);
    ASSERT_TRUE(combined != NULL);
    memcpy(combined, hdr, (size_t)hdr_len);
    memcpy(combined + hdr_len, body, body_len);
    (void)write(fd, combined, combined_len);
    kl_free(&alloc, combined, combined_len);

    char buf[4096];
    read_response(fd, buf, sizeof(buf));
    close(fd);

    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);
    ASSERT_TRUE(strstr(buf, "parts=2") != NULL);
    ASSERT_TRUE(strstr(buf, "name0=field1") != NULL);
    ASSERT_TRUE(strstr(buf, "name1=file") != NULL);
    ASSERT_TRUE(strstr(buf, "len1=14") != NULL);

    stop_server();
}

/* ── Chunked transfer-encoding tests ────────────────────────────────── */

UTEST(integration, chunked_post) {
    start_server();

    int fd = connect_to(test_port);
    ASSERT_TRUE(fd >= 0);

    const char *req = "POST /echo HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Transfer-Encoding: chunked\r\n"
                      "Connection: close\r\n"
                      "\r\n"
                      "5\r\nhello\r\n0\r\n\r\n";
    (void)write(fd, req, strlen(req));

    char buf[4096];
    read_response(fd, buf, sizeof(buf));
    close(fd);

    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);
    ASSERT_TRUE(strstr(buf, "hello") != NULL);

    stop_server();
}

UTEST(integration, chunked_multi_chunk) {
    start_server();

    int fd = connect_to(test_port);
    ASSERT_TRUE(fd >= 0);

    const char *req = "POST /echo HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Transfer-Encoding: chunked\r\n"
                      "Connection: close\r\n"
                      "\r\n"
                      "5\r\nhello\r\n"
                      "1\r\n \r\n"
                      "5\r\nworld\r\n"
                      "0\r\n\r\n";
    (void)write(fd, req, strlen(req));

    char buf[4096];
    read_response(fd, buf, sizeof(buf));
    close(fd);

    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);
    ASSERT_TRUE(strstr(buf, "hello world") != NULL);

    stop_server();
}

UTEST(integration, chunked_keepalive) {
    start_server();

    int fd = connect_to(test_port);
    ASSERT_TRUE(fd >= 0);

    /* First request: chunked POST */
    const char *req1 = "POST /echo HTTP/1.1\r\n"
                       "Host: localhost\r\n"
                       "Transfer-Encoding: chunked\r\n"
                       "\r\n"
                       "5\r\nfirst\r\n0\r\n\r\n";
    (void)write(fd, req1, strlen(req1));

    /* Read first response */
    char buf[8192];
    ssize_t total = 0;
    ssize_t n;
    while ((n = read(fd, buf + total, sizeof(buf) - (size_t)total - 1)) > 0) {
        total += n;
        buf[total] = '\0';
        char *body_start = strstr(buf, "\r\n\r\n");
        if (body_start) {
            body_start += 4;
            char *cl = strstr(buf, "Content-Length:");
            if (cl) {
                size_t cl_val = (size_t)strtol(cl + 15, NULL, 10);
                size_t body_received = (size_t)(total - (body_start - buf));
                if (body_received >= cl_val) break;
            }
        }
    }

    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);
    ASSERT_TRUE(strstr(buf, "first") != NULL);

    /* Second request: plain GET on same connection */
    const char *req2 = "GET /hello HTTP/1.1\r\n"
                       "Host: localhost\r\n"
                       "Connection: close\r\n"
                       "\r\n";
    (void)write(fd, req2, strlen(req2));

    char buf2[4096];
    read_response(fd, buf2, sizeof(buf2));
    close(fd);

    ASSERT_TRUE(strstr(buf2, "200 OK") != NULL);
    ASSERT_TRUE(strstr(buf2, "{\"msg\":\"hello\"}") != NULL);

    stop_server();
}

UTEST(integration, chunked_100_continue) {
    start_server();

    int fd = connect_to(test_port);
    ASSERT_TRUE(fd >= 0);

    /* Send headers with Expect: 100-continue and chunked TE */
    const char *hdr = "POST /echo HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Transfer-Encoding: chunked\r\n"
                      "Expect: 100-continue\r\n"
                      "Connection: close\r\n"
                      "\r\n";
    (void)write(fd, hdr, strlen(hdr));

    /* Wait for 100 Continue */
    char buf[4096];
    ssize_t rn = 0;
    for (int i = 0; i < 20 && rn == 0; i++) {
        usleep(50000);
        ssize_t r = read(fd, buf + rn, sizeof(buf) - (size_t)rn - 1);
        if (r > 0) rn += r;
    }
    buf[rn] = '\0';
    ASSERT_TRUE(strstr(buf, "100 Continue") != NULL);

    /* Send chunked body */
    const char *body = "b\r\nhello world\r\n0\r\n\r\n";
    (void)write(fd, body, strlen(body));

    char buf2[4096];
    read_response(fd, buf2, sizeof(buf2));
    close(fd);

    ASSERT_TRUE(strstr(buf2, "200 OK") != NULL);
    ASSERT_TRUE(strstr(buf2, "hello world") != NULL);

    stop_server();
}

UTEST(integration, chunked_empty_body) {
    start_server();

    int fd = connect_to(test_port);
    ASSERT_TRUE(fd >= 0);

    const char *req = "POST /echo HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Transfer-Encoding: chunked\r\n"
                      "Connection: close\r\n"
                      "\r\n"
                      "0\r\n\r\n";
    (void)write(fd, req, strlen(req));

    char buf[4096];
    read_response(fd, buf, sizeof(buf));
    close(fd);

    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);
    ASSERT_TRUE(strstr(buf, "no body") != NULL);

    stop_server();
}

UTEST(integration, chunked_large_body) {
    start_server();

    int fd = connect_to(test_port);
    ASSERT_TRUE(fd >= 0);

    /* Send headers */
    const char *hdr = "POST /echo HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Transfer-Encoding: chunked\r\n"
                      "Connection: close\r\n"
                      "\r\n";
    (void)write(fd, hdr, strlen(hdr));

    /* Send 16KB in 1KB chunks */
    char chunk_data[1024];
    memset(chunk_data, 'Z', sizeof(chunk_data));

    for (int i = 0; i < 16; i++) {
        char chunk_hdr[16];
        int hlen = snprintf(chunk_hdr, sizeof(chunk_hdr), "400\r\n");
        (void)write(fd, chunk_hdr, (size_t)hlen);
        (void)write(fd, chunk_data, sizeof(chunk_data));
        (void)write(fd, "\r\n", 2);
    }
    (void)write(fd, "0\r\n\r\n", 5);

    char buf[32768];
    ssize_t total = read_response(fd, buf, sizeof(buf));
    close(fd);

    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);

    /* Find body start */
    char *body_start = strstr(buf, "\r\n\r\n");
    ASSERT_TRUE(body_start != NULL);
    body_start += 4;
    size_t resp_body_len = (size_t)(total - (body_start - buf));
    ASSERT_EQ(resp_body_len, (size_t)(16 * 1024));

    /* Verify all bytes are 'Z' */
    int all_z = 1;
    for (size_t j = 0; j < resp_body_len; j++) {
        if (body_start[j] != 'Z') { all_z = 0; break; }
    }
    ASSERT_TRUE(all_z);

    stop_server();
}

/* ── Signal handling test ───────────────────────────────────────────── */

static KlServer signal_server;

static void *signal_server_thread(void *arg) {
    (void)arg;
    kl_server_run(&signal_server);
    return NULL;
}

UTEST(integration, signal_stop) {
    KlConfig cfg = {
        .port = 0,
        .install_signal_handlers = 1,
    };
    kl_server_init(&signal_server, &cfg);
    kl_server_route(&signal_server, "GET", "/hello", handle_hello, NULL, NULL);

    pthread_t tid;
    pthread_create(&tid, NULL, signal_server_thread, NULL);
    wait_for_bind(&signal_server);
    int sig_port = signal_server.bound_port;

    /* Verify server is up */
    int fd = connect_to(sig_port);
    ASSERT_TRUE(fd >= 0);
    const char *req = "GET /hello HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Connection: close\r\n"
                      "\r\n";
    (void)write(fd, req, strlen(req));
    char buf[4096];
    read_response(fd, buf, sizeof(buf));
    close(fd);
    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);

    /* Send SIGTERM to ourselves — handler should stop the server */
    kill(getpid(), SIGTERM);
    pthread_join(tid, NULL);
    kl_server_free(&signal_server);

    /* If we get here, the server exited cleanly */
    ASSERT_TRUE(1);
}

/* ── Middleware integration tests ────────────────────────────────────── */

static int cors_middleware(KlRequest *req, KlResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_response_header(res, "Access-Control-Allow-Origin", "*");
    return 0;
}

static int auth_middleware(KlRequest *req, KlResponse *res, void *ctx) {
    const char *secret = ctx;
    size_t tok_len;
    const char *tok = kl_request_header_len(req, "Authorization", &tok_len);
    size_t secret_len = strlen(secret);
    if (!tok || tok_len != secret_len || memcmp(tok, secret, secret_len) != 0) {
        kl_response_error(res, 401, "Unauthorized");
        return 1;
    }
    return 0;
}

static void handle_mw_hello(KlRequest *req, KlResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_response_json(res, 200, "{\"ok\":true}", 11);
}

static void handle_mw_echo(KlRequest *req, KlResponse *res, void *ctx) {
    (void)ctx;
    KlBufReader *br = (KlBufReader *)req->body_reader;
    kl_response_status(res, 200);
    kl_response_header(res, "Content-Type", "text/plain");
    if (br && br->len > 0)
        kl_response_body_borrow(res, br->data, br->len);
    else
        kl_response_body_borrow(res, "no body", 7);
}

static KlServer mw_server;

static void *mw_server_thread(void *arg) {
    (void)arg;
    kl_server_run(&mw_server);
    return NULL;
}

static pthread_t mw_server_tid;
static int mw_port;

static void start_mw_server(void) {
    KlConfig cfg = {.port = 0};
    kl_server_init(&mw_server, &cfg);

    /* Register middleware */
    kl_server_use(&mw_server, "*", "/*", cors_middleware, NULL);
    kl_server_use(&mw_server, "GET", "/api/*", auth_middleware, (void *)"secret123");

    /* Register routes */
    kl_server_route(&mw_server, "GET", "/hello", handle_mw_hello, NULL, NULL);
    kl_server_route(&mw_server, "GET", "/api/data", handle_mw_hello, NULL, NULL);
    kl_server_route(&mw_server, "POST", "/api/echo", handle_mw_echo,
                    (void *)(size_t)(64 * 1024), kl_body_reader_buffer);

    pthread_create(&mw_server_tid, NULL, mw_server_thread, NULL);
    wait_for_bind(&mw_server);
    mw_port = mw_server.bound_port;
}

static void stop_mw_server(void) {
    kl_server_stop(&mw_server);
    pthread_join(mw_server_tid, NULL);
    kl_server_free(&mw_server);
}

UTEST(integration, middleware_cors) {
    start_mw_server();

    int fd = connect_to(mw_port);
    ASSERT_TRUE(fd >= 0);

    const char *req = "GET /hello HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Connection: close\r\n"
                      "\r\n";
    (void)write(fd, req, strlen(req));

    char buf[4096];
    read_response(fd, buf, sizeof(buf));
    close(fd);

    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);
    ASSERT_TRUE(strstr(buf, "Access-Control-Allow-Origin: *") != NULL);
    ASSERT_TRUE(strstr(buf, "{\"ok\":true}") != NULL);

    stop_mw_server();
}

UTEST(integration, middleware_auth_reject) {
    start_mw_server();

    int fd = connect_to(mw_port);
    ASSERT_TRUE(fd >= 0);

    /* No Authorization header → should get 401 */
    const char *req = "GET /api/data HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Connection: close\r\n"
                      "\r\n";
    (void)write(fd, req, strlen(req));

    char buf[4096];
    read_response(fd, buf, sizeof(buf));
    close(fd);

    ASSERT_TRUE(strstr(buf, "401") != NULL);
    /* CORS header should still be present (runs before auth) */
    ASSERT_TRUE(strstr(buf, "Access-Control-Allow-Origin: *") != NULL);

    stop_mw_server();
}

UTEST(integration, middleware_auth_pass) {
    start_mw_server();

    int fd = connect_to(mw_port);
    ASSERT_TRUE(fd >= 0);

    const char *req = "GET /api/data HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Authorization: secret123\r\n"
                      "Connection: close\r\n"
                      "\r\n";
    (void)write(fd, req, strlen(req));

    char buf[4096];
    read_response(fd, buf, sizeof(buf));
    close(fd);

    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);
    ASSERT_TRUE(strstr(buf, "Access-Control-Allow-Origin: *") != NULL);
    ASSERT_TRUE(strstr(buf, "{\"ok\":true}") != NULL);

    stop_mw_server();
}

static int mw_add_x_first(KlRequest *req, KlResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_response_header(res, "X-First", "1");
    return 0;
}

static int mw_add_x_second(KlRequest *req, KlResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_response_header(res, "X-Second", "2");
    return 0;
}

static KlServer chain_server;

static void *chain_server_thread(void *arg) {
    (void)arg;
    kl_server_run(&chain_server);
    return NULL;
}

UTEST(integration, middleware_chain_order) {
    KlConfig cfg = {.port = 0};
    kl_server_init(&chain_server, &cfg);
    kl_server_use(&chain_server, "*", "/*", mw_add_x_first, NULL);
    kl_server_use(&chain_server, "*", "/*", mw_add_x_second, NULL);
    kl_server_route(&chain_server, "GET", "/hello", handle_mw_hello, NULL, NULL);

    pthread_t tid;
    pthread_create(&tid, NULL, chain_server_thread, NULL);
    wait_for_bind(&chain_server);

    int fd = connect_to(chain_server.bound_port);
    ASSERT_TRUE(fd >= 0);

    const char *req = "GET /hello HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Connection: close\r\n"
                      "\r\n";
    (void)write(fd, req, strlen(req));

    char buf[4096];
    read_response(fd, buf, sizeof(buf));
    close(fd);

    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);
    ASSERT_TRUE(strstr(buf, "X-First: 1") != NULL);
    ASSERT_TRUE(strstr(buf, "X-Second: 2") != NULL);

    /* Verify order: X-First appears before X-Second in response */
    char *p1 = strstr(buf, "X-First");
    char *p2 = strstr(buf, "X-Second");
    ASSERT_TRUE(p1 != NULL);
    ASSERT_TRUE(p2 != NULL);
    ASSERT_TRUE(p1 < p2);

    kl_server_stop(&chain_server);
    pthread_join(tid, NULL);
    kl_server_free(&chain_server);
}

static KlServer sc_body_server;

static void *sc_body_server_thread(void *arg) {
    (void)arg;
    kl_server_run(&sc_body_server);
    return NULL;
}

static int mw_reject_all(KlRequest *req, KlResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_response_error(res, 403, "Forbidden");
    return 1;
}

UTEST(integration, middleware_short_circuit_body) {
    KlConfig cfg = {.port = 0};
    kl_server_init(&sc_body_server, &cfg);
    kl_server_use(&sc_body_server, "*", "/*", mw_reject_all, NULL);
    kl_server_route(&sc_body_server, "POST", "/echo", handle_mw_echo,
                    (void *)(size_t)(64 * 1024), kl_body_reader_buffer);

    pthread_t tid;
    pthread_create(&tid, NULL, sc_body_server_thread, NULL);
    wait_for_bind(&sc_body_server);

    int fd = connect_to(sc_body_server.bound_port);
    ASSERT_TRUE(fd >= 0);

    /* POST with body — middleware should reject before body is read */
    const char *req = "POST /echo HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Content-Length: 5\r\n"
                      "Connection: close\r\n"
                      "\r\n"
                      "hello";
    (void)write(fd, req, strlen(req));

    char buf[4096];
    read_response(fd, buf, sizeof(buf));
    close(fd);

    ASSERT_TRUE(strstr(buf, "403") != NULL);

    kl_server_stop(&sc_body_server);
    pthread_join(tid, NULL);
    kl_server_free(&sc_body_server);
}

UTEST(integration, route_params) {
    start_server();

    int fd = connect_to(test_port);
    ASSERT_TRUE(fd >= 0);

    const char *req = "GET /users/42 HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Connection: close\r\n"
                      "\r\n";
    (void)write(fd, req, strlen(req));

    char buf[4096];
    read_response(fd, buf, sizeof(buf));
    close(fd);

    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);
    ASSERT_TRUE(strstr(buf, "\"id\":\"42\"") != NULL);
    ASSERT_TRUE(strstr(buf, "\"num_params\":1") != NULL);

    stop_server();
}

/* ── Post-body middleware integration tests ──────────────────────────── */

static int post_mw_check_body(KlRequest *req, KlResponse *res, void *ctx) {
    (void)ctx;
    KlBufReader *br = (KlBufReader *)req->body_reader;
    if (br && br->len >= 6 && memcmp(br->data, "secret", 6) == 0) {
        return 0;  /* continue to handler */
    }
    kl_response_error(res, 403, "Forbidden");
    return 1;
}

static void handle_post_mw_echo(KlRequest *req, KlResponse *res, void *ctx) {
    (void)ctx;
    KlBufReader *br = (KlBufReader *)req->body_reader;
    kl_response_status(res, 200);
    kl_response_header(res, "Content-Type", "text/plain");
    if (br && br->len > 0)
        kl_response_body_borrow(res, br->data, br->len);
    else
        kl_response_body_borrow(res, "no body", 7);
}

static KlServer post_mw_server;

static void *post_mw_server_thread(void *arg) {
    (void)arg;
    kl_server_run(&post_mw_server);
    return NULL;
}

UTEST(integration, post_middleware_body_access) {
    KlConfig cfg = {.port = 0};
    kl_server_init(&post_mw_server, &cfg);
    kl_server_use_post(&post_mw_server, "POST", "/*", post_mw_check_body, NULL);
    kl_server_route(&post_mw_server, "POST", "/submit", handle_post_mw_echo,
                    (void *)(size_t)(64 * 1024), kl_body_reader_buffer);
    kl_server_route(&post_mw_server, "GET", "/hello", handle_hello, NULL, NULL);

    pthread_t tid;
    pthread_create(&tid, NULL, post_mw_server_thread, NULL);
    wait_for_bind(&post_mw_server);
    int pmw_port = post_mw_server.bound_port;

    /* POST with valid body — should pass post-middleware */
    int fd = connect_to(pmw_port);
    ASSERT_TRUE(fd >= 0);
    const char *req1 = "POST /submit HTTP/1.1\r\n"
                       "Host: localhost\r\n"
                       "Content-Length: 11\r\n"
                       "Connection: close\r\n"
                       "\r\n"
                       "secret data";
    (void)write(fd, req1, strlen(req1));
    char buf[4096];
    read_response(fd, buf, sizeof(buf));
    close(fd);
    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);
    ASSERT_TRUE(strstr(buf, "secret data") != NULL);

    /* POST with invalid body — should be rejected by post-middleware */
    fd = connect_to(pmw_port);
    ASSERT_TRUE(fd >= 0);
    const char *req2 = "POST /submit HTTP/1.1\r\n"
                       "Host: localhost\r\n"
                       "Content-Length: 5\r\n"
                       "Connection: close\r\n"
                       "\r\n"
                       "wrong";
    (void)write(fd, req2, strlen(req2));
    read_response(fd, buf, sizeof(buf));
    close(fd);
    ASSERT_TRUE(strstr(buf, "403") != NULL);

    /* GET skips POST-only post-middleware — should succeed */
    fd = connect_to(pmw_port);
    ASSERT_TRUE(fd >= 0);
    const char *req3 = "GET /hello HTTP/1.1\r\n"
                       "Host: localhost\r\n"
                       "Connection: close\r\n"
                       "\r\n";
    (void)write(fd, req3, strlen(req3));
    read_response(fd, buf, sizeof(buf));
    close(fd);
    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);

    kl_server_stop(&post_mw_server);
    pthread_join(tid, NULL);
    kl_server_free(&post_mw_server);
}

static KlServer post_mw_ka_server;

static void *post_mw_ka_server_thread(void *arg) {
    (void)arg;
    kl_server_run(&post_mw_ka_server);
    return NULL;
}

UTEST(integration, post_middleware_keepalive_preserved) {
    KlConfig cfg = {.port = 0};
    kl_server_init(&post_mw_ka_server, &cfg);
    kl_server_use_post(&post_mw_ka_server, "POST", "/*", post_mw_check_body, NULL);
    kl_server_route(&post_mw_ka_server, "POST", "/submit", handle_post_mw_echo,
                    (void *)(size_t)(64 * 1024), kl_body_reader_buffer);
    kl_server_route(&post_mw_ka_server, "GET", "/hello", handle_hello, NULL, NULL);

    pthread_t tid;
    pthread_create(&tid, NULL, post_mw_ka_server_thread, NULL);
    wait_for_bind(&post_mw_ka_server);

    /* Post-middleware rejection should preserve keep-alive */
    int fd = connect_to(post_mw_ka_server.bound_port);
    ASSERT_TRUE(fd >= 0);

    /* First: POST rejected by post-middleware (body consumed → keep-alive OK) */
    const char *req1 = "POST /submit HTTP/1.1\r\n"
                       "Host: localhost\r\n"
                       "Content-Length: 5\r\n"
                       "\r\n"
                       "wrong";
    (void)write(fd, req1, strlen(req1));

    /* Read first response */
    char buf[8192];
    ssize_t total = 0;
    ssize_t n;
    while ((n = read(fd, buf + total, sizeof(buf) - (size_t)total - 1)) > 0) {
        total += n;
        buf[total] = '\0';
        char *body_start = strstr(buf, "\r\n\r\n");
        if (body_start) {
            body_start += 4;
            char *cl = strstr(buf, "Content-Length:");
            if (cl) {
                size_t cl_val = (size_t)strtol(cl + 15, NULL, 10);
                size_t body_received = (size_t)(total - (body_start - buf));
                if (body_received >= cl_val) break;
            }
        }
    }
    ASSERT_TRUE(strstr(buf, "403") != NULL);

    /* Second: GET on same connection — should work if keep-alive preserved */
    const char *req2 = "GET /hello HTTP/1.1\r\n"
                       "Host: localhost\r\n"
                       "Connection: close\r\n"
                       "\r\n";
    (void)write(fd, req2, strlen(req2));

    char buf2[4096];
    read_response(fd, buf2, sizeof(buf2));
    close(fd);
    ASSERT_TRUE(strstr(buf2, "200 OK") != NULL);
    ASSERT_TRUE(strstr(buf2, "{\"msg\":\"hello\"}") != NULL);

    kl_server_stop(&post_mw_ka_server);
    pthread_join(tid, NULL);
    kl_server_free(&post_mw_ka_server);
}

/* ── Global body size limit tests ─────────────────────────────────── */

UTEST(integration, global_body_limit_413) {
    start_server();

    int fd = connect_to(test_port);
    ASSERT_TRUE(fd >= 0);

    /* POST to route with no body reader, Content-Length exceeds 4KB limit */
    const char *req = "POST /no-reader HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Content-Length: 100000\r\n"
                      "Connection: close\r\n"
                      "\r\n";
    (void)write(fd, req, strlen(req));

    char buf[4096];
    read_response(fd, buf, sizeof(buf));
    close(fd);

    ASSERT_TRUE(strstr(buf, "413") != NULL);

    stop_server();
}

UTEST(integration, global_body_limit_ok) {
    start_server();

    int fd = connect_to(test_port);
    ASSERT_TRUE(fd >= 0);

    /* POST to route with no body reader, body within 4KB limit */
    char body[100];
    memset(body, 'X', sizeof(body));

    char hdr[256];
    snprintf(hdr, sizeof(hdr),
             "POST /no-reader HTTP/1.1\r\n"
             "Host: localhost\r\n"
             "Content-Length: %zu\r\n"
             "Connection: close\r\n"
             "\r\n", sizeof(body));
    (void)write(fd, hdr, strlen(hdr));
    (void)write(fd, body, sizeof(body));

    char buf[4096];
    read_response(fd, buf, sizeof(buf));
    close(fd);

    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);

    stop_server();
}

UTEST(integration, per_route_limit_unaffected) {
    start_server();

    int fd = connect_to(test_port);
    ASSERT_TRUE(fd >= 0);

    /* POST to /echo with 32KB body — has 64KB body reader, should succeed
     * even though global max_body_size is 4KB (only applies to discard path) */
    size_t body_len = 32768;
    char *body = malloc(body_len);
    ASSERT_TRUE(body != NULL);
    memset(body, 'Y', body_len);

    char hdr[256];
    snprintf(hdr, sizeof(hdr),
             "POST /echo HTTP/1.1\r\n"
             "Host: localhost\r\n"
             "Content-Length: %zu\r\n"
             "Connection: close\r\n"
             "\r\n", body_len);
    (void)write(fd, hdr, strlen(hdr));
    (void)write(fd, body, body_len);

    char buf[65536];
    ssize_t total = read_response(fd, buf, sizeof(buf));
    close(fd);

    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);

    /* Verify echoed body length */
    char *body_start = strstr(buf, "\r\n\r\n");
    ASSERT_TRUE(body_start != NULL);
    body_start += 4;
    size_t resp_body_len = (size_t)(total - (body_start - buf));
    ASSERT_EQ(resp_body_len, body_len);

    free(body);
    stop_server();
}

UTEST_MAIN();
