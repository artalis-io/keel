/* Fixture for check_readiness_identity.pl: MUST pass (KlStream udata / NULL). Not compiled. */
void f(void) {
    /* single-line, correct */
    kl_event_add(&s->ev.loop, c->stream.fd, KL_EVENT_READ, &c->stream);

    /* multiline, correct */
    kl_event_mod(&s->ev.loop, conn->stream.fd,
                 KL_EVENT_WRITE, &conn->stream);

    /* parenthesized mask arg, correct udata */
    kl_event_mod(&s->ev.loop, c->stream.fd,
                 (KlEventMask)c->tls_want, &c->stream);

    /* listen socket: NULL udata is fine */
    kl_event_add(&s->ev.loop, s->listen_fd, KL_EVENT_READ, NULL);

    /* a legitimately conn-less future event may opt out with an explicit marker */
    kl_event_add(&s->ev.loop, some_fd, KL_EVENT_READ, my_thing /* readiness-identity-exempt */);
}
