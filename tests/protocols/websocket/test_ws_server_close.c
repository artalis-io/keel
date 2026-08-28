/*
 * test_ws_server_close.c: direct behavioral tests for kl_ws_server_close. The emitted CLOSE frame is
 * captured by routing the connection's writes through a drain with a capturing write_fn (the same
 * white-box path test_drain.c uses for the WS server conn). Covers: the emitted close frame (opcode,
 * length, status code, reason), idempotence (a second close is a no-op that emits nothing), the empty
 * (code 0) payload, and invalid-state handling (NULL conn; sends refused after close).
 */
#include "utest.h"
#include "../../../src/protocols/websocket/websocket_server_internal.h"  /* KlWsServerConn layout */
#include <keel/keel.h>
#include <keel/websocket_server.h>
#include <keel/websocket.h>
#include <string.h>

/* capture the bytes the conn would send */
static unsigned char g_cap[256];
static size_t        g_cap_len;
static kl_ssize_t cap_write(const char *data, size_t len, void *ctx) {
    (void)ctx;
    if (g_cap_len + len <= sizeof(g_cap)) { memcpy(g_cap + g_cap_len, data, len); g_cap_len += len; }
    return (kl_ssize_t)len;   /* accept-all: writes go straight through the drain */
}

static void ws_setup(KlWsServerConn *ws, KlWsServerConfig *cfg, KlAllocator *a) {
    memset(ws, 0, sizeof(*ws));
    ws->alloc = a;
    kl_ws_server_config_init(cfg);
    ws->config = cfg;
    ws->drain_enabled = 1;
    kl_drain_init(&ws->drain, cap_write, NULL, a);
    g_cap_len = 0;
}

/* A close emits a CLOSE control frame carrying the status code + reason. */
UTEST(ws_server_close, emits_close_frame) {
    KlAllocator a = kl_allocator_default();
    KlWsServerConn ws; KlWsServerConfig cfg; ws_setup(&ws, &cfg, &a);

    ASSERT_EQ(kl_ws_server_close(&ws, 1000, "bye", 3), 0);
    ASSERT_EQ(ws.close_sent, 1);
    ASSERT_EQ((int)ws.close_code, 1000);

    /* Server frame, unmasked: 0x88 (FIN|CLOSE), len 5, [0x03,0xE8]=1000, "bye". */
    ASSERT_EQ(g_cap_len, (size_t)7);
    ASSERT_EQ(g_cap[0], (unsigned char)(KL_WS_FIN_BIT | KL_WS_OP_CLOSE));
    ASSERT_EQ(g_cap[1], (unsigned char)5);
    ASSERT_EQ(g_cap[2], (unsigned char)0x03);
    ASSERT_EQ(g_cap[3], (unsigned char)0xE8);
    ASSERT_EQ(memcmp(g_cap + 4, "bye", 3), 0);

    kl_drain_free(&ws.drain);
}

/* A second close is an idempotent no-op: returns 0, emits nothing, code unchanged. */
UTEST(ws_server_close, idempotent) {
    KlAllocator a = kl_allocator_default();
    KlWsServerConn ws; KlWsServerConfig cfg; ws_setup(&ws, &cfg, &a);

    ASSERT_EQ(kl_ws_server_close(&ws, 1000, "x", 1), 0);
    size_t after_first = g_cap_len;
    ASSERT_GT(after_first, (size_t)0);

    ASSERT_EQ(kl_ws_server_close(&ws, 1001, "y", 1), 0);  /* no-op */
    ASSERT_EQ(g_cap_len, after_first);                    /* nothing new emitted */
    ASSERT_EQ((int)ws.close_code, 1000);                  /* first code preserved */
    ASSERT_EQ(ws.close_sent, 1);

    kl_drain_free(&ws.drain);
}

/* Code 0 emits a CLOSE frame with an empty payload. */
UTEST(ws_server_close, code_zero_empty_payload) {
    KlAllocator a = kl_allocator_default();
    KlWsServerConn ws; KlWsServerConfig cfg; ws_setup(&ws, &cfg, &a);

    ASSERT_EQ(kl_ws_server_close(&ws, 0, NULL, 0), 0);
    ASSERT_EQ(g_cap_len, (size_t)2);                      /* header only, zero payload */
    ASSERT_EQ(g_cap[0], (unsigned char)(KL_WS_FIN_BIT | KL_WS_OP_CLOSE));
    ASSERT_EQ(g_cap[1], (unsigned char)0);

    kl_drain_free(&ws.drain);
}

/* Invalid state: NULL conn is rejected; sends after close are refused. */
UTEST(ws_server_close, invalid_state) {
    ASSERT_EQ(kl_ws_server_close(NULL, 1000, NULL, 0), -1);

    KlAllocator a = kl_allocator_default();
    KlWsServerConn ws; KlWsServerConfig cfg; ws_setup(&ws, &cfg, &a);
    ASSERT_EQ(kl_ws_server_close(&ws, 1000, NULL, 0), 0);
    /* after a close, further application sends are refused. */
    ASSERT_EQ(kl_ws_server_send_text(&ws, "hi", 2), -1);
    ASSERT_EQ(kl_ws_server_send_binary(&ws, "hi", 2), -1);
    ASSERT_EQ(kl_ws_server_send_ping(&ws, "hi", 2), -1);
    kl_drain_free(&ws.drain);
}

/* ── server-emitted send frames (kl_ws_server_send_ping / _send_binary) ─────── */

/* A ping emits a PING control frame carrying the payload. */
UTEST(ws_server_send, emits_ping_frame) {
    KlAllocator a = kl_allocator_default();
    KlWsServerConn ws; KlWsServerConfig cfg; ws_setup(&ws, &cfg, &a);

    ASSERT_EQ(kl_ws_server_send_ping(&ws, "pong", 4), 0);
    ASSERT_EQ(g_cap_len, (size_t)6);   /* 2 header + 4 payload */
    ASSERT_EQ(g_cap[0], (unsigned char)(KL_WS_FIN_BIT | KL_WS_OP_PING));
    ASSERT_EQ(g_cap[1], (unsigned char)4);
    ASSERT_EQ(memcmp(g_cap + 2, "pong", 4), 0);

    /* an over-125 control payload is rejected. */
    char big[126]; memset(big, 'x', sizeof(big));
    ASSERT_EQ(kl_ws_server_send_ping(&ws, big, sizeof(big)), -1);

    kl_drain_free(&ws.drain);
}

/* A binary send emits a BINARY frame carrying the payload. */
UTEST(ws_server_send, emits_binary_frame) {
    KlAllocator a = kl_allocator_default();
    KlWsServerConn ws; KlWsServerConfig cfg; ws_setup(&ws, &cfg, &a);

    ASSERT_EQ(kl_ws_server_send_binary(&ws, "\x00\x01\x02", 3), 0);
    ASSERT_EQ(g_cap_len, (size_t)5);   /* 2 header + 3 payload */
    ASSERT_EQ(g_cap[0], (unsigned char)(KL_WS_FIN_BIT | KL_WS_OP_BINARY));
    ASSERT_EQ(g_cap[1], (unsigned char)3);
    ASSERT_EQ(g_cap[2], (unsigned char)0x00);
    ASSERT_EQ(g_cap[4], (unsigned char)0x02);

    kl_drain_free(&ws.drain);
}

UTEST_MAIN();
