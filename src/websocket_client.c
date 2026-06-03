/*
 * websocket_client.c — Async WebSocket client
 *
 * State machine: CONNECTING -> TLS_HANDSHAKE -> WS_HANDSHAKE -> OPEN -> CLOSING -> CLOSED
 *
 * Reuses KlWsFrameParser for parsing server frames (unmasked).
 * Client frames are masked with a random 4-byte key per RFC 6455 Section 5.3.
 */

#include <keel/websocket_client.h>
#include <keel/timer.h>
#include <keel/url.h>

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>

#include "sha1.h"
#include "base64.h"
#include "utf8.h"

/* ── Connection states ──────────────────────────────────────────── */

typedef enum {
    WSC_CONNECTING,
    WSC_TLS_HANDSHAKE,
    WSC_WS_HANDSHAKE,
    WSC_OPEN,
    WSC_CLOSING,
    WSC_CLOSED
} WscState;

/* ── Connection struct ──────────────────────────────────────────── */

struct KlWsClientConn {
    int              fd;
    WscState         state;
    KlEventCtx      *ev;
    KlAllocator     *alloc;
    KlWsClientCallbacks cbs;
    void            *user_data;

    /* Config */
    size_t           max_frame_size;
    KlTlsConfig     *tls_cfg;

    /* TLS (NULL if ws://) */
    KlTls           *tls;
    char             host_buf[256];

    /* WS handshake */
    char             ws_key_b64[KL_WS_CLIENT_KEY_B64_SIZE];
    char            *upgrade_buf;      /* send buffer for HTTP upgrade request */
    size_t           upgrade_len;
    size_t           upgrade_sent;
    char            *handshake_buf;    /* receive buffer for HTTP response */
    size_t           handshake_len;
    size_t           handshake_cap;

    /* Frame parser + message assembly */
    KlWsFrameParser  fp;
    char            *msg_buf;          /* assembled message data */
    size_t           msg_len;
    size_t           msg_cap;
    int              msg_opcode;       /* first fragment's opcode */
    uint32_t         utf8_state;       /* incremental UTF-8 validation */

    /* Close handshake state */
    int              close_sent;
    int              close_received;

    /* Auto-ping keep-alive */
    int              ping_interval_ms;
    int64_t          ping_timer_id;     /* -1 = no timer */
};

/* ── Forward declarations ───────────────────────────────────────── */

static void wsc_on_event(int fd, KlEventMask ready, void *user_data);
static void wsc_error(KlWsClientConn *ws, const char *msg);
static void wsc_close_connection(KlWsClientConn *ws);
static int  wsc_process_frames(KlWsClientConn *ws, const uint8_t *data, size_t len);
static void wsc_ping_timer(void *user_data);

/* ── I/O abstraction (plain or TLS) ────────────────────────────── */

static ssize_t wsc_write(KlWsClientConn *ws, const void *buf, size_t len)
{
    if (ws->tls)
        return ws->tls->write(ws->tls, ws->fd, buf, len);
    ssize_t r;
#ifdef MSG_NOSIGNAL
    do { r = send(ws->fd, buf, len, MSG_NOSIGNAL); } while (r < 0 && errno == EINTR);
#else
    do { r = write(ws->fd, buf, len); } while (r < 0 && errno == EINTR);
#endif
    return r;
}

static ssize_t wsc_read(KlWsClientConn *ws, void *buf, size_t len)
{
    if (ws->tls)
        return ws->tls->read(ws->tls, ws->fd, buf, len);
    ssize_t r;
    do { r = read(ws->fd, buf, len); } while (r < 0 && errno == EINTR);
    return r;
}

static int wsc_write_all(KlWsClientConn *ws, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    size_t sent = 0;
    while (sent < len) {
        ssize_t w = wsc_write(ws, p + sent, len - sent);
        if (w < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return -2;  /* would block — caller should retry */
            return -1;
        }
        if (w == 0) return -1;
        sent += (size_t)w;
    }
    return 0;
}

/* ── Random bytes for mask key ──────────────────────────────────── */

static void wsc_random_bytes(uint8_t *buf, size_t len)
{
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
    arc4random_buf(buf, len);
#else
    /* Fallback: /dev/urandom */
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        size_t total = 0;
        while (total < len) {
            ssize_t r = read(fd, buf + total, len - total);
            if (r <= 0) break;
            total += (size_t)r;
        }
        close(fd);
        if (total == len) return;
    }
    /* Last resort: time-seeded PRNG (not cryptographic, but WS masking
       is defense-in-depth, not a security boundary) */
    static int seeded = 0;
    if (!seeded) { srand((unsigned)time(NULL)); seeded = 1; }
    for (size_t i = 0; i < len; i++)
        buf[i] = (uint8_t)(rand() & 0xFF);
