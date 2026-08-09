/*
 * stream.c — Phase-B KlStream base initializer (step 6A). The facet machinery lives in
 * stream_write.c / stream_read.c / stream_close.c; this owns the whole-object base init that
 * establishes the invariants they build on. See <keel/stream.h>.
 */
#include "stream_write.h"   /* shim → <keel/stream.h> + <keel/stream_detail.h> */
#include <string.h>

int kl_stream_init(KlStream *stream, char *read_buffer, size_t read_capacity) {
    if (!stream || !read_buffer || read_capacity == 0) return -1;
    memset(stream, 0, sizeof(*stream));
    stream->fd       = KL_INVALID_SOCKET;   /* un-configured: no platform handle yet */
    stream->read_buf = read_buffer;         /* caller-owned stable receive buffer */
    stream->read_cap = read_capacity;
    /* All facet state (write/read/close) is zero = uninitialised; the facet _init calls install
     * hooks on top. close_state == 0 == KL_STREAM_STATE_OPEN; on_retire NULL keeps the read/write
     * machinery unaware of a close lifecycle until kl_stream_close_init() runs. */
    return 0;
}
