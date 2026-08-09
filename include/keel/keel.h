/**
 * @file keel.h
 * @brief KEEL — Minimal C11 HTTP client/server library built on epoll/kqueue/io_uring/poll.
 *
 * Umbrella header that includes all public KEEL modules.
 *
 * @defgroup version Version
 * @brief Compile-time version macros.
 * @{
 */

#ifndef KEEL_H
#define KEEL_H

/* ── W^X / no-runtime-codegen invariant ────────────────────────────────
 *
 * Keel is structurally W^X: it contains no JIT, no `dlopen`, no
 * `mmap PROT_EXEC`, no `memfd_create`, no `MAP_JIT`. The HTTP/2,
 * WebSocket, multipart, and URL parsers all operate on heap + stack
 * memory only; libFuzzer targets in `fuzz/` exercise these paths.
 *
 * Keel does not own a process boundary — W^X enforcement at the
 * kernel-sandbox layer (seccomp / Seatbelt / Hardened Runtime) is the
 * host application's responsibility. See SECURITY.md for the host
 * policy Keel composes under.
 *
 * The macros below are reserved opt-in flags. We do not define them;
 * any future configuration that turns one on must clear this guard
 * and intentionally weaken Keel's posture. The build fails until
 * that happens, so the policy violation cannot land silently. */
#if defined(KEEL_ENABLE_JIT)
#error "Keel's W^X policy forbids runtime JIT (KEEL_ENABLE_JIT)."
#endif
#if defined(KEEL_ENABLE_DYNAMIC_CODE)
#error "Keel's W^X policy forbids runtime dynamic code (KEEL_ENABLE_DYNAMIC_CODE)."
#endif
#if defined(KEEL_ENABLE_DLOPEN)
#error "Keel's W^X policy forbids dlopen (KEEL_ENABLE_DLOPEN)."
#endif

/** @brief Major version number. */
#define KL_VERSION_MAJOR  2
/** @brief Minor version number. */
#define KL_VERSION_MINOR  9
/** @brief Patch version number. */
#define KL_VERSION_PATCH  0
/** @brief Version string ("major.minor.patch"). */
#define KL_VERSION_STRING "2.9.0"

/** @brief Packed version: major*10000 + minor*100 + patch (e.g. 20900). Suitable
 *  for `#if KL_VERSION_NUMBER >= 20900` compile-time gating. */
#define KL_VERSION_NUMBER \
    ((KL_VERSION_MAJOR) * 10000 + (KL_VERSION_MINOR) * 100 + (KL_VERSION_PATCH))

/** @brief Version string of the *linked* library ("major.minor.patch").
 *  Compile-time macros describe the headers; this reports the compiled library,
 *  so a consumer that static-relinks a newer Keel can verify it at runtime. */
const char *kl_version(void);

/** @brief Packed version (@ref KL_VERSION_NUMBER) of the *linked* library. */
int kl_version_number(void);

/** @} */

#include <keel/error.h>
#include <keel/allocator.h>
#include <keel/handle.h>
#include <keel/socket.h>
#include <keel/event.h>
#include <keel/event_ctx.h>
#include <keel/request.h>
#include <keel/body_reader.h>
#include <keel/body_reader_multipart.h>
#include <keel/chunked.h>
#include <keel/parser.h>
#include <keel/response.h>
#include <keel/router.h>
#include <keel/tls.h>
#include <keel/h2.h>
#include <keel/h2_server.h>
#include <keel/h2_client.h>
#include <keel/file_io.h>
#include <keel/connection.h>
#include <keel/server.h>
#include <keel/cors.h>
#include <keel/websocket.h>
#include <keel/websocket_server.h>
#include <keel/websocket_client.h>
#include <keel/async.h>
#include <keel/thread_pool.h>
#include <keel/url.h>
#include <keel/resolver.h>
#include <keel/resolver_cache.h>
#include <keel/dns_resolver.h>
#include <keel/client.h>
#include <keel/client_pool.h>
#include <keel/redirect.h>
#include <keel/compress.h>
#include <keel/decompress.h>
#include <keel/drain.h>
#include <keel/udp.h>
#include <keel/udp_server.h>
#include <keel/sse.h>
#include <keel/timer.h>

/* Phase-B transport — EXPERIMENTAL / UNSTABLE candidate contract headers (step 6A). These expose
 * KlStream, KlListener, and KlConnectOp as an independently-usable transport surface; their
 * signatures may change before stabilization (step 6D). The matching per-type detail layout
 * headers (keel/stream_detail.h etc.) are OPT-IN for embedders only and are deliberately NOT
 * included by this umbrella. */
#include <keel/stream.h>
#include <keel/listener.h>
#include <keel/connect.h>

#endif
