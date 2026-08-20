/* Fixture for check_readiness_identity.pl — MUST be flagged (bare-KlHttpConn udata). Not compiled. */
void f(void) {
    /* single-line violation */
    kl_event_add(&s->ev.loop, c->stream.fd, KL_EVENT_READ, c);

    /* multiline violation — the udata is on a continuation line */
    kl_event_mod(&s->ev.loop, conn->stream.fd,
                 KL_EVENT_WRITE, conn);

    /* multiline violation with a parenthesized mask arg before the udata */
    kl_event_mod(&s->ev.loop, c->stream.fd,
                 (KlEventMask)c->tls_want, nc);

    /* contract violation under an UNFAMILIAR variable name (a name denylist would miss this) */
    kl_event_add(&s->ev.loop, connection->stream.fd, KL_EVENT_READ, connection);
}