#endif
}

/* ── Generate WebSocket key ─────────────────────────────────────── */

static void wsc_generate_key(KlWsClientConn *ws)
{
    uint8_t raw[16];
    wsc_random_bytes(raw, sizeof(raw));
    size_t out_len;
    kl_base64_encode(raw, sizeof(raw), ws->ws_key_b64, &out_len);
}

/* ── Validate Sec-WebSocket-Accept ──────────────────────────────── */

static int wsc_validate_accept(const KlWsClientConn *ws, const char *accept_val,
                                size_t accept_len)
{
    /* Compute expected: SHA-1(key + magic GUID) -> base64 */
    size_t key_len = strlen(ws->ws_key_b64);
    size_t guid_len = strlen(KL_WS_MAGIC_GUID);
    char concat[64 + 36 + 1];
    if (key_len + guid_len >= sizeof(concat))
        return -1;
    memcpy(concat, ws->ws_key_b64, key_len);
    memcpy(concat + key_len, KL_WS_MAGIC_GUID, guid_len);
    concat[key_len + guid_len] = '\0';

    uint8_t sha[20];
    kl_sha1(concat, key_len + guid_len, sha);

    char expected[32];
    size_t exp_len;
    kl_base64_encode(sha, 20, expected, &exp_len);

    if (accept_len != exp_len)
        return -1;
    return memcmp(accept_val, expected, exp_len) == 0 ? 0 : -1;
}

/* ── Build HTTP upgrade request ─────────────────────────────────── */

static int wsc_build_upgrade(KlWsClientConn *ws, const KlUrl *url,
                              const char *protocol)
{
    /* Estimate buffer size */
    size_t cap = KL_WS_CLIENT_UPGRADE_BUF_INIT + url->path_len + url->host_len;
    ws->upgrade_buf = kl_malloc(ws->alloc, cap);
    if (!ws->upgrade_buf) return -1;

    int off = snprintf(ws->upgrade_buf, cap,
        "GET %.*s HTTP/1.1\r\n"
        "Host: %.*s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: %s\r\n",
        (int)url->path_len, url->path,
        (int)url->host_len, url->host,
        ws->ws_key_b64);

    if (off < 0 || (size_t)off >= cap) {
        kl_free(ws->alloc, ws->upgrade_buf, cap);
        ws->upgrade_buf = NULL;
        return -1;
    }

    if (protocol) {
        int n = snprintf(ws->upgrade_buf + off, cap - (size_t)off,
                         "Sec-WebSocket-Protocol: %s\r\n", protocol);
        if (n < 0 || (size_t)(off + n) >= cap) {
            kl_free(ws->alloc, ws->upgrade_buf, cap);
            ws->upgrade_buf = NULL;
            return -1;
        }
        off += n;
    }

    int n = snprintf(ws->upgrade_buf + off, cap - (size_t)off, "\r\n");
    if (n < 0 || (size_t)(off + n) >= cap) {
        kl_free(ws->alloc, ws->upgrade_buf, cap);
        ws->upgrade_buf = NULL;
        return -1;
    }
    off += n;

    ws->upgrade_len = (size_t)off;
    ws->upgrade_sent = 0;

    /* Shrink to actual size */
    if ((size_t)off < cap) {
        char *shrunk = kl_malloc(ws->alloc, (size_t)off);
        if (shrunk) {
            memcpy(shrunk, ws->upgrade_buf, (size_t)off);
            kl_free(ws->alloc, ws->upgrade_buf, cap);
            ws->upgrade_buf = shrunk;
        }
        /* else: keep oversized buffer, not an error */
    }

    return 0;
}

/* ── Parse HTTP upgrade response ────────────────────────────────── */

