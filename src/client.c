/*
 * client.c — HTTP/1.1 client (sync + async)
 *
 * Sync: blocking poll()-based request/response.
 * Async: non-blocking state machine driven by KlEventCtx watchers.
 *
 * All allocation through KlAllocator. No Hull dependencies.
 */

#include <keel/client.h>
#include <keel/client_pool.h>
#include <keel/decompress.h>
#include <keel/dns_resolver.h>
#include <keel/parser.h>
#include <keel/timer.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>

#include "socket.h"

/* ── Proxy constants ─────────────────────────────────────────────── */

#define KL_PROXY_RESPONSE_MAX 4096

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

static ssize_t io_write(const KlSocketProvider *p, int fd, KlTls *tls,
                        const void *buf, size_t len)
{
    if (tls)
        return tls->write(tls, fd, buf, len);
    return kl_sock_send(p, fd, buf, len);
}

static ssize_t io_read(const KlSocketProvider *p, int fd, KlTls *tls,
                       void *buf, size_t len)
{
    if (tls)
        return tls->read(tls, fd, buf, len);
    return kl_sock_recv(p, fd, buf, len);
}

/* ── Connect with timeout ────────────────────────────────────────── */

static int connect_with_timeout(const char *host, size_t host_len,
                                 int port, int timeout_ms,
                                 KlError *out_err)
{
    char host_buf[KL_CLIENT_HOSTNAME_MAX];
    if (host_len >= sizeof(host_buf)) {
        if (out_err) *out_err = KL_ERR_INVALID_ARG;
        return -1;
    }
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
    if (rc != 0 || !res) {
        if (out_err) *out_err = KL_ERR_DNS;
        return -1;
    }

    KlSocketHandle fd = kl_sock_socket(NULL, res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        if (out_err) *out_err = KL_ERR_SOCKET;
        freeaddrinfo(res);
        return -1;
    }

    kl_sock_set_nosigpipe(NULL, fd);

    /* Saved for restore after the blocking connect-with-timeout below. */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        if (out_err) *out_err = KL_ERR_SOCKET;
        close(fd);
        freeaddrinfo(res);
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        if (out_err) *out_err = KL_ERR_SOCKET;
        close(fd);
        freeaddrinfo(res);
        return -1;
    }

    rc = kl_sock_connect(NULL, fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    if (rc < 0 && errno != EINPROGRESS) {
        if (out_err) *out_err = KL_ERR_CONNECT;
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
            if (out_err) *out_err = (rc == 0) ? KL_ERR_TIMEOUT : KL_ERR_CONNECT;
            close(fd);
            return -1;
        }

        int err = 0;
        socklen_t errlen = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen);
        if (err != 0) {
            if (out_err) *out_err = KL_ERR_CONNECT;
            close(fd);
            return -1;
        }
    }

    /* Restore blocking mode */
    fcntl(fd, F_SETFL, flags);

    return fd;
}

/* ── UNIX socket address + connect ───────────────────────────────── */

/*
 * Fill a sockaddr_un for `path`. Returns the address length, or 0 if the
 * path is empty or too long for sun_path. Shared by the sync and async
 * connect paths.
 */
static socklen_t fill_sockaddr_un(struct sockaddr_un *addr, const char *path)
{
    size_t path_len = strlen(path);
    if (path_len == 0 || path_len >= sizeof(addr->sun_path))
        return 0;
    memset(addr, 0, sizeof(*addr));
    addr->sun_family = AF_UNIX;
    memcpy(addr->sun_path, path, path_len + 1);
    return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + path_len + 1);
}

static int unix_connect_with_timeout(const char *path, int timeout_ms,
                                     KlError *out_err)
{
    struct sockaddr_un addr;
    socklen_t addr_len = fill_sockaddr_un(&addr, path);
    if (addr_len == 0) {
        if (out_err) *out_err = KL_ERR_INVALID_ARG;
        return -1;
    }

    KlSocketHandle fd = kl_sock_socket(NULL, AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        if (out_err) *out_err = KL_ERR_SOCKET;
        return -1;
    }

    kl_sock_set_nosigpipe(NULL, fd);

    /* Saved for restore after the blocking connect-with-timeout below. */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        if (out_err) *out_err = KL_ERR_SOCKET;
        close(fd);
        return -1;
    }

    int rc = kl_sock_connect(NULL, fd, (struct sockaddr *)&addr, addr_len);
    if (rc < 0 && errno != EINPROGRESS) {
        if (out_err) *out_err = KL_ERR_CONNECT;
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
            if (out_err) *out_err = (rc == 0) ? KL_ERR_TIMEOUT : KL_ERR_CONNECT;
            close(fd);
            return -1;
        }
        int err = 0;
        socklen_t errlen = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen);
        if (err != 0) {
            if (out_err) *out_err = KL_ERR_CONNECT;
            close(fd);
            return -1;
        }
    }

    fcntl(fd, F_SETFL, flags);  /* restore blocking mode */
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

/* ── Proxy CONNECT handshake (sync) ──────────────────────────────── */

static int proxy_connect_sync(int fd, const char *host, uint16_t port,
                                const char *proxy_auth, int timeout_ms)
{
    char buf[KL_PROXY_RESPONSE_MAX];
    int n;
    if (proxy_auth)
        n = snprintf(buf, sizeof(buf),
                     "CONNECT %s:%u HTTP/1.1\r\nHost: %s:%u\r\n"
                     "Proxy-Authorization: %s\r\n\r\n",
                     host, port, host, port, proxy_auth);
    else
        n = snprintf(buf, sizeof(buf),
                     "CONNECT %s:%u HTTP/1.1\r\nHost: %s:%u\r\n\r\n",
                     host, port, host, port);

    if (n < 0 || (size_t)n >= sizeof(buf))
        return -1;

    /* Send CONNECT request */
    size_t sent = 0;
    while (sent < (size_t)n) {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr <= 0)
            return -1;

        ssize_t w = write(fd, buf + sent, (size_t)n - sent);
        if (w <= 0) {
            if (w < 0 && errno == EINTR)
                continue;
            return -1;
        }
        sent += (size_t)w;
    }

    /* Read proxy response — look for "HTTP/1.x 200" */
    size_t recv_len = 0;
    for (;;) {
        if (recv_len >= sizeof(buf) - 1)
            return -1;  /* response too large */

        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr <= 0)
            return -1;

        ssize_t r = read(fd, buf + recv_len, sizeof(buf) - 1 - recv_len);
        if (r <= 0) {
            if (r < 0 && errno == EINTR)
                continue;
            return -1;
        }
        recv_len += (size_t)r;
        buf[recv_len] = '\0';

        /* Check for end of headers */
        if (strstr(buf, "\r\n\r\n")) {
            /* Verify 200 status */
            if (recv_len < 12)
                return -1;
            if (strncmp(buf, "HTTP/1.", 7) != 0)
                return -1;
            if (buf[9] != '2' || buf[10] != '0' || buf[11] != '0')
                return -1;
            return 0;
        }
    }
}

/* ── Build request into stack buffer, send ───────────────────────── */

static int send_request_sync(int fd, KlTls *tls,
                              const char *method, const KlUrl *url,
                              const KlClientHeader *headers, int num_headers,
                              const char *body, size_t body_len,
                              int timeout_ms, int keep_alive,
                              const char *absolute_url)
{
    if (has_crlf(method, strlen(method)))
        return -1;
    if (url->path_len > INT_MAX || url->host_len > INT_MAX)
        return -1;

    /* When proxied (HTTP forwarding), use absolute-form URL as target */
    const char *target;
    int target_len;
    const char path_fallback[] = "/";
    if (absolute_url) {
        target = absolute_url;
        target_len = (int)strlen(absolute_url);
    } else if (url->path_len > 0) {
        target = url->path;
        target_len = (int)url->path_len;
    } else {
        target = path_fallback;
        target_len = 1;
    }

    char buf[KL_CLIENT_REQ_BUF_SIZE];
    int off = snprintf(buf, sizeof(buf), "%s %.*s HTTP/1.1\r\nHost: %.*s\r\n",
                       method,
                       target_len, target,
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
                     "Connection: %s\r\n\r\n",
                     keep_alive ? "keep-alive" : "close");
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

        ssize_t w = io_write(NULL, fd, tls, buf + sent, (size_t)off - sent);
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

            ssize_t w = io_write(NULL, fd, tls, body + sent, body_len - sent);
            if (w <= 0)
                return -1;
            sent += (size_t)w;
        }
    }

    return 0;
}

/* ── Send headers-only (for chunked body streaming) ──────────────── */

static int send_headers_sync(int fd, KlTls *tls,
                               const char *method, const KlUrl *url,
                               const KlClientHeader *headers, int num_headers,
                               int timeout_ms, int keep_alive,
                               const char *absolute_url)
{
    if (has_crlf(method, strlen(method)))
        return -1;
    if (url->path_len > INT_MAX || url->host_len > INT_MAX)
        return -1;

    const char *target;
    int target_len;
    const char path_fallback[] = "/";
    if (absolute_url) {
        target = absolute_url;
        target_len = (int)strlen(absolute_url);
    } else if (url->path_len > 0) {
        target = url->path;
        target_len = (int)url->path_len;
    } else {
        target = path_fallback;
        target_len = 1;
    }

    char buf[KL_CLIENT_REQ_BUF_SIZE];
    int off = snprintf(buf, sizeof(buf), "%s %.*s HTTP/1.1\r\nHost: %.*s\r\n",
                       method,
                       target_len, target,
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

    int n = snprintf(buf + off, sizeof(buf) - (size_t)off,
                     "Transfer-Encoding: chunked\r\nConnection: %s\r\n\r\n",
                     keep_alive ? "keep-alive" : "close");
    if (n < 0 || (size_t)(off + n) >= sizeof(buf))
        return -1;
    off += n;

    size_t sent = 0;
    while (sent < (size_t)off) {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr <= 0)
            return -1;

        ssize_t w = io_write(NULL, fd, tls, buf + sent, (size_t)off - sent);
        if (w <= 0)
            return -1;
        sent += (size_t)w;
    }

    return 0;
}

/* ── Send chunked body from body_read callback (sync) ────────────── */

static int send_all_sync(int fd, KlTls *tls, const char *data, size_t len,
                           int timeout_ms)
{
    size_t sent = 0;
    while (sent < len) {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr <= 0)
            return -1;

        ssize_t w = io_write(NULL, fd, tls, data + sent, len - sent);
        if (w <= 0)
            return -1;
        sent += (size_t)w;
    }
    return 0;
}

