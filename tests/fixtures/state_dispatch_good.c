/* Fixture for check-state-dispatch: exhaustive, no default:. Must PASS. */
enum KlHttpConnStateFake { KL_HTTP_CONN_READING, KL_HTTP_CONN_CLOSED };
void good(int st) {
    switch (st) {
    case KL_HTTP_CONN_READING:
        break;
    case KL_HTTP_CONN_CLOSED:
        break;
    }
}