static int wsc_parse_handshake_response(const KlWsClientConn *ws)
{
    /* Look for \r\n\r\n */
    const char *end = NULL;
    for (size_t i = 0; i + 3 < ws->handshake_len; i++) {
        if (ws->handshake_buf[i] == '\r' && ws->handshake_buf[i+1] == '\n' &&
            ws->handshake_buf[i+2] == '\r' && ws->handshake_buf[i+3] == '\n') {
            end = ws->handshake_buf + i + 4;
            break;
        }
    }
    if (!end) return 0;  /* incomplete */

    /* Check status line: "HTTP/1.1 101" */
    if (ws->handshake_len < 12) return -1;
    if (strncmp(ws->handshake_buf, "HTTP/1.1 101", 12) != 0)
        return -1;

    /* Find Sec-WebSocket-Accept header */
    const char *p = ws->handshake_buf;
    const char *accept_val = NULL;
    size_t accept_len = 0;

    while (p < end) {
        const char *line_end = strstr(p, "\r\n");
        if (!line_end) break;
        if (line_end == p) break;  /* empty line = end of headers */

        if (strncasecmp(p, "Sec-WebSocket-Accept:", 21) == 0) {
            const char *val = p + 21;
            while (val < line_end && *val == ' ') val++;
            accept_val = val;
            accept_len = (size_t)(line_end - val);
        }
        p = line_end + 2;
    }

    if (!accept_val)
        return -1;
    if (wsc_validate_accept(ws, accept_val, accept_len) != 0)
        return -1;

    return 1;  /* success */
}

/* ── Send masked frame ──────────────────────────────────────────── */

static int wsc_send_frame(KlWsClientConn *ws, int opcode, const char *data,
                           size_t len)
{
    uint8_t hdr[KL_WS_FRAME_HEADER_MAX];
    size_t hdr_len;

    /* FIN=1, no RSV, opcode */
    hdr[0] = (uint8_t)(KL_WS_FIN_BIT | (opcode & KL_WS_OPCODE_MASK));

    /* Client frames MUST be masked (RFC 6455 Section 5.3) */
    uint8_t mask_key[KL_WS_MASK_KEY_LEN];
    wsc_random_bytes(mask_key, KL_WS_MASK_KEY_LEN);

    size_t mask_offset;
    if (len < 126) {
        hdr[1] = (uint8_t)(KL_WS_MASK_BIT | len);
        mask_offset = 2;
        hdr_len = 2 + KL_WS_MASK_KEY_LEN;
    } else if (len <= 65535) {
        hdr[1] = (uint8_t)(KL_WS_MASK_BIT | 126);
        hdr[2] = (uint8_t)(len >> 8);
        hdr[3] = (uint8_t)(len);
        mask_offset = 4;
        hdr_len = 4 + KL_WS_MASK_KEY_LEN;
    } else {
        hdr[1] = (uint8_t)(KL_WS_MASK_BIT | 127);
        uint64_t plen = (uint64_t)len;
        hdr[2] = (uint8_t)(plen >> 56);
        hdr[3] = (uint8_t)(plen >> 48);
        hdr[4] = (uint8_t)(plen >> 40);
        hdr[5] = (uint8_t)(plen >> 32);
        hdr[6] = (uint8_t)(plen >> 24);
        hdr[7] = (uint8_t)(plen >> 16);
        hdr[8] = (uint8_t)(plen >> 8);
        hdr[9] = (uint8_t)(plen);
        mask_offset = 10;
        hdr_len = 10 + KL_WS_MASK_KEY_LEN;
    }

    /* Append mask key to header */
    memcpy(hdr + mask_offset, mask_key, KL_WS_MASK_KEY_LEN);

    /* Send header */
    if (wsc_write_all(ws, hdr, hdr_len) < 0) return -1;

    /* Send masked payload */
    if (len > 0) {
        /* Mask in chunks to avoid allocating a full copy for large frames */
        size_t sent = 0;
        uint8_t chunk[4096];
        while (sent < len) {
            size_t chunk_len = len - sent;
            if (chunk_len > sizeof(chunk)) chunk_len = sizeof(chunk);
            memcpy(chunk, data + sent, chunk_len);
            for (size_t i = 0; i < chunk_len; i++)
                chunk[i] ^= mask_key[(sent + i) & 3];
            if (wsc_write_all(ws, chunk, chunk_len) < 0) return -1;
            sent += chunk_len;
        }
    }

    return 0;
}

/* ── Message assembly ───────────────────────────────────────────── */

