/*
 * freestanding_headers.c — the freestanding public-header CI gate (step A3).
 *
 * This translation unit #includes the maximal subset of Keel's public headers
 * that a *freestanding* consumer (UEFI / bare-metal / no hosted libc — see
 * docs/phase10_uefi_feasibility_design.md §6, UEFI review finding F-1) must be
 * able to compile with NO POSIX / hosted types leaking in. It is compiled with
 *
 *     cc -ffreestanding -DKEEL_FREESTANDING -Iinclude -Ivendor/llhttp -c ...
 *
 * and the `make freestanding-headers` target additionally proves — by grepping
 * the generated -M dependency list — that none of these headers pull a POSIX
 * socket/system header (sys/socket.h, netinet, arpa, strings.h, sys/types.h,
 * …). If any appears, the gate fails.
 *
 * IN-GATE (this file): the client-facing / protocol-layer headers whose API is
 * already address-ABI-neutral (KlSockAddr) and byte-count-neutral (kl_ssize_t):
 *
 *   error, allocator, handle, sockaddr, socket, event, event_ctx, timer, url,
 *   parser, request, body_reader, chunked, drain, tls, h2, h2_client,
 *   response, file_io
 *
 * response.h / file_io.h joined the gate in the off_t-neutralization step: the
 * file size/offset types (KlHttpResponse.file_size/offset, kl_http_response_file, the
 * KlFileIO submit offset) are now uint64_t — non-negative, same neutral type as
 * the internal sendfile seam (src/socket.h) — so neither header needs the hosted
 * <sys/types.h> off_t any more.
 *
 * The SERVER protocol surface joined the gate in the Phase 10 UEFI *server* step
 * (docs/phase10_uefi_server_design.md, S-1): router, compress, proxy_protocol,
 * h2_server, connection, server. Their peer/PROXY addresses are already the
 * neutral KlSockAddr (kl_http_request_peer_sockaddr, kl_proxy_protocol_parse,
 * kl_cidr_match) and their file size/offset the uint64_t sendfile seam — so a
 * freestanding UEFI server can compile the whole protocol-layer public API with
 * no POSIX socket/type header. (server.h's AF_UNIX / peer-cred references are
 * feature flags + a path string, not a native sockaddr in the ABI.)
 *
 * OUT-OF-GATE (deliberately NOT included here) and WHY:
 *   - resolver.h, udp.h, udp_server.h, client.h, client_pool.h, net.h —
 *     legitimately expose native socket addresses (sockaddr_storage / struct
 *     sockaddr) for EXCLUDED features; their native->KlSockAddr conversion is a
 *     later phase.
 */

#include <keel/error.h>
#include <keel/allocator.h>
#include <keel/handle.h>
#include <keel/sockaddr.h>
#include <keel/socket.h>
#include <keel/event.h>
#include <keel/event_ctx.h>
#include <keel/timer.h>
#include <keel/url.h>
#include <keel/parser.h>
#include <keel/http_request.h>
#include <keel/body_reader.h>
#include <keel/chunked.h>
#include <keel/drain.h>
#include <keel/tls.h>
#include <keel/h2.h>
#include <keel/h2_client.h>
#include <keel/http_response.h>
#include <keel/file_io.h>
/* Server protocol surface (S-1): the whole inbound public API, freestanding-clean. */
#include <keel/router.h>
#include <keel/compress.h>
#include <keel/proxy_protocol.h>
#include <keel/h2_server.h>
#include <keel/connection.h>
#include <keel/server.h>

/* Compile-time assertions that the neutral integer types are the intended
 * pointer-width shapes (and thus need no hosted <sys/types.h>). */
_Static_assert(sizeof(kl_ssize_t) == sizeof(void *),
               "kl_ssize_t must be pointer-width (intptr_t)");
_Static_assert(sizeof(KlSocketHandle) == sizeof(void *),
               "KlSocketHandle must be pointer-width (intptr_t)");

/* Touch a few types so the headers are genuinely parsed, not just preprocessed.
 * No hosted runtime is required — this TU is compiled, not linked. */
static kl_ssize_t kl_freestanding_probe(KlSocketHandle h) {
    KlIoVec v = { (void *)0, 0 };
    (void)v;
    return kl_handle_valid(h) ? (kl_ssize_t)0 : (kl_ssize_t)-1;
}

/* Keep the probe reachable so -Wunused doesn't fire under -Werror. */
kl_ssize_t (*kl_freestanding_probe_ref)(KlSocketHandle) = kl_freestanding_probe;
