/*
 * smoke_pollcomp_ws.c — WebSocket-over-completion roundtrip on POSIX (pollcomp).
 *
 * Runtime-tests WebSocket over the completion loop (comp_ws_drive, 8e-1): a KlHttpServer on
 * the pollcomp completion loop with a WS echo route, hit by a hand-rolled raw WS client
 * (HTTP Upgrade handshake + one masked text frame; reads the server's unmasked echo).
 * The completion driver reuses kl_ws_server_on_readable_data verbatim — this test proves
 * that plumbing end to end. No dependency on the async kl_ws_client.
 */
#include <keel/keel.h>
#include "../src/socket.h"     /* internal kl_socket_provider_pollcomp() */

#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define PORT 18098
#define MSG  "hello-websocket-over-completion"

static void nap_ms(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static void ws_echo(KlWsServerConn *ws, const char *data, size_t len,
                    int is_binary, void *ud) {
    (void)is_binary; (void)ud;
    kl_ws_server_send_text(ws, data, len);
}

static KlHttpServer g_srv;
static void *server_thread(void *arg) { (void)arg; kl_http_server_run(&g_srv); return NULL; }

/* Full read of exactly n bytes (blocking, timeout via SO_RCVTIMEO). */
static int read_n(int fd, unsigned char *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, buf + got, n - got);
        if (r <= 0) return 0;
        got += (size_t)r;
    }
    return 1;
}

/* Hand-rolled raw WS client: Upgrade handshake, send one masked text frame, read the
 * server's echo (unmasked). Returns 1 if the echo matches MSG. */
static int ws_roundtrip(void) {
    int cs = socket(AF_INET, SOCK_STREAM, 0);
    if (cs < 0) return 0;
    struct timeval tmo = { 2, 0 };
    setsockopt(cs, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof(tmo));
    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &to.sin_addr);
    if (connect(cs, (struct sockaddr *)&to, sizeof(to)) < 0) { close(cs); return 0; }

    /* RFC 6455 handshake — the canonical example key. */
    const char *hs =
        "GET /ws HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";
    if (write(cs, hs, strlen(hs)) < 0) { close(cs); return 0; }

    /* Read the 101 response up to the CRLFCRLF terminator. */
    char rbuf[512];
    size_t got = 0;
    int have_101 = 0;
    while (got < sizeof(rbuf) - 1) {
        ssize_t r = read(cs, rbuf + got, sizeof(rbuf) - 1 - got);
        if (r <= 0) break;
        got += (size_t)r;
        rbuf[got] = 0;
        if (strstr(rbuf, "\r\n\r\n")) { have_101 = (strstr(rbuf, " 101 ") != NULL); break; }
    }
    if (!have_101) { close(cs); return 0; }

    /* Send a masked text frame (FIN=1, opcode=text). len < 126. */
    size_t mlen = sizeof(MSG) - 1;
    unsigned char frame[8 + sizeof(MSG)];
    frame[0] = 0x81;                         /* FIN | text */
    frame[1] = 0x80 | (unsigned char)mlen;   /* MASK | len */
    unsigned char mask[4] = { 0x12, 0x34, 0x56, 0x78 };
    memcpy(frame + 2, mask, 4);
    for (size_t i = 0; i < mlen; i++)
        frame[6 + i] = (unsigned char)MSG[i] ^ mask[i % 4];
    if (write(cs, frame, 6 + mlen) < 0) { close(cs); return 0; }

    /* Read the echo: server->client frames are unmasked. Expect 0x81, len, payload. */
    unsigned char hdr[2];
    if (!read_n(cs, hdr, 2)) { close(cs); return 0; }
    if (hdr[0] != 0x81 || (size_t)(hdr[1] & 0x7f) != mlen) { close(cs); return 0; }
    unsigned char payload[sizeof(MSG)];
    int ok = read_n(cs, payload, mlen) && memcmp(payload, MSG, mlen) == 0;
    close(cs);
    return ok;
}

int main(void) {
    KlHttpServerConfig cfg = { .port = PORT, .bind_addr = "127.0.0.1",
                     .sockets = kl_socket_provider_pollcomp() };
    if (kl_http_server_init(&g_srv, &cfg) < 0) {
        fprintf(stderr, "smoke-pollcomp-ws: server init failed (err=%d)\n", g_srv.last_error);
        return 1;
    }
    KlWsServerConfig ws_cfg;
    kl_ws_server_config_init(&ws_cfg);
    ws_cfg.callbacks.on_message = ws_echo;
    kl_http_server_ws_upgrade(&g_srv, "/ws", &ws_cfg);

    pthread_t th;
    if (pthread_create(&th, NULL, server_thread, NULL) != 0) {
        kl_http_server_free(&g_srv);
        return 1;
    }

    int ok = 0;
    for (int i = 0; i < 50 && !ok; i++) {
        nap_ms(50);
        ok = ws_roundtrip();
    }

    kl_http_server_stop(&g_srv);
    pthread_join(th, NULL);
    kl_http_server_free(&g_srv);

    if (!ok) {
        fprintf(stderr, "smoke-pollcomp-ws: WebSocket echo roundtrip FAILED\n");
        return 1;
    }
    printf("smoke-pollcomp-ws: WebSocket-over-completion echo roundtrip OK\n");
    return 0;
}
