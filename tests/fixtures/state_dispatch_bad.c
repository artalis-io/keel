/* Fixture for check-state-dispatch: a default: hides a missing state. Must FAIL. */
enum KlHttpConnStateFake { KL_HTTP_CONN_READING, KL_HTTP_CONN_CLOSED };
void bad(int st) {
    switch (st) {
    case KL_HTTP_CONN_READING:
        break;
    default:
        break;
    }
}
