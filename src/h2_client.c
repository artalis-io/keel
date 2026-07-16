/*
 * h2_client.c — Async HTTP/2 client
 *
 * State machine: CONNECTING -> TLS_HANDSHAKE -> H2_INIT -> ACTIVE -> CLOSED
 *
 * Uses the pluggable KlH2ClientSession vtable so the actual HTTP/2
 * framing can be backed by nghttp2 or any other library. Tests use
 * a mock session — no nghttp2 dependency.
 */

#include <keel/h2_client.h>
#include <keel/url.h>

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>

/* ── Connection states ──────────────────────────────────────────── */

typedef enum {
    H2C_CONNECTING,
    H2C_TLS_HANDSHAKE,
    H2C_H2_INIT,
    H2C_ACTIVE,
    H2C_CLOSED
} H2cState;

/* ── Per-stream tracking ────────────────────────────────────────── */

struct KlH2ClientStream {
    int32_t                stream_id;
    KlH2ClientResponse     resp;
    KlH2ClientResponseFn   on_resp;
    void                  *user_data;
    KlH2ClientStream      *next;
};

/* ── Connection struct ──────────────────────────────────────────── */

struct KlH2ClientConn {
    int                    fd;
    H2cState               state;
    KlEventCtx            *ev;
    KlAllocator           *alloc;

    /* Session */
    KlH2ClientSession     *session;
    KlH2ClientConfig       cfg;
    char                   host_buf[256];
    char                   authority[280];

    /* TLS */
    KlTls                 *tls;

    /* Streams */
    KlH2ClientStream      *streams;
    int                    num_streams;

    /* Callbacks */
    KlH2ClientErrorFn      on_error;
    void                  *user_data;
};

/* ── Forward declarations ───────────────────────────────────────── */

static void h2c_on_event(int fd, KlEventMask ready, void *user_data);
static void h2c_error(KlH2ClientConn *c, const char *msg);
static void h2c_close_connection(KlH2ClientConn *c);

/* ── I/O abstraction ───────────────────────────────────────────── */

static ssize_t h2c_write(KlH2ClientConn *c, const void *buf, size_t len)
{
    if (c->tls)
        return c->tls->write(c->tls, c->fd, buf, len);
    ssize_t r;
#ifdef MSG_NOSIGNAL
    do { r = send(c->fd, buf, len, MSG_NOSIGNAL); } while (r < 0 && errno == EINTR);
#else
    do { r = write(c->fd, buf, len); } while (r < 0 && errno == EINTR);
#endif
    return r;
}

static ssize_t h2c_read(KlH2ClientConn *c, void *buf, size_t len)
{
    if (c->tls)
        return c->tls->read(c->tls, c->fd, buf, len);
    ssize_t r;
    do { r = read(c->fd, buf, len); } while (r < 0 && errno == EINTR);
    return r;
}

/* ── Stream tracking ────────────────────────────────────────────── */

static KlH2ClientStream *h2c_stream_find(KlH2ClientConn *c, int32_t stream_id)
{
    for (KlH2ClientStream *s = c->streams; s; s = s->next) {
        if (s->stream_id == stream_id)
            return s;
    }
    return NULL;
}

static KlH2ClientStream *h2c_stream_create(KlH2ClientConn *c, int32_t stream_id,
                                             KlH2ClientResponseFn on_resp,
                                             void *user_data)
{
    int max = c->cfg.max_concurrent_streams > 0
                  ? c->cfg.max_concurrent_streams
                  : KL_H2_DEFAULT_MAX_STREAMS;
    if (c->num_streams >= max)
        return NULL;

    KlH2ClientStream *s = kl_malloc(c->alloc, sizeof(KlH2ClientStream));
    if (!s) return NULL;
    memset(s, 0, sizeof(*s));

    s->stream_id = stream_id;
    s->on_resp = on_resp;
    s->user_data = user_data;
    s->next = c->streams;
    c->streams = s;
    c->num_streams++;
    return s;
}

