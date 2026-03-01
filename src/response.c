#include <keel/response.h>
#include <keel/tls.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/uio.h>
#include <stdint.h>

#if defined(__linux__)
  #include <sys/sendfile.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
#elif defined(__APPLE__)
  #include <sys/socket.h>
#endif

#define KL_HDR_INIT_CAP 512
#define KL_FILE_BUF_SIZE 8192   /* stack buffer for pread+write fallback */
#define KL_WRITE_SPIN_MAX 256  /* max retries on EAGAIN/WANT_WRITE before giving up */

/* ── Pre-built status lines — no snprintf in the hot path ─────────── */

typedef struct { const char *str; size_t len; } KlStatusLine;

#define SL(s) { (s), sizeof(s) - 1 }

static KlStatusLine status_line_for(int code) {
    switch (code) {
        case 200: return (KlStatusLine)SL("HTTP/1.1 200 OK\r\n");
        case 201: return (KlStatusLine)SL("HTTP/1.1 201 Created\r\n");
        case 202: return (KlStatusLine)SL("HTTP/1.1 202 Accepted\r\n");
        case 204: return (KlStatusLine)SL("HTTP/1.1 204 No Content\r\n");
        case 206: return (KlStatusLine)SL("HTTP/1.1 206 Partial Content\r\n");
        case 301: return (KlStatusLine)SL("HTTP/1.1 301 Moved Permanently\r\n");
        case 302: return (KlStatusLine)SL("HTTP/1.1 302 Found\r\n");
        case 304: return (KlStatusLine)SL("HTTP/1.1 304 Not Modified\r\n");
        case 307: return (KlStatusLine)SL("HTTP/1.1 307 Temporary Redirect\r\n");
        case 308: return (KlStatusLine)SL("HTTP/1.1 308 Permanent Redirect\r\n");
        case 400: return (KlStatusLine)SL("HTTP/1.1 400 Bad Request\r\n");
        case 401: return (KlStatusLine)SL("HTTP/1.1 401 Unauthorized\r\n");
        case 403: return (KlStatusLine)SL("HTTP/1.1 403 Forbidden\r\n");
        case 404: return (KlStatusLine)SL("HTTP/1.1 404 Not Found\r\n");
        case 405: return (KlStatusLine)SL("HTTP/1.1 405 Method Not Allowed\r\n");
        case 409: return (KlStatusLine)SL("HTTP/1.1 409 Conflict\r\n");
        case 410: return (KlStatusLine)SL("HTTP/1.1 410 Gone\r\n");
        case 413: return (KlStatusLine)SL("HTTP/1.1 413 Payload Too Large\r\n");
        case 415: return (KlStatusLine)SL("HTTP/1.1 415 Unsupported Media Type\r\n");
        case 422: return (KlStatusLine)SL("HTTP/1.1 422 Unprocessable Entity\r\n");
        case 429: return (KlStatusLine)SL("HTTP/1.1 429 Too Many Requests\r\n");
        case 500: return (KlStatusLine)SL("HTTP/1.1 500 Internal Server Error\r\n");
        case 502: return (KlStatusLine)SL("HTTP/1.1 502 Bad Gateway\r\n");
        case 503: return (KlStatusLine)SL("HTTP/1.1 503 Service Unavailable\r\n");
        case 504: return (KlStatusLine)SL("HTTP/1.1 504 Gateway Timeout\r\n");
        default:  return (KlStatusLine)SL("HTTP/1.1 500 Internal Server Error\r\n");
    }
}

/* ── Fast integer formatting — replaces snprintf for Content-Length ── */

static int format_uint(char *p, size_t n) {
    char tmp[21];  /* 20 digits max for 64-bit + safety margin */
    int ndigits = 0;
    if (n == 0) {
        *p = '0';
        return 1;
    }
    while (n > 0) {
        tmp[ndigits++] = '0' + (char)(n % 10);
        n /= 10;
    }
    for (int i = ndigits - 1; i >= 0; i--)
        *p++ = tmp[i];
    return ndigits;
}

/* "Content-Length: <n>\r\n" into buf, returns total length */
static int format_content_length(char *buf, size_t n) {
    memcpy(buf, "Content-Length: ", 16);
    int dlen = format_uint(buf + 16, n);
    buf[16 + dlen] = '\r';
    buf[17 + dlen] = '\n';
    return 18 + dlen;
}