static int wsc_msg_append(KlWsClientConn *ws, const char *data, size_t len)
{
    if (len == 0) return 0;

    if (ws->msg_len > SIZE_MAX / 2 || len > SIZE_MAX / 2)
        return -1;

    size_t needed = ws->msg_len + len;
    if (needed > ws->max_frame_size)
        return -1;

    if (needed > ws->msg_cap) {
        size_t new_cap = ws->msg_cap ? ws->msg_cap * 2 : 1024;
        if (new_cap < needed) new_cap = needed;
        char *new_buf = kl_malloc(ws->alloc, new_cap);
        if (!new_buf) return -1;
        if (ws->msg_buf) {
            memcpy(new_buf, ws->msg_buf, ws->msg_len);
            kl_free(ws->alloc, ws->msg_buf, ws->msg_cap);
        }
        ws->msg_buf = new_buf;
        ws->msg_cap = new_cap;
    }

    memcpy(ws->msg_buf + ws->msg_len, data, len);
    ws->msg_len += len;
    return 0;
}

static void wsc_msg_reset(KlWsClientConn *ws)
{
    ws->msg_len = 0;
    ws->msg_opcode = 0;
    ws->utf8_state = KL_UTF8_ACCEPT;
}

/* ── State handlers ─────────────────────────────────────────────── */

static void wsc_handle_connecting(KlWsClientConn *ws)
{
    int err = 0;
    socklen_t errlen = sizeof(err);
    getsockopt(ws->fd, SOL_SOCKET, SO_ERROR, &err, &errlen);
    if (err != 0) {
        wsc_error(ws, "connect failed");
        return;
    }

    if (ws->tls_cfg && ws->tls_cfg->factory) {
        ws->tls = ws->tls_cfg->factory(ws->tls_cfg->ctx, ws->alloc);
        if (!ws->tls) {
            wsc_error(ws, "TLS factory failed");
            return;
        }
        if (ws->tls->set_hostname && ws->host_buf[0])
            ws->tls->set_hostname(ws->tls, ws->host_buf);

        ws->state = WSC_TLS_HANDSHAKE;
        /* Fall through to TLS handshake */
        KlTlsResult r = ws->tls->handshake(ws->tls, ws->fd);
        if (r == KL_TLS_OK) {
            ws->state = WSC_WS_HANDSHAKE;
            kl_watcher_mod(ws->ev, ws->fd, KL_EVENT_WRITE);
        } else if (r == KL_TLS_WANT_READ) {
            kl_watcher_mod(ws->ev, ws->fd, KL_EVENT_READ);
        } else if (r == KL_TLS_WANT_WRITE) {
            kl_watcher_mod(ws->ev, ws->fd, KL_EVENT_WRITE);
        } else {
            wsc_error(ws, "TLS handshake failed");
        }
    } else {
        ws->state = WSC_WS_HANDSHAKE;
        kl_watcher_mod(ws->ev, ws->fd, KL_EVENT_WRITE);
    }
}

static void wsc_handle_tls_handshake(KlWsClientConn *ws)
{
    KlTlsResult r = ws->tls->handshake(ws->tls, ws->fd);
    if (r == KL_TLS_OK) {
        ws->state = WSC_WS_HANDSHAKE;
        kl_watcher_mod(ws->ev, ws->fd, KL_EVENT_WRITE);
    } else if (r == KL_TLS_WANT_READ) {
        kl_watcher_mod(ws->ev, ws->fd, KL_EVENT_READ);
    } else if (r == KL_TLS_WANT_WRITE) {
        kl_watcher_mod(ws->ev, ws->fd, KL_EVENT_WRITE);
    } else {
        wsc_error(ws, "TLS handshake failed");
    }
}

