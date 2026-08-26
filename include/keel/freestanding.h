#ifndef KEEL_FREESTANDING_H
#define KEEL_FREESTANDING_H

/*
 * freestanding.h: the freestanding client-subset umbrella.
 *
 * A single entry point for a *freestanding* consumer (UEFI / bare-metal / no
 * hosted libc; see docs/archive/phases/phase10_uefi_feasibility_design.md) that wants the
 * client + protocol layer without pulling the full <keel/keel.h> umbrella (which
 * drags in the server, UDP, DNS, thread-pool and native-socket surfaces that a
 * freestanding build deliberately EXCLUDES, and (via net.h) the platform
 * socket headers).
 *
 * It #includes EXACTLY the client-facing / protocol-layer public headers proven
 * freestanding-clean by the `make freestanding-headers` gate (the same set as
 * tests/freestanding_headers.c). Every one is address-ABI-neutral (KlSockAddr)
 * and byte-count-neutral (kl_ssize_t / uint64_t offsets), so this header itself
 * compiles with:
 *
 *     cc -ffreestanding -DKEEL_FREESTANDING -Iinclude -Ivendor/llhttp -c ...
 *
 * pulling ZERO POSIX/system headers (enforced by the freestanding-headers gate,
 * which now compiles this umbrella too).
 *
 * The version macros/accessors from <keel/keel.h> are duplicated here so a
 * freestanding consumer that never includes the full umbrella can still query
 * the linked-library version. They are kept in sync with <keel/keel.h>.
 *
 * DELIBERATELY OUT (and why): resolver.h, datagram*.h/socket_dgram.h, http_server.h, http_client_pool.h has a
 * subtlety: http_client_pool.h IS part of the freestanding client archive, but it is
 * NOT in the header gate (it exposes a native socket fd type in its public API),
 * so it is not re-exported here; a freestanding pool consumer includes it
 * directly. http_connection.h / net.h / proxy_protocol.h expose native socket
 * addresses for excluded features and stay out.
 */

/* ── Linked-library version (mirrors <keel/keel.h>; keep in sync) ──────────── */
#ifndef KL_VERSION_STRING
#define KL_VERSION_MAJOR  2
#define KL_VERSION_MINOR  9
#define KL_VERSION_PATCH  0
#define KL_VERSION_STRING "2.9.0"
#define KL_VERSION_NUMBER \
    ((KL_VERSION_MAJOR) * 10000 + (KL_VERSION_MINOR) * 100 + (KL_VERSION_PATCH))
const char *kl_version(void);
int         kl_version_number(void);
#endif

/* ── The freestanding-clean client + protocol header subset ───────────────────
 * Identical to tests/freestanding_headers.c; that gate is the source of truth. */
#include <keel/error.h>
#include <keel/allocator.h>
#include <keel/handle.h>
#include <keel/sockaddr.h>
#include <keel/socket.h>
#include <keel/event.h>
#include <keel/event_ctx.h>
#include <keel/timer.h>
#include <keel/url.h>
#include <keel/http1_parser.h>
#include <keel/http_request.h>
#include <keel/http_body_reader.h>
#include <keel/http1_chunked.h>
#include <keel/drain.h>
#include <keel/tls.h>
#include <keel/http2.h>
#include <keel/http2_client.h>
#include <keel/http_response.h>
#include <keel/file_io.h>

#endif /* KEEL_FREESTANDING_H */