/* "<hex>\r\n" into buf, returns total length */
static int format_chunk_hdr(char *buf, size_t n) {
    static const char hex[] = "0123456789abcdef";
    char tmp[16];
    int ndigits = 0;
    size_t v = n;
    do {
        tmp[ndigits++] = hex[v & 0xf];
        v >>= 4;
    } while (v > 0);
    char *p = buf;
    for (int i = ndigits - 1; i >= 0; i--)
        *p++ = tmp[i];
    *p++ = '\r';
    *p++ = '\n';
    return ndigits + 2;
}

/* ── Header buffer management ────────────────────────────────────── */

static int hdr_append(KlResponse *res, const char *data, size_t len) {
    if (len > SIZE_MAX - res->hdr_len) return -1;
    while (res->hdr_len + len > res->hdr_cap) {
        size_t new_cap;
        if (res->hdr_cap > SIZE_MAX / 2)
            return -1;
        new_cap = res->hdr_cap * 2;
        char *new_buf = kl_realloc(res->alloc, res->hdr_buf,
                                   res->hdr_cap, new_cap);
        if (!new_buf) return -1;
        res->hdr_buf = new_buf;
        res->hdr_cap = new_cap;
    }
    memcpy(res->hdr_buf + res->hdr_len, data, len);
    res->hdr_len += len;
    return 0;
}

/* ── Init / Reset / Free ─────────────────────────────────────────── */

int kl_response_init(KlResponse *res, KlAllocator *alloc) {
    memset(res, 0, sizeof(*res));
    res->alloc = alloc;
    res->status = 200;
    res->file_fd = -1;
    res->hdr_buf = kl_malloc(alloc, KL_HDR_INIT_CAP);
    if (!res->hdr_buf) return -1;
    res->hdr_cap = KL_HDR_INIT_CAP;
    return 0;
}

void kl_response_reset(KlResponse *res) {
    /* Close file descriptor if one was set for sendfile */
    if (res->file_fd >= 0) {
        close(res->file_fd);
    }

    /* Fast reinit for keep-alive — reuses header buffer, no alloc */
    char *buf = res->hdr_buf;
    size_t cap = res->hdr_cap;
    KlAllocator *alloc = res->alloc;
    int conn_fd = res->conn_fd;
    KlTls *tls = res->tls;

    memset(res, 0, sizeof(*res));
    res->alloc = alloc;
    res->conn_fd = conn_fd;
    res->tls = tls;
    res->status = 200;
    res->file_fd = -1;
    res->hdr_buf = buf;
    res->hdr_cap = cap;
}

void kl_response_free(KlResponse *res) {
    if (res->file_fd >= 0) {
        close(res->file_fd);
        res->file_fd = -1;
    }
    if (res->hdr_buf) {
        kl_free(res->alloc, res->hdr_buf, res->hdr_cap);
        res->hdr_buf = NULL;
    }
}

/* ── Public API ──────────────────────────────────────────────────── */

void kl_response_status(KlResponse *res, int code) {
    res->status = code;
}

/* Reject strings containing \r or \n to prevent header injection */
static int contains_crlf(const char *s, size_t len) {
    for (size_t i = 0; i < len; i++)
        if (s[i] == '\r' || s[i] == '\n') return 1;
    return 0;
}

void kl_response_header(KlResponse *res, const char *name, const char *value) {
    if (!name || !value) return;
    size_t name_len = strlen(name);
    size_t value_len = strlen(value);
    if (contains_crlf(name, name_len) || contains_crlf(value, value_len))
        return;
    hdr_append(res, name, name_len);
    hdr_append(res, ": ", 2);
    hdr_append(res, value, value_len);
    hdr_append(res, "\r\n", 2);
}

void kl_response_body(KlResponse *res, const char *data, size_t len) {
    if (len > 0 && !data) return;
    res->body_mode = KL_BODY_BUFFER;
    res->body = data;
    res->body_len = len;
}

void kl_response_file(KlResponse *res, int fd, off_t size) {
    res->body_mode = KL_BODY_FILE;
    res->file_fd = fd;
    res->file_size = size;
    res->file_offset = 0;
}

void kl_response_json(KlResponse *res, int code, const char *json, size_t len) {
    kl_response_status(res, code);
    kl_response_header(res, "Content-Type", "application/json");
    kl_response_body(res, json, len);
}

void kl_response_error(KlResponse *res, int code, const char *message) {
    kl_response_status(res, code);
    kl_response_header(res, "Content-Type", "text/plain");
    if (!message) message = "";
    kl_response_body(res, message, strlen(message));
}

/* ── sendfile wrapper ────────────────────────────────────────────── */

