/*
 * client_sync.c — HTTP/1.1 client, blocking (hosted) API
 *
 * Freestanding step B2b: the blocking poll()-based request/response path lives
 * here — connect_with_timeout, the sync TLS handshake, sync proxy CONNECT, the
 * send_*_sync / recv_response_sync loops, and the public kl_client_request[_s]
 * + kl_client_request_pooled (blocking) entry points. This is the only client
 * TU that uses read()/write()/kl_plat_poll1()/errno and blocking DNS, so a
 * freestanding async build links client_common + client_async without it.
 *
 * All allocation through KlAllocator. No Hull dependencies.
 */

#include <keel/client.h>
#include <keel/client_pool.h>
#include <keel/decompress.h>
#include <keel/parser.h>

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>   /* strcasecmp (no longer pulled transitively via request.h) */
#include <unistd.h>
#include <stddef.h>
#include <sys/types.h>

#include "socket.h"       /* seam: kl_sock_* + KlSockAddr (no direct sockaddr) */
#include "resolve_sync.h" /* kl_resolve_sync — blocking name resolution -> KlSockAddr */
#include "platform.h"     /* kl_plat_poll1 — sync readiness wait (poll/WSAPoll) */
#include "client_internal.h"

/* ── Connect with timeout ────────────────────────────────────────── */

static KlSocketHandle connect_with_timeout(const char *host, size_t host_len,
                                 int port, int timeout_ms,
                                 const KlSocketProvider *sockets,
                                 KlError *out_err)
{
    char host_buf[KL_CLIENT_HOSTNAME_MAX];
    if (host_len >= sizeof(host_buf)) {
        if (out_err) *out_err = KL_ERR_INVALID_ARG;
        return -1;
    }
    memcpy(host_buf, host, host_len);
    host_buf[host_len] = '\0';

    KlSockAddr addrs[KL_RESOLVE_MAX_ADDRS];
    int naddr = 0;
    if (kl_resolve_sync(host_buf, (uint16_t)port, SOCK_STREAM,
                        addrs, KL_RESOLVE_MAX_ADDRS, &naddr) != 0) {
        if (out_err) *out_err = KL_ERR_DNS;
        return -1;
    }
    const KlSockAddr *csa = &addrs[0];
    int family = (kl_sockaddr_family(csa) == KL_AF_INET6) ? AF_INET6 : AF_INET;

    KlSocketHandle fd = kl_sock_socket(sockets, family, SOCK_STREAM, 0);
    if (!kl_handle_valid(fd)) {
        if (out_err) *out_err = KL_ERR_SOCKET;
        return -1;
    }

    kl_sock_set_nosigpipe(sockets, fd);

    /* Nonblocking for the timed connect; restored to blocking below. */
    if (kl_sock_set_nonblocking(sockets, fd) < 0) {
        if (out_err) *out_err = KL_ERR_SOCKET;
        kl_sock_close(sockets, fd);
        return -1;
    }

    int rc = kl_sock_connect(sockets, fd, csa);

    if (rc < 0 && errno != EINPROGRESS) {
        if (out_err) *out_err = KL_ERR_CONNECT;
        kl_sock_close(sockets, fd);
        return -1;
    }

    if (rc < 0) {
        int pr = kl_plat_poll1(fd, KL_POLL_OUT, timeout_ms);
        if (pr <= 0) {
            if (out_err) *out_err = (pr == 0) ? KL_ERR_TIMEOUT : KL_ERR_CONNECT;
            kl_sock_close(sockets, fd);
            return -1;
        }

        int err = 0;
        kl_sock_get_so_error(sockets, fd, &err);
        if (err != 0) {
            if (out_err) *out_err = KL_ERR_CONNECT;
            kl_sock_close(sockets, fd);
            return -1;
        }
    }

    kl_sock_set_blocking(sockets, fd);   /* restore blocking mode */

    return fd;
}

/* ── UNIX socket address + connect ───────────────────────────────── */

