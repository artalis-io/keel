/*
 * http_client_sync.c: HTTP/1.1 client, blocking (hosted) API
 *
 * The blocking poll()-based request/response path lives
 * here: connect_with_timeout, the sync TLS handshake, sync proxy CONNECT, the
 * send_*_sync / recv_response_sync loops, and the public kl_http_client_request[_s]
 * + kl_http_client_request_pooled (blocking) entry points. All I/O routes through the
 * socket-provider seam (kl_sock_send/recv + kl_sock_io_status, no raw read()/
 * write()/errno); the only hosted dependencies are the blocking wait
 * (kl_plat_poll1) and blocking DNS, so a freestanding async build links
 * client_common + client_async without this TU.
 *
 * All allocation through KlAllocator. No Hull dependencies.
 */

#include <keel/http_client.h>
#include <keel/http_client_pool.h>
#include <keel/decompress.h>
#include <keel/http1_parser.h>

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <sys/types.h>

#include "socket.h"       /* seam: kl_sock_* + KlSockAddr (no direct sockaddr) */
#include "../../allocator_validate.h"   /* kl_allocator_ops_valid: valid-allocator gate */
#include "resolve_sync.h" /* kl_resolve_sync: blocking name resolution -> KlSockAddr */
#include "platform.h"     /* kl_plat_poll1: sync readiness wait (poll/WSAPoll) */
#include "http_client_internal.h"
#include "http_client_proxy.h"   /* shared CONNECT serialization + status (no sync/async drift) */

/* ── Request deadline ────────────────────────────────────────────── */

/* The whole request runs against one deadline (cfg->timeout_ms from the start of the call): the
 * connect, the TLS handshake, every send and every receive. The socket stays non-blocking
 * throughout, so no single step can outlast it; each wait gets only the time that is left. */
static uint64_t deadline_after(int timeout_ms)
{
    return kl_monotonic_ms() + (uint64_t)timeout_ms;
}

static int deadline_passed(uint64_t deadline)
{
    return kl_monotonic_ms() >= deadline;
}

/* Wait until fd is ready for events or the deadline passes. >0 ready, 0 deadline, -1 failure. */
static int wait_ready(KlSocketHandle fd, int events, uint64_t deadline)
{
    for (;;) {
        uint64_t now = kl_monotonic_ms();
        if (now >= deadline)
            return 0;
        uint64_t left = deadline - now;
        int pr = kl_plat_poll1(fd, events, left > (uint64_t)INT_MAX ? INT_MAX : (int)left);
        if (pr != 0)
            return pr;
    }
}

/* The error for a step that failed: the deadline if it has passed, else the step's own. */
static KlError step_error(uint64_t deadline, KlError err)
{
    return deadline_passed(deadline) ? KL_ERR_TIMEOUT : err;
}

/* ── Connect with timeout ────────────────────────────────────────── */

/* One non-blocking connect to sa, bounded by attempt_deadline. The socket is returned
 * non-blocking. */
static KlSocketHandle connect_one(const KlSocketProvider *sockets, int family,
                                  const KlSockAddr *sa, uint64_t attempt_deadline,
                                  KlError *out_err)
{
    KlSocketHandle fd = kl_sock_socket(sockets, family, SOCK_STREAM, 0);
    if (!kl_handle_valid(fd)) {
        *out_err = KL_ERR_SOCKET;
        return -1;
    }
    kl_sock_set_cloexec(sockets, fd);   /* never inherited by an embedder's children */

    kl_sock_set_nosigpipe(sockets, fd);

    if (kl_sock_set_nonblocking(sockets, fd) < 0) {
        *out_err = KL_ERR_SOCKET;
        kl_sock_close(sockets, fd);
        return -1;
    }

    int rc = kl_sock_connect(sockets, fd, sa);

    if (rc < 0 && kl_sock_io_status(sockets) != KL_IO_PENDING) {
        *out_err = KL_ERR_CONNECT;
        kl_sock_close(sockets, fd);
        return -1;
    }

    if (rc < 0) {
        int pr = wait_ready(fd, KL_POLL_OUT, attempt_deadline);
        if (pr <= 0) {
            *out_err = (pr == 0) ? KL_ERR_TIMEOUT : KL_ERR_CONNECT;
            kl_sock_close(sockets, fd);
            return -1;
        }

        int err = 0;
        kl_sock_get_so_error(sockets, fd, &err);
        if (err != 0) {
            *out_err = KL_ERR_CONNECT;
            kl_sock_close(sockets, fd);
            return -1;
        }
    }

    return fd;
}

