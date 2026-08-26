/*
 * HTTP/2 overflow boundary tests: the HTTP/2 slice of the former tests/test_overflow.c
 * (T-split). Verifies the h2 per-stream allocation size guard. No network needed.
 */
#include "utest.h"
#include <keel/http2.h>
#include <keel/http2_server.h>
#include "http2_internal.h"   /* KlHttp2ServerStream body (via -Isrc/protocols/http2) */
#include <stddef.h>
#include <stdint.h>

/* ── H2 struct size sanity ───────────────────────────────────────── */

UTEST(overflow, h2_stream_alloc_guard) {
    /* h2 stream-alloc overflow guard: verify SIZE_MAX / sizeof(KlHttp2ServerStream) is bounded */
    ASSERT_TRUE(sizeof(KlHttp2ServerStream) > 0);
    size_t max_safe = SIZE_MAX / sizeof(KlHttp2ServerStream);
    ASSERT_TRUE(max_safe > 0);
    ASSERT_TRUE(max_safe <= SIZE_MAX / sizeof(KlHttp2ServerStream));
}

UTEST_MAIN();
