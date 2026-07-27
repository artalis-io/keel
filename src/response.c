#include <keel/response.h>
#include <keel/tls.h>
#include <keel/event_ctx.h>
#include "socket.h"
#include "response_internal.h"
#include "platform.h"
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
/* KlIoVec + the socket seam come from socket.h; sockaddr/TCP_NODELAY via
 * socket.h -> sockcompat.h. No raw net headers, and no platform iovec here —
 * scatter-gather uses the Keel-owned KlIoVec, translated to struct iovec/WSABUF
 * only inside the provider TUs. */

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
        case 303: return (KlStatusLine)SL("HTTP/1.1 303 See Other\r\n");
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
    /* Free owned body copy if any */
    if (res->body_owned) {
        kl_free(res->alloc, res->body_owned, res->body_owned_size);
    }
    /* Free drain buffer if enabled */
    if (res->drain_enabled) {
        kl_drain_free(&res->drain);
    }

    /* Fast reinit for keep-alive — reuses header buffer, no alloc */
    char *buf = res->hdr_buf;
    size_t cap = res->hdr_cap;
    KlAllocator *alloc = res->alloc;
    int conn_fd = res->conn_fd;
    KlTls *tls = res->tls;
    struct KlEventCtx *ctx = res->ctx;

    memset(res, 0, sizeof(*res));
    res->alloc = alloc;
    res->conn_fd = conn_fd;
    res->tls = tls;
    res->ctx = ctx;
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
    if (res->body_owned) {
        kl_free(res->alloc, res->body_owned, res->body_owned_size);
        res->body_owned = NULL;
        res->body_owned_size = 0;
    }
    if (res->drain_enabled) {
        kl_drain_free(&res->drain);
        res->drain_enabled = 0;
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

int kl_response_header(KlResponse *res, const char *name, const char *value) {
    if (!name || !value) return -1;
    size_t name_len = strlen(name);
    size_t value_len = strlen(value);
    if (contains_crlf(name, name_len) || contains_crlf(value, value_len))
        return -1;
    size_t saved_len = res->hdr_len;
    if (hdr_append(res, name, name_len) < 0 ||
        hdr_append(res, ": ", 2) < 0 ||
        hdr_append(res, value, value_len) < 0 ||
        hdr_append(res, "\r\n", 2) < 0) {
        res->hdr_len = saved_len;  /* rollback partial append */
        return -1;
    }
    return 0;
}

void kl_response_body_borrow(KlResponse *res, const char *data, size_t len) {
    if (len > 0 && !data) return;
    res->body_mode = KL_BODY_BUFFER;
    res->body = data;
    res->body_len = len;
}

int kl_response_body_copy(KlResponse *res, const char *data, size_t len) {
    if (len > 0 && !data) return -1;

    /* Free previous owned body if any */
    if (res->body_owned) {
        kl_free(res->alloc, res->body_owned, res->body_owned_size);
        res->body_owned = NULL;
        res->body_owned_size = 0;
    }

    if (len == 0) {
        res->body_mode = KL_BODY_BUFFER;
        res->body = "";
        res->body_len = 0;
        return 0;
    }

    res->body_owned = kl_malloc(res->alloc, len);
    if (!res->body_owned) return -1;
    res->body_owned_size = len;
    memcpy(res->body_owned, data, len);

    res->body_mode = KL_BODY_BUFFER;
    res->body = res->body_owned;
    res->body_len = len;
    return 0;
}

void kl_response_file(KlResponse *res, KlSocketHandle fd, off_t size) {
    res->body_mode = KL_BODY_FILE;
    res->file_fd = fd;
    res->file_size = size;
    res->file_offset = 0;
}

int kl_response_json(KlResponse *res, int code, const char *json, size_t len) {
    kl_response_status(res, code);
    if (kl_response_header(res, "Content-Type", "application/json") < 0)
        return -1;
    kl_response_body_borrow(res, json, len);
    return 0;
}

int kl_response_error(KlResponse *res, int code, const char *message) {
    kl_response_status(res, code);
    if (kl_response_header(res, "Content-Type", "text/plain") < 0)
        return -1;
    if (!message) message = "";
    kl_response_body_borrow(res, message, strlen(message));
    return 0;
}

/* The socket provider for a response (ctx->sockets; NULL = POSIX). */
static inline const KlSocketProvider *res_provider(const KlResponse *res) {
    return res->ctx ? res->ctx->sockets : NULL;
}

/* Vectored write through the socket provider: the raw writev() when the provider
 * supports vectored I/O (POSIX / NULL), else a serialized kl_sock_send over the
 * iov. Same return contract as writev (bytes written, -1 on error, errno set). */
static ssize_t seam_writev(const KlSocketProvider *p, int fd,
                           const KlIoVec *iov, int iovcnt) {
    if (!p || p->ops->writev)                     /* NULL=POSIX, or a writev op */
        return kl_sock_writev(p, fd, iov, iovcnt);
    ssize_t total = 0;                            /* no writev op → serialize */
    for (int i = 0; i < iovcnt; i++) {
        size_t off = 0;
        while (off < iov[i].len) {
            ssize_t nw = kl_sock_send(p, fd, (char *)iov[i].base + off,
                                      iov[i].len - off);
            if (nw < 0) return total > 0 ? total : -1;   /* errno from send */
            if (nw == 0) return total;
            off += (size_t)nw;
            total += nw;
        }
    }
    return total;
}

/* ── try_writev — single attempt, returns bytes written ──────────── */

static ssize_t try_writev(const KlSocketProvider *p, int fd, KlTls *tls,
                          KlIoVec *iov, int iovcnt) {
    if (!tls) {
        ssize_t nw = seam_writev(p, fd, iov, iovcnt);
        if (nw < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        if (nw < 0 && errno == EINTR) return 0;
        return nw;
    }
    /* TLS: write first non-empty segment */
    for (int i = 0; i < iovcnt; i++) {
        if (iov[i].len == 0) continue;
        ssize_t nw = tls->write(tls, fd, iov[i].base, iov[i].len);
        if (nw < 0) return -1;
        return nw;  /* 0 = WANT_WRITE, >0 = bytes written */
    }
    return 0;
}

/* ── stream_writev_all — spin-write for streaming (small chunks) ── */

static int stream_writev_all(const KlSocketProvider *p, int fd, KlTls *tls,
                             KlIoVec *iov, int iovcnt) {
    int spins = 0;

    if (!tls) {
        while (iovcnt > 0) {
            ssize_t nw = seam_writev(p, fd, iov, iovcnt);
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
            while (iovcnt > 0 && written >= iov[0].len) {
                written -= iov[0].len;
                iov++;
                iovcnt--;
            }
            if (iovcnt > 0 && written > 0) {
                iov[0].base = (char *)iov[0].base + written;
                iov[0].len -= written;
            }
        }
        return 0;
    }

    /* TLS — linearize segments through tls->write */
    for (int i = 0; i < iovcnt; i++) {
        const char *seg = iov[i].base;
        size_t remaining = iov[i].len;
        while (remaining > 0) {
            ssize_t nw = tls->write(tls, fd, seg, remaining);
            if (nw < 0) return -1;
            if (nw == 0) {
                if (++spins > KL_WRITE_SPIN_MAX) return -1;
                continue;
            }
            spins = 0;
            seg += nw;
            remaining -= (size_t)nw;
        }
    }
    return 0;
}

/* ── Send: headers + body with partial-send resume ───────────────── */

static const char kl_keepalive_hdr[] = "Connection: keep-alive\r\n";

/* Assemble the buffered-response wire bytes into iov — the single source of truth
 * for the byte layout, shared by kl_response_send (synchronous seam writev) and the
 * IOCP completion driver (overlapped WSASend). See response_internal.h. */
int kl_response_build_iovec(KlResponse *res, KlIoVec *iov, int cap,
                            char *cl_buf, size_t cl_buf_cap, size_t *total_out) {
    (void)cl_buf_cap;   /* caller guarantees >= 48 (Content-Length line) */
    /* FILE mode assembles the HEAD only (Content-Length = file_size, no body iov);
     * the caller sends the file bytes separately (TransmitFile / sendfile). */
    int is_file = (res->body_mode == KL_BODY_FILE);
    if (res->body_mode != KL_BODY_BUFFER && res->body_mode != KL_BODY_NONE && !is_file)
        return -1;
    if (cap < 7)
        return -1;

    KlStatusLine sl = status_line_for(res->status);
    size_t clen = is_file ? (size_t)res->file_size : res->body_len;
    int cl_len = format_content_length(cl_buf, clen);

    int n = 0;
    size_t total = 0;

    iov[n].base = (void *)sl.str;  iov[n].len = sl.len;  total += sl.len;  n++;

    if (res->hdr_len > 0) {
        iov[n].base = res->hdr_buf; iov[n].len = res->hdr_len;
        total += res->hdr_len; n++;
    }
    if (cl_len > 0) {
        iov[n].base = cl_buf; iov[n].len = (size_t)cl_len;
        total += (size_t)cl_len; n++;
    }
    if (res->keep_alive) {
        iov[n].base = (void *)kl_keepalive_hdr;
        iov[n].len = sizeof(kl_keepalive_hdr) - 1;
        total += sizeof(kl_keepalive_hdr) - 1; n++;
    }
    iov[n].base = (void *)"\r\n"; iov[n].len = 2; total += 2; n++;

    if (!is_file && res->body_len > 0 && !res->head_request) {
        iov[n].base = (void *)res->body; iov[n].len = res->body_len;
        total += res->body_len; n++;
    }

    *total_out = total;
    return n;
}

int kl_response_send(KlResponse *res) {
    /* Buffer body path: single-attempt writev with send_offset tracking */
    if (res->body_mode == KL_BODY_BUFFER || res->body_mode == KL_BODY_NONE) {
        char cl_buf[48];
        KlIoVec iov[7];
        size_t total = 0;
        int iovcnt = kl_response_build_iovec(res, iov, 7, cl_buf,
                                             sizeof(cl_buf), &total);
        if (iovcnt < 0) return -1;

        /* Skip past already-sent bytes (send_offset) */
        size_t skip = res->send_offset;
        int first = 0;
        while (first < iovcnt && skip >= iov[first].len) {
            skip -= iov[first].len;
            first++;
        }
        if (first >= iovcnt)
            return 0;  /* already done */

        /* Adjust first iovec segment */
        iov[first].base = (char *)iov[first].base + skip;
        iov[first].len -= skip;

        ssize_t nw = try_writev(res_provider(res), res->conn_fd, res->tls,
                                &iov[first], iovcnt - first);
        if (nw < 0) return -1;
        res->send_offset += (size_t)nw;
        res->headers_sent = 1;

        if (res->send_offset >= total)
            return 0;  /* done */
        return 1;  /* partial — yield to event loop */
    }

    if (!res->headers_sent) {
        KlStatusLine sl = status_line_for(res->status);

        char cl_buf[48];
        int cl_len = 0;
        if (res->body_mode == KL_BODY_FILE) {
            cl_len = format_content_length(cl_buf, (size_t)res->file_size);
        }

        KlIoVec iov[5];
        int iovcnt = 0;

        iov[iovcnt].base = (void *)sl.str;
        iov[iovcnt].len = sl.len;
        iovcnt++;

        if (res->hdr_len > 0) {
            iov[iovcnt].base = res->hdr_buf;
            iov[iovcnt].len = res->hdr_len;
            iovcnt++;
        }

        if (cl_len > 0) {
            iov[iovcnt].base = cl_buf;
            iov[iovcnt].len = (size_t)cl_len;
            iovcnt++;
        }

        if (res->keep_alive) {
            iov[iovcnt].base = (void *)kl_keepalive_hdr;
            iov[iovcnt].len = sizeof(kl_keepalive_hdr) - 1;
            iovcnt++;
        }

        iov[iovcnt].base = (void *)"\r\n";
        iov[iovcnt].len = 2;
        iovcnt++;

        if (stream_writev_all(res_provider(res), res->conn_fd, res->tls, iov, iovcnt) < 0)
            return -1;
        res->headers_sent = 1;

        if (res->head_request)
            return 0;
    }

    /* Drain-based stream flush (backpressure path) */
    if (res->body_mode == KL_BODY_STREAM && res->drain_enabled) {
        int r = kl_drain_flush(&res->drain);
        if (r < 0) return -1;
        if (r == 1) return 1;  /* more pending */
        /* Fully drained — if stream ended, we're done */
        if (res->stream_ended) return 0;
        return 1;  /* handler may produce more data */
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
            ssize_t nr = kl_plat_file_pread(res->file_fd, buf, to_read, (long long)res->file_offset);
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

        /* Provider without sendfile support (e.g. a mock, or a stack whose fds
         * aren't sendfile-able): pread + provider send, one chunk per tick —
         * same shape as the TLS branch. */
        const KlSocketProvider *sp = res_provider(res);
        if (sp && !sp->ops->sendfile) {
            if (res->file_offset >= res->file_size) return 0;
            size_t remaining = (size_t)(res->file_size - res->file_offset);
            char buf[KL_FILE_BUF_SIZE];
            size_t to_read = remaining < sizeof(buf) ? remaining : sizeof(buf);
            ssize_t nr = kl_plat_file_pread(res->file_fd, buf, to_read, (long long)res->file_offset);
            if (nr <= 0) return -1;
            const char *p = buf;
            size_t left = (size_t)nr;
            while (left > 0) {
                ssize_t nw = kl_sock_send(sp, res->conn_fd, p, left);
                if (nw < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    return -1;
                }
                if (nw == 0) break;
                p += nw;
                left -= (size_t)nw;
            }
            res->file_offset += (off_t)((size_t)nr - left);
            return (res->file_offset < res->file_size) ? 1 : 0;
        }

        /* Cork: coalesce headers + file data into fewer segments (best-effort). */
        (void)kl_sock_set_cork(sp, res->conn_fd, 1);
        size_t remaining = (size_t)(res->file_size - res->file_offset);
        while (remaining > 0) {
            /* The seam offset is uint64_t (no off_t on the public API); convert
             * around the call and write the advanced offset back to file_offset. */
            uint64_t off = (uint64_t)res->file_offset;
            ssize_t sent = kl_sock_sendfile(sp, res->conn_fd, res->file_fd,
                                            &off, remaining);
            res->file_offset = (off_t)off;
            if (sent < 0) {
                if (errno == EAGAIN) return 1;
                return -1;
            }
            if (sent == 0) break;
            remaining = (size_t)(res->file_size - res->file_offset);
        }
        (void)kl_sock_set_cork(sp, res->conn_fd, 0);   /* uncork -> flush */
        return 0;
    }

    return 0;
}

/* ── Drain writer — wraps raw write/TLS for KlDrainWriteFn ───────── */

static ssize_t response_drain_writer(const char *data, size_t len, void *ctx) {
    KlResponse *res = ctx;
    ssize_t nw;
    if (res->tls) {
        nw = res->tls->write(res->tls, res->conn_fd, data, len);
        if (nw < 0) return -1;
        return nw;  /* 0 = WANT_WRITE (would-block) */
    }
    nw = kl_sock_send(res_provider(res), res->conn_fd, data, len);
    if (nw < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return 0;
        return -1;
    }
    return nw;
}

int kl_response_enable_drain(KlResponse *res, KlAllocator *alloc,
                              size_t max_size) {
    if (!res || !alloc) return -1;
    kl_drain_init(&res->drain, response_drain_writer, res, alloc);
    if (max_size > 0)
        kl_drain_set_max_size(&res->drain, max_size);
    res->drain_enabled = 1;
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

    if (res->drain_enabled) {
        if (kl_drain_write(&res->drain, hdr, (size_t)hdr_len) < 0 ||
            kl_drain_write(&res->drain, data, len) < 0 ||
            kl_drain_write(&res->drain, "\r\n", 2) < 0) {
            res->stream_error = 1;
            return -1;
        }
        return 0;
    }

    KlIoVec iov[3] = {
        { .base = hdr,          .len = (size_t)hdr_len },
        { .base = (void *)data, .len = len },
        { .base = (void *)"\r\n", .len = 2 },
    };

    if (stream_writev_all(res_provider(res), res->conn_fd, res->tls, iov, 3) < 0) {
        res->stream_error = 1;
        return -1;
    }
    return 0;
}

int kl_response_begin_stream(KlResponse *res, int status,
                             KlWriteFn *out_write, void **out_ctx) {
    res->body_mode = KL_BODY_STREAM;
    kl_response_status(res, status);
    if (kl_response_header(res, "Transfer-Encoding", "chunked") < 0)
        return -1;

    KlStatusLine sl = status_line_for(res->status);

    KlIoVec iov[3];
    int iovcnt = 0;

    iov[iovcnt].base = (void *)sl.str;
    iov[iovcnt].len = sl.len;
    iovcnt++;

    if (res->hdr_len > 0) {
        iov[iovcnt].base = res->hdr_buf;
        iov[iovcnt].len = res->hdr_len;
        iovcnt++;
    }

    iov[iovcnt].base = (void *)"\r\n";
    iov[iovcnt].len = 2;
    iovcnt++;

    if (stream_writev_all(res_provider(res), res->conn_fd, res->tls, iov, iovcnt) < 0) return -1;

    res->headers_sent = 1;
    *out_write = kl_stream_write;
    *out_ctx = res;
    return 0;
}

int kl_response_end_stream(KlResponse *res) {
    if (res->stream_error) return -1;
    if (res->head_request) return 0;

    if (res->drain_enabled) {
        if (kl_drain_write(&res->drain, "0\r\n\r\n", 5) < 0) {
            res->stream_error = 1;
            return -1;
        }
        res->stream_ended = 1;
        return 0;
    }

    KlIoVec iov = { .base = (void *)"0\r\n\r\n", .len = 5 };
    if (stream_writev_all(res_provider(res), res->conn_fd, res->tls, &iov, 1) < 0) {
        res->stream_error = 1;
        return -1;
    }
    return 0;
}
