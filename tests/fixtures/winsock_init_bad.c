/*
 * Fixture for check_winsock_init.pl: an ungated native boundary. NOT COMPILED.
 *
 * No PAL-gate declaration, and ungated_entry() enters ws2_32 directly. This is the exact shape that
 * must not reach main: it compiles, links, and fails only when it happens to run before anything else
 * has opened a socket, reporting WSANOTINITIALISED on an unrelated operation.
 */
int ungated_entry(int fd, void *buf, int len) {
    return recv(fd, buf, len, 0);
}
