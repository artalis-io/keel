/*
 * websocket.c — the SHARED WebSocket frame codec (RFC 6455 §5).
 *
 * Just the incremental frame parser: used by BOTH the server (http_server_ws.c) and the
 * client (websocket_client.c). The WebSocket
 * SERVER logic (kl_ws_server_*) lives in http_server_ws.c — mirroring the client's
 * websocket_client.c and the http_server_core.c / http2_server.c naming. A freestanding
 * HTTP/1.1 server links neither this codec nor http_server_ws.c.
 */

#include <keel/websocket.h>
#include <string.h>
#include <stdint.h>

void kl_ws_frame_init(KlWsFrameParser *fp) {
    memset(fp, 0, sizeof(*fp));
    fp->state = KL_WS_FRAME_HEADER;
    fp->header_need = 2;  /* minimum header: 2 bytes */
}

/*
 * Parse WebSocket frame incrementally.
 * Returns:  1 = frame complete
 *           0 = need more data
 *          -1 = protocol error
 */
int kl_ws_frame_parse(KlWsFrameParser *fp, const uint8_t *data,
                       size_t len, size_t *consumed) {
    *consumed = 0;

    if (fp->state == KL_WS_FRAME_HEADER) {
        /* Accumulate header bytes */
        while (*consumed < len && fp->header_len < fp->header_need) {
            fp->header_buf[fp->header_len++] = data[*consumed];
            (*consumed)++;

            /* After 2 bytes, determine full header size */
            if (fp->header_len == 2) {
                fp->fin = (fp->header_buf[0] >> 7) & 1;
                int rsv = (fp->header_buf[0] >> 4) & 7;
                fp->opcode = fp->header_buf[0] & KL_WS_OPCODE_MASK;
                fp->masked = (fp->header_buf[1] >> 7) & 1;
                uint8_t len7 = fp->header_buf[1] & 0x7F;

                /* RSV bits must be 0 (no extensions) */
                if (rsv != 0) return -1;

                /* Reject reserved opcodes (RFC 6455 §5.2): only 0x0-0x2
                 * (continuation/text/binary) and 0x8-0xA (close/ping/pong)
                 * are defined; 0x3-0x7 and 0xB-0xF must fail the connection. */
                if ((fp->opcode > 0x2 && fp->opcode < 0x8) || fp->opcode > 0xA)
                    return -1;

                /* Determine header size based on payload length encoding */
                size_t extra = 0;
                if (len7 == 126) extra = 2;
                else if (len7 == 127) extra = 8;
                if (fp->masked) extra += KL_WS_MASK_KEY_LEN;
                fp->header_need = 2 + extra;
            }
        }

        if (fp->header_len < fp->header_need) return 0;

        /* Decode payload length */
        uint8_t len7 = fp->header_buf[1] & 0x7F;
        size_t mask_offset;

        if (len7 < 126) {
            fp->payload_len = len7;
            mask_offset = 2;
        } else if (len7 == 126) {
            fp->payload_len = ((size_t)fp->header_buf[2] << 8) |
                              (size_t)fp->header_buf[3];
            mask_offset = 4;
        } else {
            /* 64-bit length — check for overflow and absurd sizes */
            uint64_t plen = 0;
            for (int i = 0; i < 8; i++)
                plen = (plen << 8) | fp->header_buf[2 + i];
            /* MSB must be 0 per RFC */
            if (plen >> 63) return -1;
            if (plen > SIZE_MAX / 2) return -1;
            fp->payload_len = (size_t)plen;
            mask_offset = 10;
        }

        /* Extract mask key */
        if (fp->masked) {
            memcpy(fp->mask_key, fp->header_buf + mask_offset,
                   KL_WS_MASK_KEY_LEN);
        }

        /* Control frames: max 125 bytes, must not be fragmented */
        if (fp->opcode >= 0x8) {
            if (fp->payload_len > 125) return -1;
            if (!fp->fin) return -1;
        }

        fp->payload_read = 0;
        fp->state = (fp->payload_len == 0) ? KL_WS_FRAME_COMPLETE
                                           : KL_WS_FRAME_PAYLOAD;
    }

    /* Return complete if no payload */
    if (fp->state == KL_WS_FRAME_COMPLETE) return 1;

    /* Payload phase — just track how much has been consumed */
    if (fp->state == KL_WS_FRAME_PAYLOAD) {
        size_t remaining = fp->payload_len - fp->payload_read;
        size_t avail = len - *consumed;
        size_t take = (avail < remaining) ? avail : remaining;
        /* Unmasking is done by the caller on the data slice */
        *consumed += take;
        fp->payload_read += take;

        if (fp->payload_read >= fp->payload_len) {
            fp->state = KL_WS_FRAME_COMPLETE;
            return 1;
        }
    }

    return 0;
}
