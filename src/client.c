/*
 * client.c — HTTP/1.1 client (sync + async)
 *
 * Sync: blocking poll()-based request/response.
 * Async: non-blocking state machine driven by KlEventCtx watchers.
 *
 * All allocation through KlAllocator. No Hull dependencies.
 */

#include <keel/client.h>
#include <keel/parser.h>

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>

/* ── CRLF injection guard ────────────────────────────────────────── */

static int has_crlf(const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\r' || s[i] == '\n')
            return 1;
    }
    return 0;
}

/* ── I/O abstraction (plain or TLS) ──────────────────────────────── */

static ssize_t io_write(int fd, KlTls *tls, const void *buf, size_t len)
{
    if (tls)
        return tls->write(tls, fd, buf, len);
    ssize_t r;
#ifdef MSG_NOSIGNAL
    do { r = send(fd, buf, len, MSG_NOSIGNAL); } while (r < 0 && errno == EINTR);
#else
    do { r = write(fd, buf, len); } while (r < 0 && errno == EINTR);
#endif
    return r;
}

static ssize_t io_read(int fd, KlTls *tls, void *buf, size_t len)
{
    if (tls)
        return tls->read(tls, fd, buf, len);
    ssize_t r;
    do { r = read(fd, buf, len); } while (r < 0 && errno == EINTR);
    return r;
}

/* ── Connect with timeout ────────────────────────────────────────── */

static int connect_with_timeout(const char *host, size_t host_len,
                                 int port, int timeout_ms)
{
    char host_buf[KL_CLIENT_HOSTNAME_MAX];
    if (host_len >= sizeof(host_buf))
        return -1;
    memcpy(host_buf, host, host_len);
    host_buf[host_len] = '\0';

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    int rc = getaddrinfo(host_buf, port_str, &hints, &res);
    if (rc != 0 || !res)
        return -1;

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return -1;
    }

#ifdef SO_NOSIGPIPE
    {
        int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
    }
#endif

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        close(fd);
        freeaddrinfo(res);
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(fd);
        freeaddrinfo(res);
        return -1;
    }

    rc = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    if (rc < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }

    if (rc < 0) {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        rc = poll(&pfd, 1, timeout_ms);
        if (rc <= 0) {
            close(fd);
            return -1;
        }

        int err = 0;
        socklen_t errlen = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen);
        if (err != 0) {
            close(fd);
            return -1;
        }
    }

    /* Restore blocking mode */
    fcntl(fd, F_SETFL, flags);

    return fd;
}

/* ── TLS handshake (sync) ────────────────────────────────────────── */

static KlTls *do_tls_handshake(int fd, KlTlsConfig *tls_cfg,
                                 const char *host, size_t host_len,
                                 int timeout_ms, KlAllocator *alloc)
{
    if (!tls_cfg || !tls_cfg->factory)
        return NULL;

    KlTls *tls = tls_cfg->factory(tls_cfg->ctx, alloc);
    if (!tls)
        return NULL;

    /* Set SNI hostname via vtable (backend-agnostic) */
    if (tls->set_hostname) {
        char host_buf[KL_CLIENT_HOSTNAME_MAX];
        if (host_len < sizeof(host_buf)) {
            memcpy(host_buf, host, host_len);
            host_buf[host_len] = '\0';
            tls->set_hostname(tls, host_buf);
        }
    }

    int elapsed = 0;
    int step = 100;

    for (;;) {
        KlTlsResult r = tls->handshake(tls, fd);
        if (r == KL_TLS_OK)
            return tls;
        if (r == KL_TLS_ERROR) {
            tls->destroy(tls);
            return NULL;
        }

        short events = (r == KL_TLS_WANT_READ) ? POLLIN : POLLOUT;
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = events;
        pfd.revents = 0;
        int pr = poll(&pfd, 1, step);
        if (pr < 0) {
            tls->destroy(tls);
            return NULL;
        }

        elapsed += step;
        if (timeout_ms > 0 && elapsed >= timeout_ms) {
            tls->destroy(tls);
            return NULL;
        }
    }
}

/* ── Build request into stack buffer, send ───────────────────────── */

