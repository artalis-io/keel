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

/** @brief Major version number. */
#define KL_VERSION_MAJOR  1
/** @brief Minor version number. */
#define KL_VERSION_MINOR  0
/** @brief Patch version number. */
#define KL_VERSION_PATCH  0
/** @brief Version string ("major.minor.patch"). */
#define KL_VERSION_STRING "1.0.0"

/** @} */

#include <keel/error.h>
#include <keel/allocator.h>
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
#include <keel/client.h>
#include <keel/client_pool.h>
#include <keel/redirect.h>
#include <keel/compress.h>
#include <keel/decompress.h>
#include <keel/drain.h>
#include <keel/sse.h>
#include <keel/timer.h>

#endif