/* Connect to host:port, trying each resolved address in turn until one connects. Every address but
 * the last gets an equal share of the time left, so one that never answers cannot use up the whole
 * request deadline. */
static KlSocketHandle connect_with_timeout(const char *host, size_t host_len,
                                 int port, uint64_t deadline,
                                 const KlSocketProvider *sockets,
                                 KlError *out_err)
{
    char host_buf[KL_HTTP_CLIENT_HOSTNAME_MAX];
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

    KlError err = KL_ERR_CONNECT;
    for (int i = 0; i < naddr; i++) {
        uint64_t now = kl_monotonic_ms();
        if (now >= deadline) {
            err = KL_ERR_TIMEOUT;
            break;
        }
        uint64_t attempt_deadline = now + (deadline - now) / (uint64_t)(naddr - i);
        int family = (kl_sockaddr_family(&addrs[i]) == KL_AF_INET6) ? AF_INET6 : AF_INET;
        KlSocketHandle fd = connect_one(sockets, family, &addrs[i], attempt_deadline, &err);
        if (kl_handle_valid(fd))
            return fd;
    }
    if (out_err) *out_err = err;
    return -1;
}

/* ── UNIX socket address + connect ───────────────────────────────── */

static KlSocketHandle unix_connect_with_timeout(const char *path, uint64_t deadline,
                                     const KlSocketProvider *sockets,
                                     KlError *out_err)
{
    KlSockAddr usa;
    if (kl_sockaddr_from_unix(&usa, path) != 0) {
        if (out_err) *out_err = KL_ERR_INVALID_ARG;
        return -1;
    }
    KlError err = KL_ERR_CONNECT;
    KlSocketHandle fd = connect_one(sockets, AF_UNIX, &usa, deadline, &err);
    if (!kl_handle_valid(fd) && out_err)
        *out_err = err;
    return fd;
}

/* ── TLS handshake (sync) ────────────────────────────────────────── */

static KlTls *do_tls_handshake(KlSocketHandle fd, KlTlsConfig *tls_cfg,
                                 const char *host, size_t host_len,
                                 uint64_t deadline, KlAllocator *alloc,
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

    /* Set SNI hostname via vtable (backend-agnostic). FAIL CLOSED: if the host
     * does not fit the buffer, or set_hostname() reports failure, abort; a
     * missing hostname check would let a cert for the wrong host verify. */
    if (!tls->set_hostname) {   /* no way to verify the host name: never proceed without it */
        tls->destroy(tls);
        return NULL;
    }
    {
        char host_buf[KL_HTTP_CLIENT_HOSTNAME_MAX];
        if (host_len >= sizeof(host_buf)) {
            tls->destroy(tls);
            return NULL;
        }
        memcpy(host_buf, host, host_len);
        host_buf[host_len] = '\0';
        if (tls->set_hostname(tls, host_buf) != 0) {
            tls->destroy(tls);
            return NULL;
        }
    }

    for (;;) {
        KlTlsResult r = tls->handshake(tls, fd);
        if (r == KL_TLS_OK)
            return tls;
        if (r == KL_TLS_ERROR) {
            tls->destroy(tls);
            return NULL;
        }

        int events = (r == KL_TLS_WANT_READ) ? KL_POLL_IN : KL_POLL_OUT;
        if (wait_ready(fd, events, deadline) <= 0) {
            tls->destroy(tls);
            return NULL;
        }
    }
}

/* ── Send all (sync) ─────────────────────────────────────────────── */

/* Send len bytes, plain or through TLS, waiting for writable whenever the socket (or the TLS
 * engine, with WANT_WRITE) cannot take more. 0 = all sent, -1 = failure or the deadline. */
static int send_all_sync(const KlSocketProvider *sockets, KlSocketHandle fd, KlTls *tls,
                         const char *data, size_t len, uint64_t deadline)
{
    size_t sent = 0;
    while (sent < len) {
        kl_ssize_t w = kl_http_client_io_write(sockets, fd, tls, data + sent, len - sent);
        if (w > 0) {
            sent += (size_t)w;
            continue;
        }
        if (tls ? w < 0 : w == 0)
            return -1;   /* TLS: -1 is an error; plain: a send that moved nothing */
        if (w < 0) {
            KlIoStatus st = kl_sock_io_status(sockets);
            if (st == KL_IO_INTERRUPTED)
                continue;
            if (st != KL_IO_WOULD_BLOCK)
                return -1;
        }
        if (wait_ready(fd, KL_POLL_OUT, deadline) <= 0)
            return -1;
    }
    return 0;
}