static KlSocketHandle unix_connect_with_timeout(const char *path, int timeout_ms,
                                     const KlSocketProvider *sockets,
                                     KlError *out_err)
{
    KlSockAddr usa;
    if (kl_sockaddr_from_unix(&usa, path) != 0) {
        if (out_err) *out_err = KL_ERR_INVALID_ARG;
        return -1;
    }

    KlSocketHandle fd = kl_sock_socket(sockets, AF_UNIX, SOCK_STREAM, 0);
    if (!kl_handle_valid(fd)) {
        if (out_err) *out_err = KL_ERR_SOCKET;
        return -1;
    }

    kl_sock_set_nosigpipe(sockets, fd);

    /* Nonblocking for the timed connect; restored to blocking below. */
    if (kl_sock_set_nonblocking(sockets, fd) < 0) {
        if (out_err) *out_err = KL_ERR_SOCKET;
        kl_sock_close(sockets, fd);
        return -1;
    }

    int rc = kl_sock_connect(sockets, fd, &usa);
    if (rc < 0 && errno != EINPROGRESS) {
        if (out_err) *out_err = KL_ERR_CONNECT;
        kl_sock_close(sockets, fd);
        return -1;
    }

    if (rc < 0) {
        int pr = kl_plat_poll1(fd, KL_POLL_OUT, timeout_ms);
        if (pr <= 0) {
            if (out_err) *out_err = (pr == 0) ? KL_ERR_TIMEOUT : KL_ERR_CONNECT;
            kl_sock_close(sockets, fd);
            return -1;
        }
        int err = 0;
        kl_sock_get_so_error(sockets, fd, &err);
        if (err != 0) {
            if (out_err) *out_err = KL_ERR_CONNECT;
            kl_sock_close(sockets, fd);
            return -1;
        }
    }

    kl_sock_set_blocking(sockets, fd);   /* restore blocking mode */
    return fd;
}

/* ── TLS handshake (sync) ────────────────────────────────────────── */

static KlTls *do_tls_handshake(KlSocketHandle fd, KlTlsConfig *tls_cfg,
                                 const char *host, size_t host_len,
                                 int timeout_ms, KlAllocator *alloc,
                                 const KlSocketProvider *sockets)
{
    if (!tls_cfg || !tls_cfg->factory)
        return NULL;

    KlTls *tls = tls_cfg->factory(tls_cfg->ctx, alloc);
    if (!tls)
        return NULL;

    /* Route the TLS socket-BIO through the client's socket provider (e.g. lwIP),
     * so TLS I/O matches the connection's stack without per-app config. */
    if (tls->set_socket_provider)
        tls->set_socket_provider(tls, sockets);

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

        int events = (r == KL_TLS_WANT_READ) ? KL_POLL_IN : KL_POLL_OUT;
        int pr = kl_plat_poll1(fd, events, step);
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

static int proxy_connect_sync(KlSocketHandle fd, const char *host, uint16_t port,
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
        int pr = kl_plat_poll1(fd, KL_POLL_OUT, timeout_ms);
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

        int pr = kl_plat_poll1(fd, KL_POLL_IN, timeout_ms);
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

static int send_request_sync(const KlSocketProvider *sockets, KlSocketHandle fd, KlTls *tls,
                              const char *method, const KlUrl *url,
                              const KlClientHeader *headers, int num_headers,
                              const char *body, size_t body_len,
                              int timeout_ms, int keep_alive,
                              const char *absolute_url)
{
    if (kl_client_has_crlf(method, strlen(method)))
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
        if (kl_client_has_crlf(headers[i].name, strlen(headers[i].name)) ||
            kl_client_has_crlf(headers[i].value, strlen(headers[i].value)))
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
        int pr = kl_plat_poll1(fd, KL_POLL_OUT, timeout_ms);
        if (pr <= 0)
            return -1;

        ssize_t w = kl_client_io_write(sockets, fd, tls, buf + sent, (size_t)off - sent);
        if (w <= 0)
            return -1;
        sent += (size_t)w;
    }

    /* Send body */
    if (body && body_len > 0) {
        sent = 0;
        while (sent < body_len) {
            int pr = kl_plat_poll1(fd, KL_POLL_OUT, timeout_ms);
            if (pr <= 0)
                return -1;

            ssize_t w = kl_client_io_write(sockets, fd, tls, body + sent, body_len - sent);
            if (w <= 0)
                return -1;
            sent += (size_t)w;
        }
    }

    return 0;
}

/* ── Send headers-only (for chunked body streaming) ──────────────── */

static int send_headers_sync(const KlSocketProvider *sockets, KlSocketHandle fd, KlTls *tls,
                               const char *method, const KlUrl *url,
                               const KlClientHeader *headers, int num_headers,
                               int timeout_ms, int keep_alive,
                               const char *absolute_url)
{
    if (kl_client_has_crlf(method, strlen(method)))
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
        if (kl_client_has_crlf(headers[i].name, strlen(headers[i].name)) ||
            kl_client_has_crlf(headers[i].value, strlen(headers[i].value)))
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
        int pr = kl_plat_poll1(fd, KL_POLL_OUT, timeout_ms);
        if (pr <= 0)
            return -1;

        ssize_t w = kl_client_io_write(sockets, fd, tls, buf + sent, (size_t)off - sent);
        if (w <= 0)
            return -1;
        sent += (size_t)w;
    }

    return 0;
}

