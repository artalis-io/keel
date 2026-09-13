/*
 * Fixture for check_winsock_init.pl: every native boundary states the PAL invariant. NOT COMPILED.
 *
 * PAL-gate: dominated-by gated_entry
 * Exercises both accepted forms, plus the two ways a violation can hide: an API name inside a string
 * literal and one inside a comment, neither of which is a call.
 */
int gated_entry(int fd) {
    if (kl_plat_socket_runtime_init() != 0) return -1;
    return listen(fd, 1);
}

/* Dominated by gated_entry, declared in the header above. */
static int helper(int fd) {
    return setsockopt(fd, 0, 0, 0, 0);
}

/* Its own gate, even though a dominator is declared: always acceptable. */
int independent(int fd) {
    if (kl_plat_socket_runtime_init() != 0) return -1;
    return WSAPoll(fd, 1, 0);
}

/* No ws2_32 call at all: inet_pton and htons were both confirmed not to need the runtime. */
int presentation_only(const char *s, void *out) {
    return inet_pton(2, s, out) + htons(80);
}

/* Neither of these is a call. */
const char *prose(void) {
    /* We used to call accept() here. */
    return "accept(";
}
