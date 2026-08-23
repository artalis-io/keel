/*
 * stream_read.h — INTERNAL shim. The KlStream read contract now lives in the public candidate
 * header <keel/stream.h>; the struct layout is in <keel/stream_detail.h>. This shim keeps
 * the historical include path ("stream_read.h") working for src/ and the unit tests.
 */
#ifndef KEEL_SRC_STREAM_READ_H
#define KEEL_SRC_STREAM_READ_H

#include <keel/stream.h>
#include <keel/stream_detail.h>

#endif /* KEEL_SRC_STREAM_READ_H */