static int send_request_sync(int fd, KlTls *tls,
                              const char *method, const KlUrl *url,
                              const KlClientHeader *headers, int num_headers,
                              const char *body, size_t body_len,
                              int timeout_ms)
{
    if (has_crlf(method, strlen(method)))
        return -1;

    char buf[KL_CLIENT_REQ_BUF_SIZE];
    int off = snprintf(buf, sizeof(buf), "%s %.*s HTTP/1.1\r\nHost: %.*s\r\n",
                       method,
                       (int)url->path_len, url->path,
                       (int)url->host_len, url->host);

    if (off < 0 || (size_t)off >= sizeof(buf))
        return -1;

    for (int i = 0; i < num_headers; i++) {
        if (has_crlf(headers[i].name, strlen(headers[i].name)) ||
            has_crlf(headers[i].value, strlen(headers[i].value)))
            return -1;
        int n = snprintf(buf + off, sizeof(buf) - (size_t)off,
                         "%s: %s\r\n", headers[i].name, headers[i].value);
        if (n < 0 || (size_t)(off + n) >= sizeof(buf))
            return -1;
        off += n;
    }

    if (body && body_len > 0) {
        int n = snprintf(buf + off, sizeof(buf) - (size_t)off,
                         "Content-Length: %zu\r\n", body_len);
        if (n < 0 || (size_t)(off + n) >= sizeof(buf))
            return -1;
        off += n;
    }

    int n = snprintf(buf + off, sizeof(buf) - (size_t)off,
                     "Connection: close\r\n\r\n");
    if (n < 0 || (size_t)(off + n) >= sizeof(buf))
        return -1;
    off += n;

    /* Send header block */
    size_t sent = 0;
    while (sent < (size_t)off) {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr <= 0)
            return -1;

        ssize_t w = io_write(fd, tls, buf + sent, (size_t)off - sent);
        if (w <= 0)
            return -1;
        sent += (size_t)w;
    }

    /* Send body */
    if (body && body_len > 0) {
        sent = 0;
        while (sent < body_len) {
            struct pollfd pfd;
            pfd.fd = fd;
            pfd.events = POLLOUT;
            pfd.revents = 0;
            int pr = poll(&pfd, 1, timeout_ms);
            if (pr <= 0)
                return -1;

            ssize_t w = io_write(fd, tls, body + sent, body_len - sent);
            if (w <= 0)
                return -1;
            sent += (size_t)w;
        }
    }

    return 0;
}

/* ── Receive + parse response (sync) ─────────────────────────────── */

static int recv_response_sync(int fd, KlTls *tls, KlClientResponse *resp,
                               size_t max_response_size, int timeout_ms,
                               KlAllocator *alloc)
{
    KlResponseParser *parser = kl_response_parser_llhttp(max_response_size,
                                                          alloc);
    if (!parser)
        return -1;

    char buf[KL_CLIENT_RECV_BUF_SIZE];
    int ret = -1;

    for (;;) {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr <= 0)
            break;

        ssize_t nread = io_read(fd, tls, buf, sizeof(buf));
        if (nread < 0)
            break;
        if (nread == 0) {
            if (resp->status > 0)
                ret = 0;
            break;
        }

        size_t consumed;
        KlParseResult pr2 = parser->parse(parser, resp,
                                            buf, (size_t)nread, &consumed);
        if (pr2 == KL_PARSE_OK) {
            ret = 0;
            break;
        }
        if (pr2 == KL_PARSE_ERROR)
            break;
    }

    parser->destroy(parser);
    return ret;
}

/* ── Sync public API ─────────────────────────────────────────────── */

