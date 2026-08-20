/*
 * server_ws.c — the WebSocket SERVER logic (kl_ws_server_*): handshake/upgrade,
 * framed send/recv over a KlHttpConn, drain + idle keepalive, and the upgrade-seam
 * registration (proto_hooks.h). Split out of websocket.c in the Phase 10 UEFI
 * server work (S-1) — the shared frame codec (kl_ws_frame_*) stays in websocket.c;
 * this TU mirrors the client's websocket_client.c and the server_core.c /
 * server_h2.c naming. A freestanding HTTP/1.1 server links none of it.
 */

#include <keel/websocket.h>
#include <keel/websocket_server.h>
#include <keel/http_connection.h>
#include <keel/http_request.h>
#include <keel/server.h>   /* KlServer — kl_server_ws registration API lives here */
#include <keel/router.h>   /* kl_router_add + KlRoute.ws_config */
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include "internal.h"
#include "proto_hooks.h"   /* WS server upgrade seam — registered for the core */
#include "sha1.h"
#include "base64.h"
#include "utf8.h"

/* ── Default limits ──────────────────────────────────────────────── */

#define WS_DEFAULT_MAX_MESSAGE  (1024 * 1024)  /* 1 MB */
#define WS_DEFAULT_MAX_FRAME    (64 * 1024)    /* 64 KB */
#define WS_DEFAULT_CLOSE_TIMEOUT 5000          /* 5 seconds */

/* ── Config ──────────────────────────────────────────────────────── */

void kl_ws_server_config_init(KlWsServerConfig *config) {
    memset(config, 0, sizeof(*config));
}

static size_t ws_max_message(const KlWsServerConfig *cfg) {
    return cfg->max_message_size ? cfg->max_message_size
                                : WS_DEFAULT_MAX_MESSAGE;
}

static size_t ws_max_frame(const KlWsServerConfig *cfg) {
    return cfg->max_frame_size ? cfg->max_frame_size
                               : WS_DEFAULT_MAX_FRAME;
}

static int ws_close_timeout(const KlWsServerConfig *cfg) {
    return cfg->close_timeout_ms ? cfg->close_timeout_ms
                                 : WS_DEFAULT_CLOSE_TIMEOUT;
}

/* Unmask payload data in-place */
static void ws_unmask(uint8_t *data, size_t len, const uint8_t mask[4],
                      size_t offset) {
    /* Process 4 bytes at a time where aligned */
    size_t i = 0;
    size_t start_offset = offset & 3;

    /* Handle unaligned prefix */
    for (; i < len && ((start_offset + i) & 3) != 0; i++)
        data[i] ^= mask[(start_offset + i) & 3];

    /* 4-byte blocks */
    if (start_offset == 0 || i > 0) {
        for (; i + 3 < len; i += 4) {
            size_t base = (start_offset + i) & 3;
            data[i]     ^= mask[base];
            data[i + 1] ^= mask[(base + 1) & 3];
            data[i + 2] ^= mask[(base + 2) & 3];
            data[i + 3] ^= mask[(base + 3) & 3];
        }
    }

    /* Handle remaining bytes */
    for (; i < len; i++)
        data[i] ^= mask[(start_offset + i) & 3];
}

/* ── Drain writer — wraps conn_write for KlDrainWriteFn ──────────── */

static kl_ssize_t ws_drain_writer(const char *data, size_t len, void *ctx) {
    KlWsServerConn *ws = ctx;
    ssize_t nw = conn_write(ws->conn, data, len);
    if (nw < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        return -1;
    }
    return nw;
}

int kl_ws_server_peer_cred(const KlWsServerConn *ws, KlPeerCred *out) {
    if (!ws || !ws->conn)
        return -1;
    return kl_peer_cred_fd(ws->conn->stream.fd, out);
}

