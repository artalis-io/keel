/*
 * listener.h — INTERNAL shim. The KlListener contract now lives in the public candidate header
 * <keel/listener.h>; the struct layout is in <keel/listener_detail.h>. This shim keeps
 * the historical include path ("listener.h") working for src/ and the unit tests, which embed/
 * stack-allocate KlListener and therefore need the detail layout.
 */
#ifndef KEEL_SRC_LISTENER_H
#define KEEL_SRC_LISTENER_H

#include <keel/listener.h>
#include <keel/listener_detail.h>

#endif /* KEEL_SRC_LISTENER_H */