static ssize_t kl_sendfile_impl(int out_fd, int in_fd, off_t *offset, size_t count) {
#if defined(__linux__)
    return sendfile(out_fd, in_fd, offset, count);
#elif defined(__APPLE__)
    off_t len = (off_t)count;
    int r = sendfile(in_fd, out_fd, *offset, &len, NULL, 0);
    if (r < 0 && errno != EAGAIN) return -1;
    *offset += len;
    return (ssize_t)len;
#else
    char buf[KL_FILE_BUF_SIZE];
    size_t to_read = count < sizeof(buf) ? count : sizeof(buf);
    ssize_t nr = pread(in_fd, buf, to_read, *offset);
    if (nr <= 0) return nr;
    /* Retry partial writes on the fallback path */
    const char *p = buf;
    size_t remaining = (size_t)nr;
    while (remaining > 0) {
        ssize_t nw = write(out_fd, p, remaining);
        if (nw < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* Wrote some but not all — update offset for progress */
                ssize_t wrote = (ssize_t)((size_t)nr - remaining);
                *offset += wrote;
                return wrote > 0 ? wrote : -1;
            }
            return -1;
        }
        p += nw;
        remaining -= (size_t)nw;
    }
    *offset += nr;
    return nr;
#endif
}

/* ── writev helper — handles short writes and EAGAIN ─────────────── */

static int writev_all(int fd, KlTls *tls, struct iovec *iov, int iovcnt) {
    int spins = 0;

    if (!tls) {
        /* Plaintext — use writev for scatter-gather I/O */
        while (iovcnt > 0) {
            ssize_t nw = writev(fd, iov, iovcnt);
            if (nw < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    if (++spins > KL_WRITE_SPIN_MAX) return -1;
                    continue;
                }
                return -1;
            }
            spins = 0;
            size_t written = (size_t)nw;
            while (iovcnt > 0 && written >= iov[0].iov_len) {
                written -= iov[0].iov_len;
                iov++;
                iovcnt--;
            }
            if (iovcnt > 0 && written > 0) {
                iov[0].iov_base = (char *)iov[0].iov_base + written;
                iov[0].iov_len -= written;
            }
        }
        return 0;
    }

    /* TLS — linearize segments through tls->write */
    for (int i = 0; i < iovcnt; i++) {
        const char *p = iov[i].iov_base;
        size_t remaining = iov[i].iov_len;
        while (remaining > 0) {
            ssize_t nw = tls->write(tls, fd, p, remaining);
            if (nw < 0) return -1;
            if (nw == 0) {
                if (++spins > KL_WRITE_SPIN_MAX) return -1;
                continue;
            }
            spins = 0;
            p += nw;
            remaining -= (size_t)nw;
        }
    }
    return 0;
}

/* ── Send: single writev for headers + body ──────────────────────── */

static const char kl_keepalive_hdr[] = "Connection: keep-alive\r\n";