static int send_body_chunked_sync(int fd, KlTls *tls,
                                    KlClientReadFn body_read, void *user_data,
                                    int timeout_ms)
{
    char data_buf[KL_CLIENT_CHUNK_BUF_SIZE];
    char hdr_buf[KL_CLIENT_CHUNK_HDR_SIZE];

    for (;;) {
        ssize_t nread = body_read(data_buf, sizeof(data_buf), user_data);
        if (nread < 0)
            return -1;

        if (nread == 0) {
            /* Final chunk: 0\r\n\r\n */
            if (send_all_sync(fd, tls, "0\r\n\r\n",
                               KL_CLIENT_FINAL_CHUNK_LEN, timeout_ms) != 0)
                return -1;
            return 0;
        }

        /* Chunk header: <hex-len>\r\n */
        int hdr_len = snprintf(hdr_buf, sizeof(hdr_buf), "%zx\r\n", (size_t)nread);
        if (hdr_len < 0)
            return -1;

        if (send_all_sync(fd, tls, hdr_buf, (size_t)hdr_len, timeout_ms) != 0)
            return -1;
        if (send_all_sync(fd, tls, data_buf, (size_t)nread, timeout_ms) != 0)
            return -1;
        if (send_all_sync(fd, tls, "\r\n", sizeof("\r\n") - 1, timeout_ms) != 0)
            return -1;
    }
}

/* ── Receive + parse response (sync, with optional streaming) ────── */

