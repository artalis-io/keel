#ifndef KEEL_SRC_UNIX_SOCKET_NODE_H
#define KEEL_SRC_UNIX_SOCKET_NODE_H

/*
 * unix_socket_node.h: substrate contract for the AF_UNIX listener's FILESYSTEM-NODE lifecycle.
 *
 * This is transport/socket-axis work (trusted-directory resolution, stale-node reclamation,
 * ownership/mode mutation, teardown) that must NOT live in a protocol translation unit. The
 * interface is HTTP-neutral and cross-platform: it names no protocol type, includes no protocol
 * header, and carries no POSIX types. The hardened implementation lives per platform
 * (unix_socket_node_posix.c; a Windows counterpart is the future home for that spike), all behind
 * this one contract. See docs/archive/designs/unix_socket_cleanup_security_design.md.
 */

#include <stddef.h>
#include <keel/handle.h>      /* KlSocketHandle */
#include <keel/allocator.h>   /* KlAllocator (transient name-resolution buffers) */

struct KlSocketProvider;      /* socket seam; the node module binds through it */

/* Bytes the caller reserves (aligned to a max-align type) for the opaque per-bind lifecycle state.
 * The concrete state struct is defined only in the platform implementation, which asserts its
 * sizeof against this bound. Callers treat the storage as opaque and pass its address. */
#define KL_UNIX_NODE_STORAGE 192

/* HTTP-neutral, cross-platform outcome. The caller maps these to its own error taxonomy + logging;
 * this module never logs and never touches caller state. */
typedef enum {
    KL_UNIX_NODE_OK = 0,
    KL_UNIX_NODE_ERR_INVALID_PATH,      /* empty / overlong / trailing-slash / "."/".." / ".." component */
    KL_UNIX_NODE_ERR_UNKNOWN_OWNER,     /* owner or group name did not resolve */
    KL_UNIX_NODE_ERR_UNTRUSTED_PARENT,  /* fail closed: parent directory outside the trust boundary */
    KL_UNIX_NODE_ERR_FOREIGN_NODE,      /* fail closed: existing node not a socket owned by an accepted uid */
    KL_UNIX_NODE_ERR_SOCKET,            /* socket() / provider setup failed (see out_errno) */
    KL_UNIX_NODE_ERR_BIND,              /* bind / reclaim / chown / chmod syscall failed (see out_errno) */
    KL_UNIX_NODE_ERR_NOMEM,             /* transient allocation failure during name resolution */
    KL_UNIX_NODE_ERR_UNSUPPORTED        /* fail closed: platform/filesystem/API cannot provide the
                                         * identity-anchored guarantee (e.g. Windows on a non-NTFS
                                         * volume, or missing FileIdInfo / reparse / POSIX disposition) */
} KlUnixNodeStatus;

/* Neutral policy: what the caller wants done with the AF_UNIX filesystem node. No POSIX or protocol
 * types. Owner/group are NAMES, resolved inside the platform implementation (so name resolution does
 * not leak into the protocol layer, and no uid_t/gid_t enters this cross-platform header). */
typedef struct {
    const char  *path;          /* AF_UNIX path (required) */
    int          unlink_stale;  /* reclaim a stale, owned socket node before bind + remove on teardown */
    const char  *owner;         /* chown the node to this user name; NULL = leave */
    const char  *group;         /* chown the node to this group name; NULL = leave */
    unsigned int mode;          /* chmod bits, applied when set_mode */
    int          set_mode;      /* apply mode */
} KlUnixNodePolicy;

/* Initialize the caller's reserved KL_UNIX_NODE_STORAGE bytes. Optional: a zero-filled buffer
 * (e.g. from memset/calloc) is already a valid "not open" state, so callers that zero their storage
 * need not call this. Provided for explicit initialization. */
void kl_unix_socket_node_init(void *state);

/* Bind an AF_UNIX listener at policy->path through @sockets, applying the hardened trust-boundary
 * lifecycle: a component-walked, O_NOFOLLOW-opened parent directory descriptor is held in @state for
 * the socket's lifetime; a stale node is reclaimed only when it validates as a socket owned by an
 * accepted uid; owner/mode are applied via directory-relative operations with identity
 * revalidation; the bound node's device/inode are captured for teardown. On KL_UNIX_NODE_OK,
 * *out_fd is the listening handle and @state holds the teardown state. On failure it returns a
 * status, mutates nothing that was not ours, and leaves @state closed; when non-NULL, *out_errno
 * receives errno for the ERR_SOCKET / ERR_BIND syscall cases (0 otherwise). @alloc backs only
 * transient name-resolution buffers. */
KlUnixNodeStatus kl_unix_socket_node_bind(const KlUnixNodePolicy *policy,
                                          const struct KlSocketProvider *sockets,
                                          KlAllocator *alloc, void *state,
                                          KlSocketHandle *out_fd, int *out_errno);

/* Teardown: when @unlink_stale and the node is still exactly the one we bound (a socket with an
 * accepted owner and matching device/inode under a still-trusted parent), remove it; otherwise
 * leave the ambiguous entry (fail closed). Always closes the held directory descriptor exactly once.
 * Idempotent; safe to call once after a successful OR failed bind. Returns the teardown decision for
 * optional caller logging (OK = removed or nothing to do; a fail-closed status otherwise). */
KlUnixNodeStatus kl_unix_socket_node_teardown(void *state, int unlink_stale);

#endif /* KEEL_SRC_UNIX_SOCKET_NODE_H */
