/*
 * stream_write.h: INTERNAL shim. The KlStream write contract now lives in the public candidate
 * header <keel/stream.h>; the struct layout is in <keel/stream_detail.h>. This shim keeps
 * the historical include path ("stream_write.h") working for src/ and the unit tests.
 */
#ifndef KEEL_SRC_STREAM_WRITE_H
#define KEEL_SRC_STREAM_WRITE_H

#include <keel/stream.h>
#include <keel/stream_detail.h>

/* Cancel a pending late-install writable check (kl_stream_on_writable), if one is scheduled. Called
 * by the close lifecycle when a close begins; internal, not part of the public surface. */
void kl_stream_writable_defer_cancel(KlStream *s);

/* Install the adapter's keep-alive pair (internal; adapters only). The late-install writable check
 * runs from a loop timer, outside any adapter frame, so nothing else keeps the adapter's storage alive
 * while the callback runs: a callback that releases the adapter (kl_pipe_free) would otherwise free
 * the stream under the frame. When set, the deferred fire is bracketed by retain(ctx) before the
 * callback and release(ctx) after the stream's last access, exactly as the adapter's own readiness
 * and completion paths bracket their progress points. Either may be NULL (no adapter lifetime: the
 * callback must not free the stream, as the public contract says). Returns 0, or -1 for NULL s. */
int kl_stream_set_owner_hold(KlStream *s, void (*retain)(void *ctx), void (*release)(void *ctx),
                             void *ctx);

#endif /* KEEL_SRC_STREAM_WRITE_H */
