#include "utest.h"
#include "../../../src/protocols/http/http_conn_internal.h"
#include <keel/websocket.h>
#include <keel/websocket_server.h>
#include <keel/http_connection.h>
#include <keel/allocator.h>
#include <string.h>
#include <stdint.h>
#include "net_compat.h"

/* Pull in internal headers for direct testing */
#include "sha1.h"
#include "base64.h"
#include "utf8.h"

/* ═══════════════════════════════════════════════════════════════════
 * SHA-1 tests
 * ═══════════════════════════════════════════════════════════════════ */

UTEST(sha1, empty_string) {
    uint8_t out[20];
    kl_sha1("", 0, out);
    /* SHA-1("") = da39a3ee 5e6b4b0d 3255bfef 95601890 afd80709 */
    uint8_t expected[] = {
        0xda, 0x39, 0xa3, 0xee, 0x5e, 0x6b, 0x4b, 0x0d,
        0x32, 0x55, 0xbf, 0xef, 0x95, 0x60, 0x18, 0x90,
        0xaf, 0xd8, 0x07, 0x09
    };
    ASSERT_TRUE(memcmp(out, expected, 20) == 0);
}

UTEST(sha1, abc) {
    uint8_t out[20];
    kl_sha1("abc", 3, out);
    /* SHA-1("abc") = a9993e36 4706816a ba3e2571 7850c26c 9cd0d89d */
    uint8_t expected[] = {
        0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a,
        0xba, 0x3e, 0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c,
        0x9c, 0xd0, 0xd8, 0x9d
    };
    ASSERT_TRUE(memcmp(out, expected, 20) == 0);
}

UTEST(sha1, long_input) {
    /* SHA-1("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") */
    const char *input = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    uint8_t out[20];
    kl_sha1(input, strlen(input), out);
    uint8_t expected[] = {
        0x84, 0x98, 0x3e, 0x44, 0x1c, 0x3b, 0xd2, 0x6e,
        0xba, 0xae, 0x4a, 0xa1, 0xf9, 0x51, 0x29, 0xe5,
        0xe5, 0x46, 0x70, 0xf1
    };
    ASSERT_TRUE(memcmp(out, expected, 20) == 0);
}

/* ═══════════════════════════════════════════════════════════════════
 * Base64 tests
 * ═══════════════════════════════════════════════════════════════════ */

UTEST(base64, empty) {
    char out[8];
    size_t len;
    kl_base64_encode((const uint8_t *)"", 0, out, &len);
    ASSERT_EQ(len, (size_t)0);
    ASSERT_STREQ(out, "");
}

UTEST(base64, hello) {
    char out[16];
    size_t len;
    kl_base64_encode((const uint8_t *)"hello", 5, out, &len);
    ASSERT_EQ(len, (size_t)8);
    ASSERT_STREQ(out, "aGVsbG8=");
}

UTEST(base64, sha1_output) {
    /* Base64 of the SHA-1("abc") hash */
    uint8_t sha1_abc[] = {
        0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a,
        0xba, 0x3e, 0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c,
        0x9c, 0xd0, 0xd8, 0x9d
    };
    char out[32];
    size_t len;
    kl_base64_encode(sha1_abc, 20, out, &len);
    ASSERT_EQ(len, (size_t)28);
    ASSERT_STREQ(out, "qZk+NkcGgWq6PiVxeFDCbJzQ2J0=");
}

/* ═══════════════════════════════════════════════════════════════════
 * UTF-8 tests
 * ═══════════════════════════════════════════════════════════════════ */

UTEST(utf8, valid_ascii) {
    uint32_t state = KL_UTF8_ACCEPT;
    kl_utf8_validate(&state, (const uint8_t *)"Hello, World!", 13);
    ASSERT_EQ(state, (uint32_t)KL_UTF8_ACCEPT);
}

UTEST(utf8, valid_multibyte) {
    uint32_t state = KL_UTF8_ACCEPT;
    /* "日本語" in UTF-8 */
    const uint8_t data[] = {0xE6, 0x97, 0xA5, 0xE6, 0x9C, 0xAC,
                             0xE8, 0xAA, 0x9E};
    kl_utf8_validate(&state, data, sizeof(data));
    ASSERT_EQ(state, (uint32_t)KL_UTF8_ACCEPT);
}

UTEST(utf8, overlong_rejection) {
    uint32_t state = KL_UTF8_ACCEPT;
    /* Overlong encoding of '/' (0x2F): C0 AF */
    const uint8_t data[] = {0xC0, 0xAF};
    kl_utf8_validate(&state, data, sizeof(data));
    ASSERT_EQ(state, (uint32_t)KL_UTF8_REJECT);
}

UTEST(utf8, surrogate_rejection) {
    uint32_t state = KL_UTF8_ACCEPT;
    /* UTF-8 encoded surrogate U+D800: ED A0 80 */
    const uint8_t data[] = {0xED, 0xA0, 0x80};
    kl_utf8_validate(&state, data, sizeof(data));
    ASSERT_EQ(state, (uint32_t)KL_UTF8_REJECT);
}

