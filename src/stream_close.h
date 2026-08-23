/*
 * stream_close.h — INTERNAL shim. The KlStream close/detachment contract now lives in the public
 * candidate header <keel/stream.h>; the struct layout is in <keel/stream_detail.h>. This
 * shim keeps the historical include path ("stream_close.h") working for src/ and the unit tests.
 */
#ifndef KEEL_SRC_STREAM_CLOSE_H
#define KEEL_SRC_STREAM_CLOSE_H

#include <keel/stream.h>
#include <keel/stream_detail.h>

#endif /* KEEL_SRC_STREAM_CLOSE_H */