static int recv_response_sync(int fd, KlTls *tls, KlClientResponse *resp,
                               size_t max_response_size, int timeout_ms,
                               KlAllocator *alloc,
                               const KlClientStreamCfg *stream)
{
    KlResponseParser *parser;
    if (stream && stream->on_body) {
        parser = kl_response_parser_llhttp_s(max_response_size, alloc,
                                               stream->on_body,
                                               stream->on_headers,
                                               stream->on_complete,
                                               stream->user_data);
    } else {
        parser = kl_response_parser_llhttp(max_response_size, alloc);
    }
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

        ssize_t nread = io_read(NULL, fd, tls, buf, sizeof(buf));
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

/* ── Response decompression helpers ───────────────────────────────── */

static const char *find_header_value(const KlClientResponse *resp,
                                      const char *name)
{
    for (int i = 0; i < resp->num_headers; i++) {
        if (strcasecmp(resp->headers[i].name, name) == 0)
            return resp->headers[i].value;
    }
    return NULL;
}

static void remove_header(KlClientResponse *resp, const char *name)
{
    for (int i = 0; i < resp->num_headers; i++) {
        if (strcasecmp(resp->headers[i].name, name) == 0) {
            /* Free the header strings */
            kl_free(&resp->alloc, (char *)resp->headers[i].name,
                    strlen(resp->headers[i].name) + 1);
            kl_free(&resp->alloc, (char *)resp->headers[i].value,
                    strlen(resp->headers[i].value) + 1);
            /* Shift remaining headers down */
            for (int j = i; j < resp->num_headers - 1; j++)
                resp->headers[j] = resp->headers[j + 1];
            resp->num_headers--;
            return;
        }
    }
}

/**
 * Post-process a buffered response: decompress body if Content-Encoding
 * matches the decompressor's encoding. Replaces body and removes header.
 */
static int decompress_response_body(KlClientResponse *resp,
                                      KlDecompressConfig *dcfg)
{
    if (!dcfg || !dcfg->factory)
        return 0;  /* no decompression configured — not an error */
    if (!resp->body || resp->body_len == 0)
        return 0;

    const char *enc = find_header_value(resp, "Content-Encoding");
    if (!enc)
        return 0;  /* no encoding — nothing to do */

    /* Create session and check encoding match */
    KlDecompress *decomp = dcfg->factory(dcfg->ctx, &resp->alloc);
    if (!decomp)
        return -1;

    const char *supported = decomp->encoding(decomp);
    if (strcasecmp(enc, supported) != 0) {
        decomp->destroy(decomp);
        return 0;  /* encoding mismatch — leave body as-is */
    }

    /* Decompress */
    char *out = NULL;
    size_t out_len = 0;
    int rc = decomp->decompress(decomp, resp->body, resp->body_len,
                                 &out, &out_len, &resp->alloc);
    decomp->destroy(decomp);

    if (rc < 0)
        return -1;

    /* Replace body */
    kl_free(&resp->alloc, resp->body, resp->body_len + 1);
    resp->body = out;
    resp->body_len = out_len;

    /* Remove Content-Encoding header */
    remove_header(resp, "Content-Encoding");

    return 0;
}

/* ── Streaming decompression wrapper ─────────────────────────────── */

typedef struct {
    /* User's original callbacks */
    KlClientBodyFn     user_on_body;
    KlClientHeadersFn  user_on_headers;
    void             (*user_on_complete)(void *user_data);
    void              *user_data;

    /* Decompression state */
    KlDecompressStream  ds;
    int                 active;  /* 1 if decompression is active */
    KlDecompressConfig *dcfg;
} DecompStreamWrap;

static int decomp_emit_to_user(void *ctx, const char *data, size_t len)
{
    DecompStreamWrap *w = ctx;
    if (!w->user_on_body)
        return 0;
    return w->user_on_body(data, len, w->user_data);
}

static int decomp_on_headers(int status, const KlClientHeader *headers,
                               int num_headers, void *user_data)
{
    DecompStreamWrap *w = user_data;

    /* Check if Content-Encoding matches our decompressor */
    for (int i = 0; i < num_headers; i++) {
        if (strcasecmp(headers[i].name, "Content-Encoding") == 0) {
            /* Create session and check encoding */
            KlDecompress *decomp = w->dcfg->factory(w->dcfg->ctx,
                                                      w->ds.alloc);
            if (decomp) {
                const char *supported = decomp->encoding(decomp);
                if (strcasecmp(headers[i].value, supported) == 0) {
                    w->ds.decomp = decomp;
                    w->ds.error = 0;
                    w->active = 1;
                } else {
                    decomp->destroy(decomp);
                }
            }
            break;
        }
    }

    if (w->user_on_headers)
        return w->user_on_headers(status, headers, num_headers, w->user_data);
    return 0;
}

static int decomp_on_body(const char *data, size_t len, void *user_data)
{
    DecompStreamWrap *w = user_data;
    if (w->active) {
        return kl_decompress_stream_feed(&w->ds, data, len, 0,
                                          decomp_emit_to_user, w);
    }
    /* Passthrough */
    if (w->user_on_body)
        return w->user_on_body(data, len, w->user_data);
    return 0;
}

static void decomp_on_complete(void *user_data)
{
    DecompStreamWrap *w = user_data;
    if (w->active) {
        /* Final flush */
        kl_decompress_stream_feed(&w->ds, NULL, 0, 1,
                                   decomp_emit_to_user, w);
        kl_decompress_stream_free(&w->ds);
        w->active = 0;
    }
    if (w->user_on_complete)
        w->user_on_complete(w->user_data);
}

/* ── Sync public API ─────────────────────────────────────────────── */

int kl_client_request_s(KlAllocator *alloc, const KlClientConfig *cfg,
                         const char *method, const char *url_str,
                         const KlClientHeader *headers, int num_headers,
                         const char *body, size_t body_len,
                         const KlClientStreamCfg *stream,
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
    if (kl_url_parse(url_str, &parsed) != 0) {
        resp->error = KL_ERR_URL;
        return -1;
    }

    KlTlsConfig *tls_cfg = cfg ? cfg->tls : NULL;
    if (parsed.is_https && !tls_cfg) {
        resp->error = KL_ERR_URL;
        return -1;
    }
    if (!parsed.is_https)
        tls_cfg = NULL;

    /* Proxy routing: connect to proxy host instead of target */
    const KlProxyConfig *proxy = cfg ? cfg->proxy : NULL;
    int is_proxied = (proxy && proxy->host);

    /* UNIX socket target: no host/port. Normalize the Host header to
     * "localhost" so all downstream request building works unchanged; the
     * connection itself goes to parsed.unix_path. Proxying is incompatible. */
    if (parsed.is_unix) {
        if (is_proxied) {
            resp->error = KL_ERR_INVALID_ARG;
            return -1;
        }
        parsed.host = "localhost";
        parsed.host_len = 9;
    }

    KlError conn_err = KL_ERR_NONE;
    KlSocketHandle fd;
    if (parsed.is_unix) {
        fd = unix_connect_with_timeout(parsed.unix_path, timeout_ms, &conn_err);
    } else if (is_proxied) {
        fd = connect_with_timeout(proxy->host, strlen(proxy->host),
                                   proxy->port, timeout_ms, &conn_err);
    } else {
        fd = connect_with_timeout(parsed.host, parsed.host_len,
                                   parsed.port, timeout_ms, &conn_err);
    }
    if (fd < 0) {
        resp->error = conn_err;
        return -1;
    }

    KlTls *tls = NULL;
    int ret = -1;
    int decomp_installed = 0;   /* streaming decompressor wrapper active */

    if (is_proxied && parsed.is_https) {
        /* CONNECT tunnel through proxy, then TLS handshake */
        char target_host[KL_CLIENT_HOSTNAME_MAX];
        if (parsed.host_len >= sizeof(target_host)) {
            resp->error = KL_ERR_INVALID_ARG;
            goto cleanup;
        }
        memcpy(target_host, parsed.host, parsed.host_len);
        target_host[parsed.host_len] = '\0';

        if (proxy_connect_sync(fd, target_host, (uint16_t)parsed.port,
                                 proxy->auth, timeout_ms) != 0) {
            resp->error = KL_ERR_PROXY;
            goto cleanup;
        }
        tls = do_tls_handshake(fd, tls_cfg, parsed.host, parsed.host_len,
                                timeout_ms, alloc);
        if (!tls) {
            resp->error = KL_ERR_TLS_HANDSHAKE;
            goto cleanup;
        }
    } else if (parsed.is_https) {
        tls = do_tls_handshake(fd, tls_cfg, parsed.host, parsed.host_len,
                                timeout_ms, alloc);
        if (!tls) {
            resp->error = KL_ERR_TLS_HANDSHAKE;
            goto cleanup;
        }
    }

    /* Build absolute-form URL for HTTP forwarding through proxy */
    char abs_url_buf[KL_CLIENT_REQ_BUF_SIZE];
    const char *absolute_url = NULL;
    if (is_proxied && !parsed.is_https) {
        char host_z[KL_CLIENT_HOSTNAME_MAX];
        if (parsed.host_len >= sizeof(host_z)) {
            resp->error = KL_ERR_INVALID_ARG;
            goto cleanup;
        }
        memcpy(host_z, parsed.host, parsed.host_len);
        host_z[parsed.host_len] = '\0';

        const char *path = (parsed.path_len > 0) ? parsed.path : "/";
        int path_len = (parsed.path_len > 0) ? (int)parsed.path_len : 1;

        int n;
        if (parsed.port == 80)
            n = snprintf(abs_url_buf, sizeof(abs_url_buf),
                         "http://%s%.*s", host_z, path_len, path);
        else
            n = snprintf(abs_url_buf, sizeof(abs_url_buf),
                         "http://%s:%d%.*s", host_z, parsed.port,
                         path_len, path);
        if (n < 0 || (size_t)n >= sizeof(abs_url_buf)) {
            resp->error = KL_ERR_OVERFLOW;
            goto cleanup;
        }
        absolute_url = abs_url_buf;
    }

    /* Set up streaming decompression wrapper if needed */
    KlDecompressConfig *dcfg = cfg ? cfg->decompress : NULL;
    DecompStreamWrap decomp_wrap;
    const KlClientStreamCfg *actual_stream = stream;
    KlClientStreamCfg wrapped_stream;

    if (dcfg && dcfg->factory && stream && stream->on_body) {
        memset(&decomp_wrap, 0, sizeof(decomp_wrap));
        decomp_wrap.user_on_body = stream->on_body;
        decomp_wrap.user_on_headers = stream->on_headers;
        decomp_wrap.user_on_complete = stream->on_complete;
        decomp_wrap.user_data = stream->user_data;
        decomp_wrap.dcfg = dcfg;
        decomp_wrap.ds.alloc = alloc;

        wrapped_stream.on_body = decomp_on_body;
        wrapped_stream.on_headers = decomp_on_headers;
        wrapped_stream.on_complete = decomp_on_complete;
        wrapped_stream.body_read = stream->body_read;
        wrapped_stream.user_data = &decomp_wrap;
        actual_stream = &wrapped_stream;
        decomp_installed = 1;
    }

    /* Request streaming: send headers + chunked body */
    if (stream && stream->body_read) {
        if (send_headers_sync(fd, tls, method, &parsed,
                                headers, num_headers, timeout_ms, 0,
                                absolute_url) != 0) {
            if (!resp->error) resp->error = KL_ERR_IO;
            goto cleanup;
        }
        if (send_body_chunked_sync(fd, tls, stream->body_read,
                                     stream->user_data, timeout_ms) != 0) {
            if (!resp->error) resp->error = KL_ERR_IO;
            goto cleanup;
        }
    } else {
        if (send_request_sync(fd, tls, method, &parsed,
                               headers, num_headers, body, body_len,
                               timeout_ms, 0, absolute_url) != 0) {
            if (!resp->error) resp->error = KL_ERR_IO;
            goto cleanup;
        }
    }

    if (recv_response_sync(fd, tls, resp, max_resp, timeout_ms, alloc,
                            actual_stream) != 0) {
        if (!resp->error) resp->error = KL_ERR_PARSE;
        goto cleanup;
    }

    /* Decompress buffered response body if applicable */
    if (!stream || !stream->on_body) {
        if (decompress_response_body(resp, dcfg) < 0) {
            if (!resp->error) resp->error = KL_ERR_COMPRESS;
            goto cleanup;
        }
    }

    ret = 0;

cleanup:
    /* Free the streaming decompressor session if it was installed.  It is
     * otherwise freed only by decomp_on_complete (fired on parser
     * message-complete), so error paths and EOF-terminated success would
     * leak it.  kl_decompress_stream_free is idempotent, so freeing after a
     * normal completion is safe. */
    if (decomp_installed)
        kl_decompress_stream_free(&decomp_wrap.ds);
    if (tls) {
        tls->shutdown(tls, fd);
        tls->destroy(tls);
    }
    close(fd);

    if (ret != 0)
        kl_client_response_free(resp);

    return ret;
}

int kl_client_request(KlAllocator *alloc, const KlClientConfig *cfg,
                      const char *method, const char *url_str,
                      const KlClientHeader *headers, int num_headers,
                      const char *body, size_t body_len,
                      KlClientResponse *resp)
{
    return kl_client_request_s(alloc, cfg, method, url_str,
                                headers, num_headers, body, body_len,
                                NULL, resp);
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
    KL_HCLIENT_RESOLVING,
    KL_HCLIENT_CONNECTING,
    KL_HCLIENT_PROXY_CONNECTING,   /* sending CONNECT request */
    KL_HCLIENT_PROXY_HANDSHAKE,    /* reading proxy 200 response */
    KL_HCLIENT_TLS_HANDSHAKE,
    KL_HCLIENT_SENDING,
    KL_HCLIENT_SENDING_STREAM,  /* chunked body from body_read callback */
    KL_HCLIENT_RECEIVING,
    KL_HCLIENT_DONE
} KlClientState;

/* One in-flight racing connect socket (Happy Eyeballs). */
typedef struct { KlSocketHandle fd; int active; } KlConnAttempt;

struct KlClient {
    KlSocketHandle     fd;
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
    KlError            error;

    /* TLS (NULL if plain HTTP) */
    KlTls             *tls;
    KlTlsConfig       *tls_cfg;
    char               host_buf[KL_CLIENT_HOSTNAME_MAX];

    /* Async DNS resolver (NULL = sync getaddrinfo was used) */
    KlResolver        *resolver;
    KlResolveReq      *resolve_req;
    int                owns_resolver;   /* 1 = auto-created, destroy on teardown */

    /* Happy Eyeballs — racing connect over the resolved address list (RFC 8305).
     * Only active on the async resolver path (conn_racing=1); the UNIX and sync
     * getaddrinfo paths stay single-fd. */
    KlConnAttempt      conn_attempts[KL_RESOLVE_MAX_ADDRS];
    KlResolveResult    conn_addrs;      /* full list, copied from the resolver */
    int                conn_next;       /* index of next address to dial */
    int                conn_pending;    /* number of in-flight attempts */
    int                conn_racing;     /* 1 = HE attempts in conn_attempts[] */
    int64_t            conn_delay_timer;/* Connection Attempt Delay timer (-1) */
    int64_t            deadline_timer;  /* overall request deadline timer (-1) */
    int                timeout_ms;      /* overall deadline (0 = none) */
    int                connect_delay_ms;/* Connection Attempt Delay */
    KlError            conn_last_err;   /* last connect error, for the all-fail case */

    /* Completion callback */
    KlClientDoneFn     on_done;
    void              *user_data;

    /* Request streaming (chunked body send) */
    KlClientReadFn     body_read;
    void              *stream_user_data;
    char               chunk_buf[KL_CLIENT_CHUNK_BUF_SIZE];
    size_t             chunk_len;    /* bytes in chunk_buf to send */
    size_t             chunk_sent;   /* bytes of chunk_buf already sent */
    int                chunk_phase;  /* 0=read, 1=send hdr, 2=send data, 3=send crlf, 4=eof */
    char               chunk_hdr[KL_CLIENT_CHUNK_HDR_SIZE];
    size_t             chunk_hdr_len;
    size_t             chunk_hdr_sent;

    /* Pool integration (NULL = legacy close-on-complete) */
    KlClientPool      *pool;
    KlClientPoolConn   pool_conn;
    int                pool_port;
    int                pool_is_tls;

    /* Response decompression */
    KlDecompressConfig *decompress_cfg;
    DecompStreamWrap   *decomp_wrap;     /* heap-allocated for streaming */

    /* Proxy state */
    int             is_proxied;     /* connected via proxy */
    int             is_tunnel;      /* CONNECT tunnel (HTTPS through proxy) */
    char           *connect_buf;    /* CONNECT request buffer (heap) */
    size_t          connect_len;
    size_t          connect_sent;
    char           *proxy_recv;     /* CONNECT response buffer (heap) */
    size_t          proxy_recv_len;
    const char     *proxy_auth;     /* borrowed from config */
    uint16_t        target_port;    /* original target port for CONNECT */
};

/* Forward declarations */
static void async_on_event(KlSocketHandle fd, KlEventMask ready, void *user_data);
static void async_complete_success(KlClient *c);
static void async_complete_error(KlClient *c);
static void async_handle_tls_handshake(KlClient *c);
static void async_handle_sending_stream(KlClient *c);
static void async_handle_proxy_connecting(KlClient *c);
static void async_handle_proxy_handshake(KlClient *c);
static int  start_connect(KlClient *c, const struct sockaddr *addr,
                           socklen_t addrlen, int family, int socktype,
                           int protocol);
/* Happy Eyeballs helpers (defined below start_connect) */
static void he_proceed_after_connect(KlClient *c);
static void he_start_next(KlClient *c);
static void he_win(KlClient *c, KlSocketHandle fd);
static void he_on_writable(KlClient *c, KlSocketHandle fd);
static void he_close_attempts(KlClient *c, KlSocketHandle keep_fd);
static void he_cancel_timers(KlClient *c);
static void he_on_deadline(void *user_data);
static void he_on_delay(void *user_data);

/* ── Build request into heap buffer ──────────────────────────────── */

static char *build_request(KlAllocator *alloc,
                            const char *method, const KlUrl *url,
                            const KlClientHeader *headers, int num_headers,
                            const char *body, size_t body_len,
                            size_t *out_len, int keep_alive,
                            const char *absolute_url)
{
    if (has_crlf(method, strlen(method)))
        return NULL;
    if (url->path_len > INT_MAX || url->host_len > INT_MAX)
        return NULL;

    const char *target;
    int target_len;
    if (absolute_url) {
        target = absolute_url;
        target_len = (int)strlen(absolute_url);
    } else if (url->path_len > 0) {
        target = url->path;
        target_len = (int)url->path_len;
    } else {
        target = "/";
        target_len = 1;
    }

    char buf[KL_CLIENT_REQ_BUF_SIZE];
    int off = snprintf(buf, sizeof(buf), "%s %.*s HTTP/1.1\r\nHost: %.*s\r\n",
                       method,
                       target_len, target,
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
                     "Connection: %s\r\n\r\n",
                     keep_alive ? "keep-alive" : "close");
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

/* ── Build headers-only request into heap buffer (chunked TE) ────── */

static char *build_request_headers_only(KlAllocator *alloc,
                                          const char *method, const KlUrl *url,
                                          const KlClientHeader *headers,
                                          int num_headers, size_t *out_len,
                                          int keep_alive,
                                          const char *absolute_url)
{
    if (has_crlf(method, strlen(method)))
        return NULL;
    if (url->path_len > INT_MAX || url->host_len > INT_MAX)
        return NULL;

    const char *target;
    int target_len;
    if (absolute_url) {
        target = absolute_url;
        target_len = (int)strlen(absolute_url);
    } else if (url->path_len > 0) {
        target = url->path;
        target_len = (int)url->path_len;
    } else {
        target = "/";
        target_len = 1;
    }

    char buf[KL_CLIENT_REQ_BUF_SIZE];
    int off = snprintf(buf, sizeof(buf), "%s %.*s HTTP/1.1\r\nHost: %.*s\r\n",
                       method,
                       target_len, target,
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

    int n = snprintf(buf + off, sizeof(buf) - (size_t)off,
                     "Transfer-Encoding: chunked\r\nConnection: %s\r\n\r\n",
                     keep_alive ? "keep-alive" : "close");
    if (n < 0 || (size_t)(off + n) >= sizeof(buf))
        return NULL;
    off += n;

    char *req = kl_malloc(alloc, (size_t)off);
    if (!req)
        return NULL;
    memcpy(req, buf, (size_t)off);
    *out_len = (size_t)off;
    return req;
}

/* ── Build CONNECT request into heap buffer ──────────────────────── */

static int build_connect_request(KlClient *c, const char *host,
                                   uint16_t port, const char *auth)
{
    char buf[KL_PROXY_RESPONSE_MAX];
    int n;
    if (auth)
        n = snprintf(buf, sizeof(buf),
                     "CONNECT %s:%u HTTP/1.1\r\nHost: %s:%u\r\n"
                     "Proxy-Authorization: %s\r\n\r\n",
                     host, port, host, port, auth);
    else
        n = snprintf(buf, sizeof(buf),
                     "CONNECT %s:%u HTTP/1.1\r\nHost: %s:%u\r\n\r\n",
                     host, port, host, port);

    if (n < 0 || (size_t)n >= sizeof(buf))
        return -1;

    c->connect_buf = kl_malloc(c->alloc, (size_t)n);
    if (!c->connect_buf)
        return -1;
    memcpy(c->connect_buf, buf, (size_t)n);
    c->connect_len = (size_t)n;
    c->connect_sent = 0;
    return 0;
}

/* ── Post-DNS: create socket, connect, register watcher ──────────── */

static int start_connect(KlClient *c, const struct sockaddr *addr,
                          socklen_t addrlen, int family, int socktype,
                          int protocol)
{
    KlSocketHandle fd = kl_sock_socket(c->ev_ctx->sockets, family, socktype, protocol);
    if (!kl_handle_valid(fd))
        return -1;

    kl_sock_set_nosigpipe(c->ev_ctx->sockets, fd);
    if (kl_sock_set_nonblocking(c->ev_ctx->sockets, fd) < 0) {
        kl_sock_close(c->ev_ctx->sockets, fd);
        return -1;
    }

    int rc = kl_sock_connect(c->ev_ctx->sockets, fd, addr, addrlen);
    if (rc < 0 && errno != EINPROGRESS) {
        kl_sock_close(c->ev_ctx->sockets, fd);
        return -1;
    }

    c->fd = fd;
    c->state = (rc == 0) ? KL_HCLIENT_SENDING : KL_HCLIENT_CONNECTING;

    if (kl_watcher_add(c->ev_ctx, fd, KL_EVENT_WRITE, async_on_event, c) != 0) {
        kl_sock_close(c->ev_ctx->sockets, fd);
        c->fd = KL_INVALID_SOCKET;
        return -1;
    }

    return 0;
}

/* ── Happy Eyeballs — racing connect over the resolved list (RFC 8305) ─── */

/* Cancel the Connection Attempt Delay + overall deadline timers (idempotent). */
static void he_cancel_timers(KlClient *c)
{
    if (c->conn_delay_timer >= 0) {
        kl_timer_cancel(c->ev_ctx, c->conn_delay_timer);
        c->conn_delay_timer = -1;
    }
    if (c->deadline_timer >= 0) {
        kl_timer_cancel(c->ev_ctx, c->deadline_timer);
        c->deadline_timer = -1;
    }
}

/* Close every active racing attempt except keep_fd (the winner, or -1 for all). */
static void he_close_attempts(KlClient *c, KlSocketHandle keep_fd)
{
    for (int i = 0; i < c->conn_next; i++) {
        if (c->conn_attempts[i].active && c->conn_attempts[i].fd != keep_fd) {
            kl_watcher_del(c->ev_ctx, c->conn_attempts[i].fd);
            kl_sock_close(c->ev_ctx->sockets, c->conn_attempts[i].fd);
            c->conn_attempts[i].active = 0;
        }
    }
    c->conn_pending = 0;
}

/* A winner has connected: stop the stagger, drop the losers, adopt its fd, and
 * hand off to the shared post-connect path. The overall deadline stays armed to
 * bound the remaining TLS/send/recv. */
static void he_win(KlClient *c, KlSocketHandle fd)
{
    if (c->conn_delay_timer >= 0) {
        kl_timer_cancel(c->ev_ctx, c->conn_delay_timer);
        c->conn_delay_timer = -1;
    }
    /* Mark the winner inactive so he_close_attempts won't touch its fd. */
    for (int i = 0; i < c->conn_next; i++)
        if (c->conn_attempts[i].active && c->conn_attempts[i].fd == fd)
            c->conn_attempts[i].active = 0;
    he_close_attempts(c, fd);
    c->fd = fd;
    he_proceed_after_connect(c);
}

/* Arm the Connection Attempt Delay to stagger the next address, if any remain. */
static void he_arm_delay(KlClient *c)
{
    if (c->conn_delay_timer >= 0) {
        kl_timer_cancel(c->ev_ctx, c->conn_delay_timer);
        c->conn_delay_timer = -1;
    }
    if (c->state == KL_HCLIENT_CONNECTING && c->conn_next < c->conn_addrs.naddrs)
        c->conn_delay_timer = kl_timer_add(c->ev_ctx,
                                           (uint64_t)c->connect_delay_ms,
                                           he_on_delay, c);
}

/* Start a nonblocking connect to address index idx. Returns 0 if an attempt is
 * now in flight (or already won), -1 on a hard local failure (try the next). */
static int he_new_attempt(KlClient *c, int idx)
{
    const struct sockaddr *sa = (const struct sockaddr *)&c->conn_addrs.addrs[idx];
    socklen_t sl = c->conn_addrs.addrlens[idx];
    int fam = c->conn_addrs.addrs[idx].ss_family;

    KlSocketHandle fd = kl_sock_socket(c->ev_ctx->sockets, fam, c->conn_addrs.ai_socktype, c->conn_addrs.ai_protocol);
    if (!kl_handle_valid(fd))
        return -1;

    kl_sock_set_nosigpipe(c->ev_ctx->sockets, fd);
    if (kl_sock_set_nonblocking(c->ev_ctx->sockets, fd) < 0) {
        kl_sock_close(c->ev_ctx->sockets, fd);
        return -1;
    }

    int rc = kl_sock_connect(c->ev_ctx->sockets, fd, sa, sl);
    if (rc < 0 && errno != EINPROGRESS) {
        c->conn_last_err = KL_ERR_CONNECT;
        kl_sock_close(c->ev_ctx->sockets, fd);
        return -1;
    }

    if (kl_watcher_add(c->ev_ctx, fd, KL_EVENT_WRITE, async_on_event, c) != 0) {
        kl_sock_close(c->ev_ctx->sockets, fd);
        return -1;
    }

    c->conn_attempts[idx].fd = fd;
    c->conn_attempts[idx].active = 1;
    c->conn_pending++;

    if (rc == 0)            /* connected immediately (e.g. loopback) */
        he_win(c, fd);
    return 0;
}

/* Advance the cursor and start the next viable address; skip hard local
 * failures without consuming the delay. Complete with an error when the list is
 * exhausted and nothing is pending. */
static void he_start_next(KlClient *c)
{
    while (c->state == KL_HCLIENT_CONNECTING &&
           c->conn_next < c->conn_addrs.naddrs) {
        int idx = c->conn_next++;
        if (he_new_attempt(c, idx) == 0) {
            he_arm_delay(c);   /* schedule the next staggered attempt */
            return;
        }
    }
    if (c->state == KL_HCLIENT_CONNECTING && c->conn_pending == 0) {
        c->error = (c->conn_last_err != KL_ERR_NONE) ? c->conn_last_err
                                                     : KL_ERR_CONNECT;
        async_complete_error(c);
    }
}

/* Connection Attempt Delay fired: start the next address if we're still racing. */
static void he_on_delay(void *user_data)
{
    KlClient *c = user_data;
    c->conn_delay_timer = -1;
    if (c->state == KL_HCLIENT_CONNECTING)
        he_start_next(c);
}

/* Overall request deadline fired: fail the whole request (covers connect racing
 * and the otherwise-untimed TLS handshake / send / recv). */
static void he_on_deadline(void *user_data)
{
    KlClient *c = user_data;
    c->deadline_timer = -1;
    if (c->state == KL_HCLIENT_DONE)
        return;
    he_close_attempts(c, KL_INVALID_SOCKET);    /* drop any racing sockets */
    c->error = KL_ERR_TIMEOUT;
    async_complete_error(c);
}

/* One racing attempt failed: drop it, then either fast-start the next address
 * (a failure must not wait out the delay, §5) or complete when all are gone. */
static void he_fail_attempt(KlClient *c, KlSocketHandle fd)
{
    for (int i = 0; i < c->conn_next; i++) {
        if (c->conn_attempts[i].active && c->conn_attempts[i].fd == fd) {
            kl_watcher_del(c->ev_ctx, fd);
            kl_sock_close(c->ev_ctx->sockets, fd);
            c->conn_attempts[i].active = 0;
            c->conn_pending--;
            break;
        }
    }
    c->conn_last_err = KL_ERR_CONNECT;

    if (c->conn_next < c->conn_addrs.naddrs)
        he_start_next(c);
    else if (c->conn_pending == 0) {
        c->error = c->conn_last_err;
        async_complete_error(c);
    }
}

/* A racing socket became writable: SO_ERROR==0 wins the race, else it failed. */
static void he_on_writable(KlClient *c, KlSocketHandle fd)
{
    int err = 0;
    kl_sock_get_so_error(c->ev_ctx->sockets, fd, &err);
    if (err == 0)
        he_win(c, fd);
    else
        he_fail_attempt(c, fd);
}

/* ── Async DNS resolve callback ──────────────────────────────────── */

/* Arm the overall request deadline (0 = none). */
static void he_arm_deadline(KlClient *c)
{
    if (c->timeout_ms > 0)
        c->deadline_timer = kl_timer_add(c->ev_ctx, (uint64_t)c->timeout_ms,
                                         he_on_deadline, c);
}

static void dns_resolved(KlResolveReq *req, const KlResolveResult *result,
                          int error, void *user_data)
{
    KlClient *c = user_data;
    (void)req;
    c->resolve_req = NULL;

    if (error || !result || result->naddrs < 1) {
        c->error = KL_ERR_DNS;
        async_complete_error(c);
        return;
    }

    /* Copy the whole (family-interleaved, preferred-first) list and race it. */
    c->conn_addrs = *result;
    if (c->conn_addrs.naddrs > KL_RESOLVE_MAX_ADDRS)
        c->conn_addrs.naddrs = KL_RESOLVE_MAX_ADDRS;
    c->conn_next = 0;
    c->conn_pending = 0;
    c->conn_racing = 1;
    c->conn_last_err = KL_ERR_NONE;
    c->state = KL_HCLIENT_CONNECTING;

    he_arm_deadline(c);
    he_start_next(c);
}

/* Select the resolver for an async request. Precedence: explicit cfg->resolver
 * (borrowed) → cfg->system_dns (NULL → blocking getaddrinfo) → auto-created
 * built-in async resolver (owned; *owned = 1). Returns NULL to fall back to
 * getaddrinfo (also on auto-create failure — better than failing the request). */
static KlResolver *client_pick_resolver(const KlClientConfig *cfg,
                                        KlEventCtx *ev_ctx, int *owned) {
    *owned = 0;
    if (cfg && cfg->resolver)
        return cfg->resolver;
    if (cfg && cfg->system_dns)
        return NULL;
    KlResolver *r = kl_dns_resolver_create(ev_ctx, NULL);
    if (r)
        *owned = 1;
    return r;
}

/* ── State: CONNECTING ───────────────────────────────────────────── */

/* Single-fd connect completion (UNIX socket + sync getaddrinfo paths). The
 * Happy Eyeballs path uses he_on_writable instead. */
static void async_handle_connecting(KlClient *c)
{
    int err = 0;
    kl_sock_get_so_error(c->ev_ctx->sockets, c->fd, &err);
    if (err != 0) {
        c->error = KL_ERR_CONNECT;
        async_complete_error(c);
        return;
    }
    he_proceed_after_connect(c);
}

/* Shared post-connect path: c->fd is a connected socket (Happy Eyeballs winner,
 * or the single-fd UNIX / sync-getaddrinfo connect). Advances to the proxy /
 * TLS / sending state. */
static void he_proceed_after_connect(KlClient *c)
{
    /* Proxy: HTTPS target needs CONNECT tunnel first */
    if (c->is_proxied && c->is_tunnel) {
        if (build_connect_request(c, c->host_buf, c->target_port,
                                    c->proxy_auth) != 0) {
            c->error = KL_ERR_ALLOC;
            async_complete_error(c);
            return;
        }
        c->state = KL_HCLIENT_PROXY_CONNECTING;
        kl_watcher_mod(c->ev_ctx, c->fd, KL_EVENT_WRITE);
        return;
    }

    /* Proxy: HTTP target — request already has absolute-form URL, send directly */
    if (c->is_proxied) {
        c->state = KL_HCLIENT_SENDING;
        kl_watcher_mod(c->ev_ctx, c->fd, KL_EVENT_WRITE);
        return;
    }

    if (c->tls_cfg && c->tls_cfg->factory) {
        c->tls = c->tls_cfg->factory(c->tls_cfg->ctx, c->alloc);
        if (!c->tls) {
            c->error = KL_ERR_TLS_INIT;
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

/* ── State: PROXY_CONNECTING (send CONNECT request) ──────────────── */

static void async_handle_proxy_connecting(KlClient *c)
{
    while (c->connect_sent < c->connect_len) {
        ssize_t w = write(c->fd, c->connect_buf + c->connect_sent,
                           c->connect_len - c->connect_sent);
        if (w < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                kl_watcher_rearm(c->ev_ctx, c->fd);
                return;
            }
            if (errno == EINTR)
                continue;
            c->error = KL_ERR_IO;
            async_complete_error(c);
            return;
        }
        if (w == 0) {
            c->error = KL_ERR_IO;
            async_complete_error(c);
            return;
        }
        c->connect_sent += (size_t)w;
    }

    /* CONNECT request fully sent — switch to reading proxy response */
    kl_free(c->alloc, c->connect_buf, c->connect_len);
    c->connect_buf = NULL;

    /* Allocate proxy response buffer */
    c->proxy_recv = kl_malloc(c->alloc, KL_PROXY_RESPONSE_MAX);
    if (!c->proxy_recv) {
        c->error = KL_ERR_ALLOC;
        async_complete_error(c);
        return;
    }
    c->proxy_recv_len = 0;

    c->state = KL_HCLIENT_PROXY_HANDSHAKE;
    kl_watcher_mod(c->ev_ctx, c->fd, KL_EVENT_READ);
}

/* ── State: PROXY_HANDSHAKE (read proxy 200 response) ────────────── */

static void async_handle_proxy_handshake(KlClient *c)
{
    for (;;) {
        if (c->proxy_recv_len >= KL_PROXY_RESPONSE_MAX - 1) {
            c->error = KL_ERR_PROXY;
            async_complete_error(c);
            return;
        }

        ssize_t r = read(c->fd, c->proxy_recv + c->proxy_recv_len,
                          KL_PROXY_RESPONSE_MAX - 1 - c->proxy_recv_len);
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                kl_watcher_rearm(c->ev_ctx, c->fd);
                return;
            }
            if (errno == EINTR)
                continue;
            c->error = KL_ERR_IO;
            async_complete_error(c);
            return;
        }
        if (r == 0) {
            c->error = KL_ERR_PROXY;
            async_complete_error(c);
            return;
        }

        c->proxy_recv_len += (size_t)r;
        c->proxy_recv[c->proxy_recv_len] = '\0';

        /* Check for end of headers */
        if (!strstr(c->proxy_recv, "\r\n\r\n"))
            continue;

        /* Verify HTTP/1.x 200 status */
        if (c->proxy_recv_len < 12 ||
            strncmp(c->proxy_recv, "HTTP/1.", 7) != 0 ||
            c->proxy_recv[9] != '2' || c->proxy_recv[10] != '0' ||
            c->proxy_recv[11] != '0') {
            c->error = KL_ERR_PROXY;
            async_complete_error(c);
            return;
        }

        /* CONNECT tunnel established — free buffer, start TLS */
        kl_free(c->alloc, c->proxy_recv, KL_PROXY_RESPONSE_MAX);
        c->proxy_recv = NULL;

        /* Create TLS session over the tunnel */
        if (!c->tls_cfg || !c->tls_cfg->factory) {
            c->error = KL_ERR_TLS_INIT;
            async_complete_error(c);
            return;
        }

        c->tls = c->tls_cfg->factory(c->tls_cfg->ctx, c->alloc);
        if (!c->tls) {
            c->error = KL_ERR_TLS_INIT;
            async_complete_error(c);
            return;
        }

        if (c->tls->set_hostname && c->host_buf[0])
            c->tls->set_hostname(c->tls, c->host_buf);

        c->state = KL_HCLIENT_TLS_HANDSHAKE;
        async_handle_tls_handshake(c);
        return;
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
    c->error = KL_ERR_TLS_HANDSHAKE;
    async_complete_error(c);
}

/* ── State: SENDING ──────────────────────────────────────────────── */

static void async_handle_sending(KlClient *c)
{
    while (c->request_sent < c->request_len) {
        ssize_t w = io_write(c->ev_ctx->sockets, c->fd, c->tls,
                              c->request_buf + c->request_sent,
                              c->request_len - c->request_sent);
        if (w < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                kl_watcher_rearm(c->ev_ctx, c->fd);
                return;
            }
            c->error = KL_ERR_IO;
            async_complete_error(c);
            return;
        }
        if (w == 0) {
            c->error = KL_ERR_IO;
            async_complete_error(c);
            return;
        }
        c->request_sent += (size_t)w;
    }

    /* If streaming body, transition to chunk-send state */
    if (c->body_read) {
        c->state = KL_HCLIENT_SENDING_STREAM;
        c->chunk_phase = 0;
        async_handle_sending_stream(c);
        return;
    }

    c->state = KL_HCLIENT_RECEIVING;
    kl_watcher_mod(c->ev_ctx, c->fd, KL_EVENT_READ);
}

/* ── State: SENDING_STREAM (chunked body from body_read) ─────────── */

static void async_handle_sending_stream(KlClient *c)
{
    for (;;) {
        switch (c->chunk_phase) {
        case 0: {
            /* Read next chunk from body_read */
            ssize_t nr = c->body_read(c->chunk_buf, sizeof(c->chunk_buf),
                                       c->stream_user_data);
            if (nr < 0) {
                async_complete_error(c);
                return;
            }
            if (nr == 0) {
                /* EOF — send final chunk */
                memcpy(c->chunk_hdr, "0\r\n\r\n", KL_CLIENT_FINAL_CHUNK_LEN);
                c->chunk_hdr_len = KL_CLIENT_FINAL_CHUNK_LEN;
                c->chunk_hdr_sent = 0;
                c->chunk_phase = 4;
                continue;
            }
            c->chunk_len = (size_t)nr;
            c->chunk_sent = 0;
            /* Format chunk header */
            int hl = snprintf(c->chunk_hdr, sizeof(c->chunk_hdr),
                               "%zx\r\n", (size_t)nr);
            if (hl < 0) {
                async_complete_error(c);
                return;
            }
            c->chunk_hdr_len = (size_t)hl;
            c->chunk_hdr_sent = 0;
            c->chunk_phase = 1;
        }
            /* fall through */

        case 1:
            /* Send chunk header */
            while (c->chunk_hdr_sent < c->chunk_hdr_len) {
                ssize_t w = io_write(c->ev_ctx->sockets, c->fd, c->tls,
                                      c->chunk_hdr + c->chunk_hdr_sent,
                                      c->chunk_hdr_len - c->chunk_hdr_sent);
                if (w < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        kl_watcher_rearm(c->ev_ctx, c->fd);
                        return;
                    }
                    async_complete_error(c);
                    return;
                }
                if (w == 0) { async_complete_error(c); return; }
                c->chunk_hdr_sent += (size_t)w;
            }
            c->chunk_phase = 2;
            /* fall through */

        case 2:
            /* Send chunk data */
            while (c->chunk_sent < c->chunk_len) {
                ssize_t w = io_write(c->ev_ctx->sockets, c->fd, c->tls,
                                      c->chunk_buf + c->chunk_sent,
                                      c->chunk_len - c->chunk_sent);
                if (w < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        kl_watcher_rearm(c->ev_ctx, c->fd);
                        return;
                    }
                    async_complete_error(c);
                    return;
                }
                if (w == 0) { async_complete_error(c); return; }
                c->chunk_sent += (size_t)w;
            }
            /* Reuse chunk_hdr for trailing \r\n */
            c->chunk_hdr[0] = '\r';
            c->chunk_hdr[1] = '\n';
            c->chunk_hdr_len = 2;
            c->chunk_hdr_sent = 0;
            c->chunk_phase = 3;
            /* fall through */

        case 3:
            /* Send trailing \r\n */
            while (c->chunk_hdr_sent < c->chunk_hdr_len) {
                ssize_t w = io_write(c->ev_ctx->sockets, c->fd, c->tls,
                                      c->chunk_hdr + c->chunk_hdr_sent,
                                      c->chunk_hdr_len - c->chunk_hdr_sent);
                if (w < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        kl_watcher_rearm(c->ev_ctx, c->fd);
                        return;
                    }
                    async_complete_error(c);
                    return;
                }
                if (w == 0) { async_complete_error(c); return; }
                c->chunk_hdr_sent += (size_t)w;
            }
            /* Next chunk */
            c->chunk_phase = 0;
            continue;

        case 4:
            /* Send final chunk (0\r\n\r\n) */
            while (c->chunk_hdr_sent < c->chunk_hdr_len) {
                ssize_t w = io_write(c->ev_ctx->sockets, c->fd, c->tls,
                                      c->chunk_hdr + c->chunk_hdr_sent,
                                      c->chunk_hdr_len - c->chunk_hdr_sent);
                if (w < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        kl_watcher_rearm(c->ev_ctx, c->fd);
                        return;
                    }
                    async_complete_error(c);
                    return;
                }
                if (w == 0) { async_complete_error(c); return; }
                c->chunk_hdr_sent += (size_t)w;
            }
            /* Done sending — switch to receiving */
            c->state = KL_HCLIENT_RECEIVING;
            kl_watcher_mod(c->ev_ctx, c->fd, KL_EVENT_READ);
            return;

        default:
            async_complete_error(c);
            return;
        }
    }
}

