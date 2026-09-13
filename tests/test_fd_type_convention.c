/*
 * test_fd_type_convention.c: a FILE descriptor is an int; a SOCKET handle is a KlSocketHandle.
 *
 * Keel's public surface distinguishes the two deliberately, and says so: KlSocketOps.sendfile takes
 * `int in_fd` beside `KlSocketHandle out_fd`, and KlFileIO.submit takes `int file_fd` beside
 * `KlSocketHandle sock_fd`. The distinction is not cosmetic. On 64-bit Windows a socket handle is
 * pointer-width and a file descriptor is a small int, so conflating them narrows silently.
 *
 * WHY THIS EXISTS AS A TEST. The convention was broken once, mechanically: commit c5a852e ("retype the
 * public socket-handle API surface to KlSocketHandle") swept up kl_http_response_file's FILE descriptor
 * along with the socket handles, and it stayed wrong from then until native MSVC diagnosed the
 * narrowing four years later. Nothing in the tree objected, because nothing was watching. A sweep like
 * that is exactly the kind of change that will happen again.
 *
 * The pins below are function-pointer initialisations with the exact expected signature. Assigning a
 * function to an incompatible function pointer is a constraint violation in C, so a retype of any of
 * these parameters fails the BUILD rather than a runtime assertion -- which is the right moment to
 * learn, since the damage is a silently narrowed descriptor and not a wrong answer.
 */
#include "utest.h"
#include <keel/http_response.h>
#include <keel/socket.h>
#include <keel/file_io.h>
#include <keel/handle.h>

/* The one that was wrong. */
static void (*const pin_response_file)(KlHttpResponse *, int, uint64_t) = kl_http_response_file;

/* Its siblings, pinned in the same place so the CONVENTION is guarded and not just the one bug. Both
 * carry a file descriptor and a socket handle in one signature, which is what makes them the clearest
 * statement of the rule anywhere in the tree. */
typedef kl_ssize_t (*PinSendfile)(void *, KlSocketHandle, int, uint64_t *, size_t);
typedef int (*PinFileSubmit)(KlFileIO *, int, void *, size_t, uint64_t, KlSocketHandle, void *);

UTEST(fd_type_convention, response_file_takes_an_int_file_descriptor) {
    /* The pin above is the real check and happens at compile time; this keeps the symbol referenced
     * and documents what the failure would look like. */
    ASSERT_TRUE(pin_response_file == kl_http_response_file);
    ASSERT_EQ(sizeof(int), sizeof(((KlHttpResponse *)0)->file_fd));
}

UTEST(fd_type_convention, the_seam_vtables_keep_fd_and_handle_distinct) {
    /* Assigning the real vtable members to the pinned types is the check. */
    KlSocketOps ops;
    KlFileIO fio;
    memset(&ops, 0, sizeof(ops));
    memset(&fio, 0, sizeof(fio));
    PinSendfile s = ops.sendfile;
    PinFileSubmit f = fio.submit;
    ASSERT_TRUE(s == NULL);   /* zeroed above; the types having matched is the assertion */
    ASSERT_TRUE(f == NULL);
}

/* A socket handle must stay wide enough for a Windows SOCKET, and a file descriptor must stay an int.
 * If these ever became the same width the pins above would still pass while the distinction had
 * quietly stopped meaning anything, so the widths are asserted separately. */
UTEST(fd_type_convention, the_two_types_are_not_interchangeable_by_accident) {
    ASSERT_EQ(sizeof(void *), sizeof(KlSocketHandle));   /* pointer-width, per handle.h */
    ASSERT_TRUE(sizeof(KlSocketHandle) >= sizeof(int));
}

UTEST_MAIN();