static void h2c_stream_remove(KlH2ClientConn *c, int32_t stream_id)
{
    KlH2ClientStream **pp = &c->streams;
    while (*pp) {
        if ((*pp)->stream_id == stream_id) {
            KlH2ClientStream *s = *pp;
            *pp = s->next;
            c->num_streams--;

            /* Free accumulated response */
            kl_h2_client_response_free(&s->resp, c->alloc);
            kl_free(c->alloc, s, sizeof(KlH2ClientStream));
            return;
        }
        pp = &(*pp)->next;
    }
}

/* ── Session callbacks ──────────────────────────────────────────── */

static int h2c_on_send(KlH2ClientSession *s, const void *data, size_t len)
{
    KlH2ClientConn *c = s->keel_ctx;
    const char *p = (const char *)data;
    size_t sent = 0;
    while (sent < len) {
        ssize_t w = h2c_write(c, p + sent, len - sent);
        if (w < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return (int)sent;  /* partial send */
            return -1;
        }
        if (w == 0) return -1;
        sent += (size_t)w;
    }
    return (int)sent;
}

static void h2c_on_response(KlH2ClientSession *s, int32_t stream_id,
                              int status, const KlH2ClientHeader *hdrs, int n)
{
    KlH2ClientConn *c = s->keel_ctx;
    KlH2ClientStream *st = h2c_stream_find(c, stream_id);
    if (!st) return;

    st->resp.status = status;

    /* Copy headers */
    if (n > 0 && hdrs) {
        if (n > st->resp.headers_cap) {
            if ((size_t)n > SIZE_MAX / sizeof(KlH2ClientHeader)) return;
            size_t sz = (size_t)n * sizeof(KlH2ClientHeader);
            KlH2ClientHeader *new_hdrs = kl_malloc(c->alloc, sz);
            if (!new_hdrs) return;
            if (st->resp.headers)
                kl_free(c->alloc, st->resp.headers,
                        (size_t)st->resp.headers_cap * sizeof(KlH2ClientHeader));
            st->resp.headers = new_hdrs;
            st->resp.headers_cap = n;
        }
        int copied = 0;
        for (int i = 0; i < n; i++) {
            /* Duplicate name/value strings */
            size_t nlen = strlen(hdrs[i].name) + 1;
            size_t vlen = strlen(hdrs[i].value) + 1;
            char *name = kl_malloc(c->alloc, nlen);
            char *value = kl_malloc(c->alloc, vlen);
            if (!name || !value) {
                if (name) kl_free(c->alloc, name, nlen);
                if (value) kl_free(c->alloc, value, vlen);
                st->resp.num_headers = copied;
                return;
            }
            memcpy(name, hdrs[i].name, nlen);
            memcpy(value, hdrs[i].value, vlen);
            st->resp.headers[i].name = name;
            st->resp.headers[i].value = value;
            copied++;
        }
        st->resp.num_headers = copied;
    }
}

static void h2c_on_data(KlH2ClientSession *s, int32_t stream_id,
                          const char *data, size_t len)
{
    KlH2ClientConn *c = s->keel_ctx;
    KlH2ClientStream *st = h2c_stream_find(c, stream_id);
    if (!st || len == 0) return;

    /* Grow body buffer */
    if (len > SIZE_MAX - st->resp.body_len) return;
    size_t needed = st->resp.body_len + len;
    if (needed > SIZE_MAX / 2) return;

    if (needed > st->resp.body_cap) {
        size_t new_cap = st->resp.body_cap ? st->resp.body_cap * 2 : 4096;
        if (new_cap < needed) new_cap = needed;
        char *new_body = kl_malloc(c->alloc, new_cap);
        if (!new_body) return;
        if (st->resp.body) {
            memcpy(new_body, st->resp.body, st->resp.body_len);
            kl_free(c->alloc, st->resp.body, st->resp.body_cap);
        }
        st->resp.body = new_body;
        st->resp.body_cap = new_cap;
    }

    memcpy(st->resp.body + st->resp.body_len, data, len);
    st->resp.body_len += len;
}