/* ── State: RECEIVING ────────────────────────────────────────────── */

static void async_handle_receiving(KlClient *c)
{
    char buf[KL_CLIENT_RECV_BUF_SIZE];

    for (;;) {
        ssize_t nread = io_read(c->ev_ctx->sockets, c->fd, c->tls, buf, sizeof(buf));
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
            else {
                c->error = KL_ERR_PARSE;
                async_complete_error(c);
            }
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
            c->error = KL_ERR_PARSE;
            async_complete_error(c);
            return;
        }
        /* KL_PARSE_INCOMPLETE — try to read more (non-blocking) */
    }
}

/* ── KlWatcher callback ─────────────────────────────────────────── */

static void async_on_event(KlSocketHandle fd, KlEventMask ready, void *user_data)
{
    KlClient *c = user_data;
    (void)ready;

    switch (c->state) {
    case KL_HCLIENT_RESOLVING:
        break;  /* DNS resolution handled by resolver callback, not watcher */
    case KL_HCLIENT_CONNECTING:
        /* Happy Eyeballs races several fds — dispatch by the fd that fired.
         * The single-fd UNIX / sync-getaddrinfo path uses c->fd directly. */
        if (c->conn_racing)
            he_on_writable(c, fd);
        else
            async_handle_connecting(c);
        break;
    case KL_HCLIENT_PROXY_CONNECTING:
        async_handle_proxy_connecting(c);
        break;
    case KL_HCLIENT_PROXY_HANDSHAKE:
        async_handle_proxy_handshake(c);
        break;
    case KL_HCLIENT_TLS_HANDSHAKE:
        async_handle_tls_handshake(c);
        break;
    case KL_HCLIENT_SENDING:
        async_handle_sending(c);
        break;
    case KL_HCLIENT_SENDING_STREAM:
        async_handle_sending_stream(c);
        break;
    case KL_HCLIENT_RECEIVING:
        async_handle_receiving(c);
        break;
    case KL_HCLIENT_DONE:
        break;
    }
}