int kl_client_request(KlAllocator *alloc, const KlClientConfig *cfg,
                      const char *method, const char *url_str,
                      const KlClientHeader *headers, int num_headers,
                      const char *body, size_t body_len,
                      KlClientResponse *resp)
{
    if (!alloc || !method || !url_str || !resp)
        return -1;
    if (num_headers < 0 || num_headers > KL_CLIENT_MAX_REQ_HEADERS)
        return -1;
    if (num_headers > 0 && !headers)
        return -1;

    memset(resp, 0, sizeof(*resp));

    int timeout_ms = (cfg && cfg->timeout_ms > 0) ? cfg->timeout_ms
                                                    : KL_CLIENT_DEFAULT_TIMEOUT_MS;
    size_t max_resp = (cfg && cfg->max_response_size > 0) ? cfg->max_response_size
                                                            : (size_t)KL_CLIENT_DEFAULT_MAX_RESP;

    KlUrl parsed;
    if (kl_url_parse(url_str, &parsed) != 0)
        return -1;

    KlTlsConfig *tls_cfg = cfg ? cfg->tls : NULL;
    if (parsed.is_https && !tls_cfg)
        return -1;
    if (!parsed.is_https)
        tls_cfg = NULL;

    int fd = connect_with_timeout(parsed.host, parsed.host_len,
                                   parsed.port, timeout_ms);
    if (fd < 0)
        return -1;

    KlTls *tls = NULL;
    int ret = -1;

    if (parsed.is_https) {
        tls = do_tls_handshake(fd, tls_cfg, parsed.host, parsed.host_len,
                                timeout_ms, alloc);
        if (!tls)
            goto cleanup;
    }

    if (send_request_sync(fd, tls, method, &parsed,
                           headers, num_headers, body, body_len,
                           timeout_ms) != 0)
        goto cleanup;

    if (recv_response_sync(fd, tls, resp, max_resp, timeout_ms, alloc) != 0)
        goto cleanup;

    ret = 0;

cleanup:
    if (tls) {
        tls->shutdown(tls, fd);
        tls->destroy(tls);
    }
    close(fd);

    if (ret != 0)
        kl_client_response_free(resp);

    return ret;
}

void kl_client_response_free(KlClientResponse *resp)
{
    if (!resp || !resp->alloc.malloc)
        return;

    if (resp->body) {
        kl_free(&resp->alloc, resp->body, resp->body_len + 1);
        resp->body = NULL;
        resp->body_len = 0;
    }

    if (resp->headers) {
        for (int i = 0; i < resp->num_headers; i++) {
            kl_free(&resp->alloc, (char *)resp->headers[i].name,
                    strlen(resp->headers[i].name) + 1);
            kl_free(&resp->alloc, (char *)resp->headers[i].value,
                    strlen(resp->headers[i].value) + 1);
        }
        kl_free(&resp->alloc, resp->headers,
                (size_t)resp->num_headers * sizeof(KlClientHeader));
        resp->headers = NULL;
    }
    resp->num_headers = 0;
    resp->status = 0;
    memset(&resp->alloc, 0, sizeof(resp->alloc));
}

/* ══════════════════════════════════════════════════════════════════════
 * Async HTTP client — state machine driven by KlEventCtx watchers
 * ══════════════════════════════════════════════════════════════════════ */

typedef enum {
    KL_HCLIENT_CONNECTING,
    KL_HCLIENT_TLS_HANDSHAKE,
    KL_HCLIENT_SENDING,
    KL_HCLIENT_RECEIVING,
    KL_HCLIENT_DONE
} KlClientState;

struct KlClient {
    int                fd;
    KlClientState      state;
    KlEventCtx        *ev_ctx;
    KlAllocator       *alloc;

    /* Request (heap-copied, owned) */
    char              *request_buf;
    size_t             request_len;
    size_t             request_sent;

    /* Response */
    KlClientResponse   resp;
    KlResponseParser  *parser;
    int                error;

    /* TLS (NULL if plain HTTP) */
    KlTls             *tls;
    KlTlsConfig       *tls_cfg;
    char               host_buf[KL_CLIENT_HOSTNAME_MAX];

    /* Completion callback */
    KlClientDoneFn     on_done;
    void              *user_data;
};

/* Forward declarations */
static void async_on_event(int fd, KlEventMask ready, void *user_data);
static void async_complete_success(KlClient *c);
static void async_complete_error(KlClient *c);
static void async_handle_tls_handshake(KlClient *c);

/* ── Build request into heap buffer ──────────────────────────────── */