/* ── Send chunked body from body_read callback (sync) ────────────── */

static int send_all_sync(const KlSocketProvider *sockets, KlSocketHandle fd, KlTls *tls, const char *data, size_t len,
                           int timeout_ms)
{
    size_t sent = 0;
    while (sent < len) {
        int pr = kl_plat_poll1(fd, KL_POLL_OUT, timeout_ms);
        if (pr <= 0)
            return -1;

        ssize_t w = kl_client_io_write(sockets, fd, tls, data + sent, len - sent);
        if (w <= 0)
            return -1;
        sent += (size_t)w;
    }
    return 0;
}

static int send_body_chunked_sync(const KlSocketProvider *sockets, KlSocketHandle fd, KlTls *tls,
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
            if (send_all_sync(sockets, fd, tls, "0\r\n\r\n",
                               KL_CLIENT_FINAL_CHUNK_LEN, timeout_ms) != 0)
                return -1;
            return 0;
        }

        /* Chunk header: <hex-len>\r\n */
        int hdr_len = snprintf(hdr_buf, sizeof(hdr_buf), "%zx\r\n", (size_t)nread);
        if (hdr_len < 0)
            return -1;

        if (send_all_sync(sockets, fd, tls, hdr_buf, (size_t)hdr_len, timeout_ms) != 0)
            return -1;
        if (send_all_sync(sockets, fd, tls, data_buf, (size_t)nread, timeout_ms) != 0)
            return -1;
        if (send_all_sync(sockets, fd, tls, "\r\n", sizeof("\r\n") - 1, timeout_ms) != 0)
            return -1;
    }
}

/* ── Receive + parse response (sync, with optional streaming) ────── */

static int recv_response_sync(const KlSocketProvider *sockets, KlSocketHandle fd, KlTls *tls, KlClientResponse *resp,
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
        int pr = kl_plat_poll1(fd, KL_POLL_IN, timeout_ms);
        if (pr <= 0)
            break;

        ssize_t nread = kl_client_io_read(sockets, fd, tls, buf, sizeof(buf));
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

    /* Selected socket provider (NULL = built-in default) — threaded through the
     * whole sync path (connect + I/O), never hardcoded. */
    const KlSocketProvider *sockets = cfg ? cfg->sockets : NULL;

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
        fd = unix_connect_with_timeout(parsed.unix_path, timeout_ms, sockets, &conn_err);
    } else if (is_proxied) {
        fd = connect_with_timeout(proxy->host, strlen(proxy->host),
                                   proxy->port, timeout_ms, sockets, &conn_err);
    } else {
        fd = connect_with_timeout(parsed.host, parsed.host_len,
                                   parsed.port, timeout_ms, sockets, &conn_err);
    }
    if (!kl_handle_valid(fd)) {
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
                                timeout_ms, alloc, sockets);
        if (!tls) {
            resp->error = KL_ERR_TLS_HANDSHAKE;
            goto cleanup;
        }
    } else if (parsed.is_https) {
        tls = do_tls_handshake(fd, tls_cfg, parsed.host, parsed.host_len,
                                timeout_ms, alloc, sockets);
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

        wrapped_stream.on_body = kl_client_decomp_on_body;
        wrapped_stream.on_headers = kl_client_decomp_on_headers;
        wrapped_stream.on_complete = kl_client_decomp_on_complete;
        wrapped_stream.body_read = stream->body_read;
        wrapped_stream.user_data = &decomp_wrap;
        actual_stream = &wrapped_stream;
        decomp_installed = 1;
    }

    /* Request streaming: send headers + chunked body */
    if (stream && stream->body_read) {
        if (send_headers_sync(sockets, fd, tls, method, &parsed,
                                headers, num_headers, timeout_ms, 0,
                                absolute_url) != 0) {
            if (!resp->error) resp->error = KL_ERR_IO;
            goto cleanup;
        }
        if (send_body_chunked_sync(sockets, fd, tls, stream->body_read,
                                     stream->user_data, timeout_ms) != 0) {
            if (!resp->error) resp->error = KL_ERR_IO;
            goto cleanup;
        }
    } else {
        if (send_request_sync(sockets, fd, tls, method, &parsed,
                               headers, num_headers, body, body_len,
                               timeout_ms, 0, absolute_url) != 0) {
            if (!resp->error) resp->error = KL_ERR_IO;
            goto cleanup;
        }
    }

    if (recv_response_sync(sockets, fd, tls, resp, max_resp, timeout_ms, alloc,
                            actual_stream) != 0) {
        if (!resp->error) resp->error = KL_ERR_PARSE;
        goto cleanup;
    }

    /* Decompress buffered response body if applicable */
    if (!stream || !stream->on_body) {
        if (kl_client_decompress_response_body(resp, dcfg) < 0) {
            if (!resp->error) resp->error = KL_ERR_COMPRESS;
            goto cleanup;
        }
    }

    ret = 0;