int kl_ws_server_enable_drain(KlWsServerConn *ws, size_t max_size) {
    if (!ws) return -1;
    kl_drain_init(&ws->drain, ws_drain_writer, ws, ws->alloc);
    if (max_size > 0)
        kl_drain_set_max_size(&ws->drain, max_size);
    ws->drain_enabled = 1;
    return 0;
}

/* ── Send frame ──────────────────────────────────────────────────── */

static int ws_send_frame(KlWsServerConn *ws, int opcode, const char *data,
                         size_t len) {
    uint8_t hdr[10];
    size_t hdr_len;

    /* FIN=1, no RSV, opcode — server frames are never masked */
    hdr[0] = (uint8_t)(KL_WS_FIN_BIT | (opcode & KL_WS_OPCODE_MASK));

    if (len < 126) {
        hdr[1] = (uint8_t)len;
        hdr_len = 2;
    } else if (len <= 65535) {
        hdr[1] = 126;
        hdr[2] = (uint8_t)(len >> 8);
        hdr[3] = (uint8_t)(len);
        hdr_len = 4;
    } else {
        hdr[1] = 127;
        uint64_t plen = (uint64_t)len;
        hdr[2] = (uint8_t)(plen >> 56);
        hdr[3] = (uint8_t)(plen >> 48);
        hdr[4] = (uint8_t)(plen >> 40);
        hdr[5] = (uint8_t)(plen >> 32);
        hdr[6] = (uint8_t)(plen >> 24);
        hdr[7] = (uint8_t)(plen >> 16);
        hdr[8] = (uint8_t)(plen >> 8);
        hdr[9] = (uint8_t)(plen);
        hdr_len = 10;
    }

    if (ws->drain_enabled) {
        if (kl_drain_write(&ws->drain, (const char *)hdr, hdr_len) < 0)
            return -1;
        if (len > 0 && kl_drain_write(&ws->drain, data, len) < 0)
            return -1;
        return 0;
    }

    /* Write header + payload with retry for short writes (TLS WANT_WRITE) */
    if (conn_write_all(ws->conn, hdr, hdr_len) < 0) return -1;
    if (len > 0 && conn_write_all(ws->conn, data, len) < 0) return -1;

    return 0;
}

/* ── Public send functions ───────────────────────────────────────── */

int kl_ws_server_send_text(KlWsServerConn *ws, const char *data, size_t len) {
    if (!ws) return -1;
    if (ws->close_sent) return -1;
    return ws_send_frame(ws, KL_WS_OP_TEXT, data, len);
}

int kl_ws_server_send_binary(KlWsServerConn *ws, const char *data, size_t len) {
    if (!ws) return -1;
    if (ws->close_sent) return -1;
    return ws_send_frame(ws, KL_WS_OP_BINARY, data, len);
}

int kl_ws_server_send_ping(KlWsServerConn *ws, const char *data, size_t len) {
    if (!ws) return -1;
    if (ws->close_sent) return -1;
    if (len > 125) return -1;
    return ws_send_frame(ws, KL_WS_OP_PING, data, len);
}

int kl_ws_server_close(KlWsServerConn *ws, uint16_t code, const char *reason,
                        size_t reason_len) {
    if (!ws) return -1;
    if (ws->close_sent) return 0;
    ws->close_sent = 1;
    ws->close_code = code;
    ws->close_deadline_ms = kl_monotonic_ms() +
                            (uint64_t)ws_close_timeout(ws->config);

    /* Build close payload: 2-byte code + reason */
    uint8_t buf[127];
    size_t plen = 0;
    if (code != 0) {
        buf[0] = (uint8_t)(code >> 8);
        buf[1] = (uint8_t)(code);
        plen = 2;
        if (reason && reason_len > 0) {
            if (reason_len > 123) reason_len = 123;  /* max control payload 125 */
            memcpy(buf + 2, reason, reason_len);
            plen += reason_len;
        }
    }

    return ws_send_frame(ws, KL_WS_OP_CLOSE, (const char *)buf, plen);
}