static char *build_request(KlAllocator *alloc,
                            const char *method, const KlUrl *url,
                            const KlClientHeader *headers, int num_headers,
                            const char *body, size_t body_len,
                            size_t *out_len)
{
    if (has_crlf(method, strlen(method)))
        return NULL;

    char buf[KL_CLIENT_REQ_BUF_SIZE];
    int off = snprintf(buf, sizeof(buf), "%s %.*s HTTP/1.1\r\nHost: %.*s\r\n",
                       method,
                       (int)url->path_len, url->path,
                       (int)url->host_len, url->host);

    if (off < 0 || (size_t)off >= sizeof(buf))
        return NULL;

    for (int i = 0; i < num_headers; i++) {
        if (has_crlf(headers[i].name, strlen(headers[i].name)) ||
            has_crlf(headers[i].value, strlen(headers[i].value)))
            return NULL;
        int n = snprintf(buf + off, sizeof(buf) - (size_t)off,
                         "%s: %s\r\n", headers[i].name, headers[i].value);
        if (n < 0 || (size_t)(off + n) >= sizeof(buf))
            return NULL;
        off += n;
    }

    if (body && body_len > 0) {
        int n = snprintf(buf + off, sizeof(buf) - (size_t)off,
                         "Content-Length: %zu\r\n", body_len);
        if (n < 0 || (size_t)(off + n) >= sizeof(buf))
            return NULL;
        off += n;
    }

    int n = snprintf(buf + off, sizeof(buf) - (size_t)off,
                     "Connection: close\r\n\r\n");
    if (n < 0 || (size_t)(off + n) >= sizeof(buf))
        return NULL;
    off += n;

    if (body_len > SIZE_MAX - (size_t)off)
        return NULL;
    size_t total = (size_t)off + body_len;
    char *req = kl_malloc(alloc, total);
    if (!req)
        return NULL;

    memcpy(req, buf, (size_t)off);
    if (body && body_len > 0)
        memcpy(req + off, body, body_len);

    *out_len = total;
    return req;
}

/* ── State: CONNECTING ───────────────────────────────────────────── */

static void async_handle_connecting(KlClient *c)
{
    int err = 0;
    socklen_t errlen = sizeof(err);
    getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &errlen);
    if (err != 0) {
        async_complete_error(c);
        return;
    }

    if (c->tls_cfg && c->tls_cfg->factory) {
        c->tls = c->tls_cfg->factory(c->tls_cfg->ctx, c->alloc);
        if (!c->tls) {
            async_complete_error(c);
            return;
        }

        if (c->tls->set_hostname && c->host_buf[0])
            c->tls->set_hostname(c->tls, c->host_buf);

        c->state = KL_HCLIENT_TLS_HANDSHAKE;
        async_handle_tls_handshake(c);
    } else {
        c->state = KL_HCLIENT_SENDING;
        kl_watcher_mod(c->ev_ctx, c->fd, KL_EVENT_WRITE);
    }
}

/* ── State: TLS_HANDSHAKE ────────────────────────────────────────── */

static void async_handle_tls_handshake(KlClient *c)
{
    KlTlsResult r = c->tls->handshake(c->tls, c->fd);
    if (r == KL_TLS_OK) {
        c->state = KL_HCLIENT_SENDING;
        kl_watcher_mod(c->ev_ctx, c->fd, KL_EVENT_WRITE);
        return;
    }
    if (r == KL_TLS_WANT_READ) {
        kl_watcher_mod(c->ev_ctx, c->fd, KL_EVENT_READ);
        return;
    }
    if (r == KL_TLS_WANT_WRITE) {
        kl_watcher_mod(c->ev_ctx, c->fd, KL_EVENT_WRITE);
        return;
    }
    async_complete_error(c);
}

/* ── State: SENDING ──────────────────────────────────────────────── */

static void async_handle_sending(KlClient *c)
{
    while (c->request_sent < c->request_len) {
        ssize_t w = io_write(c->fd, c->tls,
                              c->request_buf + c->request_sent,
                              c->request_len - c->request_sent);
        if (w < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                kl_watcher_rearm(c->ev_ctx, c->fd);
                return;
            }
            async_complete_error(c);
            return;
        }
        if (w == 0) {
            async_complete_error(c);
            return;
        }
        c->request_sent += (size_t)w;
    }

    c->state = KL_HCLIENT_RECEIVING;
    kl_watcher_mod(c->ev_ctx, c->fd, KL_EVENT_READ);
}

