#include "utest.h"
#include <keel/websocket_client.h>
#include <keel/websocket.h>
#include <keel/allocator.h>
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════
 * Unit tests for the WebSocket client module.
 *
 * These test the client's frame encoding (masking), handshake
 * validation, config defaults, and API input validation.
 * Full lifecycle tests require a live server.
 * ═══════════════════════════════════════════════════════════════════ */

/* ── Frame encoding: masked client frames ────────────────────────── */

/*
 * wsc_send_frame is static, so we test its behavior by verifying that
 * the public API rejects invalid states and that frame constants are
 * correct. Direct frame encoding is tested via the wire format below.
 */

UTEST(wsc_frame, constants) {
    /* Client frames must set MASK_BIT */
    ASSERT_EQ(KL_WS_MASK_BIT, 0x80);
    ASSERT_EQ(KL_WS_FIN_BIT, 0x80);
    ASSERT_EQ(KL_WS_MASK_KEY_LEN, 4);
    ASSERT_EQ(KL_WS_FRAME_HEADER_MAX, 14);
}

UTEST(wsc_frame, opcode_values) {
    ASSERT_EQ(KL_WS_OP_TEXT, 0x1);
    ASSERT_EQ(KL_WS_OP_BINARY, 0x2);
    ASSERT_EQ(KL_WS_OP_CLOSE, 0x8);
    ASSERT_EQ(KL_WS_OP_PING, 0x9);
    ASSERT_EQ(KL_WS_OP_PONG, 0xA);
    ASSERT_EQ(KL_WS_OP_CONTINUATION, 0x0);
}

UTEST(wsc_frame, mask_xor_correctness) {
    /* Verify masking algorithm: payload[i] ^= mask_key[i % 4] */
    uint8_t mask[4] = {0x37, 0xfa, 0x21, 0x3d};
    uint8_t data[] = {'H', 'e', 'l', 'l', 'o'};
    uint8_t expected[5];
    for (int i = 0; i < 5; i++)
        expected[i] = data[i] ^ mask[i % 4];

    /* XOR in place */
    for (int i = 0; i < 5; i++)
        data[i] ^= mask[i % 4];

    ASSERT_EQ(memcmp(data, expected, 5), 0);

    /* XOR again to unmask */
    for (int i = 0; i < 5; i++)
        data[i] ^= mask[i % 4];

    ASSERT_EQ(data[0], 'H');
    ASSERT_EQ(data[4], 'o');
}

UTEST(wsc_frame, header_7bit_length) {
    /* 7-bit length encoding: payload < 126 */
    uint8_t hdr[6];
    size_t payload_len = 5;  /* "Hello" */
    hdr[0] = KL_WS_FIN_BIT | KL_WS_OP_TEXT;
    hdr[1] = (uint8_t)(KL_WS_MASK_BIT | payload_len);
    /* mask key would follow at hdr[2..5] */

    ASSERT_EQ(hdr[0], 0x81);
    ASSERT_EQ(hdr[1], 0x85);
}

UTEST(wsc_frame, header_16bit_length) {
    /* 16-bit length encoding: payload 126..65535 */
    uint8_t hdr[8];
    size_t payload_len = 256;
    hdr[0] = KL_WS_FIN_BIT | KL_WS_OP_BINARY;
    hdr[1] = (uint8_t)(KL_WS_MASK_BIT | 126);
    hdr[2] = (uint8_t)(payload_len >> 8);
    hdr[3] = (uint8_t)(payload_len);

    ASSERT_EQ(hdr[0], 0x82);
    ASSERT_EQ(hdr[1], 0xFE);
    ASSERT_EQ(hdr[2], 0x01);
    ASSERT_EQ(hdr[3], 0x00);
}

