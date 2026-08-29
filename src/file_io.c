#include <keel/file_io.h>

/* Stub: no async file-I/O backend is built (file responses ride zero-copy splice on the io_uring
 * completion backend). It always returns NULL, so it never allocates and needs no allocator check; a
 * restored backend would validate `alloc` here (see src/allocator_validate.h) before creating one. */
KlFileIO *kl_file_io_create(KlEventLoop *loop, KlAllocator *alloc) {
    (void)loop;
    (void)alloc;
    return NULL;
}