static void wsc_handle_ws_handshake(KlWsClientConn *ws, KlEventMask ready)
{
    /* Phase 1: send upgrade request */
    if (ws->upgrade_sent < ws->upgrade_len) {
        if (!(ready & KL_EVENT_WRITE)) return;

        ssize_t w = wsc_write(ws, ws->upgrade_buf + ws->upgrade_sent,
                               ws->upgrade_len - ws->upgrade_sent);
        if (w < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                kl_watcher_rearm(ws->ev, ws->fd);
                return;
            }
            wsc_error(ws, "handshake send failed");
            return;
        }
        if (w == 0) {
            wsc_error(ws, "connection closed during handshake");
            return;
        }
        ws->upgrade_sent += (size_t)w;

        if (ws->upgrade_sent >= ws->upgrade_len) {
            /* Done sending, switch to reading response */
            kl_watcher_mod(ws->ev, ws->fd, KL_EVENT_READ);
        } else {
            kl_watcher_rearm(ws->ev, ws->fd);
        }
        return;
    }

    /* Phase 2: read upgrade response */
    if (!(ready & KL_EVENT_READ)) return;

    if (!ws->handshake_buf) {
        ws->handshake_cap = KL_WS_CLIENT_UPGRADE_BUF_INIT;
        ws->handshake_buf = kl_malloc(ws->alloc, ws->handshake_cap);
        if (!ws->handshake_buf) {
            wsc_error(ws, "allocation failed");
            return;
        }
        ws->handshake_len = 0;
    }

    /* Grow if needed */
    if (ws->handshake_len >= ws->handshake_cap) {
        if (ws->handshake_cap > SIZE_MAX / 2) {
            wsc_error(ws, "handshake response too large");
            return;
        }
        size_t new_cap = ws->handshake_cap * 2;
        char *new_buf = kl_malloc(ws->alloc, new_cap);
        if (!new_buf) {
            wsc_error(ws, "allocation failed");
            return;
        }
        memcpy(new_buf, ws->handshake_buf, ws->handshake_len);
        kl_free(ws->alloc, ws->handshake_buf, ws->handshake_cap);
        ws->handshake_buf = new_buf;
        ws->handshake_cap = new_cap;
    }

    ssize_t nread = wsc_read(ws, ws->handshake_buf + ws->handshake_len,
                              ws->handshake_cap - ws->handshake_len);
    if (nread < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            kl_watcher_rearm(ws->ev, ws->fd);
            return;
        }
        wsc_error(ws, "handshake read failed");
        return;
    }
    if (nread == 0) {
        wsc_error(ws, "connection closed during handshake");
        return;
    }
    ws->handshake_len += (size_t)nread;

    int rc = wsc_parse_handshake_response(ws);
    if (rc < 0) {
        wsc_error(ws, "invalid handshake response");
        return;
    }
    if (rc == 0) {
        /* Need more data */
        kl_watcher_rearm(ws->ev, ws->fd);
        return;
    }

    /* Compute leftover bytes after HTTP headers (may contain WS frame data
     * that arrived in the same TCP segment as the upgrade response) */
    size_t http_end_offset = 0;
    for (size_t i = 0; i + 3 < ws->handshake_len; i++) {
        if (ws->handshake_buf[i] == '\r' && ws->handshake_buf[i+1] == '\n' &&
            ws->handshake_buf[i+2] == '\r' && ws->handshake_buf[i+3] == '\n') {
            http_end_offset = i + 4;
            break;
        }
    }
    size_t leftover_len = ws->handshake_len - http_end_offset;
    char *leftover = NULL;
    if (leftover_len > 0) {
        leftover = kl_malloc(ws->alloc, leftover_len);
        if (leftover)
            memcpy(leftover, ws->handshake_buf + http_end_offset, leftover_len);
    }

    /* Handshake complete — free handshake buffers */
    if (ws->upgrade_buf) {
        kl_free(ws->alloc, ws->upgrade_buf, ws->upgrade_len);
        ws->upgrade_buf = NULL;
    }
    if (ws->handshake_buf) {
        kl_free(ws->alloc, ws->handshake_buf, ws->handshake_cap);
        ws->handshake_buf = NULL;
    }

    /* Init frame parser */
    kl_ws_frame_init(&ws->fp);

    ws->state = WSC_OPEN;
    kl_watcher_mod(ws->ev, ws->fd, KL_EVENT_READ);

    if (ws->ping_interval_ms > 0) {
        ws->ping_timer_id = kl_timer_add(ws->ev, (uint64_t)ws->ping_interval_ms,
                                          wsc_ping_timer, ws);
    }

    if (ws->cbs.on_open)
        ws->cbs.on_open(ws, ws->user_data);

    /* Feed leftover bytes into frame parser (data that arrived with the
     * HTTP upgrade response in the same TCP segment) */
    if (leftover && leftover_len > 0 && ws->state == WSC_OPEN) {
        wsc_process_frames(ws, (const uint8_t *)leftover, leftover_len);
        kl_free(ws->alloc, leftover, leftover_len);
    } else if (leftover) {
        kl_free(ws->alloc, leftover, leftover_len);
    }
}

/* Process WS frame data. Called from wsc_handle_open (normal reads) and
 * from handshake completion (leftover bytes). Returns 0 to rearm, -1 on
 * close/error (caller must not access ws after -1). */
