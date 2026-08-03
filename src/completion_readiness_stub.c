/*
 * completion_readiness_stub.c — the kl_comp_ops_builtin() stub for READINESS builds
 * (epoll / kqueue / poll / WSAPoll). Linked by the Makefile when the selected EVENT_SRC
 * is a readiness backend; a completion backend provides its own kl_comp_ops_builtin().
 *
 * A readiness loop's caps lack KL_EVENT_CAP_COMPLETION, so the server never enters the
 * completion branch and the completion dispatchers (completion_dispatch.c) are never
 * called — hence returning NULL is safe (the NULL is never dereferenced). This exists
 * only so the always-linked completion_dispatch.c resolves its kl_comp_ops_builtin()
 * reference without a platform #ifdef. Mirrors the old io_engine.c stub's role for the
 * completion-primitive axis. See completion.h / event_dispatch.c.
 */
#include "completion.h"

const KlCompletionOps *kl_comp_ops_builtin(void) {
    return NULL;   /* readiness build — never dereferenced (no KL_EVENT_CAP_COMPLETION) */
}