int kl_response_send(KlResponse *res) {
    if (!res->headers_sent) {
        KlStatusLine sl = status_line_for(res->status);

        char cl_buf[48];
        int cl_len = 0;
        if (res->body_mode == KL_BODY_BUFFER) {
            cl_len = format_content_length(cl_buf, res->body_len);
        } else if (res->body_mode == KL_BODY_FILE) {
            cl_len = format_content_length(cl_buf, (size_t)res->file_size);
        }

        struct iovec iov[7];
        int iovcnt = 0;

        /* Status line (constant string, no alloc) */
        iov[iovcnt].iov_base = (void *)sl.str;
        iov[iovcnt].iov_len = sl.len;
        iovcnt++;

        /* User headers */
        if (res->hdr_len > 0) {
            iov[iovcnt].iov_base = res->hdr_buf;
            iov[iovcnt].iov_len = res->hdr_len;
            iovcnt++;
        }

        /* Content-Length */
        if (cl_len > 0) {
            iov[iovcnt].iov_base = cl_buf;
            iov[iovcnt].iov_len = (size_t)cl_len;
            iovcnt++;
        }

        /* Connection: keep-alive (only when flagged) */
        if (res->keep_alive) {
            iov[iovcnt].iov_base = (void *)kl_keepalive_hdr;
            iov[iovcnt].iov_len = sizeof(kl_keepalive_hdr) - 1;
            iovcnt++;
        }

        /* Header terminator */
        iov[iovcnt].iov_base = (void *)"\r\n";
        iov[iovcnt].iov_len = 2;
        iovcnt++;

        /* Inline body for buffered mode (skip for HEAD) */
        if (res->body_mode == KL_BODY_BUFFER && res->body_len > 0 &&
            !res->head_request) {
            iov[iovcnt].iov_base = (void *)res->body;
            iov[iovcnt].iov_len = res->body_len;
            iovcnt++;
        }

        if (writev_all(res->conn_fd, res->tls, iov, iovcnt) < 0) return -1;
        res->headers_sent = 1;

        if (res->body_mode == KL_BODY_BUFFER || res->head_request)
            return 0;
    }

    /* Send file body (already skipped above for HEAD) */
    if (res->body_mode == KL_BODY_FILE) {
        if (res->tls) {
            /* TLS: sendfile bypasses userspace — incompatible with encryption.
             * Fall back to pread + tls->write, one chunk per event tick
             * to avoid head-of-line blocking on slow connections. */
            if (res->file_offset >= res->file_size) return 0;
            size_t remaining = (size_t)(res->file_size - res->file_offset);
            char buf[KL_FILE_BUF_SIZE];
            size_t to_read = remaining < sizeof(buf) ? remaining : sizeof(buf);
            ssize_t nr = pread(res->file_fd, buf, to_read, res->file_offset);
            if (nr <= 0) return -1;
            const char *p = buf;
            size_t left = (size_t)nr;
            while (left > 0) {
                ssize_t nw = res->tls->write(res->tls, res->conn_fd, p, left);
                if (nw < 0) return -1;
                if (nw == 0) break;  /* WANT_WRITE — yield to event loop */
                p += nw;
                left -= (size_t)nw;
            }
            res->file_offset += (off_t)((size_t)nr - left);
            return (res->file_offset < res->file_size) ? 1 : 0;
        }

#if defined(__linux__)
        /* TCP_CORK: coalesce headers + file data into fewer TCP segments */
        int cork = 1;
        (void)setsockopt(res->conn_fd, IPPROTO_TCP, TCP_CORK, &cork, sizeof(cork));
#endif
        size_t remaining = (size_t)(res->file_size - res->file_offset);
        while (remaining > 0) {
            ssize_t sent = kl_sendfile_impl(res->conn_fd, res->file_fd,
                                            &res->file_offset, remaining);
            if (sent < 0) {
                if (errno == EAGAIN) return 1;
                return -1;
            }
            if (sent == 0) break;
            remaining = (size_t)(res->file_size - res->file_offset);
        }
#if defined(__linux__)
        cork = 0;
        (void)setsockopt(res->conn_fd, IPPROTO_TCP, TCP_CORK, &cork, sizeof(cork));
#endif
        return 0;
    }

    return 0;
}

/* ── Chunked streaming ───────────────────────────────────────────── */

static int kl_stream_write(void *ctx, const char *data, size_t len) {
    KlResponse *res = ctx;
    if (res->stream_error) return -1;
    if (len == 0) return 0;
    if (res->head_request) return 0;

    char hdr[24];
    int hdr_len = format_chunk_hdr(hdr, len);

    struct iovec iov[3] = {
        { .iov_base = hdr,          .iov_len = (size_t)hdr_len },
        { .iov_base = (void *)data, .iov_len = len },
        { .iov_base = (void *)"\r\n", .iov_len = 2 },
    };

    if (writev_all(res->conn_fd, res->tls, iov, 3) < 0) {
        res->stream_error = 1;
        return -1;
    }
    return 0;
}

int kl_response_begin_stream(KlResponse *res, int status,
                             KlWriteFn *out_write, void **out_ctx) {
    res->body_mode = KL_BODY_STREAM;
    kl_response_status(res, status);
    kl_response_header(res, "Transfer-Encoding", "chunked");

    KlStatusLine sl = status_line_for(res->status);

    struct iovec iov[3];
    int iovcnt = 0;

    iov[iovcnt].iov_base = (void *)sl.str;
    iov[iovcnt].iov_len = sl.len;
    iovcnt++;

    if (res->hdr_len > 0) {
        iov[iovcnt].iov_base = res->hdr_buf;
        iov[iovcnt].iov_len = res->hdr_len;
        iovcnt++;
    }

    iov[iovcnt].iov_base = (void *)"\r\n";
    iov[iovcnt].iov_len = 2;
    iovcnt++;

    if (writev_all(res->conn_fd, res->tls, iov, iovcnt) < 0) return -1;

    res->headers_sent = 1;
    *out_write = kl_stream_write;
    *out_ctx = res;
    return 0;
}

int kl_response_end_stream(KlResponse *res) {
    if (res->stream_error) return -1;
    if (res->head_request) return 0;
    struct iovec iov = { .iov_base = (void *)"0\r\n\r\n", .iov_len = 5 };
    if (writev_all(res->conn_fd, res->tls, &iov, 1) < 0) {
        res->stream_error = 1;
        return -1;
    }
    return 0;
}