/* ── Completion helpers ──────────────────────────────────────────── */

static int server_wants_close(const KlClientResponse *resp)
{
    for (int i = 0; i < resp->num_headers; i++) {
        if (strcasecmp(resp->headers[i].name, "Connection") == 0 &&
            strcasecmp(resp->headers[i].value, "close") == 0)
            return 1;
    }
    return 0;
}

static void async_complete_success(KlClient *c)
{
    he_cancel_timers(c);
    kl_watcher_del(c->ev_ctx, c->fd);

    if (c->pool) {
        /* Pool-aware: release or discard based on Connection header */
        c->pool_conn.fd = c->fd;
        c->pool_conn.tls = c->tls;
        if (server_wants_close(&c->resp)) {
            kl_cpool_discard(c->pool, &c->pool_conn);
        } else {
            kl_cpool_release(c->pool, &c->pool_conn,
                              c->host_buf, c->pool_port, c->pool_is_tls,
                              NULL, 0);
        }
        c->tls = NULL;
        c->fd = KL_INVALID_SOCKET;
    } else {
        if (c->tls) {
            c->tls->shutdown(c->tls, c->fd);
            c->tls->destroy(c->tls);
            c->tls = NULL;
        }
        kl_sock_close(c->ev_ctx->sockets, c->fd);
        c->fd = KL_INVALID_SOCKET;
    }

    if (c->request_buf) {
        kl_free(c->alloc, c->request_buf, c->request_len);
        c->request_buf = NULL;
    }
    if (c->parser) {
        c->parser->destroy(c->parser);
        c->parser = NULL;
    }

    /* Decompress buffered response body if applicable */
    if (!c->decomp_wrap) {
        decompress_response_body(&c->resp, c->decompress_cfg);
    }

    c->state = KL_HCLIENT_DONE;
    c->error = KL_ERR_NONE;
    if (c->on_done)
        c->on_done(c, c->user_data);
}

