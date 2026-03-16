/**
 * @file h2.h
 * @brief Shared HTTP/2 protocol constants
 *
 * Contains constants shared by both server (h2_server.h) and client (h2_client.h).
 */

#ifndef KEEL_H2_H
#define KEEL_H2_H

/** @brief Default maximum concurrent streams. */
#define KL_H2_DEFAULT_MAX_STREAMS 128
/** @brief Default initial window size (bytes). */
#define KL_H2_DEFAULT_WINDOW_SIZE 65535

#endif
