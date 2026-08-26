/*
 * kl_cstr.h: locale-free, bounded C-string primitives (freestanding-safe).
 *
 * These replace the formatted-I/O + locale surface (snprintf / strtol /
 * strcasecmp / strstr / strchr) in the freestanding client TUs so that
 * libkeel_freestanding.a's C-runtime dependency shrinks to essentially
 * mem* + strlen. Every helper is ASCII-only (no locale), bounds-checked,
 * and overflow-guarded, matching the libc semantics of the call it replaces.
 *
 * These TUs are SHARED with the hosted build (server + sync client), so the
 * helpers must be behavior-identical to the libc calls they displace.
 */
#ifndef KEEL_KL_CSTR_H
#define KEEL_KL_CSTR_H

#include <stddef.h>
#include <stdint.h>

/**
 * ASCII-only, locale-independent case-insensitive compare of two NUL-terminated
 * strings. Matches strcasecmp(3) semantics for ASCII input: returns 0 when the
 * strings are equal ignoring A-Z/a-z case, else the (signed) difference of the
 * first differing lowercased byte.
 */
int kl_ascii_strcasecmp(const char *a, const char *b);

/**
 * ASCII-only, locale-independent case-insensitive compare of the first @p n
 * bytes of two NUL-terminated strings. Matches strncasecmp(3) semantics for
 * ASCII input (stops early at a NUL like libc does).
 */
int kl_ascii_strncasecmp(const char *a, const char *b, size_t n);

/**
 * Parse @p len bytes of decimal ASCII in @p s as a uint16 (e.g. a TCP port).
 * Rejects an empty span, any non-digit byte, and values > 65535. On success
 * writes the value to @p out and returns 0; returns -1 otherwise. Replaces
 * strtol() for port parsing (no locale, no sign, bounded).
 */
int kl_parse_u16_decimal(const char *s, size_t len, uint16_t *out);

/**
 * Format @p v as decimal ASCII into @p buf (NOT NUL-terminated). Returns the
 * number of bytes written, or 0 if @p cap is too small (buf left untouched
 * on overflow). Replaces snprintf("%u"/"%zu", ...) for integer emission.
 */
size_t kl_u64_to_dec(char *buf, size_t cap, uint64_t v);

/** Format @p v as lowercase-hex ASCII into @p buf (NOT NUL-terminated).
 * Returns bytes written, or 0 on overflow (buf untouched). Replaces "%zx". */
size_t kl_u64_to_hex(char *buf, size_t cap, uint64_t v);

/**
 * Append the NUL-terminated string @p s to @p buf at *@p off, bounded by @p cap
 * (leaving room for a trailing NUL that this function writes). On success
 * advances *@p off and returns 0. On overflow returns -1 and leaves *@p off
 * unchanged (partial data may have been written but the offset is not advanced,
 * so callers treat -1 as failure). Replaces snprintf("%s", ...).
 */
int kl_buf_append(char *buf, size_t cap, size_t *off, const char *s);

/** Like kl_buf_append but appends exactly @p len bytes of @p s. Replaces
 * snprintf("%.*s", len, s). */
int kl_buf_append_n(char *buf, size_t cap, size_t *off, const char *s,
                    size_t len);

/** Append @p v as decimal ASCII. Replaces snprintf("%u"/"%d"/"%zu", ...). */
int kl_buf_append_u64(char *buf, size_t cap, size_t *off, uint64_t v);

/** Append @p v as lowercase-hex ASCII. Replaces snprintf("%x"/"%zx", ...). */
int kl_buf_append_hex(char *buf, size_t cap, size_t *off, uint64_t v);

/** Case-SENSITIVE equality of two NUL-terminated strings: 1 iff equal byte for
 * byte (== strcmp(a, b) == 0), locale-free. */
int kl_streq(const char *a, const char *b);

/**
 * Case-SENSITIVE prefix test: returns 1 iff NUL-terminated @p s begins with the
 * NUL-terminated @p prefix (stopping at either NUL, exactly like
 * strncmp(s, prefix, strlen(prefix)) == 0). Byte-exact, no case folding;
 * preserves the URL parser's existing scheme-match semantics. */
int kl_str_startswith(const char *s, const char *prefix);

/**
 * Locate the first occurrence of NUL-terminated @p needle within NUL-terminated
 * @p hay. Returns a pointer into @p hay, or NULL. Matches strstr(3). */
const char *kl_strstr(const char *hay, const char *needle);

/** First occurrence of byte @p c in NUL-terminated @p s, or NULL. Matches
 * strchr(3) for a non-NUL @p c (does not match the terminator). */
const char *kl_strchr(const char *s, int c);

#endif /* KEEL_KL_CSTR_H */
