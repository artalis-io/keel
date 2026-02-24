#ifndef KEEL_BASE64_INTERNAL_H
#define KEEL_BASE64_INTERNAL_H

/*
 * Minimal Base64 encoder for WebSocket handshake (RFC 6455 Section 4.2.2).
 * Internal to KEEL, not part of the public API.
 */

#include <stddef.h>
#include <stdint.h>

static const char kl_b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/*
 * Base64-encode data into out buffer.
 * @param data     Input bytes.
 * @param len      Input length.
 * @param out      Output buffer (must be at least ((len+2)/3)*4 + 1 bytes).
 * @param out_len  Receives output length (excluding null terminator).
 */
static void kl_base64_encode(const uint8_t *data, size_t len,
                             char *out, size_t *out_len) {
    size_t i = 0, j = 0;

    for (; i + 2 < len; i += 3) {
        uint32_t v = ((uint32_t)data[i] << 16) |
                     ((uint32_t)data[i + 1] << 8) |
                     ((uint32_t)data[i + 2]);
        out[j++] = kl_b64_table[(v >> 18) & 0x3F];
        out[j++] = kl_b64_table[(v >> 12) & 0x3F];
        out[j++] = kl_b64_table[(v >> 6) & 0x3F];
        out[j++] = kl_b64_table[v & 0x3F];
    }

    if (i < len) {
        uint32_t v = (uint32_t)data[i] << 16;
        if (i + 1 < len) v |= (uint32_t)data[i + 1] << 8;
        out[j++] = kl_b64_table[(v >> 18) & 0x3F];
        out[j++] = kl_b64_table[(v >> 12) & 0x3F];
        out[j++] = (i + 1 < len) ? kl_b64_table[(v >> 6) & 0x3F] : '=';
        out[j++] = '=';
    }

    out[j] = '\0';
    *out_len = j;
}

#endif