/* ── Handshake ───────────────────────────────────────────────────── */

/*
 * Check if Connection header contains "upgrade" token.
 * The Connection header may contain multiple comma-separated tokens.
 */
static int connection_has_upgrade(const char *val, size_t val_len) {
    const char *p = val;
    const char *end = val + val_len;

    while (p < end) {
        /* Skip whitespace and commas */
        while (p < end && (*p == ' ' || *p == ',' || *p == '\t')) p++;
        const char *tok = p;
        while (p < end && *p != ',' && *p != ' ' && *p != '\t') p++;
        size_t tok_len = (size_t)(p - tok);
        if (tok_len == 7 && strncasecmp(tok, "upgrade", 7) == 0)
            return 1;
    }
    return 0;
}

static const char ws_400_response[] =
    "HTTP/1.1 400 Bad Request\r\n"
    "Content-Length: 0\r\n"
    "Connection: close\r\n"
    "\r\n";

int kl_ws_server_upgrade(KlHttpConn *c, const char *leftover,
                          size_t leftover_len) {
    KlWsServerConfig *cfg = c->route->ws_config;

    /* Validate required headers */
    size_t conn_len = 0, upgrade_len = 0, ver_len = 0, key_len = 0;
    const char *conn_hdr = kl_http_request_header_len(&c->req, "Connection",
                                                  &conn_len);
    const char *upgrade_hdr = kl_http_request_header_len(&c->req, "Upgrade",
                                                     &upgrade_len);
    const char *version = kl_http_request_header_len(&c->req,
                                                 "Sec-WebSocket-Version",
                                                 &ver_len);
    const char *key = kl_http_request_header_len(&c->req, "Sec-WebSocket-Key",
                                             &key_len);

    /* Validate: Connection must contain "upgrade" */
    if (!conn_hdr || !connection_has_upgrade(conn_hdr, conn_len)) {
        best_effort_conn_write(c, ws_400_response,
                               sizeof(ws_400_response) - 1);
        return KL_HTTP_CONN_CLOSED;
    }

    /* Validate: Upgrade must be "websocket" */
    if (!upgrade_hdr || upgrade_len != 9 ||
        strncasecmp(upgrade_hdr, "websocket", 9) != 0) {
        best_effort_conn_write(c, ws_400_response,
                               sizeof(ws_400_response) - 1);
        return KL_HTTP_CONN_CLOSED;
    }

    /* Validate: Version must be "13" */
    if (!version || ver_len != 2 || memcmp(version, "13", 2) != 0) {
        best_effort_conn_write(c, ws_400_response,
                               sizeof(ws_400_response) - 1);
        return KL_HTTP_CONN_CLOSED;
    }

    /* Validate: Key must be present (24 base64 chars) */
    if (!key || key_len != 24) {
        best_effort_conn_write(c, ws_400_response,
                               sizeof(ws_400_response) - 1);
        return KL_HTTP_CONN_CLOSED;
    }

    /* Validate: Key must contain only valid base64 characters */
    for (size_t i = 0; i < 24; i++) {
        char ch = key[i];
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9') || ch == '+' || ch == '/' ||
              ch == '=')) {
            best_effort_conn_write(c, ws_400_response,
                                   sizeof(ws_400_response) - 1);
            return KL_HTTP_CONN_CLOSED;
        }
    }

    /* Compute Sec-WebSocket-Accept = Base64(SHA-1(key + magic)) */
    char concat[60 + 1];  /* 24 + 36 + null */
    memcpy(concat, key, key_len);
    memcpy(concat + key_len, KL_WS_MAGIC_GUID, 36);
    concat[key_len + 36] = '\0';

    uint8_t sha1_out[20];
    kl_sha1(concat, key_len + 36, sha1_out);

    char accept_b64[32];
    size_t accept_len;
    kl_base64_encode(sha1_out, 20, accept_b64, &accept_len);

    /* Build 101 Switching Protocols response */
    char resp[256];
    int resp_len = snprintf(resp, sizeof(resp),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %.*s\r\n"
        "\r\n",
        (int)accept_len, accept_b64);

    if (resp_len < 0 || (size_t)resp_len >= sizeof(resp)) {
        best_effort_conn_write(c, ws_400_response,
                               sizeof(ws_400_response) - 1);
        return KL_HTTP_CONN_CLOSED;
    }

    if (conn_write_all(c, resp, (size_t)resp_len) < 0) {
        return KL_HTTP_CONN_CLOSED;
    }

    /* Allocate WebSocket connection state */
    KlWsServerConn *ws = kl_malloc(c->stream.alloc, sizeof(KlWsServerConn));
    if (!ws) return KL_HTTP_CONN_CLOSED;
    memset(ws, 0, sizeof(*ws));

    ws->config = cfg;
    ws->conn = c;
    ws->alloc = c->stream.alloc;
    ws->utf8_state = KL_UTF8_ACCEPT;
    kl_ws_frame_init(&ws->frame);
    if (cfg->ping_interval_ms > 0)
        ws->next_ping_ms = kl_monotonic_ms() + (uint64_t)cfg->ping_interval_ms;

    c->ws = ws;
    c->state = KL_HTTP_CONN_WEBSOCKET;

    /* Call on_open */
    if (cfg->callbacks.on_open)
        cfg->callbacks.on_open(ws, cfg->user_data);

    /* Feed any leftover bytes after HTTP headers into the frame parser */
    if (leftover_len > 0) {
        /* Process leftover data as WebSocket frames */
        return kl_ws_server_on_readable_data(c, (uint8_t *)leftover,
                                              leftover_len);
    }

    return KL_HTTP_CONN_WEBSOCKET;
}