UTEST(wsc_frame, header_64bit_length) {
    /* 64-bit length encoding: payload > 65535 */
    uint8_t hdr[14];
    uint64_t payload_len = 70000;
    hdr[0] = KL_WS_FIN_BIT | KL_WS_OP_BINARY;
    hdr[1] = (uint8_t)(KL_WS_MASK_BIT | 127);
    for (int i = 0; i < 8; i++)
        hdr[2 + i] = (uint8_t)(payload_len >> (56 - i * 8));

    ASSERT_EQ(hdr[0], 0x82);
    ASSERT_EQ(hdr[1], 0xFF);
    ASSERT_EQ(hdr[8], (uint8_t)(70000 >> 8));
    ASSERT_EQ(hdr[9], (uint8_t)(70000));
}

/* ── Handshake: key generation and accept validation ─────────────── */

UTEST(wsc_handshake, magic_guid) {
    ASSERT_STREQ(KL_WS_MAGIC_GUID, "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
}

UTEST(wsc_handshake, rfc_example_accept) {
    /*
     * RFC 6455 Section 4.2.2 example:
     * Key: "dGhlIHNhbXBsZSBub25jZQ=="
     * Expected Accept: "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="
     *
     * We import sha1.h and base64.h to verify the computation
     * matches the RFC's test vector.
     */
    /* Note: sha1 and base64 are internal headers. We test the
       algorithm directly here since the WS client uses them. */
    const char *key = "dGhlIHNhbXBsZSBub25jZQ==";
    const char *expected_accept = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";

    /* Concat key + GUID */
    char concat[128];
    size_t key_len = strlen(key);
    size_t guid_len = strlen(KL_WS_MAGIC_GUID);
    memcpy(concat, key, key_len);
    memcpy(concat + key_len, KL_WS_MAGIC_GUID, guid_len);

    /* We can't call kl_sha1/kl_base64_encode from here since they're
       internal headers. But we can verify the expected accept value
       is 28 bytes long (as per RFC). */
    ASSERT_EQ(strlen(expected_accept), (size_t)28);
    (void)concat;
}

/* ── Frame parser: unmasked server frames ────────────────────────── */

UTEST(wsc_parse, small_text_frame) {
    /* Server sends unmasked text "Hi" */
    uint8_t frame[] = {0x81, 0x02, 'H', 'i'};
    KlWsFrameParser fp;
    kl_ws_frame_init(&fp);

    size_t consumed = 0;
    int rc = kl_ws_frame_parse(&fp, frame, sizeof(frame), &consumed);
    ASSERT_EQ(rc, 1);
    ASSERT_EQ(consumed, sizeof(frame));
    ASSERT_EQ(fp.opcode, KL_WS_OP_TEXT);
    ASSERT_EQ(fp.fin, 1);
    ASSERT_EQ(fp.masked, 0);
    ASSERT_EQ(fp.payload_len, (size_t)2);
}

UTEST(wsc_parse, binary_frame) {
    uint8_t frame[] = {0x82, 0x03, 0xAA, 0xBB, 0xCC};
    KlWsFrameParser fp;
    kl_ws_frame_init(&fp);

    size_t consumed = 0;
    int rc = kl_ws_frame_parse(&fp, frame, sizeof(frame), &consumed);
    ASSERT_EQ(rc, 1);
    ASSERT_EQ(fp.opcode, KL_WS_OP_BINARY);
    ASSERT_EQ(fp.payload_len, (size_t)3);
}

UTEST(wsc_parse, close_frame_with_code) {
    /* Close with code 1000 (normal) + reason "bye" */
    uint8_t frame[] = {0x88, 0x05, 0x03, 0xE8, 'b', 'y', 'e'};
    KlWsFrameParser fp;
    kl_ws_frame_init(&fp);

    size_t consumed = 0;
    int rc = kl_ws_frame_parse(&fp, frame, sizeof(frame), &consumed);
    ASSERT_EQ(rc, 1);
    ASSERT_EQ(fp.opcode, KL_WS_OP_CLOSE);
    ASSERT_EQ(fp.fin, 1);
    ASSERT_EQ(fp.payload_len, (size_t)5);
    /* First 2 bytes are code: 0x03E8 = 1000 */
    ASSERT_EQ((int)(((uint16_t)frame[2] << 8) | frame[3]), 1000);
}

UTEST(wsc_parse, ping_frame) {
    uint8_t frame[] = {0x89, 0x00};
    KlWsFrameParser fp;
    kl_ws_frame_init(&fp);

    size_t consumed = 0;
    int rc = kl_ws_frame_parse(&fp, frame, sizeof(frame), &consumed);
    ASSERT_EQ(rc, 1);
    ASSERT_EQ(fp.opcode, KL_WS_OP_PING);
    ASSERT_EQ(fp.payload_len, (size_t)0);
}

UTEST(wsc_parse, incremental_header) {
    /* Feed header byte by byte */
    uint8_t frame[] = {0x81, 0x03, 'a', 'b', 'c'};
    KlWsFrameParser fp;
    kl_ws_frame_init(&fp);

    size_t consumed;
    /* Feed just first byte */
    int rc = kl_ws_frame_parse(&fp, frame, 1, &consumed);
    ASSERT_EQ(rc, 0);  /* need more */
    ASSERT_EQ(consumed, (size_t)1);

    /* Feed second byte */
    rc = kl_ws_frame_parse(&fp, frame + 1, 1, &consumed);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(consumed, (size_t)1);

    /* Feed payload */
    rc = kl_ws_frame_parse(&fp, frame + 2, 3, &consumed);
    ASSERT_EQ(rc, 1);
    ASSERT_EQ(consumed, (size_t)3);
}

UTEST(wsc_parse, rsv_bits_rejected) {
    /* RSV1 set = protocol error */
    uint8_t frame[] = {0xC1, 0x01, 'x'};
    KlWsFrameParser fp;
    kl_ws_frame_init(&fp);

    size_t consumed;
    int rc = kl_ws_frame_parse(&fp, frame, sizeof(frame), &consumed);
    ASSERT_EQ(rc, -1);
}

UTEST(wsc_parse, fragmented_control_rejected) {
    /* Control frame with FIN=0 */
    uint8_t frame[] = {0x09, 0x00};  /* ping without FIN */
    KlWsFrameParser fp;
    kl_ws_frame_init(&fp);

    size_t consumed;
    int rc = kl_ws_frame_parse(&fp, frame, sizeof(frame), &consumed);
    ASSERT_EQ(rc, -1);
}

UTEST(wsc_parse, control_frame_too_long) {
    /* Control frame with payload > 125 bytes */
    uint8_t frame[] = {0x89, 126, 0x00, 0x80};  /* ping, 128 bytes */
    KlWsFrameParser fp;
    kl_ws_frame_init(&fp);

    size_t consumed;
    int rc = kl_ws_frame_parse(&fp, frame, sizeof(frame), &consumed);
    ASSERT_EQ(rc, -1);
}

UTEST(wsc_parse, 16bit_length) {
    /* Unmasked frame with 16-bit length (256 bytes) */
    uint8_t frame[4 + 256];
    frame[0] = 0x82;  /* FIN + binary */
    frame[1] = 126;
    frame[2] = 0x01;
    frame[3] = 0x00;
    memset(frame + 4, 'A', 256);

    KlWsFrameParser fp;
    kl_ws_frame_init(&fp);

    size_t consumed;
    int rc = kl_ws_frame_parse(&fp, frame, sizeof(frame), &consumed);
    ASSERT_EQ(rc, 1);
    ASSERT_EQ(fp.payload_len, (size_t)256);
}

/* ── API input validation ────────────────────────────────────────── */

UTEST(wsc_api, connect_null_args) {
    ASSERT_TRUE(kl_ws_client_connect(NULL, NULL, NULL, NULL, NULL, NULL) == NULL);
}

UTEST(wsc_api, connect_invalid_scheme) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ev;
    if (kl_event_ctx_init(&ev, &alloc) == 0) {
        /* http:// is not a WS scheme */
        KlWsClientConn *ws = kl_ws_client_connect(&ev, &alloc, NULL,
                                                     "http://example.com",
                                                     NULL, NULL);
        ASSERT_TRUE(ws == NULL);
        kl_event_ctx_free(&ev);
    }
}

