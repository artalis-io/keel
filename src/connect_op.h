/*
 * connect_op.h — INTERNAL shim. The KlConnectOp contract now lives in the public candidate header
 * <keel/connect.h> (step 6A); the struct layout is in <keel/connect_detail.h>. This shim keeps the
 * historical include path ("connect_op.h") working for src/ and the unit tests, which embed/stack-
 * allocate KlConnectOp and therefore need the detail layout.
 */
#ifndef KEEL_SRC_CONNECT_OP_H
#define KEEL_SRC_CONNECT_OP_H

#include <keel/connect.h>
#include <keel/connect_detail.h>

#endif /* KEEL_SRC_CONNECT_OP_H */