/* ── Proxy CONNECT handshake (sync) ──────────────────────────────── */

static int proxy_connect_sync(const KlSocketProvider *sockets, KlSocketHandle fd,
                                const char *host, uint16_t port,
                                const char *proxy_auth, uint64_t deadline)
{
    char buf[KL_PROXY_RESPONSE_MAX];
    size_t req_len = 0;
    /* Shared serialization (http_client_proxy.c): identical bytes to the async client. */
    if (kl_proxy_build_connect(buf, sizeof(buf), &req_len, host, port, proxy_auth) != 0)
        return -1;

    /* Send CONNECT request */
    if (send_all_sync(sockets, fd, NULL, buf, req_len, deadline) != 0)
        return -1;

    /* Read proxy response: look for "HTTP/1.x 2xx" */
    size_t recv_len = 0;
    for (;;) {
        if (recv_len >= sizeof(buf) - 1)
            return -1;  /* response too large */

        int pr = wait_ready(fd, KL_POLL_IN, deadline);
        if (pr <= 0)
            return -1;

        kl_ssize_t r = kl_sock_recv(sockets, fd, buf + recv_len, sizeof(buf) - 1 - recv_len);
        if (r <= 0) {
            if (r < 0) {
                KlIoStatus st = kl_sock_io_status(sockets);
                if (st == KL_IO_INTERRUPTED || st == KL_IO_WOULD_BLOCK)
                    continue;
            }
            return -1;
        }
        recv_len += (size_t)r;
        buf[recv_len] = '\0';

        /* Shared status check (http_client_proxy.c): 1 = tunnel up, 0 = need more, -1 = error. */
        int st = kl_proxy_connect_status(buf, recv_len);
        if (st != 0)
            return st == 1 ? 0 : -1;
    }
}

/* ── Build request into stack buffer, send ───────────────────────── */