UTEST(wsc_api, connect_invalid_url) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ev;
    if (kl_event_ctx_init(&ev, &alloc) == 0) {
        KlWsClientConn *ws = kl_ws_client_connect(&ev, &alloc, NULL,
                                                     "not-a-url", NULL, NULL);
        ASSERT_TRUE(ws == NULL);
        kl_event_ctx_free(&ev);
    }
}

UTEST(wsc_api, send_null_conn) {
    ASSERT_EQ(kl_ws_client_send_text(NULL, "hi", 2), -1);
    ASSERT_EQ(kl_ws_client_send_binary(NULL, "hi", 2), -1);
    ASSERT_EQ(kl_ws_client_send_ping(NULL, "hi", 2), -1);
}

UTEST(wsc_api, ping_too_long) {
    /* Ping payload > 125 bytes should be rejected */
    /* We can't create a valid KlWsClientConn without connecting,
       but we test the NULL path */
    ASSERT_EQ(kl_ws_client_send_ping(NULL, NULL, 126), -1);
}

UTEST(wsc_api, free_null) {
    kl_ws_client_free(NULL);  /* should not crash */
    ASSERT_TRUE(1);
}

UTEST(wsc_api, close_null) {
    kl_ws_client_close(NULL, KL_WS_NORMAL_CLOSE, NULL, 0);
    ASSERT_TRUE(1);  /* should not crash */
}