static int wsc_process_frames(KlWsClientConn *ws, const uint8_t *data,
                               size_t len)
{
    size_t pos = 0;
    while (pos < len) {
        size_t payload_before = ws->fp.payload_read;
        size_t consumed = 0;
        int rc = kl_ws_frame_parse(&ws->fp, data + pos,
                                    len - pos, &consumed);

        if (rc < 0) {
            wsc_error(ws, "frame parse error");
            return -1;
        }

        /* Extract payload bytes from this parse call */
        size_t payload_consumed = ws->fp.payload_read - payload_before;
        if (payload_consumed > 0 && ws->fp.state != KL_WS_FRAME_HEADER) {
            /* Payload bytes are at end of consumed range */
            size_t payload_offset = pos + consumed - payload_consumed;
            const char *payload_data = (const char *)(data + payload_offset);

            /* Frame size check */
            if (ws->fp.payload_len > ws->max_frame_size) {
                wsc_error(ws, "frame too large");
                return -1;
            }

            int opcode = ws->fp.opcode;

            /* Control frames */
            if (opcode >= 0x8) {
                if (rc == 1) {
                    if (opcode == KL_WS_OP_CLOSE) {
                        uint16_t code = 0;
                        const char *reason = NULL;
                        size_t reason_len = 0;
                        if (payload_consumed >= 2) {
                            code = (uint16_t)(((uint8_t)payload_data[0] << 8) |
                                              (uint8_t)payload_data[1]);
                            reason = payload_data + 2;
                            reason_len = payload_consumed - 2;
                        }
                        ws->close_received = 1;
                        if (!ws->close_sent) {
                            ws->close_sent = 1;
                            wsc_send_frame(ws, KL_WS_OP_CLOSE,
                                           payload_data, payload_consumed);
                        }
                        if (ws->cbs.on_close)
                            ws->cbs.on_close(ws, code, reason, reason_len,
                                             ws->user_data);
                        ws->state = WSC_CLOSED;
                        wsc_close_connection(ws);
                        return -1;
                    } else if (opcode == KL_WS_OP_PING) {
                        wsc_send_frame(ws, KL_WS_OP_PONG,
                                       payload_data, payload_consumed);
                    }
                    /* Pong: ignore */
                }
            } else {
                /* Data frame — reassemble */
                int is_first = (opcode != KL_WS_OP_CONTINUATION);
                if (is_first) {
                    wsc_msg_reset(ws);
                    ws->msg_opcode = opcode;
                } else if (ws->msg_opcode == 0) {
                    wsc_error(ws, "continuation without start");
                    return -1;
                }

                if (wsc_msg_append(ws, payload_data, payload_consumed) != 0) {
                    wsc_error(ws, "message too large");
                    return -1;
                }

                /* Incremental UTF-8 validation */
                if (ws->msg_opcode == KL_WS_OP_TEXT) {
                    kl_utf8_validate(&ws->utf8_state,
                                     (const uint8_t *)payload_data,
                                     payload_consumed);
                    if (ws->utf8_state == KL_UTF8_REJECT) {
                        wsc_error(ws, "invalid UTF-8");
                        return -1;
                    }
                }

                if (rc == 1 && ws->fp.fin) {
                    if (ws->msg_opcode == KL_WS_OP_TEXT &&
                        ws->utf8_state != KL_UTF8_ACCEPT) {
                        wsc_error(ws, "invalid UTF-8");
                        return -1;
                    }
                    if (ws->cbs.on_message)
                        ws->cbs.on_message(ws, ws->msg_buf, ws->msg_len,
                                           ws->msg_opcode == KL_WS_OP_BINARY,
                                           ws->user_data);
                    wsc_msg_reset(ws);
                }
            }
        } else if (rc == 1) {
            /* Frame complete with no payload in this chunk */
            int opcode = ws->fp.opcode;
            if (opcode == KL_WS_OP_CLOSE) {
                ws->close_received = 1;
                if (!ws->close_sent) {
                    ws->close_sent = 1;
                    wsc_send_frame(ws, KL_WS_OP_CLOSE, NULL, 0);
                }
                if (ws->cbs.on_close)
                    ws->cbs.on_close(ws, 1005, NULL, 0, ws->user_data);
                ws->state = WSC_CLOSED;
                wsc_close_connection(ws);
                return -1;
            } else if (opcode == KL_WS_OP_PING) {
                wsc_send_frame(ws, KL_WS_OP_PONG, NULL, 0);
            } else if (opcode < 0x8 && ws->fp.fin) {
                /* Empty message */
                if (opcode != KL_WS_OP_CONTINUATION) {
                    ws->msg_opcode = opcode;
                    ws->msg_len = 0;
                    ws->utf8_state = KL_UTF8_ACCEPT;
                }
                if (ws->cbs.on_message)
                    ws->cbs.on_message(ws, ws->msg_buf ? ws->msg_buf : "",
                                       ws->msg_len,
                                       ws->msg_opcode == KL_WS_OP_BINARY,
                                       ws->user_data);
                wsc_msg_reset(ws);
            }
        }

        pos += consumed;

        /* Reset frame parser for next frame */
        if (rc == 1)
            kl_ws_frame_init(&ws->fp);

        if (consumed == 0) break;
    }

    /* Check if close handshake complete */
    if (ws->close_sent && ws->close_received) {
        ws->state = WSC_CLOSED;
        wsc_close_connection(ws);
        return -1;
    }

    return 0;
}

