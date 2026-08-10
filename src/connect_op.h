/*
 * connect_op.h — INTERNAL shim. The KlConnectOp contract is the public header <keel/connect_op.h>;
 * the struct layout is in <keel/connect_op_detail.h>. This shim keeps the include path
 * ("connect_op.h") working for src/ and the unit tests, which embed/stack-allocate KlConnectOp and
 * therefore need the detail layout.
 */
#ifndef KEEL_SRC_CONNECT_OP_H
#define KEEL_SRC_CONNECT_OP_H

#include <keel/connect_op.h>
#include <keel/connect_op_detail.h>

#endif /* KEEL_SRC_CONNECT_OP_H */