/* ── Message delivery ────────────────────────────────────────────── */

static int ws_deliver_message(KlWsServerConn *ws) {
    if (!ws->config->callbacks.on_message) return 0;

    int is_binary = (ws->msg_opcode == KL_WS_OP_BINARY);

    /* UTF-8 validation for text messages */
    if (!is_binary) {
        if (ws->utf8_state != KL_UTF8_ACCEPT) {
            kl_ws_server_close(ws, 1007, "Invalid UTF-8", 13);
            return -1;
        }
    }

    ws->config->callbacks.on_message(
        ws, ws->msg_buf ? ws->msg_buf : "",
        ws->msg_len, is_binary, ws->config->user_data);

    /* Reset reassembly buffer */
    ws->msg_len = 0;
    ws->msg_opcode = 0;
    ws->utf8_state = KL_UTF8_ACCEPT;
    return 0;
}

/* Grow message buffer to fit additional data */
static int ws_msg_grow(KlWsServerConn *ws, size_t additional) {
    if (additional > SIZE_MAX - ws->msg_len) return -1;
    size_t needed = ws->msg_len + additional;
    if (needed > ws_max_message(ws->config)) return -1;

    if (needed > ws->msg_cap) {
        size_t new_cap = ws->msg_cap ? ws->msg_cap * 2 : 4096;
        if (new_cap < needed) new_cap = needed;
        if (new_cap > ws_max_message(ws->config))
            new_cap = ws_max_message(ws->config);
        /* Overflow check */
        if (new_cap > SIZE_MAX / 2) return -1;

        char *new_buf = kl_realloc(ws->alloc, ws->msg_buf,
                                    ws->msg_cap, new_cap);
        if (!new_buf) return -1;
        ws->msg_buf = new_buf;
        ws->msg_cap = new_cap;
    }
    return 0;
}

/* ── Control frame handling ──────────────────────────────────────── */