static void wsc_handle_open(KlWsClientConn *ws)
{
    uint8_t buf[KL_WS_CLIENT_RECV_BUF_SIZE];

    ssize_t nread = wsc_read(ws, buf, sizeof(buf));
    if (nread < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            kl_watcher_rearm(ws->ev, ws->fd);
            return;
        }
        wsc_error(ws, "read error");
        return;
    }
    if (nread == 0) {
        /* Server closed connection */
        if (!ws->close_received) {
            if (ws->cbs.on_close)
                ws->cbs.on_close(ws, KL_WS_GOING_AWAY, NULL, 0,
                                 ws->user_data);
        }
        ws->state = WSC_CLOSED;
        wsc_close_connection(ws);
        return;
    }

    if (wsc_process_frames(ws, buf, (size_t)nread) == 0)
        kl_watcher_rearm(ws->ev, ws->fd);
}

/* ── Auto-ping timer callback ───────────────────────────────────── */

static void wsc_ping_timer(void *user_data) {
    KlWsClientConn *ws = user_data;
    if (ws->state != WSC_OPEN || ws->close_sent) return;
    kl_ws_client_send_ping(ws, NULL, 0);
    /* Re-add for recurring behavior */
    ws->ping_timer_id = kl_timer_add(ws->ev, (uint64_t)ws->ping_interval_ms,
                                      wsc_ping_timer, ws);
}

/* ── Event callback ─────────────────────────────────────────────── */

static void wsc_on_event(int fd, KlEventMask ready, void *user_data)
{
    KlWsClientConn *ws = user_data;
    (void)fd;

    switch (ws->state) {
    case WSC_CONNECTING:
        wsc_handle_connecting(ws);
        break;
    case WSC_TLS_HANDSHAKE:
        wsc_handle_tls_handshake(ws);
        break;
    case WSC_WS_HANDSHAKE:
        wsc_handle_ws_handshake(ws, ready);
        break;
    case WSC_OPEN:
    case WSC_CLOSING:
        wsc_handle_open(ws);
        break;
    case WSC_CLOSED:
        break;
    }
}

/* ── Error + cleanup helpers ────────────────────────────────────── */

static void wsc_close_connection(KlWsClientConn *ws)
{
    if (ws->ping_timer_id >= 0) {
        kl_timer_cancel(ws->ev, ws->ping_timer_id);
        ws->ping_timer_id = -1;
    }
    if (ws->fd >= 0) {
        kl_watcher_del(ws->ev, ws->fd);
        if (ws->tls) {
            ws->tls->shutdown(ws->tls, ws->fd);
            ws->tls->destroy(ws->tls);
            ws->tls = NULL;
        }
        close(ws->fd);
        ws->fd = -1;
    }
}

static void wsc_error(KlWsClientConn *ws, const char *msg)
{
    ws->state = WSC_CLOSED;
    wsc_close_connection(ws);
    if (ws->cbs.on_error)
        ws->cbs.on_error(ws, msg, ws->user_data);
}

/* ── Public API ─────────────────────────────────────────────────── */