static void async_complete_error(KlClient *c)
{
    he_cancel_timers(c);
    if (kl_handle_valid(c->fd))
        kl_watcher_del(c->ev_ctx, c->fd);

    if (c->pool) {
        /* Pool-aware: discard the connection on error */
        c->pool_conn.fd = c->fd;
        c->pool_conn.tls = c->tls;
        kl_cpool_discard(c->pool, &c->pool_conn);
        c->tls = NULL;
        c->fd = KL_INVALID_SOCKET;
    } else {
        if (c->tls) {
            if (kl_handle_valid(c->fd))
                c->tls->shutdown(c->tls, c->fd);
            c->tls->destroy(c->tls);
            c->tls = NULL;
        }

        if (kl_handle_valid(c->fd)) {
            kl_sock_close(c->ev_ctx->sockets, c->fd);
            c->fd = KL_INVALID_SOCKET;
        }
    }

    if (c->request_buf) {
        kl_free(c->alloc, c->request_buf, c->request_len);
        c->request_buf = NULL;
    }
    if (c->parser) {
        c->parser->destroy(c->parser);
        c->parser = NULL;
    }

    /* Free proxy buffers */
    if (c->connect_buf) {
        kl_free(c->alloc, c->connect_buf, c->connect_len);
        c->connect_buf = NULL;
    }
    if (c->proxy_recv) {
        kl_free(c->alloc, c->proxy_recv, KL_PROXY_RESPONSE_MAX);
        c->proxy_recv = NULL;
    }

    c->state = KL_HCLIENT_DONE;
    /* error already set by caller — fallback if not set */
    if (c->error == KL_ERR_NONE)
        c->error = KL_ERR_IO;
    if (c->on_done)
        c->on_done(c, c->user_data);
}

/* ── Async public API ────────────────────────────────────────────── */