static int ws_handle_close(KlWsServerConn *ws, const uint8_t *payload,
                            size_t len) {
    ws->close_received = 1;
    uint16_t code = 1005;  /* No Status Received (default) */
    const char *reason = NULL;
    size_t reason_len = 0;

    if (len >= 2) {
        code = (uint16_t)(((uint16_t)payload[0] << 8) | payload[1]);
        if (len > 2) {
            reason = (const char *)(payload + 2);
            reason_len = len - 2;
        }
    }

    if (!ws->close_sent) {
        /* Echo close back */
        kl_ws_server_close(ws, code, reason, reason_len);
    }

    /* Notify user */
    if (ws->config->callbacks.on_close)
        ws->config->callbacks.on_close(ws, code, reason, reason_len,
                                        ws->config->user_data);

    return KL_HTTP_CONN_CLOSED;
}

static int ws_handle_ping(KlWsServerConn *ws, const uint8_t *payload,
                           size_t len) {
    if (ws->config->callbacks.on_ping) {
        ws->config->callbacks.on_ping(ws, (const char *)payload, len,
                                       ws->config->user_data);
    } else {
        /* Auto-pong */
        ws_send_frame(ws, KL_WS_OP_PONG, (const char *)payload, len);
    }
    return 0;
}

/* ── Frame dispatch ──────────────────────────────────────────────── */

int kl_ws_server_on_readable_data(KlHttpConn *c, uint8_t *data, size_t len) {
    KlWsServerConn *ws = c->ws;
    size_t pos = 0;

    while (pos < len) {
        /* Remember where payload starts for this frame parse iteration */
        size_t frame_data_start = pos;
        size_t payload_before = ws->frame.payload_read;

        size_t consumed = 0;
        int rc = kl_ws_frame_parse(&ws->frame, data + pos, len - pos,
                                    &consumed);

        if (rc < 0) {
            kl_ws_server_close(ws, KL_WS_PROTOCOL_ERROR, NULL, 0);
            return KL_HTTP_CONN_CLOSED;
        }

        /* RFC 6455 §5.1: server MUST close on unmasked client frame */
        if (ws->frame.state != KL_WS_FRAME_HEADER && !ws->frame.masked) {
            kl_ws_server_close(ws, KL_WS_PROTOCOL_ERROR, NULL, 0);
            return KL_HTTP_CONN_CLOSED;
        }

        /* Extract and unmask payload data from this parse call */
        size_t payload_consumed = ws->frame.payload_read - payload_before;
        if (payload_consumed > 0 && ws->frame.state != KL_WS_FRAME_HEADER) {
            /* Payload bytes are at end of consumed range */
            size_t payload_offset = frame_data_start + consumed -
                                    payload_consumed;
            uint8_t *payload_data = data + payload_offset;

            /* Unmask in-place */
            if (ws->frame.masked) {
                ws_unmask(payload_data, payload_consumed,
                          ws->frame.mask_key, payload_before);
            }

            /* Frame size check */
            if (ws->frame.payload_len > ws_max_frame(ws->config)) {
                kl_ws_server_close(ws, KL_WS_TOO_BIG, NULL, 0);
                return KL_HTTP_CONN_CLOSED;
            }

            int opcode = ws->frame.opcode;

            /* Control frames (can interleave with fragments) */
            if (opcode >= 0x8) {
                if (rc == 1) {
                    /* Control frame complete */
                    if (opcode == KL_WS_OP_CLOSE) {
                        int state = ws_handle_close(ws, payload_data,
                                                     payload_consumed);
                        return state;
                    } else if (opcode == KL_WS_OP_PING) {
                        ws_handle_ping(ws, payload_data, payload_consumed);
                    }
                    /* Pong: ignore (RFC 6455 Section 5.5.3) */
                }
                /* Control frames must fit in one parse since max 125 bytes */
            } else {
                /* Data frame — reassemble */
                int is_first = (opcode != KL_WS_OP_CONTINUATION);

                if (is_first) {
                    /* Start of new message */
                    if (ws->msg_len > 0 && ws->msg_opcode != 0) {
                        /* Previous message not finished — protocol error */
                        kl_ws_server_close(ws, KL_WS_PROTOCOL_ERROR, NULL, 0);
                        return KL_HTTP_CONN_CLOSED;
                    }
                    ws->msg_opcode = opcode;
                    ws->msg_len = 0;
                    ws->utf8_state = KL_UTF8_ACCEPT;
                } else if (ws->msg_opcode == 0) {
                    /* Continuation without a start frame */
                    kl_ws_server_close(ws, KL_WS_PROTOCOL_ERROR, NULL, 0);
                    return KL_HTTP_CONN_CLOSED;
                }

                /* Append to message buffer */
                if (ws_msg_grow(ws, payload_consumed) < 0) {
                    kl_ws_server_close(ws, KL_WS_TOO_BIG, NULL, 0);
                    return KL_HTTP_CONN_CLOSED;
                }
                memcpy(ws->msg_buf + ws->msg_len, payload_data,
                       payload_consumed);
                ws->msg_len += payload_consumed;

                /* Incremental UTF-8 validation for text messages */
                if (ws->msg_opcode == KL_WS_OP_TEXT) {
                    kl_utf8_validate(&ws->utf8_state, payload_data,
                                      payload_consumed);
                    if (ws->utf8_state == KL_UTF8_REJECT) {
                        kl_ws_server_close(ws, 1007, "Invalid UTF-8", 13);
                        return KL_HTTP_CONN_CLOSED;
                    }
                }

                /* Deliver if final fragment */
                if (rc == 1 && ws->frame.fin) {
                    if (ws_deliver_message(ws) < 0)
                        return KL_HTTP_CONN_CLOSED;
                }
            }
        } else if (rc == 1) {
            /* Frame complete with no payload in this chunk (e.g., empty
             * control frame or already consumed) */
            int opcode = ws->frame.opcode;
            if (opcode == KL_WS_OP_CLOSE) {
                return ws_handle_close(ws, NULL, 0);
            } else if (opcode == KL_WS_OP_PING) {
                ws_handle_ping(ws, NULL, 0);
            } else if (opcode < 0x8 && ws->frame.fin) {
                /* Empty final frame or unfragmented empty message */
                if (opcode != KL_WS_OP_CONTINUATION) {
                    ws->msg_opcode = opcode;
                    ws->msg_len = 0;
                    ws->utf8_state = KL_UTF8_ACCEPT;
                }
                if (ws_deliver_message(ws) < 0)
                    return KL_HTTP_CONN_CLOSED;
            }
        }

        pos += consumed;

        /* Reset frame parser for next frame */
        if (rc == 1) {
            kl_ws_frame_init(&ws->frame);
        }

        /* If no progress was made, break to avoid infinite loop */
        if (consumed == 0) break;
    }

    /* Check close deadline */
    if (ws->close_sent && ws->close_received)
        return KL_HTTP_CONN_CLOSED;

    return KL_HTTP_CONN_WEBSOCKET;
}

