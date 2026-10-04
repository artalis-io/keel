/**
 * @file http2.h
 * @brief Shared HTTP/2 protocol constants
 *
 * Contains constants shared by both server (http2_server.h) and client (http2_client.h).
 */

#ifndef KEEL_HTTP2_H
#define KEEL_HTTP2_H

/** @brief Default maximum concurrent streams. */
#define KL_HTTP2_DEFAULT_MAX_STREAMS 128
/** @brief Default initial window size (bytes). */
#define KL_HTTP2_DEFAULT_WINDOW_SIZE 65535
/** Header list a session accepts per request (SETTINGS_MAX_HEADER_LIST_SIZE, RFC 9113 6.5.2): the
 *  sum of name + value + 32 octets per field. Larger, or more than KL_MAX_HEADERS fields, resets
 *  the stream: KEEL keeps no more, and storing them is a peer-driven memory cost (HPACK bomb). */
#define KL_HTTP2_MAX_HEADER_LIST_SIZE (64u * 1024u)

#endif
