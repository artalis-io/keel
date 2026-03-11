/*
 * websocket.h — Shared WebSocket protocol constants and frame parser
 *
 * Contains frame parser, opcodes, and status codes shared by both
 * server (websocket_server.h) and client (websocket_client.h).
 */

#ifndef KEEL_WEBSOCKET_H
#define KEEL_WEBSOCKET_H

#include <keel/allocator.h>
#include <stddef.h>
#include <stdint.h>

/* ── Status codes (RFC 6455 Section 7.4.1) ──────────────────────── */

#define KL_WS_NORMAL_CLOSE    1000
#define KL_WS_GOING_AWAY      1001
#define KL_WS_PROTOCOL_ERROR  1002
#define KL_WS_TOO_BIG         1009

/* ── Opcodes (RFC 6455 Section 5.2) ─────────────────────────────── */

#define KL_WS_OP_CONTINUATION  0x0
#define KL_WS_OP_TEXT          0x1
#define KL_WS_OP_BINARY        0x2
#define KL_WS_OP_CLOSE         0x8
#define KL_WS_OP_PING          0x9
#define KL_WS_OP_PONG          0xA

/* ── Frame constants ────────────────────────────────────────────── */

#define KL_WS_FIN_BIT          0x80
#define KL_WS_MASK_BIT         0x80
#define KL_WS_OPCODE_MASK      0x0F
#define KL_WS_MASK_KEY_LEN     4
#define KL_WS_FRAME_HEADER_MAX 14   /* 2 + 8(ext len) + 4(mask key) */

/* ── Internal frame parser state ────────────────────────────────── */

typedef enum {
    KL_WS_FRAME_HEADER,
    KL_WS_FRAME_PAYLOAD,
    KL_WS_FRAME_COMPLETE
} KlWsFrameParseState;

typedef struct {
    KlWsFrameParseState state;
    uint8_t header_buf[KL_WS_FRAME_HEADER_MAX];
    size_t header_len;
    size_t header_need;      /* total header bytes needed */
    int fin, opcode, masked;
    uint8_t mask_key[KL_WS_MASK_KEY_LEN];
    size_t payload_len;
    size_t payload_read;
} KlWsFrameParser;

/* ── Frame parser API (shared by server and client) ──────────────── */

void kl_ws_frame_init(KlWsFrameParser *fp);
int  kl_ws_frame_parse(KlWsFrameParser *fp, const uint8_t *data,
                        size_t len, size_t *consumed);

/* ── RFC 6455 magic GUID ─────────────────────────────────────────── */

#define KL_WS_MAGIC_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

#endif