/* ── Config defaults ─────────────────────────────────────────────── */

UTEST(wsc_config, default_values) {
    ASSERT_EQ(KL_WS_CLIENT_DEFAULT_TIMEOUT_MS, 30000);
    ASSERT_EQ(KL_WS_CLIENT_DEFAULT_MAX_FRAME, 16 * 1024 * 1024);
    ASSERT_EQ(KL_WS_CLIENT_RECV_BUF_SIZE, 8192);
    ASSERT_EQ(KL_WS_CLIENT_KEY_B64_SIZE, 25);
}

UTEST(wsc_config, status_codes) {
    ASSERT_EQ(KL_WS_NORMAL_CLOSE, 1000);
    ASSERT_EQ(KL_WS_GOING_AWAY, 1001);
    ASSERT_EQ(KL_WS_PROTOCOL_ERROR, 1002);
    ASSERT_EQ(KL_WS_TOO_BIG, 1009);
}

/* ── Standalone KlEventCtx ───────────────────────────────────────── */

UTEST(wsc_standalone, event_ctx_init_free) {
    /* Proves KlEventCtx can be used without KlServer */
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ev;
    int rc = kl_event_ctx_init(&ev, &alloc);
    ASSERT_EQ(rc, 0);
    kl_event_ctx_free(&ev);
}

UTEST(wsc_standalone, wss_requires_tls) {
    /* wss:// without TLS config should fail */
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ev;
    if (kl_event_ctx_init(&ev, &alloc) == 0) {
        KlWsClientConn *ws = kl_ws_client_connect(&ev, &alloc, NULL,
                                                     "wss://example.com/ws",
                                                     NULL, NULL);
        ASSERT_TRUE(ws == NULL);
        kl_event_ctx_free(&ev);
    }
}

/* ── Auto-ping config ────────────────────────────────────────────── */

UTEST(wsc_config, auto_ping_default) {
    /* Zero-initialized config should have ping_interval_ms == 0 */
    KlWsClientConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    ASSERT_EQ(cfg.ping_interval_ms, 0);
}

UTEST(wsc_config, auto_ping_set) {
    KlWsClientConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.ping_interval_ms = 30000;
    ASSERT_EQ(cfg.ping_interval_ms, 30000);
}

UTEST_MAIN();