static void h2c_on_stream_close(KlH2ClientSession *s, int32_t stream_id,
                                  int err)
{
    KlH2ClientConn *c = s->keel_ctx;
    KlH2ClientStream *st = h2c_stream_find(c, stream_id);
    if (!st) return;

    /* Deliver response */
    if (st->on_resp) {
        (void)err;
        st->on_resp(c, stream_id, &st->resp, st->user_data);
    }

    h2c_stream_remove(c, stream_id);
}

/* ── State handlers ─────────────────────────────────────────────── */

static void h2c_handle_connecting(KlH2ClientConn *c)
{
    int err = 0;
    socklen_t errlen = sizeof(err);
    getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &errlen);
    if (err != 0) {
        h2c_error(c, "connect failed");
        return;
    }

    if (c->tls) {
        /* Should not happen: TLS was set before connect */
        h2c_error(c, "internal error");
        return;
    }

    if (c->cfg.tls && c->cfg.tls->factory) {
        c->tls = c->cfg.tls->factory(c->cfg.tls->ctx, c->alloc);
        if (!c->tls) {
            h2c_error(c, "TLS factory failed");
            return;
        }
        if (c->tls->set_hostname && c->host_buf[0])
            c->tls->set_hostname(c->tls, c->host_buf);

        c->state = H2C_TLS_HANDSHAKE;
        KlTlsResult r = c->tls->handshake(c->tls, c->fd);
        if (r == KL_TLS_OK) {
            c->state = H2C_H2_INIT;
            /* Fall through to H2 init below */
        } else if (r == KL_TLS_WANT_READ) {
            kl_watcher_mod(c->ev, c->fd, KL_EVENT_READ);
            return;
        } else if (r == KL_TLS_WANT_WRITE) {
            kl_watcher_mod(c->ev, c->fd, KL_EVENT_WRITE);
            return;
        } else {
            h2c_error(c, "TLS handshake failed");
            return;
        }
    } else {
        c->state = H2C_H2_INIT;
    }

    /* H2 init: create session */
    if (!c->cfg.session) {
        h2c_error(c, "no session factory");
        return;
    }

    c->session = c->cfg.session(c->alloc);
    if (!c->session) {
        h2c_error(c, "session factory failed");
        return;
    }

    /* Wire up callbacks */
    c->session->keel_cbs.on_send = h2c_on_send;
    c->session->keel_cbs.on_response = h2c_on_response;
    c->session->keel_cbs.on_data = h2c_on_data;
    c->session->keel_cbs.on_stream_close = h2c_on_stream_close;
    c->session->keel_ctx = c;

    c->state = H2C_ACTIVE;
    kl_watcher_mod(c->ev, c->fd, KL_EVENT_READ);
}

static void h2c_handle_tls_handshake(KlH2ClientConn *c)
{
    KlTlsResult r = c->tls->handshake(c->tls, c->fd);
    if (r == KL_TLS_OK) {
        c->state = H2C_H2_INIT;
        /* Create session — reuse the init path */
        if (!c->cfg.session) {
            h2c_error(c, "no session factory");
            return;
        }
        c->session = c->cfg.session(c->alloc);
        if (!c->session) {
            h2c_error(c, "session factory failed");
            return;
        }
        c->session->keel_cbs.on_send = h2c_on_send;
        c->session->keel_cbs.on_response = h2c_on_response;
        c->session->keel_cbs.on_data = h2c_on_data;
        c->session->keel_cbs.on_stream_close = h2c_on_stream_close;
        c->session->keel_ctx = c;
        c->state = H2C_ACTIVE;
        kl_watcher_mod(c->ev, c->fd, KL_EVENT_READ);
    } else if (r == KL_TLS_WANT_READ) {
        kl_watcher_mod(c->ev, c->fd, KL_EVENT_READ);
    } else if (r == KL_TLS_WANT_WRITE) {
        kl_watcher_mod(c->ev, c->fd, KL_EVENT_WRITE);
    } else {
        h2c_error(c, "TLS handshake failed");
    }
}