/* ── State: RECEIVING ────────────────────────────────────────────── */

static void async_handle_receiving(KlClient *c)
{
    char buf[KL_CLIENT_RECV_BUF_SIZE];

    for (;;) {
        ssize_t nread = io_read(c->fd, c->tls, buf, sizeof(buf));
        if (nread < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                kl_watcher_rearm(c->ev_ctx, c->fd);
                return;
            }
            async_complete_error(c);
            return;
        }
        if (nread == 0) {
            if (c->resp.status > 0)
                async_complete_success(c);
            else
                async_complete_error(c);
            return;
        }

        size_t consumed;
        KlParseResult pr = c->parser->parse(c->parser, &c->resp,
                                              buf, (size_t)nread, &consumed);
        if (pr == KL_PARSE_OK) {
            async_complete_success(c);
            return;
        }
        if (pr == KL_PARSE_ERROR) {
            async_complete_error(c);
            return;
        }
        /* KL_PARSE_INCOMPLETE — try to read more (non-blocking) */
    }
}

/* ── KlWatcher callback ─────────────────────────────────────────── */

static void async_on_event(int fd, KlEventMask ready, void *user_data)
{
    KlClient *c = user_data;
    (void)fd;
    (void)ready;

    switch (c->state) {
    case KL_HCLIENT_CONNECTING:
        async_handle_connecting(c);
        break;
    case KL_HCLIENT_TLS_HANDSHAKE:
        async_handle_tls_handshake(c);
        break;
    case KL_HCLIENT_SENDING:
        async_handle_sending(c);
        break;
    case KL_HCLIENT_RECEIVING:
        async_handle_receiving(c);
        break;
    case KL_HCLIENT_DONE:
        break;
    }
}

/* ── Completion helpers ──────────────────────────────────────────── */

static void async_complete_success(KlClient *c)
{
    kl_watcher_del(c->ev_ctx, c->fd);

    if (c->tls) {
        c->tls->shutdown(c->tls, c->fd);
        c->tls->destroy(c->tls);
        c->tls = NULL;
    }
    close(c->fd);
    c->fd = -1;

    if (c->request_buf) {
        kl_free(c->alloc, c->request_buf, c->request_len);
        c->request_buf = NULL;
    }
    if (c->parser) {
        c->parser->destroy(c->parser);
        c->parser = NULL;
    }

    c->state = KL_HCLIENT_DONE;
    c->error = 0;
    if (c->on_done)
        c->on_done(c, c->user_data);
}

static void async_complete_error(KlClient *c)
{
    if (c->fd >= 0)
        kl_watcher_del(c->ev_ctx, c->fd);

    if (c->tls) {
        if (c->fd >= 0)
            c->tls->shutdown(c->tls, c->fd);
        c->tls->destroy(c->tls);
        c->tls = NULL;
    }

    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }

    if (c->request_buf) {
        kl_free(c->alloc, c->request_buf, c->request_len);
        c->request_buf = NULL;
    }
    if (c->parser) {
        c->parser->destroy(c->parser);
        c->parser = NULL;
    }

    c->state = KL_HCLIENT_DONE;
    c->error = -1;
    if (c->on_done)
        c->on_done(c, c->user_data);
}

/* ── Async public API ────────────────────────────────────────────── */

