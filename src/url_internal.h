/*
 * url_internal.h: URL helpers shared by the clients (src/-internal, never installed).
 */
#ifndef KEEL_SRC_URL_INTERNAL_H
#define KEEL_SRC_URL_INTERNAL_H

#include <keel/url.h>
#include <stddef.h>

/* Write url's authority as a Host header, an absolute-form target or an HTTP/2 :authority needs it
 * (RFC 9110 7.2): the host, bracketed when it is an IPv6 literal, then ":port" unless the port is the
 * scheme's default. An AF_UNIX URL names no TCP port, so it gets the host alone (the clients set the
 * host to "localhost" for a socket path). Returns the length written (NUL-terminated), or -1 if it
 * does not fit. */
int kl_url_authority(const KlUrl *url, char *out, size_t cap);

#endif /* KEEL_SRC_URL_INTERNAL_H */
