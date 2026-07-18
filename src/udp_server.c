#include <keel/udp_server.h>

#include <string.h>

/* Trampoline: KlUdp recv callback → user datagram handler. Captures the local
 * (destination) address so kl_udp_server_reply can answer from it. */
static void udp_server_on_recv(KlUdp *udp, const void *data, size_t len,
                               const struct sockaddr *src, socklen_t src_len,
                               const struct sockaddr *local, socklen_t local_len,
                               void *user_data) {
    (void)udp;
    KlUdpServer *s = user_data;
    if (local && local_len && local_len <= sizeof(s->local)) {
        memcpy(&s->local, local, local_len);
        s->local_len = local_len;
    } else {
        s->local_len = 0;
    }
    s->handler(s, data, len, src, src_len, s->user_data);
}

/* A wildcard bind (INADDR_ANY / in6addr_any) is where the reply source is
 * ambiguous on a multi-homed host, so capture the local address there. */
static int udp_addr_is_wildcard(const char *bind_addr) {
    if (!bind_addr)
        return 1;                 /* default "0.0.0.0" */
    return strcmp(bind_addr, "0.0.0.0") == 0 || strcmp(bind_addr, "::") == 0;
}

int kl_udp_server_init(KlUdpServer *s, KlEventCtx *ctx,
                       const KlUdpServerConfig *cfg,
                       KlUdpHandlerFn handler, void *user_data) {
    if (!s)
        return -1;
    memset(s, 0, sizeof(*s));
    if (!ctx || !cfg || !handler) {
        s->last_error = KL_ERR_INVALID_ARG;
        return -1;
    }
    s->handler = handler;
    s->user_data = user_data;

    KlUdpConfig uc = {
        .ctx            = ctx,
        .bind_addr      = cfg->bind_addr ? cfg->bind_addr : "0.0.0.0",
        .bind_port      = cfg->port,
        .recv_buf_size  = cfg->recv_buf_size,
        .max_send_queue = cfg->max_send_queue,
        .reuse_addr     = 1,               /* servers want quick rebind */
        .reuse_port     = cfg->reuse_port,
        .recv_pktinfo   = udp_addr_is_wildcard(cfg->bind_addr),
        .alloc          = cfg->alloc,
    };
    if (kl_udp_init(&s->udp, &uc) != 0) {
        s->last_error = kl_udp_last_error(&s->udp);
        return -1;
    }
    if (kl_udp_recv_start(&s->udp, udp_server_on_recv, s) != 0) {
        s->last_error = kl_udp_last_error(&s->udp);
        kl_udp_free(&s->udp);
        return -1;
    }
    return 0;
}

int kl_udp_server_reply(KlUdpServer *s, const void *data, size_t len,
                        const struct sockaddr *dest, socklen_t dest_len) {
    if (!s)
        return -1;
    /* Reply from the address the client hit (multi-homed correctness). */
    int rc = kl_udp_send_to_from(&s->udp, data, len, dest, dest_len,
                                 s->local_len ? (struct sockaddr *)&s->local : NULL,
                                 s->local_len);
    if (rc != 0)
        s->last_error = kl_udp_last_error(&s->udp);
    return rc;
}

void kl_udp_server_free(KlUdpServer *s) {
    if (!s)
        return;
    kl_udp_free(&s->udp);
    s->handler = NULL;
}

uint16_t kl_udp_server_local_port(const KlUdpServer *s) {
    return s ? kl_udp_local_port(&s->udp) : 0;
}

int kl_udp_server_fd(const KlUdpServer *s) {
    return s ? kl_udp_fd(&s->udp) : -1;
}

KlError kl_udp_server_last_error(const KlUdpServer *s) {
    return s ? s->last_error : KL_ERR_INVALID_ARG;
}