KlWsClientConn *kl_ws_client_connect(KlEventCtx *ev, KlAllocator *alloc,
                                      const KlWsClientConfig *cfg,
                                      const char *url,
                                      const KlWsClientCallbacks *cbs,
                                      void *user_data)
{
    if (!ev || !alloc || !url)
        return NULL;

    KlUrl parsed;
    if (kl_url_parse(url, &parsed) != 0)
        return NULL;
    if (!parsed.is_ws)
        return NULL;

    KlTlsConfig *tls_cfg = cfg ? cfg->tls : NULL;
    if (parsed.is_https && !tls_cfg)
        return NULL;
    if (!parsed.is_https)
        tls_cfg = NULL;

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

    /* Create non-blocking socket */
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

    /* Allocate connection */
    KlWsClientConn *ws = kl_malloc(alloc, sizeof(KlWsClientConn));
    if (!ws) {
        close(fd);
        return NULL;
    }
    memset(ws, 0, sizeof(*ws));

    ws->fd = fd;
    ws->state = (rc == 0) ? WSC_WS_HANDSHAKE : WSC_CONNECTING;
    ws->ev = ev;
    ws->alloc = alloc;
    ws->tls_cfg = tls_cfg;
    ws->max_frame_size = (cfg && cfg->max_frame_size > 0) ? cfg->max_frame_size
                                                            : KL_WS_CLIENT_DEFAULT_MAX_FRAME;
    ws->ping_interval_ms = cfg ? cfg->ping_interval_ms : 0;
    ws->ping_timer_id = -1;
    if (cbs)
        ws->cbs = *cbs;
    ws->user_data = user_data;

    memcpy(ws->host_buf, host_buf, parsed.host_len + 1);

    /* Generate WebSocket key and build upgrade request */
    wsc_generate_key(ws);
    if (wsc_build_upgrade(ws, &parsed, cfg ? cfg->protocol : NULL) != 0) {
        close(fd);
        kl_free(alloc, ws, sizeof(KlWsClientConn));
        return NULL;
    }

    /* Register watcher — connect in progress, wait for writable */
    if (kl_watcher_add(ev, fd, KL_EVENT_WRITE, wsc_on_event, ws) != 0) {
        if (ws->upgrade_buf)
            kl_free(alloc, ws->upgrade_buf, ws->upgrade_len);
        close(fd);
        kl_free(alloc, ws, sizeof(KlWsClientConn));
        return NULL;
    }

    return ws;
}

int kl_ws_client_send_text(KlWsClientConn *ws, const char *data, size_t len)
{
    if (!ws || ws->state != WSC_OPEN || ws->close_sent) return -1;
    return wsc_send_frame(ws, KL_WS_OP_TEXT, data, len);
}

int kl_ws_client_send_binary(KlWsClientConn *ws, const char *data, size_t len)
{
    if (!ws || ws->state != WSC_OPEN || ws->close_sent) return -1;
    return wsc_send_frame(ws, KL_WS_OP_BINARY, data, len);
}

int kl_ws_client_send_ping(KlWsClientConn *ws, const char *data, size_t len)
{
    if (!ws || ws->state != WSC_OPEN || ws->close_sent) return -1;
    if (len > 125) return -1;
    return wsc_send_frame(ws, KL_WS_OP_PING, data, len);
}

void kl_ws_client_close(KlWsClientConn *ws, uint16_t code, const char *reason,
                          size_t reason_len)
{
    if (!ws || ws->state != WSC_OPEN || ws->close_sent) return;

    ws->close_sent = 1;
    ws->state = WSC_CLOSING;

    /* Build close payload */
    uint8_t buf[127];
    size_t plen = 0;
    if (code != 0) {
        buf[0] = (uint8_t)(code >> 8);
        buf[1] = (uint8_t)(code);
        plen = 2;
        if (reason && reason_len > 0) {
            if (reason_len > 123) reason_len = 123;
            memcpy(buf + 2, reason, reason_len);
            plen += reason_len;
        }
    }

    wsc_send_frame(ws, KL_WS_OP_CLOSE, (const char *)buf, plen);
}

void kl_ws_client_free(KlWsClientConn *ws)
{
    if (!ws) return;

    wsc_close_connection(ws);

    if (ws->upgrade_buf) {
        kl_free(ws->alloc, ws->upgrade_buf, ws->upgrade_len);
        ws->upgrade_buf = NULL;
    }
    if (ws->handshake_buf) {
        kl_free(ws->alloc, ws->handshake_buf, ws->handshake_cap);
        ws->handshake_buf = NULL;
    }
    if (ws->msg_buf) {
        kl_free(ws->alloc, ws->msg_buf, ws->msg_cap);
        ws->msg_buf = NULL;
    }

    KlAllocator *alloc = ws->alloc;
    kl_free(alloc, ws, sizeof(KlWsClientConn));
}