KlClient *kl_client_start(KlEventCtx *ev_ctx, KlAllocator *alloc,
                           const KlClientConfig *cfg,
                           const char *method, const char *url_str,
                           const KlClientHeader *headers, int num_headers,
                           const char *body, size_t body_len,
                           KlClientDoneFn on_done, void *user_data)
{
    if (!ev_ctx || !alloc || !method || !url_str)
        return NULL;
    if (num_headers < 0 || num_headers > KL_CLIENT_MAX_REQ_HEADERS)
        return NULL;
    if (num_headers > 0 && !headers)
        return NULL;

    KlUrl parsed;
    if (kl_url_parse(url_str, &parsed) != 0)
        return NULL;

    KlTlsConfig *tls_cfg = cfg ? cfg->tls : NULL;
    if (parsed.is_https && !tls_cfg)
        return NULL;
    if (!parsed.is_https)
        tls_cfg = NULL;

    size_t max_resp = (cfg && cfg->max_response_size > 0) ? cfg->max_response_size
                                                            : (size_t)KL_CLIENT_DEFAULT_MAX_RESP;

    /* Build request buffer */
    size_t req_len = 0;
    char *req_buf = build_request(alloc, method, &parsed,
                                   headers, num_headers, body, body_len,
                                   &req_len);
    if (!req_buf)
        return NULL;

    /* DNS resolve (blocking — typically fast, OS-cached) */
    char host_buf[KL_CLIENT_HOSTNAME_MAX];
    if (parsed.host_len >= sizeof(host_buf)) {
        kl_free(alloc, req_buf, req_len);
        return NULL;
    }
    memcpy(host_buf, parsed.host, parsed.host_len);
    host_buf[parsed.host_len] = '\0';

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", parsed.port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    int rc = getaddrinfo(host_buf, port_str, &hints, &res);
    if (rc != 0 || !res) {
        kl_free(alloc, req_buf, req_len);
        return NULL;
    }

    /* Create non-blocking socket */
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        kl_free(alloc, req_buf, req_len);
        return NULL;
    }

#ifdef SO_NOSIGPIPE
    {
        int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
    }
#endif

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(fd);
        freeaddrinfo(res);
        kl_free(alloc, req_buf, req_len);
        return NULL;
    }

    rc = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    if (rc < 0 && errno != EINPROGRESS) {
        close(fd);
        kl_free(alloc, req_buf, req_len);
        return NULL;
    }

    /* Allocate client */
    KlClient *c = kl_malloc(alloc, sizeof(KlClient));
    if (!c) {
        close(fd);
        kl_free(alloc, req_buf, req_len);
        return NULL;
    }
    memset(c, 0, sizeof(*c));

    c->fd = fd;
    c->state = (rc == 0) ? KL_HCLIENT_SENDING : KL_HCLIENT_CONNECTING;
    c->ev_ctx = ev_ctx;
    c->alloc = alloc;
    c->tls_cfg = tls_cfg;

    c->request_buf = req_buf;
    c->request_len = req_len;
    c->request_sent = 0;

    memcpy(c->host_buf, host_buf, parsed.host_len + 1);

    c->on_done = on_done;
    c->user_data = user_data;

    /* Create response parser */
    c->parser = kl_response_parser_llhttp(max_resp, alloc);
    if (!c->parser) {
        close(fd);
        kl_free(alloc, req_buf, req_len);
        kl_free(alloc, c, sizeof(KlClient));
        return NULL;
    }

    /* Register watcher */
    if (kl_watcher_add(ev_ctx, fd, KL_EVENT_WRITE, async_on_event, c) != 0) {
        c->parser->destroy(c->parser);
        close(fd);
        kl_free(alloc, req_buf, req_len);
        kl_free(alloc, c, sizeof(KlClient));
        return NULL;
    }

    return c;
}

const KlClientResponse *kl_client_response(const KlClient *client)
{
    if (!client || client->error != 0)
        return NULL;
    return &client->resp;
}

int kl_client_error(const KlClient *client)
{
    if (!client)
        return -1;
    return client->error;
}

void kl_client_cancel(KlClient *client)
{
    if (!client)
        return;

    if (client->fd >= 0) {
        kl_watcher_del(client->ev_ctx, client->fd);

        if (client->tls) {
            client->tls->shutdown(client->tls, client->fd);
            client->tls->destroy(client->tls);
            client->tls = NULL;
        }

        close(client->fd);
        client->fd = -1;
    }

    client->state = KL_HCLIENT_DONE;
    client->error = -1;
}

void kl_client_free(KlClient *client)
{
    if (!client)
        return;

    /* Cancel if still in-flight */
    if (client->state != KL_HCLIENT_DONE)
        kl_client_cancel(client);

    if (client->request_buf) {
        kl_free(client->alloc, client->request_buf, client->request_len);
        client->request_buf = NULL;
    }

    if (client->parser) {
        client->parser->destroy(client->parser);
        client->parser = NULL;
    }

    kl_client_response_free(&client->resp);

    KlAllocator *alloc = client->alloc;
    kl_free(alloc, client, sizeof(KlClient));
}