static void h2c_handle_active(KlH2ClientConn *c)
{
    char buf[KL_H2_CLIENT_RECV_BUF_SIZE];
    ssize_t nread = h2c_read(c, buf, sizeof(buf));

    if (nread < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            kl_watcher_rearm(c->ev, c->fd);
            return;
        }
        h2c_error(c, "read error");
        return;
    }
    if (nread == 0) {
        h2c_error(c, "connection closed");
        return;
    }

    /* Feed data to session */
    if (c->session->recv(c->session, buf, (size_t)nread) < 0) {
        h2c_error(c, "session recv error");
        return;
    }

    /* Flush any pending output */
    if (c->session->flush(c->session) < 0) {
        h2c_error(c, "session flush error");
        return;
    }

    kl_watcher_rearm(c->ev, c->fd);
}

/* ── Event callback ─────────────────────────────────────────────── */

static void h2c_on_event(int fd, KlEventMask ready, void *user_data)
{
    KlH2ClientConn *c = user_data;
    (void)fd;
    (void)ready;

    switch (c->state) {
    case H2C_CONNECTING:
        h2c_handle_connecting(c);
        break;
    case H2C_TLS_HANDSHAKE:
        h2c_handle_tls_handshake(c);
        break;
    case H2C_H2_INIT:
        /* Should not happen — init is synchronous after connect/TLS */
        h2c_error(c, "unexpected state");
        break;
    case H2C_ACTIVE:
        h2c_handle_active(c);
        break;
    case H2C_CLOSED:
        break;
    }
}

/* ── Error + cleanup ────────────────────────────────────────────── */

static void h2c_close_connection(KlH2ClientConn *c)
{
    if (c->fd >= 0) {
        kl_watcher_del(c->ev, c->fd);
        if (c->tls) {
            c->tls->shutdown(c->tls, c->fd);
            c->tls->destroy(c->tls);
            c->tls = NULL;
        }
        close(c->fd);
        c->fd = -1;
    }
}

static void h2c_error(KlH2ClientConn *c, const char *msg)
{
    c->state = H2C_CLOSED;
    h2c_close_connection(c);
    if (c->on_error)
        c->on_error(c, msg, c->user_data);
}

/* ── Public API ─────────────────────────────────────────────────── */

KlH2ClientConn *kl_h2_client_connect(KlEventCtx *ev, KlAllocator *alloc,
                                      const KlH2ClientConfig *cfg,
                                      const char *url,
                                      KlH2ClientErrorFn on_error,
                                      void *user_data)
{
    if (!ev || !alloc || !cfg || !url || !cfg->session)
        return NULL;

    KlUrl parsed;
    if (kl_url_parse(url, &parsed) != 0)
        return NULL;
    /* HTTP/2 over a UNIX socket is not supported; reject http+unix:// URLs
     * here so parsed.host (NULL for unix) is never used below. */
    if (parsed.is_unix)
        return NULL;

    if (parsed.is_https && !cfg->tls)
        return NULL;

    /* DNS resolve */
    char host_buf[256];
    if (parsed.host_len >= sizeof(host_buf))
        return NULL;
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
    if (rc != 0 || !res)
        return NULL;

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
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
        return NULL;
    }

    rc = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    if (rc < 0 && errno != EINPROGRESS) {
        close(fd);
        return NULL;
    }

    KlH2ClientConn *c = kl_malloc(alloc, sizeof(KlH2ClientConn));
    if (!c) {
        close(fd);
        return NULL;
    }
    memset(c, 0, sizeof(*c));

    c->fd = fd;
    c->state = (rc == 0) ? H2C_H2_INIT : H2C_CONNECTING;
    c->ev = ev;
    c->alloc = alloc;
    c->cfg = *cfg;
    c->on_error = on_error;
    c->user_data = user_data;

    memcpy(c->host_buf, host_buf, parsed.host_len + 1);
    snprintf(c->authority, sizeof(c->authority), "%s:%d", host_buf, parsed.port);

    /* If already connected (rc == 0), create session immediately */
    if (c->state == H2C_H2_INIT) {
        c->session = cfg->session(alloc);
        if (!c->session) {
            close(fd);
            kl_free(alloc, c, sizeof(KlH2ClientConn));
            return NULL;
        }
        c->session->keel_cbs.on_send = h2c_on_send;
        c->session->keel_cbs.on_response = h2c_on_response;
        c->session->keel_cbs.on_data = h2c_on_data;
        c->session->keel_cbs.on_stream_close = h2c_on_stream_close;
        c->session->keel_ctx = c;
        c->state = H2C_ACTIVE;
    }

    if (kl_watcher_add(ev, fd, KL_EVENT_READ | KL_EVENT_WRITE,
                       h2c_on_event, c) != 0) {
        if (c->session) c->session->destroy(c->session);
        close(fd);
        kl_free(alloc, c, sizeof(KlH2ClientConn));
        return NULL;
    }

    return c;
}