/* ── Readable event ──────────────────────────────────────────────── */

int kl_ws_server_on_readable(KlHttpConn *c) {
    c->last_active_ms = kl_monotonic_ms();

    uint8_t buf[KL_HTTP_CONN_READ_BUF_SIZE];
    ssize_t nr = conn_read(c, buf, sizeof(buf));
    if (nr <= 0) {
        /* Connection closed or error */
        return KL_HTTP_CONN_CLOSED;
    }

    return kl_ws_server_on_readable_data(c, buf, (size_t)nr);
}

/* ── Write-readiness (drain flush) ────────────────────────────────── */

int kl_ws_server_on_writable(KlHttpConn *c) {
    if (!c->ws || !c->ws->drain_enabled)
        return KL_HTTP_CONN_WEBSOCKET;
    int r = kl_drain_flush(&c->ws->drain);
    if (r < 0) return KL_HTTP_CONN_CLOSED;
    return KL_HTTP_CONN_WEBSOCKET;
}

int kl_ws_server_drain_pending(const KlHttpConn *c) {
    if (!c->ws || !c->ws->drain_enabled) return 0;
    return kl_drain_pending(&c->ws->drain);
}

/* ── Cleanup ─────────────────────────────────────────────────────── */

void kl_ws_server_cleanup(KlHttpConn *c) {
    if (!c->ws) return;
    KlWsServerConn *ws = c->ws;

    /* Notify on_close if we haven't received a close frame */
    if (!ws->close_received && ws->config->callbacks.on_close)
        ws->config->callbacks.on_close(ws, 1006, NULL, 0,
                                        ws->config->user_data);

    if (ws->drain_enabled)
        kl_drain_free(&ws->drain);
    if (ws->msg_buf)
        kl_free(ws->alloc, ws->msg_buf, ws->msg_cap);
    kl_free(c->stream.alloc, ws, sizeof(KlWsServerConn));
    c->ws = NULL;
}

