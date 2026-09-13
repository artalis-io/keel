/*
 * platform_socket_posix.c: the POSIX half of the PAL socket-runtime seam (see platform_socket.h).
 *
 * POSIX has no socket runtime to start: socket(2) and its relatives are usable from the first
 * instruction of main. Both operations are therefore successful no-ops, and no lifecycle machinery is
 * invented to mirror a problem this platform does not have.
 *
 * The TU exists so the declarations in platform_socket.h mean the same thing everywhere, which is what
 * lets a SHARED TU state the invariant without a platform branch. resolve_sync.c is the one that does
 * today: it calls getaddrinfo(), which needs ws2_32 up on Windows and needs nothing here.
 */
#include "platform_socket.h"

int kl_plat_socket_runtime_init(void) { return 0; }

/* Always "attempted and succeeded": there is nothing to attempt, so reporting "not yet" would be a
 * lie that the shared assertions in the test suite would have to special-case. */
int kl_plat_socket_runtime_status(void) { return 1; }