/* ═══════════════════════════════════════════════════════════════════
 * Handshake tests
 * ═══════════════════════════════════════════════════════════════════ */

UTEST(handshake, accept_key_rfc_example) {
    /* RFC 6455 Section 4.2.2 example:
     * Key: "dGhlIHNhbXBsZSBub25jZQ=="
     * Accept: "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=" */
    const char *key = "dGhlIHNhbXBsZSBub25jZQ==";
    const char *magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

    char concat[64];
    size_t key_len = strlen(key);
    memcpy(concat, key, key_len);
    memcpy(concat + key_len, magic, 36);

    uint8_t sha1_out[20];
    kl_sha1(concat, key_len + 36, sha1_out);

    char b64_out[32];
    size_t b64_len;
    kl_base64_encode(sha1_out, 20, b64_out, &b64_len);

    ASSERT_STREQ(b64_out, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

/* ═══════════════════════════════════════════════════════════════════
 * Frame parser tests
 * ═══════════════════════════════════════════════════════════════════ */

/* Helper: build a masked client frame */
static size_t build_frame(uint8_t *buf, int fin, int opcode, int masked,
                          const uint8_t mask[4], const void *payload,
                          size_t payload_len) {
    size_t pos = 0;
    buf[pos++] = (uint8_t)((fin ? 0x80 : 0) | (opcode & 0x0F));

    uint8_t mask_bit = masked ? 0x80 : 0;
    if (payload_len < 126) {
        buf[pos++] = mask_bit | (uint8_t)payload_len;
    } else if (payload_len <= 65535) {
        buf[pos++] = mask_bit | 126;
        buf[pos++] = (uint8_t)(payload_len >> 8);
        buf[pos++] = (uint8_t)(payload_len);
    } else {
        buf[pos++] = mask_bit | 127;
        uint64_t plen = (uint64_t)payload_len;
        for (int i = 7; i >= 0; i--)
            buf[pos++] = (uint8_t)(plen >> (i * 8));
    }

    if (masked) {
        memcpy(buf + pos, mask, 4);
        pos += 4;
    }

    /* Copy and mask payload */
    const uint8_t *p = (const uint8_t *)payload;
    for (size_t i = 0; i < payload_len; i++) {
        if (masked)
            buf[pos++] = p[i] ^ mask[i & 3];
        else
            buf[pos++] = p[i];
    }

    return pos;
}

UTEST(frame, small_unmasked) {
    uint8_t buf[64];
    size_t flen = build_frame(buf, 1, 0x1, 0, NULL, (const uint8_t *)"hi", 2);

    KlWsFrameParser fp;
    kl_ws_frame_init(&fp);
    size_t consumed = 0;
    int rc = kl_ws_frame_parse(&fp, buf, flen, &consumed);

    ASSERT_EQ(rc, 1);
    ASSERT_EQ(consumed, flen);
    ASSERT_EQ(fp.fin, 1);
    ASSERT_EQ(fp.opcode, 0x1);
    ASSERT_EQ(fp.masked, 0);
    ASSERT_EQ(fp.payload_len, (size_t)2);
}

UTEST(frame, small_masked) {
    uint8_t mask[] = {0x37, 0xfa, 0x21, 0x3d};
    uint8_t buf[64];
    size_t flen = build_frame(buf, 1, 0x1, 1, mask, (const uint8_t *)"he", 2);

    KlWsFrameParser fp;
    kl_ws_frame_init(&fp);
    size_t consumed = 0;
    int rc = kl_ws_frame_parse(&fp, buf, flen, &consumed);

    ASSERT_EQ(rc, 1);
    ASSERT_EQ(fp.fin, 1);
    ASSERT_EQ(fp.opcode, 0x1);
    ASSERT_EQ(fp.masked, 1);
    ASSERT_EQ(fp.payload_len, (size_t)2);
    ASSERT_TRUE(memcmp(fp.mask_key, mask, 4) == 0);
}

UTEST(frame, medium_length_126) {
    /* 200-byte payload uses 16-bit length encoding */
    uint8_t payload[200];
    memset(payload, 'A', 200);
    uint8_t buf[256];
    size_t flen = build_frame(buf, 1, 0x2, 0, NULL, payload, 200);

    KlWsFrameParser fp;
    kl_ws_frame_init(&fp);
    size_t consumed = 0;
    int rc = kl_ws_frame_parse(&fp, buf, flen, &consumed);

    ASSERT_EQ(rc, 1);
    ASSERT_EQ(fp.payload_len, (size_t)200);
}

UTEST(frame, large_length_127) {
    /* Use 64-bit length encoding for a 70000-byte payload
     * (only parse header, don't actually build full payload) */
    uint8_t buf[14];
    buf[0] = 0x82;  /* FIN=1, opcode=binary */
    buf[1] = 127;   /* 64-bit length, no mask */
    uint64_t plen = 70000;
    for (int i = 0; i < 8; i++)
        buf[2 + i] = (uint8_t)(plen >> ((7 - i) * 8));

    KlWsFrameParser fp;
    kl_ws_frame_init(&fp);
    size_t consumed = 0;
    int rc = kl_ws_frame_parse(&fp, buf, 10, &consumed);

    /* Header parsed but payload not yet consumed; need more data */
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(fp.payload_len, (size_t)70000);
    ASSERT_EQ(fp.state, KL_WS_FRAME_PAYLOAD);
}

UTEST(frame, control_ping) {
    uint8_t buf[32];
    size_t flen = build_frame(buf, 1, 0x9, 0, NULL,
                              (const uint8_t *)"ping", 4);

    KlWsFrameParser fp;
    kl_ws_frame_init(&fp);
    size_t consumed = 0;
    int rc = kl_ws_frame_parse(&fp, buf, flen, &consumed);

    ASSERT_EQ(rc, 1);
    ASSERT_EQ(fp.opcode, 0x9);
    ASSERT_EQ(fp.payload_len, (size_t)4);
}

UTEST(frame, control_close) {
    /* Close frame with code 1000 and reason "Normal" */
    uint8_t payload[8];
    payload[0] = 0x03; payload[1] = 0xE8; /* 1000 */
    memcpy(payload + 2, "Normal", 6);

    uint8_t buf[32];
    size_t flen = build_frame(buf, 1, 0x8, 0, NULL, payload, 8);

    KlWsFrameParser fp;
    kl_ws_frame_init(&fp);
    size_t consumed = 0;
    int rc = kl_ws_frame_parse(&fp, buf, flen, &consumed);

    ASSERT_EQ(rc, 1);
    ASSERT_EQ(fp.opcode, 0x8);
    ASSERT_EQ(fp.payload_len, (size_t)8);
}

UTEST(frame, byte_at_a_time) {
    uint8_t mask[] = {0x12, 0x34, 0x56, 0x78};
    uint8_t buf[64];
    size_t flen = build_frame(buf, 1, 0x1, 1, mask,
                              (const uint8_t *)"hello", 5);

    KlWsFrameParser fp;
    kl_ws_frame_init(&fp);

    size_t total_consumed = 0;
    int rc = 0;
    for (size_t i = 0; i < flen && rc != 1; i++) {
        size_t consumed = 0;
        rc = kl_ws_frame_parse(&fp, buf + i, 1, &consumed);
        ASSERT_GE(rc, -1);
        total_consumed += consumed;
    }

    ASSERT_EQ(rc, 1);
    ASSERT_EQ(total_consumed, flen);
    ASSERT_EQ(fp.payload_len, (size_t)5);
}

UTEST(frame, split_header) {
    /* Feed header in two parts: first byte, then rest */
    uint8_t mask[] = {0xAA, 0xBB, 0xCC, 0xDD};
    uint8_t buf[64];
    size_t flen = build_frame(buf, 1, 0x1, 1, mask,
                              (const uint8_t *)"ab", 2);

    KlWsFrameParser fp;
    kl_ws_frame_init(&fp);

    /* Feed first byte */
    size_t consumed = 0;
    int rc = kl_ws_frame_parse(&fp, buf, 1, &consumed);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(consumed, (size_t)1);

    /* Feed rest */
    consumed = 0;
    rc = kl_ws_frame_parse(&fp, buf + 1, flen - 1, &consumed);
    ASSERT_EQ(rc, 1);
    ASSERT_EQ(consumed, flen - 1);
}

UTEST(frame, rsv_bits_rejection) {
    /* Frame with RSV1 set; must be rejected */
    uint8_t buf[4];
    buf[0] = 0xC1;  /* FIN=1, RSV1=1, opcode=text */
    buf[1] = 0x00;  /* no mask, 0 payload */

    KlWsFrameParser fp;
    kl_ws_frame_init(&fp);
    size_t consumed = 0;
    int rc = kl_ws_frame_parse(&fp, buf, 2, &consumed);

    ASSERT_EQ(rc, -1);
}

UTEST(frame, fragmented_control_rejection) {
    /* Control frame (ping) with FIN=0; must be rejected */
    uint8_t buf[4];
    buf[0] = 0x09;  /* FIN=0, opcode=ping */
    buf[1] = 0x00;

    KlWsFrameParser fp;
    kl_ws_frame_init(&fp);
    size_t consumed = 0;
    int rc = kl_ws_frame_parse(&fp, buf, 2, &consumed);

    ASSERT_EQ(rc, -1);
}

UTEST(frame, oversized_control_rejection) {
    /* Control frame with 126-byte payload; exceeds 125 max */
    uint8_t buf[4];
    buf[0] = 0x89;  /* FIN=1, opcode=ping */
    buf[1] = 126;   /* 16-bit length encoding → > 125 */
    buf[2] = 0x00;
    buf[3] = 0x80;  /* 128 bytes */

    KlWsFrameParser fp;
    kl_ws_frame_init(&fp);
    size_t consumed = 0;
    int rc = kl_ws_frame_parse(&fp, buf, 4, &consumed);

    ASSERT_EQ(rc, -1);
}

UTEST(frame, empty_frame) {
    /* Text frame with no payload */
    uint8_t buf[4];
    buf[0] = 0x81;  /* FIN=1, opcode=text */
    buf[1] = 0x00;  /* no mask, 0 bytes */

    KlWsFrameParser fp;
    kl_ws_frame_init(&fp);
    size_t consumed = 0;
    int rc = kl_ws_frame_parse(&fp, buf, 2, &consumed);

    ASSERT_EQ(rc, 1);
    ASSERT_EQ(fp.payload_len, (size_t)0);
}

/* ═══════════════════════════════════════════════════════════════════
 * Send frame tests (verify frame encoding)
 * ═══════════════════════════════════════════════════════════════════ */

/* We test frame encoding by parsing a server-encoded frame.
 * Server frames are unmasked (per RFC 6455 Section 5.1). */

UTEST(send, text_small) {
    /* Manually build what ws_send_frame would produce for "hi" */
    uint8_t expected[4] = {0x81, 0x02, 'h', 'i'};

    KlWsFrameParser fp;
    kl_ws_frame_init(&fp);
    size_t consumed = 0;
    int rc = kl_ws_frame_parse(&fp, expected, 4, &consumed);

    ASSERT_EQ(rc, 1);
    ASSERT_EQ(fp.opcode, 0x1);  /* text */
    ASSERT_EQ(fp.payload_len, (size_t)2);
    ASSERT_EQ(fp.masked, 0);
}

UTEST(send, binary) {
    uint8_t frame[6] = {0x82, 0x04, 0xDE, 0xAD, 0xBE, 0xEF};

    KlWsFrameParser fp;
    kl_ws_frame_init(&fp);
    size_t consumed = 0;
    int rc = kl_ws_frame_parse(&fp, frame, 6, &consumed);

    ASSERT_EQ(rc, 1);
    ASSERT_EQ(fp.opcode, 0x2);  /* binary */
    ASSERT_EQ(fp.payload_len, (size_t)4);
}

UTEST(send, ping) {
    uint8_t frame[6] = {0x89, 0x04, 'p', 'i', 'n', 'g'};

    KlWsFrameParser fp;
    kl_ws_frame_init(&fp);
    size_t consumed = 0;
    int rc = kl_ws_frame_parse(&fp, frame, 6, &consumed);

    ASSERT_EQ(rc, 1);
    ASSERT_EQ(fp.opcode, 0x9);  /* ping */
    ASSERT_EQ(fp.payload_len, (size_t)4);
}

UTEST(send, length_16bit) {
    /* Server frame with 200-byte payload: 2 header + 2 length + 200 data */
    uint8_t frame[204];
    frame[0] = 0x82;  /* FIN=1, binary */
    frame[1] = 126;
    frame[2] = 0;
    frame[3] = 200;
    memset(frame + 4, 0xAB, 200);

    KlWsFrameParser fp;
    kl_ws_frame_init(&fp);
    size_t consumed = 0;
    int rc = kl_ws_frame_parse(&fp, frame, 204, &consumed);

    ASSERT_EQ(rc, 1);
    ASSERT_EQ(fp.payload_len, (size_t)200);
}

UTEST(send, length_64bit) {
    /* Server frame header for > 65535-byte payload (64-bit length encoding) */
    uint8_t frame[10];
    frame[0] = 0x82;  /* FIN=1, binary */
    frame[1] = 127;   /* 64-bit length, no mask */
    uint64_t plen = 70000;
    for (int i = 0; i < 8; i++)
        frame[2 + i] = (uint8_t)(plen >> ((7 - i) * 8));

    KlWsFrameParser fp;
    kl_ws_frame_init(&fp);
    size_t consumed = 0;
    /* Only parse the header (no payload data provided) */
    int rc = kl_ws_frame_parse(&fp, frame, 10, &consumed);

    /* Header parsed, waiting for payload */
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(fp.payload_len, (size_t)70000);
    ASSERT_EQ(fp.masked, 0);  /* server frames unmasked */
}

UTEST(send, close_with_reason) {
    /* Close frame: code 1000 + reason "bye" */
    uint8_t frame[7];
    frame[0] = 0x88;  /* FIN=1, opcode=close */
    frame[1] = 5;     /* payload: 2 (code) + 3 (reason) */
    frame[2] = 0x03;  /* 1000 >> 8 */
    frame[3] = 0xE8;  /* 1000 & 0xFF */
    frame[4] = 'b'; frame[5] = 'y'; frame[6] = 'e';

    KlWsFrameParser fp;
    kl_ws_frame_init(&fp);
    size_t consumed = 0;
    int rc = kl_ws_frame_parse(&fp, frame, 7, &consumed);

    ASSERT_EQ(rc, 1);
    ASSERT_EQ(fp.opcode, 0x8);
    ASSERT_EQ(fp.payload_len, (size_t)5);
    ASSERT_EQ(fp.masked, 0);

    /* Extract code from payload */
    uint16_t code = (uint16_t)((frame[2] << 8) | frame[3]);
    ASSERT_EQ(code, (uint16_t)1000);
}

UTEST(send, close_empty_no_reason) {
    /* Close frame with code only (no reason string) */
    uint8_t frame[4];
    frame[0] = 0x88;  /* FIN=1, opcode=close */
    frame[1] = 2;     /* payload: just the 2-byte code */
    frame[2] = 0x03;  /* 1000 >> 8 */
    frame[3] = 0xE8;  /* 1000 & 0xFF */

    KlWsFrameParser fp;
    kl_ws_frame_init(&fp);
    size_t consumed = 0;
    int rc = kl_ws_frame_parse(&fp, frame, 4, &consumed);

    ASSERT_EQ(rc, 1);
    ASSERT_EQ(fp.opcode, 0x8);
    ASSERT_EQ(fp.payload_len, (size_t)2);
}

UTEST(send, frame_unmasked) {
    /* Verify server frames do NOT have mask bit set (RFC 6455 Section 5.1) */
    uint8_t frame[6] = {0x81, 0x04, 't', 'e', 's', 't'};

    KlWsFrameParser fp;
    kl_ws_frame_init(&fp);
    size_t consumed = 0;
    int rc = kl_ws_frame_parse(&fp, frame, 6, &consumed);

    ASSERT_EQ(rc, 1);
    ASSERT_EQ(fp.masked, 0);  /* server → client: MUST NOT mask */
    ASSERT_EQ(fp.opcode, 0x1);
    ASSERT_EQ(fp.payload_len, (size_t)4);
}

/* ═══════════════════════════════════════════════════════════════════
 * Config tests
 * ═══════════════════════════════════════════════════════════════════ */

UTEST(ws_config, defaults) {
    KlWsServerConfig cfg;
    kl_ws_server_config_init(&cfg);

    ASSERT_EQ(cfg.max_message_size, (size_t)0);
    ASSERT_EQ(cfg.max_frame_size, (size_t)0);
    ASSERT_EQ(cfg.close_timeout_ms, 0);
    ASSERT_TRUE(cfg.callbacks.on_open == NULL);
    ASSERT_TRUE(cfg.callbacks.on_message == NULL);
    ASSERT_TRUE(cfg.callbacks.on_close == NULL);
    ASSERT_TRUE(cfg.callbacks.on_ping == NULL);
    ASSERT_TRUE(cfg.user_data == NULL);
}

UTEST(ws_config, custom_limits) {
    KlWsServerConfig cfg;
    kl_ws_server_config_init(&cfg);
    cfg.max_message_size = 2048;
    cfg.max_frame_size = 512;
    cfg.close_timeout_ms = 3000;

    ASSERT_EQ(cfg.max_message_size, (size_t)2048);
    ASSERT_EQ(cfg.max_frame_size, (size_t)512);
    ASSERT_EQ(cfg.close_timeout_ms, 3000);
}

/* ═══════════════════════════════════════════════════════════════════
 * Fragmentation tests (using frame parser directly)
 * ═══════════════════════════════════════════════════════════════════ */

UTEST(fragment, two_part_message) {
    /* First fragment: opcode=text, fin=0, payload="hel" */
    uint8_t f1[16];
    size_t f1_len = build_frame(f1, 0, 0x1, 0, NULL,
                                (const uint8_t *)"hel", 3);

    /* Final fragment: opcode=continuation, fin=1, payload="lo" */
    uint8_t f2[16];
    size_t f2_len = build_frame(f2, 1, 0x0, 0, NULL,
                                (const uint8_t *)"lo", 2);

    /* Parse first fragment */
    KlWsFrameParser fp;
    kl_ws_frame_init(&fp);
    size_t consumed = 0;
    int rc = kl_ws_frame_parse(&fp, f1, f1_len, &consumed);
    ASSERT_EQ(rc, 1);
    ASSERT_EQ(fp.opcode, 0x1);
    ASSERT_EQ(fp.fin, 0);
    ASSERT_EQ(fp.payload_len, (size_t)3);

    /* Parse final fragment */
    kl_ws_frame_init(&fp);
    consumed = 0;
    rc = kl_ws_frame_parse(&fp, f2, f2_len, &consumed);
    ASSERT_EQ(rc, 1);
    ASSERT_EQ(fp.opcode, 0x0);
    ASSERT_EQ(fp.fin, 1);
    ASSERT_EQ(fp.payload_len, (size_t)2);
}

UTEST(fragment, three_part_message) {
    uint8_t f1[16], f2[16], f3[16];
    size_t f1_len = build_frame(f1, 0, 0x1, 0, NULL,
                                (const uint8_t *)"A", 1);
    size_t f2_len = build_frame(f2, 0, 0x0, 0, NULL,
                                (const uint8_t *)"B", 1);
    size_t f3_len = build_frame(f3, 1, 0x0, 0, NULL,
                                (const uint8_t *)"C", 1);

    KlWsFrameParser fp;

    kl_ws_frame_init(&fp);
    size_t consumed = 0;
    int rc = kl_ws_frame_parse(&fp, f1, f1_len, &consumed);
    ASSERT_EQ(rc, 1);
    ASSERT_EQ(fp.fin, 0);
    ASSERT_EQ(fp.opcode, 0x1);

    kl_ws_frame_init(&fp);
    consumed = 0;
    rc = kl_ws_frame_parse(&fp, f2, f2_len, &consumed);
    ASSERT_EQ(rc, 1);
    ASSERT_EQ(fp.fin, 0);
    ASSERT_EQ(fp.opcode, 0x0);

    kl_ws_frame_init(&fp);
    consumed = 0;
    rc = kl_ws_frame_parse(&fp, f3, f3_len, &consumed);
    ASSERT_EQ(rc, 1);
    ASSERT_EQ(fp.fin, 1);
    ASSERT_EQ(fp.opcode, 0x0);
}

UTEST(fragment, interleaved_ping) {
    /* Fragment 1 → Ping → Fragment 2 (final) */
    uint8_t f1[16], ping[16], f2[16];
    size_t f1_len = build_frame(f1, 0, 0x1, 0, NULL,
                                (const uint8_t *)"X", 1);
    size_t ping_len = build_frame(ping, 1, 0x9, 0, NULL,
                                  (const uint8_t *)"p", 1);
    size_t f2_len = build_frame(f2, 1, 0x0, 0, NULL,
                                (const uint8_t *)"Y", 1);

    KlWsFrameParser fp;

    /* Parse fragment 1 */
    kl_ws_frame_init(&fp);
    size_t consumed = 0;
    int rc = kl_ws_frame_parse(&fp, f1, f1_len, &consumed);
    ASSERT_EQ(rc, 1);
    ASSERT_EQ(fp.opcode, 0x1);
    ASSERT_EQ(fp.fin, 0);

    /* Parse interleaved ping */
    kl_ws_frame_init(&fp);
    consumed = 0;
    rc = kl_ws_frame_parse(&fp, ping, ping_len, &consumed);
    ASSERT_EQ(rc, 1);
    ASSERT_EQ(fp.opcode, 0x9);

    /* Parse final fragment */
    kl_ws_frame_init(&fp);
    consumed = 0;
    rc = kl_ws_frame_parse(&fp, f2, f2_len, &consumed);
    ASSERT_EQ(rc, 1);
    ASSERT_EQ(fp.opcode, 0x0);
    ASSERT_EQ(fp.fin, 1);
}

/* ═══════════════════════════════════════════════════════════════════
 * Close handshake tests
 * ═══════════════════════════════════════════════════════════════════ */

UTEST(close, frame_with_code_and_reason) {
    /* Build a close frame with code 1000 and reason "bye" */
    uint8_t payload[5] = {0x03, 0xE8, 'b', 'y', 'e'};
    uint8_t buf[16];
    size_t flen = build_frame(buf, 1, 0x8, 0, NULL, payload, 5);

    KlWsFrameParser fp;
    kl_ws_frame_init(&fp);
    size_t consumed = 0;
    int rc = kl_ws_frame_parse(&fp, buf, flen, &consumed);

    ASSERT_EQ(rc, 1);
    ASSERT_EQ(fp.opcode, 0x8);
    ASSERT_EQ(fp.fin, 1);
    ASSERT_EQ(fp.payload_len, (size_t)5);

    /* Verify code extraction */
    size_t payload_start = consumed - fp.payload_len;
    uint16_t code = (uint16_t)((buf[payload_start] << 8) |
                                buf[payload_start + 1]);
    ASSERT_EQ(code, (uint16_t)1000);
}

UTEST(close, empty_close_frame) {
    /* Close frame with no payload; valid per RFC */
    uint8_t buf[4];
    buf[0] = 0x88;  /* FIN=1, opcode=close */
    buf[1] = 0x00;

    KlWsFrameParser fp;
    kl_ws_frame_init(&fp);
    size_t consumed = 0;
    int rc = kl_ws_frame_parse(&fp, buf, 2, &consumed);

    ASSERT_EQ(rc, 1);
    ASSERT_EQ(fp.opcode, 0x8);
    ASSERT_EQ(fp.payload_len, (size_t)0);
}

UTEST(close, close_timeout_check) {
    KlWsServerConfig cfg;
    kl_ws_server_config_init(&cfg);
    cfg.close_timeout_ms = 100;

    KlWsServerConn ws;
    memset(&ws, 0, sizeof(ws));
    ws.config = &cfg;
    ws.close_sent = 1;
    ws.close_received = 0;
    ws.close_deadline_ms = 1000;

    /* Create a minimal KlHttpConn to test kl_ws_server_check_close_timeout */
    KlHttpConn conn;
    memset(&conn, 0, sizeof(conn));
    conn.ws = &ws;

    /* Before deadline */
    ASSERT_EQ(kl_ws_server_check_close_timeout(&conn, 999), 0);

    /* At deadline */
    ASSERT_EQ(kl_ws_server_check_close_timeout(&conn, 1000), 1);

    /* After deadline */
    ASSERT_EQ(kl_ws_server_check_close_timeout(&conn, 1001), 1);

    /* Not timed out if close was received */
    ws.close_received = 1;
    ASSERT_EQ(kl_ws_server_check_close_timeout(&conn, 2000), 0);
}

/* ═══════════════════════════════════════════════════════════════════
 * WsConn cleanup tests
 * ═══════════════════════════════════════════════════════════════════ */

static int cleanup_on_close_called;
static uint16_t cleanup_on_close_code;

static void test_on_close(KlWsServerConn *ws, uint16_t code,
                           const char *reason, size_t reason_len,
                           void *user_data) {
    (void)ws; (void)reason; (void)reason_len; (void)user_data;
    cleanup_on_close_called = 1;
    cleanup_on_close_code = code;
}

UTEST(cleanup, abnormal_closure_calls_on_close) {
    KlAllocator alloc = kl_allocator_default();

    KlWsServerConfig cfg;
    kl_ws_server_config_init(&cfg);
    cfg.callbacks.on_close = test_on_close;

    KlWsServerConn *ws = kl_malloc(&alloc, sizeof(KlWsServerConn));
    memset(ws, 0, sizeof(*ws));
    ws->config = &cfg;
    ws->alloc = &alloc;
    ws->close_received = 0;

    KlHttpConn conn;
    memset(&conn, 0, sizeof(conn));
    conn.stream.fd = -1;
    conn.stream.alloc = &alloc;
    conn.ws = ws;

    cleanup_on_close_called = 0;
    cleanup_on_close_code = 0;

    kl_ws_server_cleanup(&conn);

    ASSERT_EQ(cleanup_on_close_called, 1);
    ASSERT_EQ(cleanup_on_close_code, (uint16_t)1006);
    ASSERT_TRUE(conn.ws == NULL);
}

UTEST(cleanup, no_callback_if_close_received) {
    KlAllocator alloc = kl_allocator_default();

    KlWsServerConfig cfg;
    kl_ws_server_config_init(&cfg);
    cfg.callbacks.on_close = test_on_close;

    KlWsServerConn *ws = kl_malloc(&alloc, sizeof(KlWsServerConn));
    memset(ws, 0, sizeof(*ws));
    ws->config = &cfg;
    ws->alloc = &alloc;
    ws->close_received = 1;  /* Already received close */

    KlHttpConn conn;
    memset(&conn, 0, sizeof(conn));
    conn.stream.fd = -1;
    conn.stream.alloc = &alloc;
    conn.ws = ws;

    cleanup_on_close_called = 0;
    kl_ws_server_cleanup(&conn);

    ASSERT_EQ(cleanup_on_close_called, 0);
    ASSERT_TRUE(conn.ws == NULL);
}

/* ═══════════════════════════════════════════════════════════════════
 * Auto-ping tests
 * ═══════════════════════════════════════════════════════════════════ */

UTEST(auto_ping, disabled_by_default) {
    KlWsServerConfig cfg;
    kl_ws_server_config_init(&cfg);
    ASSERT_EQ(cfg.ping_interval_ms, 0);

    /* A conn with default config should have next_ping_ms = 0 */
    KlWsServerConn ws;
    memset(&ws, 0, sizeof(ws));
    ws.config = &cfg;
    /* next_ping_ms is zero from memset → auto-ping disabled */
    ASSERT_EQ(ws.next_ping_ms, (uint64_t)0);
}

UTEST(auto_ping, sends_ping) {
    /* Use socketpair so we can read the ping frame back */
    int fds[2];
    int rc = kl_test_socketpair(fds);
    ASSERT_EQ(rc, 0);

    KlAllocator alloc = kl_allocator_default();
    KlWsServerConfig cfg;
    kl_ws_server_config_init(&cfg);
    cfg.ping_interval_ms = 100;

    KlWsServerConn *ws = kl_malloc(&alloc, sizeof(KlWsServerConn));
    memset(ws, 0, sizeof(*ws));
    ws->config = &cfg;
    ws->alloc = &alloc;
    ws->next_ping_ms = 1000;  /* deadline at t=1000 */

    KlHttpConn conn;
    memset(&conn, 0, sizeof(conn));
    conn.stream.fd = fds[0];
    conn.stream.alloc = &alloc;
    conn.ws = ws;
    ws->conn = &conn;

    /* Before deadline → no ping */
    ASSERT_EQ(kl_ws_server_auto_ping(&conn, 999), 0);

    /* At deadline → sends ping */
    ASSERT_EQ(kl_ws_server_auto_ping(&conn, 1000), 1);

    /* Read the ping frame from the other end */
    uint8_t buf[16];
    ssize_t nr = kl_test_sockread(fds[1], buf, sizeof(buf));
    ASSERT_EQ(nr, 2);  /* empty ping: 2-byte header */
    ASSERT_EQ(buf[0], 0x89);  /* FIN=1, opcode=ping */
    ASSERT_EQ(buf[1], 0x00);  /* no mask, 0 payload */

    kl_test_closesock(fds[0]);
    kl_test_closesock(fds[1]);
    kl_free(&alloc, ws, sizeof(KlWsServerConn));
}

UTEST(auto_ping, reschedules) {
    KlAllocator alloc = kl_allocator_default();
    KlWsServerConfig cfg;
    kl_ws_server_config_init(&cfg);
    cfg.ping_interval_ms = 200;

    int fds[2];
    ASSERT_EQ(kl_test_socketpair(fds), 0);

    KlWsServerConn *ws = kl_malloc(&alloc, sizeof(KlWsServerConn));
    memset(ws, 0, sizeof(*ws));
    ws->config = &cfg;
    ws->alloc = &alloc;
    ws->next_ping_ms = 1000;

    KlHttpConn conn;
    memset(&conn, 0, sizeof(conn));
    conn.stream.fd = fds[0];
    conn.stream.alloc = &alloc;
    conn.ws = ws;
    ws->conn = &conn;

    kl_ws_server_auto_ping(&conn, 1000);
    /* next_ping_ms should advance by interval */
    ASSERT_EQ(ws->next_ping_ms, (uint64_t)1200);

    /* Drain the frame */
    uint8_t drain[16];
    kl_test_sockread(fds[1], drain, sizeof(drain));

    kl_test_closesock(fds[0]);
    kl_test_closesock(fds[1]);
    kl_free(&alloc, ws, sizeof(KlWsServerConn));
}

UTEST(auto_ping, skips_closing) {
    KlWsServerConfig cfg;
    kl_ws_server_config_init(&cfg);
    cfg.ping_interval_ms = 100;

    KlWsServerConn ws;
    memset(&ws, 0, sizeof(ws));
    ws.config = &cfg;
    ws.next_ping_ms = 1000;
    ws.close_sent = 1;

    KlHttpConn conn;
    memset(&conn, 0, sizeof(conn));
    conn.ws = &ws;

    ASSERT_EQ(kl_ws_server_auto_ping(&conn, 2000), 0);
}

UTEST(auto_ping, not_before_deadline) {
    KlWsServerConfig cfg;
    kl_ws_server_config_init(&cfg);
    cfg.ping_interval_ms = 100;

    KlWsServerConn ws;
    memset(&ws, 0, sizeof(ws));
    ws.config = &cfg;
    ws.next_ping_ms = 1000;

    KlHttpConn conn;
    memset(&conn, 0, sizeof(conn));
    conn.ws = &ws;

    ASSERT_EQ(kl_ws_server_auto_ping(&conn, 500), 0);
}

UTEST(cleanup, rejects_unmasked_client_frame) {
    /* RFC 6455 §5.1: server MUST close on unmasked client frame */
    KlAllocator alloc = kl_allocator_default();

    KlWsServerConfig cfg;
    kl_ws_server_config_init(&cfg);

    int fds[2];
    ASSERT_EQ(kl_test_socketpair(fds), 0);

    KlHttpConn conn;
    memset(&conn, 0, sizeof(conn));
    conn.stream.fd = fds[0];
    conn.stream.alloc = &alloc;

    KlWsServerConn *ws = kl_malloc(&alloc, sizeof(KlWsServerConn));
    memset(ws, 0, sizeof(*ws));
    ws->config = &cfg;
    ws->alloc = &alloc;
    ws->conn = &conn;
    kl_ws_frame_init(&ws->frame);
    conn.ws = ws;

    /* Build an unmasked text frame (mask bit = 0) */
    uint8_t buf[16];
    size_t flen = build_frame(buf, 1, 0x1, 0, NULL,
                              (const uint8_t *)"hi", 2);

    int rc = kl_ws_server_on_readable_data(&conn, buf, flen);
    ASSERT_EQ(rc, KL_HTTP_CONN_CLOSED);

    kl_test_closesock(fds[0]);
    kl_test_closesock(fds[1]);
    kl_free(&alloc, ws, sizeof(KlWsServerConn));
}

UTEST(cleanup, null_ws_is_noop) {
    KlHttpConn conn;
    memset(&conn, 0, sizeof(conn));
    conn.ws = NULL;

    kl_ws_server_cleanup(&conn);  /* Should not crash */
    ASSERT_TRUE(conn.ws == NULL);
}

UTEST_MAIN();