/* ── Drain close ─────────────────────────────────────────────────── */

void kl_ws_server_drain_close(KlHttpConn *c) {
    if (!c->ws || c->ws->close_sent) return;
    kl_ws_server_close(c->ws, KL_WS_GOING_AWAY, "Server shutting down", 20);
}

/* ── Close timeout check ─────────────────────────────────────────── */

int kl_ws_server_check_close_timeout(const KlHttpConn *c, uint64_t now) {
    if (!c->ws) return 0;
    if (c->ws->close_sent && !c->ws->close_received &&
        now >= c->ws->close_deadline_ms) {
        return 1;
    }
    return 0;
}

/* ── Auto-ping keep-alive ─────────────────────────────────────────── */

int kl_ws_server_auto_ping(KlHttpConn *c, uint64_t now) {
    if (!c->ws || c->ws->next_ping_ms == 0) return 0;
    if (c->ws->close_sent || c->ws->close_received) return 0;
    if (now < c->ws->next_ping_ms) return 0;
    kl_ws_server_send_ping(c->ws, NULL, 0);
    c->ws->next_ping_ms = now + (uint64_t)c->ws->config->ping_interval_ms;
    return 1;
}

/* ── WebSocket server upgrade seam registration (proto_hooks.h) ──────────────
 * The shared server core reaches WebSocket only through this table; installing it
 * both wires the core and forces server_ws.o out of the static archive. */
static const KlWsServerHooks kl_ws_server_hooks_table = {
    .upgrade             = kl_ws_server_upgrade,
    .on_readable         = kl_ws_server_on_readable,
    .on_writable         = kl_ws_server_on_writable,
    .drain_pending       = kl_ws_server_drain_pending,
    .cleanup             = kl_ws_server_cleanup,
    .auto_ping           = kl_ws_server_auto_ping,
    .check_close_timeout = kl_ws_server_check_close_timeout,
    .drain_close         = kl_ws_server_drain_close,
};

void kl_ws_server_hooks_install(void) {
    kl_ws_server_hooks_set(&kl_ws_server_hooks_table);
}

/* Public WebSocket route-registration API. Lives in the ws module (moved from server.c
 * in the Finding-1 decoupling) so the readiness server.c owns no WebSocket type — a
 * freestanding HTTP/1.1 server links neither this nor KlWsServerConfig. */
int kl_server_ws(KlServer *s, const char *pattern, KlWsServerConfig *config) {
    /* Register as a GET route with no handler — ws_config triggers the upgrade. */
    if (kl_router_add(&s->router, "GET", pattern, NULL, NULL, NULL) < 0)
        return -1;
    s->router.routes[s->router.count - 1].ws_config = config;
    return 0;
}

/* Also self-install at load, so a consumer driving connection.c's dispatch directly
 * (without kl_server_init — e.g. the unit tests) has the seam wired. Runs only if
 * this object is linked (a direct kl_ws_server_* reference pulls it in). */
__attribute__((constructor))
static void kl_ws_server_hooks_autoinstall(void) {
    kl_ws_server_hooks_install();
}