int32_t kl_h2_client_request(KlH2ClientConn *c, const char *method,
                              const char *path,
                              const KlH2ClientHeader *hdrs, int n,
                              const char *body, size_t body_len,
                              KlH2ClientResponseFn on_resp, void *ud)
{
    if (!c || c->state != H2C_ACTIVE || !c->session)
        return -1;
    if (!method || !path)
        return -1;

    int32_t stream_id = c->session->submit_request(
        c->session, method, path, c->authority, hdrs, n, body, body_len);

    if (stream_id < 0)
        return -1;

    const KlH2ClientStream *st = h2c_stream_create(c, stream_id, on_resp, ud);
    if (!st)
        return -1;

    /* Flush to send the request */
    c->session->flush(c->session);

    return stream_id;
}

void kl_h2_client_close(KlH2ClientConn *c)
{
    if (!c || c->state == H2C_CLOSED) return;
    c->state = H2C_CLOSED;
    h2c_close_connection(c);
}

void kl_h2_client_free(KlH2ClientConn *c)
{
    if (!c) return;

    if (c->state != H2C_CLOSED)
        kl_h2_client_close(c);

    /* Free all pending streams */
    while (c->streams) {
        KlH2ClientStream *s = c->streams;
        c->streams = s->next;
        kl_h2_client_response_free(&s->resp, c->alloc);
        kl_free(c->alloc, s, sizeof(KlH2ClientStream));
    }

    if (c->session) {
        c->session->destroy(c->session);
        c->session = NULL;
    }

    KlAllocator *alloc = c->alloc;
    kl_free(alloc, c, sizeof(KlH2ClientConn));
}

void kl_h2_client_response_free(KlH2ClientResponse *resp, KlAllocator *alloc)
{
    if (!resp || !alloc) return;

    if (resp->headers) {
        for (int i = 0; i < resp->num_headers; i++) {
            if (resp->headers[i].name)
                kl_free(alloc, (char *)resp->headers[i].name,
                        strlen(resp->headers[i].name) + 1);
            if (resp->headers[i].value)
                kl_free(alloc, (char *)resp->headers[i].value,
                        strlen(resp->headers[i].value) + 1);
        }
        kl_free(alloc, resp->headers,
                (size_t)resp->headers_cap * sizeof(KlH2ClientHeader));
        resp->headers = NULL;
    }
    resp->num_headers = 0;
    resp->headers_cap = 0;

    if (resp->body) {
        kl_free(alloc, resp->body, resp->body_cap);
        resp->body = NULL;
    }
    resp->body_len = 0;
    resp->body_cap = 0;
    resp->status = 0;
}