static int send_request_sync(const KlSocketProvider *sockets, KlSocketHandle fd, KlTls *tls,
                              const char *method, const KlUrl *url,
                              const KlHttpClientHeader *headers, int num_headers,
                              const char *body, size_t body_len,
                              uint64_t deadline, int keep_alive,
                              const char *absolute_url)
{
    if (kl_http_client_has_crlf(method, strlen(method)))
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

    char authority[KL_HTTP_CLIENT_HOSTNAME_MAX + 16];
    int alen = kl_http_client_authority(url, authority, sizeof authority);
    if (alen < 0) return -1;
    char buf[KL_HTTP_CLIENT_REQ_BUF_SIZE];
    int off = snprintf(buf, sizeof(buf), "%s %.*s HTTP/1.1\r\nHost: %.*s\r\n",
                       method,
                       target_len, target,
                       alen, authority);

    if (off < 0 || (size_t)off >= sizeof(buf))
        return -1;

    for (int i = 0; i < num_headers; i++) {
        if (kl_http_client_has_crlf(headers[i].name, strlen(headers[i].name)) ||
            kl_http_client_has_crlf(headers[i].value, strlen(headers[i].value)))
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

    /* Send header block, then the body */
    if (send_all_sync(sockets, fd, tls, buf, (size_t)off, deadline) != 0)
        return -1;
    if (body && body_len > 0 &&
        send_all_sync(sockets, fd, tls, body, body_len, deadline) != 0)
        return -1;

    return 0;
}

/* ── Send headers-only (for chunked body streaming) ──────────────── */

static int send_headers_sync(const KlSocketProvider *sockets, KlSocketHandle fd, KlTls *tls,
                               const char *method, const KlUrl *url,
                               const KlHttpClientHeader *headers, int num_headers,
                               uint64_t deadline, int keep_alive,
                               const char *absolute_url)
{
    if (kl_http_client_has_crlf(method, strlen(method)))
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

    char authority[KL_HTTP_CLIENT_HOSTNAME_MAX + 16];
    int alen = kl_http_client_authority(url, authority, sizeof authority);
    if (alen < 0) return -1;
    char buf[KL_HTTP_CLIENT_REQ_BUF_SIZE];
    int off = snprintf(buf, sizeof(buf), "%s %.*s HTTP/1.1\r\nHost: %.*s\r\n",
                       method,
                       target_len, target,
                       alen, authority);

    if (off < 0 || (size_t)off >= sizeof(buf))
        return -1;

    for (int i = 0; i < num_headers; i++) {
        if (kl_http_client_has_crlf(headers[i].name, strlen(headers[i].name)) ||
            kl_http_client_has_crlf(headers[i].value, strlen(headers[i].value)))
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

    return send_all_sync(sockets, fd, tls, buf, (size_t)off, deadline);
}

/* ── Send chunked body from body_read callback (sync) ────────────── */

static int send_body_chunked_sync(const KlSocketProvider *sockets, KlSocketHandle fd, KlTls *tls,
                                    KlHttpClientReadFn body_read, void *user_data,
                                    uint64_t deadline)
{
    char data_buf[KL_HTTP_CLIENT_CHUNK_BUF_SIZE];
    char hdr_buf[KL_HTTP_CLIENT_CHUNK_HDR_SIZE];

    for (;;) {
        kl_ssize_t nread = body_read(data_buf, sizeof(data_buf), user_data);
        if (nread > (kl_ssize_t)sizeof(data_buf))
            return -1;   /* more than the buffer holds: never send past it */
        if (nread < 0)
            return -1;

        if (nread == 0) {
            /* Final chunk: 0\r\n\r\n */
            if (send_all_sync(sockets, fd, tls, "0\r\n\r\n",
                               KL_HTTP_CLIENT_FINAL_CHUNK_LEN, deadline) != 0)
                return -1;
            return 0;
        }

        /* Chunk header: <hex-len>\r\n */
        int hdr_len = snprintf(hdr_buf, sizeof(hdr_buf), "%zx\r\n", (size_t)nread);
        if (hdr_len < 0)
            return -1;

        if (send_all_sync(sockets, fd, tls, hdr_buf, (size_t)hdr_len, deadline) != 0)
            return -1;
        if (send_all_sync(sockets, fd, tls, data_buf, (size_t)nread, deadline) != 0)
            return -1;
        if (send_all_sync(sockets, fd, tls, "\r\n", sizeof("\r\n") - 1, deadline) != 0)
            return -1;
    }
}

/* ── Receive + parse response (sync, with optional streaming) ────── */

/* End of stream before the parser reported a complete message: only a close-delimited body
 * legitimately ends here. A parser with finish decides; without one, keep the older rule (a status
 * line arrived). A truncated response is an error, never a success missing headers or body. */
static int response_complete_at_eof(KlHttp1ResponseParser *parser, KlHttpClientResponse *resp,
                                    int status_only)
{
    if (parser->finish && !status_only)
        return parser->finish(parser, resp) == KL_HTTP1_PARSE_OK;
    return resp->status > 0;
}

/* *reusable (if non-NULL) is set to 1 only when the response ended exactly at the end of a read,
 * with nothing after it on the socket or in TLS, and not at end of stream: the only case in which a
 * pool may keep the connection. */
static int recv_response_sync(const KlSocketProvider *sockets, KlSocketHandle fd, KlTls *tls, KlHttpClientResponse *resp,
                               size_t max_response_size, uint64_t deadline,
                               KlAllocator *alloc,
                               const KlHttpClientStreamCfg *stream, int is_head, int *reusable)
{
    if (reusable) *reusable = 0;
    KlHttp1ResponseParser *parser;
    if (stream && stream->on_body) {
        parser = kl_http1_response_parser_llhttp_s(max_response_size, alloc,
                                               stream->on_body,
                                               stream->on_headers,
                                               stream->on_complete,
                                               stream->user_data);
    } else {
        parser = kl_http1_response_parser_llhttp(max_response_size, alloc);
    }
    if (!parser)
        return -1;

    /* A HEAD response has no body whatever its framing headers say. Tell the parser; one that
     * cannot be told would take the missing body for a truncation at end of stream, so fall back to
     * the status rule there instead. */
    int status_only = 0;
    if (is_head) {
        if (parser->expect_no_body)
            parser->expect_no_body(parser);
        else
            status_only = 1;
    }

    char buf[KL_HTTP_CLIENT_RECV_BUF_SIZE];
    int ret = -1;

    for (;;) {
        /* Plaintext the TLS engine already holds will not make the socket readable: read it first. */
        if (!(tls && tls->pending && tls->pending(tls) > 0)) {
            int pr = wait_ready(fd, KL_POLL_IN, deadline);
            if (pr <= 0)
                break;
        }

        kl_ssize_t nread = kl_http_client_io_read(sockets, fd, tls, buf, sizeof(buf));
        if (nread < 0 && !tls) {
            KlIoStatus st = kl_sock_io_status(sockets);
            if (st == KL_IO_INTERRUPTED || st == KL_IO_WOULD_BLOCK)
                continue;   /* readable reported with nothing to read yet: wait again */
        }
        if (nread < 0) {
            /* A clean TLS shutdown surfaces as read()==-1 (no distinct EOF code);
             * finalize a close-delimited response rather than failing it. */
            if (tls && tls->at_eof && tls->at_eof(tls) && response_complete_at_eof(parser, resp, status_only))
                ret = 0;
            break;
        }
        if (nread == 0 && tls)
            continue;   /* TLS WANT_READ: part of a record arrived; poll for the rest */
        if (nread == 0) {
            if (response_complete_at_eof(parser, resp, status_only))
                ret = 0;
            break;
        }

        size_t consumed;
        KlHttp1ParseResult pr2 = parser->parse(parser, resp,
                                            buf, (size_t)nread, &consumed);
        if (pr2 == KL_HTTP1_PARSE_OK) {
            ret = 0;
            if (reusable)
                *reusable = resp->status != 101 &&   /* the connection now speaks another protocol */
                            consumed == (size_t)nread &&
                            !(tls && tls->pending && tls->pending(tls) > 0);
            break;
        }
        if (pr2 == KL_HTTP1_PARSE_ERROR)
            break;
    }

    parser->destroy(parser);
    return ret;
}

/* ── Sync public API ─────────────────────────────────────────────── */

int kl_http_client_request_s(KlAllocator *alloc, const KlHttpClientConfig *cfg,
                         const char *method, const char *url_str,
                         const KlHttpClientHeader *headers, int num_headers,
                         const char *body, size_t body_len,
                         const KlHttpClientStreamCfg *stream,
                         KlHttpClientResponse *resp)
{
    if (!method || !url_str || !resp)
        return -1;   /* resp NULL: no error surface; the allocator is validated after memset below */
    if (num_headers < 0 || num_headers > KL_HTTP_CLIENT_MAX_REQ_HEADERS)
        return -1;
    if (num_headers > 0 && !headers)
        return -1;

    memset(resp, 0, sizeof(*resp));

    /* A NULL/malformed allocator is invalid on the sync path (no documented default):
     * reject after zeroing resp so its error surface is set deterministically. */
    if (!kl_allocator_ops_valid(alloc)) {
        resp->error = KL_ERR_INVALID_ARG;
        return -1;
    }

    /* Selected socket provider (NULL = built-in default), threaded through the
     * whole sync path (connect + I/O), never hardcoded. */
    const KlSocketProvider *sockets = cfg ? cfg->sockets : NULL;
    /* Reject a malformed provider (non-NULL, NULL ops) before connecting: the
     * kl_sock_* dispatchers would fault on the first I/O. */
    if (!kl_socket_provider_ops_valid(sockets)) {
        resp->error = KL_ERR_INVALID_ARG;
        return -1;
    }

    int timeout_ms = (cfg && cfg->timeout_ms > 0) ? cfg->timeout_ms
                                                    : KL_HTTP_CLIENT_DEFAULT_TIMEOUT_MS;
    uint64_t deadline = deadline_after(timeout_ms);   /* bounds the whole request */
    size_t max_resp = (cfg && cfg->max_response_size > 0) ? cfg->max_response_size
                                                            : (size_t)KL_HTTP_CLIENT_DEFAULT_MAX_RESP;

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
    const KlHttpProxyConfig *proxy = cfg ? cfg->proxy : NULL;
    int is_proxied = (proxy && proxy->host);
    if (is_proxied && proxy->auth && kl_http_client_has_crlf(proxy->auth, strlen(proxy->auth))) {
        resp->error = KL_ERR_INVALID_ARG;   /* a line break would inject header lines */
        return -1;
    }

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
        fd = unix_connect_with_timeout(parsed.unix_path, deadline, sockets, &conn_err);
    } else if (is_proxied) {
        fd = connect_with_timeout(proxy->host, strlen(proxy->host),
                                   proxy->port, deadline, sockets, &conn_err);
    } else {
        fd = connect_with_timeout(parsed.host, parsed.host_len,
                                   parsed.port, deadline, sockets, &conn_err);
    }
    if (!kl_handle_valid(fd)) {
        resp->error = conn_err;
        return -1;
    }

    KlTls *tls = NULL;
    int ret = -1;
    int decomp_installed = 0;   /* streaming decompressor wrapper active */
    KlHttpClientHeader *hdrs_owned = NULL;   /* headers + Proxy-Authorization, when built */
    int hdrs_owned_n = 0;

    if (is_proxied && parsed.is_https) {
        /* CONNECT tunnel through proxy, then TLS handshake */
        char target_host[KL_HTTP_CLIENT_HOSTNAME_MAX];
        if (parsed.host_len >= sizeof(target_host)) {
            resp->error = KL_ERR_INVALID_ARG;
            goto cleanup;
        }
        memcpy(target_host, parsed.host, parsed.host_len);
        target_host[parsed.host_len] = '\0';

        if (proxy_connect_sync(sockets, fd, target_host, (uint16_t)parsed.port,
                                 proxy->auth, deadline) != 0) {
            resp->error = step_error(deadline, KL_ERR_PROXY);
            goto cleanup;
        }
        tls = do_tls_handshake(fd, tls_cfg, parsed.host, parsed.host_len,
                                deadline, alloc, sockets);
        if (!tls) {
            resp->error = step_error(deadline, KL_ERR_TLS_HANDSHAKE);
            goto cleanup;
        }
    } else if (parsed.is_https) {
        tls = do_tls_handshake(fd, tls_cfg, parsed.host, parsed.host_len,
                                deadline, alloc, sockets);
        if (!tls) {
            resp->error = step_error(deadline, KL_ERR_TLS_HANDSHAKE);
            goto cleanup;
        }
    }

    /* Build absolute-form URL for HTTP forwarding through proxy */
    char abs_url_buf[KL_HTTP_CLIENT_REQ_BUF_SIZE];
    const char *absolute_url = NULL;
    if (is_proxied && !parsed.is_https) {
        if (parsed.host_len >= KL_HTTP_CLIENT_HOSTNAME_MAX) {
            resp->error = KL_ERR_INVALID_ARG;
            goto cleanup;
        }
        /* The authority brackets an IPv6 literal and carries a non-default port. */
        char authority[KL_HTTP_CLIENT_HOSTNAME_MAX + 16];
        int alen = kl_http_client_authority(&parsed, authority, sizeof authority);
        if (alen < 0) {
            resp->error = KL_ERR_OVERFLOW;
            goto cleanup;
        }

        const char *path = (parsed.path_len > 0) ? parsed.path : "/";
        int path_len = (parsed.path_len > 0) ? (int)parsed.path_len : 1;

        /* HTTP proxy absolute-form must preserve the caller's cleartext scheme. */
        // lgtm[cpp/non-https-url]
        int n = snprintf(abs_url_buf, sizeof(abs_url_buf),
                         "http://%.*s%.*s", alen, authority, path_len, path);
        if (n < 0 || (size_t)n >= sizeof(abs_url_buf)) {
            resp->error = KL_ERR_OVERFLOW;
            goto cleanup;
        }
        absolute_url = abs_url_buf;
    }

    /* Set up streaming decompression wrapper if needed */
    KlDecompressConfig *dcfg = cfg ? cfg->decompress : NULL;
    DecompStreamWrap decomp_wrap;
    const KlHttpClientStreamCfg *actual_stream = stream;
    KlHttpClientStreamCfg wrapped_stream;

    if (dcfg && dcfg->factory && stream && stream->on_body) {
        memset(&decomp_wrap, 0, sizeof(decomp_wrap));
        decomp_wrap.user_on_body = stream->on_body;
        decomp_wrap.user_on_headers = stream->on_headers;
        decomp_wrap.user_on_complete = stream->on_complete;
        decomp_wrap.user_data = stream->user_data;
        decomp_wrap.dcfg = dcfg;
        decomp_wrap.max = max_resp;
        decomp_wrap.ds.alloc = alloc;

        wrapped_stream.on_body = kl_http_client_decomp_on_body;
        wrapped_stream.on_headers = kl_http_client_decomp_on_headers;
        wrapped_stream.on_complete = kl_http_client_decomp_on_complete;
        wrapped_stream.body_read = stream->body_read;
        wrapped_stream.user_data = &decomp_wrap;
        actual_stream = &wrapped_stream;
        decomp_installed = 1;
    }

    /* A plain-HTTP request through a proxy carries the proxy credentials itself (a CONNECT tunnel
     * carries them on the CONNECT instead). */
    const KlHttpClientHeader *hdrs = headers;
    int nh = num_headers;
    if (absolute_url && proxy->auth) {
        hdrs = kl_http_client_with_proxy_auth(alloc, headers, num_headers, proxy->auth,
                                              &hdrs_owned, &nh);
        if (!hdrs) {
            resp->error = KL_ERR_ALLOC;
            goto cleanup;
        }
        hdrs_owned_n = nh;
    }

    /* Request streaming: send headers + chunked body */
    if (stream && stream->body_read) {
        if (send_headers_sync(sockets, fd, tls, method, &parsed,
                                hdrs, nh, deadline, 0,
                                absolute_url) != 0) {
            if (!resp->error) resp->error = step_error(deadline, KL_ERR_IO);
            goto cleanup;
        }
        if (send_body_chunked_sync(sockets, fd, tls, stream->body_read,
                                     stream->user_data, deadline) != 0) {
            if (!resp->error) resp->error = step_error(deadline, KL_ERR_IO);
            goto cleanup;
        }
    } else {
        if (send_request_sync(sockets, fd, tls, method, &parsed,
                               hdrs, nh, body, body_len,
                               deadline, 0, absolute_url) != 0) {
            if (!resp->error) resp->error = step_error(deadline, KL_ERR_IO);
            goto cleanup;
        }
    }

    if (recv_response_sync(sockets, fd, tls, resp, max_resp, deadline, alloc,
                            actual_stream, strcmp(method, "HEAD") == 0, NULL) != 0) {
        if (!resp->error) resp->error = step_error(deadline, KL_ERR_PARSE);
        goto cleanup;
    }

    /* Decompress buffered response body if applicable */
    if (!stream || !stream->on_body) {
        int drc = kl_http_client_decompress_response_body(resp, dcfg, max_resp);
        if (drc < 0) {
            if (!resp->error) resp->error = drc == -2 ? KL_ERR_TOO_LARGE : KL_ERR_COMPRESS;
            goto cleanup;
        }
    } else if (decomp_installed && decomp_wrap.failed) {
        /* Streaming decompression failed, at the final flush included (a truncated stream). */
        if (!resp->error) resp->error = KL_ERR_COMPRESS;
        goto cleanup;
    }

    ret = 0;

cleanup:
    if (hdrs_owned)
        kl_free(alloc, hdrs_owned, (size_t)hdrs_owned_n * sizeof(KlHttpClientHeader));
    /* Free the streaming decompressor session if it was installed.  It is
     * otherwise freed only by kl_http_client_decomp_on_complete (fired on parser
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
        kl_http_client_response_free(resp);

    return ret;
}

int kl_http_client_request(KlAllocator *alloc, const KlHttpClientConfig *cfg,
                      const char *method, const char *url_str,
                      const KlHttpClientHeader *headers, int num_headers,
                      const char *body, size_t body_len,
                      KlHttpClientResponse *resp)
{
    return kl_http_client_request_s(alloc, cfg, method, url_str,
                                headers, num_headers, body, body_len,
                                NULL, resp);
}

/* ══════════════════════════════════════════════════════════════════════
 * Pooled blocking request: connection pool integration (sync path)
 * ══════════════════════════════════════════════════════════════════════ */

int kl_http_client_request_pooled(KlHttpClientPool *pool,
                              KlAllocator *alloc, const KlHttpClientConfig *cfg,
                              const char *method, const char *url_str,
                              const KlHttpClientHeader *headers, int num_headers,
                              const char *body, size_t body_len,
                              KlHttpClientResponse *resp)
{
    if (!pool || !method || !url_str || !resp)
        return -1;   /* the allocator is validated after memset below (sets resp->error) */
    if (num_headers < 0 || num_headers > KL_HTTP_CLIENT_MAX_REQ_HEADERS)
        return -1;
    if (num_headers > 0 && !headers)
        return -1;

    memset(resp, 0, sizeof(*resp));

    /* A NULL/malformed allocator is invalid on the sync path (no documented default):
     * reject after zeroing resp so its error surface is set deterministically. */
    if (!kl_allocator_ops_valid(alloc)) {
        resp->error = KL_ERR_INVALID_ARG;
        return -1;
    }

    /* Selected socket provider (NULL = built-in default), threaded through the
     * whole sync path (connect + I/O), never hardcoded. */
    const KlSocketProvider *sockets = cfg ? cfg->sockets : NULL;
    /* Reject a malformed provider (non-NULL, NULL ops) before connecting: the
     * kl_sock_* dispatchers would fault on the first I/O. */
    if (!kl_socket_provider_ops_valid(sockets)) {
        resp->error = KL_ERR_INVALID_ARG;
        return -1;
    }

    int timeout_ms = (cfg && cfg->timeout_ms > 0) ? cfg->timeout_ms
                                                    : KL_HTTP_CLIENT_DEFAULT_TIMEOUT_MS;
    uint64_t deadline = deadline_after(timeout_ms);   /* bounds the whole request */
    size_t max_resp = (cfg && cfg->max_response_size > 0) ? cfg->max_response_size
                                                            : (size_t)KL_HTTP_CLIENT_DEFAULT_MAX_RESP;

    KlUrl parsed;
    if (kl_url_parse(url_str, &parsed) != 0) {
        resp->error = KL_ERR_URL;
        return -1;
    }
    /* UNIX sockets have no host:port to key the pool on, so bypass the pool
     * and connect directly (local-socket connect is cheap). */
    /* A proxied request is not pooled either: the pool is keyed by the target and connects to it
     * directly, which would silently bypass cfg->proxy. The non-pooled path honours the proxy. */
    if (parsed.is_unix || (cfg && cfg->proxy)) {
        return kl_http_client_request_s(alloc, cfg, method, url_str,
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
    char host_buf[KL_HTTP_CLIENT_HOSTNAME_MAX];
    if (parsed.host_len >= sizeof(host_buf)) {
        resp->error = KL_ERR_INVALID_ARG;
        return -1;
    }
    memcpy(host_buf, parsed.host, parsed.host_len);
    host_buf[parsed.host_len] = '\0';

    /* Try to acquire from pool */
    KlHttpClientPoolConn pconn;
    memset(&pconn, 0, sizeof(pconn));
    pconn.fd = -1;
    int acq = kl_http_client_pool_acquire_tls(pool, host_buf, parsed.port, tls_cfg,
                                              NULL, 0, &pconn);

    KlSocketHandle fd;
    KlTls *tls = NULL;
    int ret = -1;
    int reusable = 0;

    if (acq == 0) {
        /* Pool hit: reuse connection. It is non-blocking, as every connection of this client is. */
        fd = pconn.fd;
        tls = pconn.tls;
    } else {
        /* Pool miss: connect fresh */
        KlError conn_err = KL_ERR_NONE;
        fd = connect_with_timeout(parsed.host, parsed.host_len,
                                   parsed.port, deadline, sockets, &conn_err);
        if (!kl_handle_valid(fd)) {
            resp->error = conn_err;
            return -1;
        }

        if (is_tls) {
            tls = do_tls_handshake(fd, tls_cfg, parsed.host, parsed.host_len,
                                    deadline, alloc, sockets);
            if (!tls) {
                resp->error = step_error(deadline, KL_ERR_TLS_HANDSHAKE);
                kl_sock_close(sockets, fd);
                return -1;
            }
        }

        pconn.fd = fd;
        pconn.tls = tls;
        pconn.reused = 0;
    }

    /* Send with keep-alive (direct only: a proxied request took the non-pooled path above) */
    if (send_request_sync(sockets, fd, tls, method, &parsed,
                           headers, num_headers, body, body_len,
                           deadline, 1, NULL) != 0) {
        if (!resp->error) resp->error = step_error(deadline, KL_ERR_IO);
        goto cleanup;
    }

    if (recv_response_sync(sockets, fd, tls, resp, max_resp, deadline, alloc,
                            NULL, strcmp(method, "HEAD") == 0, &reusable) != 0) {
        if (!resp->error) resp->error = step_error(deadline, KL_ERR_PARSE);
        goto cleanup;
    }

    /* Decompress buffered response body if applicable */
    {
        KlDecompressConfig *dcfg = cfg ? cfg->decompress : NULL;
        int drc = kl_http_client_decompress_response_body(resp, dcfg, max_resp);
        if (drc < 0) {
            if (!resp->error) resp->error = drc == -2 ? KL_ERR_TOO_LARGE : KL_ERR_COMPRESS;
            goto cleanup;
        }
    }

    ret = 0;

cleanup:
    if (ret != 0) {
        kl_http_client_pool_discard(pool, &pconn);
        kl_http_client_response_free(resp);
    } else if (!reusable || kl_http_client_server_wants_close(resp)) {
        /* Not reusable: bytes followed the response, or it ended at end of stream. */
        kl_http_client_pool_discard(pool, &pconn);
    } else {
        kl_http_client_pool_release_tls(pool, &pconn, host_buf, parsed.port, tls_cfg,
                                        NULL, 0);
    }

    return ret;
}
