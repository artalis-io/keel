/*
 * WebSocket overflow boundary tests — the WebSocket-frame slice of the former
 * tests/test_overflow.c (T-split). Exercises SIZE_MAX overflow guards in the frame
 * parser with pathological 64-bit lengths. No real network connections needed.
 */
#include "utest.h"
#include <keel/websocket.h>
#include <stddef.h>
#include <stdint.h>

/* ── WebSocket frame overflow ────────────────────────────────────── */

UTEST(overflow, ws_frame_64bit_msb_set) {
    /* websocket.c:115 — MSB must be 0 per RFC */
    KlWsFrameParser fp;
    kl_ws_frame_init(&fp);

    /* FIN=1 opcode=2 masked=0 len=127 (64-bit)
     * Payload length MSB=1 (0x80...) — should fail */
    uint8_t hdr[10] = {0x82, 0x7f,
                        0x80, 0x00, 0x00, 0x00,
                        0x00, 0x00, 0x00, 0x01};
    size_t consumed = 0;
    int rc = kl_ws_frame_parse(&fp, hdr, sizeof(hdr), &consumed);
    ASSERT_EQ(rc, -1);
}

UTEST(overflow, ws_frame_64bit_all_ff) {
    /* websocket.c:115 — MSB=1 with all 0xFF */
    KlWsFrameParser fp;
    kl_ws_frame_init(&fp);

    uint8_t hdr[10] = {0x82, 0x7f,
                        0xff, 0xff, 0xff, 0xff,
                        0xff, 0xff, 0xff, 0xff};
    size_t consumed = 0;
    int rc = kl_ws_frame_parse(&fp, hdr, sizeof(hdr), &consumed);
    ASSERT_EQ(rc, -1);
}

UTEST(overflow, ws_frame_64bit_size_max_half) {
    /* websocket.c:116 — plen > SIZE_MAX/2 but MSB=0 */
    KlWsFrameParser fp;
    kl_ws_frame_init(&fp);

    /* Craft a 64-bit length just over SIZE_MAX/2 with MSB=0 */
    uint8_t hdr[10] = {0x82, 0x7f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    size_t val = SIZE_MAX / 2 + 1;
    for (int i = 9; i >= 2; i--) {
        hdr[i] = (uint8_t)(val & 0xFF);
        val >>= 8;
    }
    size_t consumed = 0;
    int rc = kl_ws_frame_parse(&fp, hdr, sizeof(hdr), &consumed);
    ASSERT_EQ(rc, -1);
}

UTEST(overflow, ws_frame_64bit_exactly_half) {
    /* websocket.c:116 — plen == SIZE_MAX/2 should succeed (boundary) */
    KlWsFrameParser fp;
    kl_ws_frame_init(&fp);

    uint8_t hdr[10] = {0x82, 0x7f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    size_t val = SIZE_MAX / 2;
    for (int i = 9; i >= 2; i--) {
        hdr[i] = (uint8_t)(val & 0xFF);
        val >>= 8;
    }
    size_t consumed = 0;
    int rc = kl_ws_frame_parse(&fp, hdr, sizeof(hdr), &consumed);
    /* Should succeed (header parsed OK), need more data for payload */
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(fp.payload_len, SIZE_MAX / 2);
}

UTEST(overflow, ws_frame_control_oversized) {
    /* websocket.c:128 — control frames max 125 bytes */
    KlWsFrameParser fp;
    kl_ws_frame_init(&fp);

    /* Ping with payload length 126 (> 125 limit) */
    uint8_t hdr[4] = {0x89, 0x7e, 0x00, 0x7e};  /* FIN=1 PING len=126 */
    size_t consumed = 0;
    int rc = kl_ws_frame_parse(&fp, hdr, sizeof(hdr), &consumed);
    ASSERT_EQ(rc, -1);
}

UTEST_MAIN();