cleanup:
    /* Free the streaming decompressor session if it was installed.  It is
     * otherwise freed only by kl_client_decomp_on_complete (fired on parser
     * message-complete), so error paths and EOF-terminated success would
     * leak it.  kl_decompress_stream_free is idempotent, so freeing after a
     * normal completion is safe. */
    if (decomp_installed)
        kl_decompress_stream_free(&decomp_wrap.ds);
    if (tls) {
        tls->shutdown(tls, fd);
        tls->destroy(tls);
    }
    kl_sock_close(sockets, fd);

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

/* ══════════════════════════════════════════════════════════════════════
 * Pooled blocking request — connection pool integration (sync path)
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

    /* Selected socket provider (NULL = built-in default) — threaded through the
     * whole sync path (connect + I/O), never hardcoded. */
    const KlSocketProvider *sockets = cfg ? cfg->sockets : NULL;

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
                                   parsed.port, timeout_ms, sockets, &conn_err);
        if (!kl_handle_valid(fd)) {
            resp->error = conn_err;
            return -1;
        }

        if (is_tls) {
            tls = do_tls_handshake(fd, tls_cfg, parsed.host, parsed.host_len,
                                    timeout_ms, alloc, sockets);
            if (!tls) {
                resp->error = KL_ERR_TLS_HANDSHAKE;
                kl_sock_close(sockets, fd);
                return -1;
            }
        }

        pconn.fd = fd;
        pconn.tls = tls;
        pconn.reused = 0;
    }

    /* Send with keep-alive (pooled = no proxy support in v1, pass NULL) */
    if (send_request_sync(sockets, fd, tls, method, &parsed,
                           headers, num_headers, body, body_len,
                           timeout_ms, 1, NULL) != 0) {
        if (!resp->error) resp->error = KL_ERR_IO;
        goto cleanup;
    }

    if (recv_response_sync(sockets, fd, tls, resp, max_resp, timeout_ms, alloc,
                            NULL) != 0) {
        if (!resp->error) resp->error = KL_ERR_PARSE;
        goto cleanup;
    }

    /* Decompress buffered response body if applicable */
    {
        KlDecompressConfig *dcfg = cfg ? cfg->decompress : NULL;
        if (kl_client_decompress_response_body(resp, dcfg) < 0) {
            if (!resp->error) resp->error = KL_ERR_COMPRESS;
            goto cleanup;
        }
    }

    ret = 0;

cleanup:
    if (ret != 0) {
        kl_cpool_discard(pool, &pconn);
        kl_client_response_free(resp);
    } else if (kl_client_server_wants_close(resp)) {
        kl_cpool_discard(pool, &pconn);
    } else {
        kl_cpool_release(pool, &pconn, host_buf, parsed.port, is_tls,
                          NULL, 0);
    }

    return ret;
}