KlClient *kl_client_start_s(KlEventCtx *ev_ctx, KlAllocator *alloc,
                              const KlClientConfig *cfg,
                              const char *method, const char *url_str,
                              const KlClientHeader *headers, int num_headers,
                              const char *body, size_t body_len,
                              const KlClientStreamCfg *stream,
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
    /* UNIX socket target: no host/port. Normalize the Host header to
     * "localhost" so all request building works unchanged; the connect
     * (below) goes to parsed.unix_path, bypassing DNS. Proxy is incompatible. */
    if (parsed.is_unix) {
        if (cfg && cfg->proxy && cfg->proxy->host)
            return NULL;
        parsed.host = "localhost";
        parsed.host_len = 9;
    }

    KlTlsConfig *tls_cfg = cfg ? cfg->tls : NULL;
    if (parsed.is_https && !tls_cfg)
        return NULL;
    if (!parsed.is_https)
        tls_cfg = NULL;

    size_t max_resp = (cfg && cfg->max_response_size > 0) ? cfg->max_response_size
                                                            : (size_t)KL_CLIENT_DEFAULT_MAX_RESP;

    /* Proxy routing */
    const KlProxyConfig *proxy = cfg ? cfg->proxy : NULL;
    int is_proxied = (proxy && proxy->host);
    int is_tunnel = is_proxied && parsed.is_https;

    /* For HTTPS through proxy: tls_cfg stays set (used after CONNECT),
     * but we don't do TLS on initial connect to proxy */

    /* Build absolute-form URL for HTTP forwarding through proxy */
    char abs_url_buf[KL_CLIENT_REQ_BUF_SIZE];
    const char *absolute_url = NULL;
    if (is_proxied && !parsed.is_https) {
        char host_z[KL_CLIENT_HOSTNAME_MAX];
        if (parsed.host_len >= sizeof(host_z))
            return NULL;
        memcpy(host_z, parsed.host, parsed.host_len);
        host_z[parsed.host_len] = '\0';

        const char *path = (parsed.path_len > 0) ? parsed.path : "/";
        int path_len = (parsed.path_len > 0) ? (int)parsed.path_len : 1;

        int n;
        if (parsed.port == 80)
            n = snprintf(abs_url_buf, sizeof(abs_url_buf),
                         "http://%s%.*s", host_z, path_len, path);
        else
            n = snprintf(abs_url_buf, sizeof(abs_url_buf),
                         "http://%s:%d%.*s", host_z, parsed.port,
                         path_len, path);
        if (n < 0 || (size_t)n >= sizeof(abs_url_buf))
            return NULL;
        absolute_url = abs_url_buf;
    }

    /* Build request buffer — headers-only for streaming, full for buffered */
    size_t req_len = 0;
    char *req_buf;
    if (stream && stream->body_read) {
        req_buf = build_request_headers_only(alloc, method, &parsed,
                                               headers, num_headers, &req_len, 0,
                                               absolute_url);
    } else {
        req_buf = build_request(alloc, method, &parsed,
                                 headers, num_headers, body, body_len,
                                 &req_len, 0, absolute_url);
    }
    if (!req_buf)
        return NULL;

    /* Copy hostname for SNI (always the target host, not the proxy) */
    char host_buf[KL_CLIENT_HOSTNAME_MAX];
    if (parsed.host_len >= sizeof(host_buf)) {
        kl_free(alloc, req_buf, req_len);
        return NULL;
    }
    memcpy(host_buf, parsed.host, parsed.host_len);
    host_buf[parsed.host_len] = '\0';

    /* Allocate client */
    KlClient *c = kl_malloc(alloc, sizeof(KlClient));
    if (!c) {
        kl_free(alloc, req_buf, req_len);
        return NULL;
    }
    memset(c, 0, sizeof(*c));

    c->fd = KL_INVALID_SOCKET;
    c->ev_ctx = ev_ctx;
    c->alloc = alloc;
    c->tls_cfg = is_tunnel ? tls_cfg : NULL;  /* only for CONNECT tunnels */
    c->request_buf = req_buf;
    c->request_len = req_len;
    c->request_sent = 0;
    c->on_done = on_done;
    c->user_data = user_data;
    memcpy(c->host_buf, host_buf, parsed.host_len + 1);

    /* Happy Eyeballs / deadline timer state (timer ids: -1 = unset). */
    c->conn_delay_timer = -1;
    c->deadline_timer = -1;
    c->timeout_ms = (cfg && cfg->timeout_ms > 0) ? cfg->timeout_ms
                                                 : KL_CLIENT_DEFAULT_TIMEOUT_MS;
    c->connect_delay_ms = (cfg && cfg->connect_attempt_delay_ms > 0)
                              ? cfg->connect_attempt_delay_ms
                              : KL_CLIENT_CONNECT_ATTEMPT_DELAY_MS;

    /* Proxy state */
    c->is_proxied = is_proxied;
    c->is_tunnel = is_tunnel;
    c->target_port = (uint16_t)parsed.port;
    c->proxy_auth = (proxy && proxy->auth) ? proxy->auth : NULL;

    /* Non-tunnel direct connections still need tls_cfg */
    if (!is_proxied)
        c->tls_cfg = tls_cfg;

    /* Response decompression config */
    KlDecompressConfig *dcfg = cfg ? cfg->decompress : NULL;
    c->decompress_cfg = dcfg;

    /* Request streaming */
    if (stream && stream->body_read) {
        c->body_read = stream->body_read;
        c->stream_user_data = stream->user_data;
    }

    /* Create response parser (streaming or buffered) */
    if (stream && stream->on_body) {
        /* Wrap streaming callbacks with decompression if configured */
        if (dcfg && dcfg->factory) {
            DecompStreamWrap *w = kl_malloc(alloc, sizeof(DecompStreamWrap));
            if (!w) {
                kl_free(alloc, req_buf, req_len);
                kl_free(alloc, c, sizeof(KlClient));
                return NULL;
            }
            memset(w, 0, sizeof(*w));
            w->user_on_body = stream->on_body;
            w->user_on_headers = stream->on_headers;
            w->user_on_complete = stream->on_complete;
            w->user_data = stream->user_data;
            w->dcfg = dcfg;
            w->ds.alloc = alloc;
            c->decomp_wrap = w;

            c->parser = kl_response_parser_llhttp_s(max_resp, alloc,
                                                      decomp_on_body,
                                                      decomp_on_headers,
                                                      decomp_on_complete,
                                                      w);
        } else {
            c->parser = kl_response_parser_llhttp_s(max_resp, alloc,
                                                      stream->on_body,
                                                      stream->on_headers,
                                                      stream->on_complete,
                                                      stream->user_data);
        }
    } else {
        c->parser = kl_response_parser_llhttp(max_resp, alloc);
    }
    if (!c->parser) {
        if (c->decomp_wrap) {
            kl_free(alloc, c->decomp_wrap, sizeof(DecompStreamWrap));
        }
        kl_free(alloc, req_buf, req_len);
        kl_free(alloc, c, sizeof(KlClient));
        return NULL;
    }

    /* UNIX socket: connect directly, bypassing DNS resolution entirely. */
    if (parsed.is_unix) {
        struct sockaddr_un un;
        socklen_t un_len = fill_sockaddr_un(&un, parsed.unix_path);
        if (un_len == 0 ||
            start_connect(c, (const struct sockaddr *)&un, un_len,
                          AF_UNIX, SOCK_STREAM, 0) < 0) {
            c->parser->destroy(c->parser);
            if (c->decomp_wrap)
                kl_free(alloc, c->decomp_wrap, sizeof(DecompStreamWrap));
            kl_free(alloc, req_buf, req_len);
            kl_free(alloc, c, sizeof(KlClient));
            return NULL;
        }
        he_arm_deadline(c);   /* bound the connect/send/recv (single-fd path) */
        return c;
    }

    /* DNS resolution — resolve proxy host when proxied, target host otherwise */
    const char *resolve_host = is_proxied ? proxy->host : host_buf;
    int resolve_port = is_proxied ? proxy->port : parsed.port;

    /* Async DNS resolver path (explicit, built-in default, or getaddrinfo). */
    int res_owned = 0;
    KlResolver *resolver = client_pick_resolver(cfg, ev_ctx, &res_owned);
    if (resolver) {
        c->resolver = resolver;
        c->owns_resolver = res_owned;
        c->state = KL_HCLIENT_RESOLVING;
        KlResolveReq *rq = resolver->resolve(resolver, ev_ctx,
                                             resolve_host, resolve_port,
                                             dns_resolved, c);
        /* The resolver contract permits synchronous completion: dns_resolved
         * may have already run inside resolve() — firing on_done (DONE) or
         * advancing the connect (CONNECTING/…) — and taken ownership of c. In
         * that case the caller owns c (and frees it via kl_client_free); a NULL
         * return is NOT a start failure and must not free c out from under
         * on_done. Only when we are still RESOLVING did resolve() truly defer. */
        if (c->state != KL_HCLIENT_RESOLVING)
            return c;                 /* rq discarded; resolve_req cleared by dns_resolved */
        c->resolve_req = rq;
        if (!c->resolve_req) {
            if (res_owned)
                resolver->destroy(resolver);
            c->resolver = NULL;
            c->owns_resolver = 0;
            if (c->decomp_wrap)
                kl_free(alloc, c->decomp_wrap, sizeof(DecompStreamWrap));
            c->parser->destroy(c->parser);
            kl_free(alloc, req_buf, req_len);
            kl_free(alloc, c, sizeof(KlClient));
            return NULL;
        }
        return c;
    }

    /* Sync DNS fallback (blocking getaddrinfo) */
    char dns_host_buf[KL_CLIENT_HOSTNAME_MAX];
    if (is_proxied) {
        size_t phl = strlen(proxy->host);
        if (phl >= sizeof(dns_host_buf)) {
            if (c->decomp_wrap)
                kl_free(alloc, c->decomp_wrap, sizeof(DecompStreamWrap));
            c->parser->destroy(c->parser);
            kl_free(alloc, req_buf, req_len);
            kl_free(alloc, c, sizeof(KlClient));
            return NULL;
        }
        memcpy(dns_host_buf, proxy->host, phl + 1);
    } else {
        memcpy(dns_host_buf, host_buf, parsed.host_len + 1);
    }

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", resolve_port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    int rc = getaddrinfo(dns_host_buf, port_str, &hints, &res);
    if (rc != 0 || !res) {
        if (c->decomp_wrap)
            kl_free(alloc, c->decomp_wrap, sizeof(DecompStreamWrap));
        c->parser->destroy(c->parser);
        kl_free(alloc, req_buf, req_len);
        kl_free(alloc, c, sizeof(KlClient));
        return NULL;
    }

    if (start_connect(c, res->ai_addr, res->ai_addrlen,
                       res->ai_family, res->ai_socktype,
                       res->ai_protocol) < 0) {
        freeaddrinfo(res);
        if (c->decomp_wrap)
            kl_free(alloc, c->decomp_wrap, sizeof(DecompStreamWrap));
        c->parser->destroy(c->parser);
        kl_free(alloc, req_buf, req_len);
        kl_free(alloc, c, sizeof(KlClient));
        return NULL;
    }
    freeaddrinfo(res);

    he_arm_deadline(c);   /* bound the connect/send/recv (single-fd path) */
    return c;
}

KlClient *kl_client_start(KlEventCtx *ev_ctx, KlAllocator *alloc,
                           const KlClientConfig *cfg,
                           const char *method, const char *url_str,
                           const KlClientHeader *headers, int num_headers,
                           const char *body, size_t body_len,
                           KlClientDoneFn on_done, void *user_data)
{
    return kl_client_start_s(ev_ctx, alloc, cfg, method, url_str,
                              headers, num_headers, body, body_len,
                              NULL, on_done, user_data);
}

const KlClientResponse *kl_client_response(const KlClient *client)
{
    if (!client || client->error != KL_ERR_NONE)
        return NULL;
    return &client->resp;
}

int kl_client_error(const KlClient *client)
{
    if (!client)
        return -1;
    return client->error != KL_ERR_NONE ? -1 : 0;
}

KlError kl_client_last_error(const KlClient *client)
{
    if (!client)
        return KL_ERR_INVALID_ARG;
    return client->error;
}

void kl_client_cancel(KlClient *client)
{
    if (!client)
        return;

    /* Cancel in-flight DNS resolution */
    if (client->resolve_req && client->resolver) {
        client->resolver->cancel(client->resolve_req);
        client->resolve_req = NULL;
    }

    /* Cancel Happy Eyeballs racing (timers + any in-flight attempt sockets). */
    he_cancel_timers(client);
    he_close_attempts(client, KL_INVALID_SOCKET);

    if (kl_handle_valid(client->fd)) {
        kl_watcher_del(client->ev_ctx, client->fd);

        if (client->pool) {
            client->pool_conn.fd = client->fd;
            client->pool_conn.tls = client->tls;
            kl_cpool_discard(client->pool, &client->pool_conn);
            client->tls = NULL;
            client->fd = KL_INVALID_SOCKET;
        } else {
            if (client->tls) {
                client->tls->shutdown(client->tls, client->fd);
                client->tls->destroy(client->tls);
                client->tls = NULL;
            }

            kl_sock_close(client->ev_ctx->sockets, client->fd);
            client->fd = KL_INVALID_SOCKET;
        }
    }

    client->state = KL_HCLIENT_DONE;
    if (client->error == KL_ERR_NONE)
        client->error = KL_ERR_IO;
}

void kl_client_free(KlClient *client)
{
    if (!client)
        return;

    /* Cancel if still in-flight (cancels any pending resolve_req first). */
    if (client->state != KL_HCLIENT_DONE)
        kl_client_cancel(client);

    /* Destroy the auto-created resolver (after any resolve_req was cancelled). */
    if (client->owns_resolver && client->resolver) {
        client->resolver->destroy(client->resolver);
        client->resolver = NULL;
        client->owns_resolver = 0;
    }

    if (client->request_buf) {
        kl_free(client->alloc, client->request_buf, client->request_len);
        client->request_buf = NULL;
    }

    if (client->parser) {
        client->parser->destroy(client->parser);
        client->parser = NULL;
    }

    kl_client_response_free(&client->resp);

    if (client->decomp_wrap) {
        if (client->decomp_wrap->active)
            kl_decompress_stream_free(&client->decomp_wrap->ds);
        kl_free(client->alloc, client->decomp_wrap, sizeof(DecompStreamWrap));
        client->decomp_wrap = NULL;
    }

    /* Free proxy buffers */
    if (client->connect_buf) {
        kl_free(client->alloc, client->connect_buf, client->connect_len);
        client->connect_buf = NULL;
    }
    if (client->proxy_recv) {
        kl_free(client->alloc, client->proxy_recv, KL_PROXY_RESPONSE_MAX);
        client->proxy_recv = NULL;
    }

    KlAllocator *alloc = client->alloc;
    kl_free(alloc, client, sizeof(KlClient));
}

/* ══════════════════════════════════════════════════════════════════════
 * Pooled client APIs — connection pool integration
 * ══════════════════════════════════════════════════════════════════════ */

int kl_client_request_pooled(KlClientPool *pool,
                              KlAllocator *alloc, const KlClientConfig *cfg,
                              const char *method, const char *url_str,
                              const KlClientHeader *headers, int num_headers,
                              const char *body, size_t body_len,
                              KlClientResponse *resp)
{
    if (!pool || !alloc || !method || !url_str || !resp)
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
    if (kl_url_parse(url_str, &parsed) != 0) {
        resp->error = KL_ERR_URL;
        return -1;
    }
    /* UNIX sockets have no host:port to key the pool on, so bypass the pool
     * and connect directly (local-socket connect is cheap). */
    if (parsed.is_unix) {
        return kl_client_request_s(alloc, cfg, method, url_str,
                                   headers, num_headers, body, body_len,
                                   NULL, resp);
    }

    KlTlsConfig *tls_cfg = cfg ? cfg->tls : NULL;
    int is_tls = parsed.is_https;
    if (is_tls && !tls_cfg) {
        resp->error = KL_ERR_URL;
        return -1;
    }
    if (!is_tls)
        tls_cfg = NULL;

    /* Host string for pool key */
    char host_buf[KL_CLIENT_HOSTNAME_MAX];
    if (parsed.host_len >= sizeof(host_buf)) {
        resp->error = KL_ERR_INVALID_ARG;
        return -1;
    }
    memcpy(host_buf, parsed.host, parsed.host_len);
    host_buf[parsed.host_len] = '\0';

    /* Try to acquire from pool */
    KlClientPoolConn pconn;
    memset(&pconn, 0, sizeof(pconn));
    pconn.fd = -1;
    int acq = kl_cpool_acquire(pool, host_buf, parsed.port, is_tls,
                                NULL, 0, &pconn);

    KlSocketHandle fd;
    KlTls *tls = NULL;
    int ret = -1;

    if (acq == 0) {
        /* Pool hit — reuse connection */
        fd = pconn.fd;
        tls = pconn.tls;
    } else {
        /* Pool miss — connect fresh */
        KlError conn_err = KL_ERR_NONE;
        fd = connect_with_timeout(parsed.host, parsed.host_len,
                                   parsed.port, timeout_ms, &conn_err);
        if (fd < 0) {
            resp->error = conn_err;
            return -1;
        }

        if (is_tls) {
            tls = do_tls_handshake(fd, tls_cfg, parsed.host, parsed.host_len,
                                    timeout_ms, alloc);
            if (!tls) {
                resp->error = KL_ERR_TLS_HANDSHAKE;
                close(fd);
                return -1;
            }
        }

        pconn.fd = fd;
        pconn.tls = tls;
        pconn.reused = 0;
    }

    /* Send with keep-alive (pooled = no proxy support in v1, pass NULL) */
    if (send_request_sync(fd, tls, method, &parsed,
                           headers, num_headers, body, body_len,
                           timeout_ms, 1, NULL) != 0) {
        if (!resp->error) resp->error = KL_ERR_IO;
        goto cleanup;
    }

    if (recv_response_sync(fd, tls, resp, max_resp, timeout_ms, alloc,
                            NULL) != 0) {
        if (!resp->error) resp->error = KL_ERR_PARSE;
        goto cleanup;
    }

    /* Decompress buffered response body if applicable */
    {
        KlDecompressConfig *dcfg = cfg ? cfg->decompress : NULL;
        if (decompress_response_body(resp, dcfg) < 0) {
            if (!resp->error) resp->error = KL_ERR_COMPRESS;
            goto cleanup;
        }
    }

    ret = 0;

cleanup:
    if (ret != 0) {
        kl_cpool_discard(pool, &pconn);
        kl_client_response_free(resp);
    } else if (server_wants_close(resp)) {
        kl_cpool_discard(pool, &pconn);
    } else {
        kl_cpool_release(pool, &pconn, host_buf, parsed.port, is_tls,
                          NULL, 0);
    }

    return ret;
}

KlClient *kl_client_start_pooled(KlClientPool *pool,
                                   KlEventCtx *ev_ctx, KlAllocator *alloc,
                                   const KlClientConfig *cfg,
                                   const char *method, const char *url_str,
                                   const KlClientHeader *headers, int num_headers,
                                   const char *body, size_t body_len,
                                   KlClientDoneFn on_done, void *user_data)
{
    if (!pool || !ev_ctx || !alloc || !method || !url_str)
        return NULL;
    if (num_headers < 0 || num_headers > KL_CLIENT_MAX_REQ_HEADERS)
        return NULL;
    if (num_headers > 0 && !headers)
        return NULL;

    KlUrl parsed;
    if (kl_url_parse(url_str, &parsed) != 0)
        return NULL;
    /* UNIX sockets are not pooled (no host:port key); bypass the pool and
     * start a normal non-pooled async request. */
    if (parsed.is_unix) {
        return kl_client_start_s(ev_ctx, alloc, cfg, method, url_str,
                                 headers, num_headers, body, body_len,
                                 NULL, on_done, user_data);
    }

    KlTlsConfig *tls_cfg = cfg ? cfg->tls : NULL;
    int is_tls = parsed.is_https;
    if (is_tls && !tls_cfg)
        return NULL;
    if (!is_tls)
        tls_cfg = NULL;

    size_t max_resp = (cfg && cfg->max_response_size > 0) ? cfg->max_response_size
                                                            : (size_t)KL_CLIENT_DEFAULT_MAX_RESP;

    /* Build request buffer with keep-alive (pooled = no proxy in v1) */
    size_t req_len = 0;
    char *req_buf = build_request(alloc, method, &parsed,
                                   headers, num_headers, body, body_len,
                                   &req_len, 1, NULL);
    if (!req_buf)
        return NULL;

    /* Copy hostname for SNI + pool key */
    char host_buf[KL_CLIENT_HOSTNAME_MAX];
    if (parsed.host_len >= sizeof(host_buf)) {
        kl_free(alloc, req_buf, req_len);
        return NULL;
    }
    memcpy(host_buf, parsed.host, parsed.host_len);
    host_buf[parsed.host_len] = '\0';

    /* Allocate client */
    KlClient *c = kl_malloc(alloc, sizeof(KlClient));
    if (!c) {
        kl_free(alloc, req_buf, req_len);
        return NULL;
    }
    memset(c, 0, sizeof(*c));

    c->fd = KL_INVALID_SOCKET;
    c->ev_ctx = ev_ctx;
    c->alloc = alloc;
    c->tls_cfg = tls_cfg;
    c->request_buf = req_buf;
    c->request_len = req_len;
    c->request_sent = 0;
    c->on_done = on_done;
    c->user_data = user_data;
    memcpy(c->host_buf, host_buf, parsed.host_len + 1);

    /* Pool integration */
    c->pool = pool;
    c->pool_port = parsed.port;
    c->pool_is_tls = is_tls;

    /* Response decompression config */
    c->decompress_cfg = cfg ? cfg->decompress : NULL;

    /* Create response parser */
    c->parser = kl_response_parser_llhttp(max_resp, alloc);
    if (!c->parser) {
        kl_free(alloc, req_buf, req_len);
        kl_free(alloc, c, sizeof(KlClient));
        return NULL;
    }

    /* Try pool acquire */
    KlClientPoolConn pconn;
    memset(&pconn, 0, sizeof(pconn));
    pconn.fd = -1;
    int acq = kl_cpool_acquire(pool, host_buf, parsed.port, is_tls,
                                NULL, 0, &pconn);

    if (acq == 0) {
        /* Pool hit — skip connect + TLS, go straight to sending */
        c->fd = pconn.fd;
        c->tls = pconn.tls;
        c->pool_conn = pconn;
        c->state = KL_HCLIENT_SENDING;

        if (kl_watcher_add(ev_ctx, c->fd, KL_EVENT_WRITE, async_on_event, c) != 0) {
            c->fd = KL_INVALID_SOCKET;
            c->tls = NULL;
            kl_cpool_discard(pool, &pconn);
            if (c->decomp_wrap)
                kl_free(alloc, c->decomp_wrap, sizeof(DecompStreamWrap));
            c->parser->destroy(c->parser);
            kl_free(alloc, req_buf, req_len);
            kl_free(alloc, c, sizeof(KlClient));
            return NULL;
        }
        return c;
    }

    /* Pool miss — normal connect flow */
    int res_owned = 0;
    KlResolver *resolver = client_pick_resolver(cfg, ev_ctx, &res_owned);
    if (resolver) {
        c->resolver = resolver;
        c->owns_resolver = res_owned;
        c->state = KL_HCLIENT_RESOLVING;
        c->resolve_req = resolver->resolve(resolver, ev_ctx,
                                            host_buf, parsed.port,
                                            dns_resolved, c);
        if (!c->resolve_req) {
            if (res_owned)
                resolver->destroy(resolver);
            c->resolver = NULL;
            c->owns_resolver = 0;
            if (c->decomp_wrap)
                kl_free(alloc, c->decomp_wrap, sizeof(DecompStreamWrap));
            c->parser->destroy(c->parser);
            kl_free(alloc, req_buf, req_len);
            kl_free(alloc, c, sizeof(KlClient));
            return NULL;
        }
        return c;
    }

    /* Sync DNS fallback */
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", parsed.port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    int rc = getaddrinfo(host_buf, port_str, &hints, &res);
    if (rc != 0 || !res) {
        if (c->decomp_wrap)
            kl_free(alloc, c->decomp_wrap, sizeof(DecompStreamWrap));
        c->parser->destroy(c->parser);
        kl_free(alloc, req_buf, req_len);
        kl_free(alloc, c, sizeof(KlClient));
        return NULL;
    }

    if (start_connect(c, res->ai_addr, res->ai_addrlen,
                       res->ai_family, res->ai_socktype,
                       res->ai_protocol) < 0) {
        freeaddrinfo(res);
        if (c->decomp_wrap)
            kl_free(alloc, c->decomp_wrap, sizeof(DecompStreamWrap));
        c->parser->destroy(c->parser);
        kl_free(alloc, req_buf, req_len);
        kl_free(alloc, c, sizeof(KlClient));
        return NULL;
    }
    freeaddrinfo(res);

    return c;
}
