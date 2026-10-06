/*
 * lwip_raw_testclient.h: TEST-ONLY raw-API TCP client for the lwIP-raw completion tests.
 *
 * This peer lives outside the production glue (lwip_raw_glue.c) so NO test-client state lives
 * in the production backend (no static mutable production state). It is compiled ONLY into the
 * test binaries (loopback-raw / loopback-raw-asan), never into a shipping build. Like the glue
 * it includes lwIP's raw headers directly; it creates its own client PCBs (tcp_connect) and does
 * not touch the server's per-conn slots.
 *
 * All calls run on the single lwIP tick thread (marshalled via KEEL timers in the tests), so
 * plain non-atomic state is safe (NO_SYS=1 single-thread).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef KEEL_LWIP_RAW_TESTCLIENT_H
#define KEEL_LWIP_RAW_TESTCLIENT_H

#include <stddef.h>
#include <stdint.h>

/* ── accumulating client (server roundtrips + byte-exact body checks) ────────── */

/* tcp_new + tcp_connect to ip4:port; on connect the internal callback sends `req` (len bytes)
 * and captures the response into an internal heap buffer. `cap` sizes the accumulator. Returns
 * 0 on the connect being issued, -1 on failure. */
int    kl_lwr_client_start_cap(const uint8_t ip4[4], uint16_t port,
                               const void *req, size_t req_len, size_t cap);
/* Convenience: small default accumulator (1 KB). */
int    kl_lwr_client_start(const uint8_t ip4[4], uint16_t port,
                           const void *req, size_t req_len);
/* Copy the captured response so far into `dst` (NUL-terminated if room); returns the count. */
size_t kl_lwr_client_response(char *dst, size_t cap);
/* Total bytes accumulated so far (headers + body). */
size_t kl_lwr_client_len(void);
/* 1 once the current roundtrip's connection has fully closed (server FIN or reset seen); lets a test
 * open a fresh connection only after the previous one is fully torn down (no accumulator overlap). */
int    kl_lwr_client_closed(void);
/* Body length after the CRLFCRLF header terminator (0 if not seen yet); also reports an
 * additive checksum over the body + its first/last byte (any out ptr may be NULL). */
size_t kl_lwr_client_body(size_t *out_checksum, unsigned char *first, unsigned char *last);
/* Free the client accumulator (test teardown; avoids an at-exit leak under LSan). */
void   kl_lwr_client_release(void);
/* Copy up to `cap` body bytes (after CRLFCRLF) from body offset `off` into `dst`. */
size_t kl_lwr_client_body_peek(size_t off, unsigned char *dst, size_t cap);

/* ── lifetime client (close/cancel/err cases) ─────────────────────────────────
 * Each start runs ONE roundtrip; mode: 0 full-read+FIN, 1 partial+RST, 2 partial+FIN.
 * abort_after = bytes to receive before the RST/FIN (partial modes). Returns 0/-1. */
int    kl_lwr_lc_start(const uint8_t ip4[4], uint16_t port, const void *req, size_t req_len,
                       int mode, size_t abort_after);
int    kl_lwr_lc_done(void);          /* this roundtrip resolved (200 seen or torn down) */
int    kl_lwr_lc_saw_200(void);       /* the response status line contained "200" */
int    kl_lwr_lc_completed(void);     /* running count of resolved roundtrips */
size_t kl_lwr_lc_recv(void);          /* bytes received this roundtrip */
void   kl_lwr_lc_reset_counter(void); /* zero the completed-roundtrip counter */

/* ── multi-connection client (concurrency tests) ──────────────────────────────
 * Up to KL_LWR_MC_MAX SIMULTANEOUS independent clients, each with its own pcb + response
 * accumulator + saw-200/done flags; so a test can open N connections at once and verify each
 * completes (or is explicitly rejected). Each slot is byte-exact verifiable via a checksum. */
#define KL_LWR_MC_MAX 32

/* Reset all multi-client slots (free any accumulator, clear counters). Call before a burst. */
void kl_lwr_mc_reset(void);
/* Start client slot `idx` (0..KL_LWR_MC_MAX-1): tcp_connect to ip4:port, send `req`, accumulate
 * up to `cap` response bytes. Returns 0 if the connect was issued, -1 on failure (bad idx /
 * OOM / connect refused). A refused connect resolves the slot as done+failed. */
int  kl_lwr_mc_start(int idx, const uint8_t ip4[4], uint16_t port,
                     const void *req, size_t req_len, size_t cap);
/* Slot `idx` resolved (a full response with CRLFCRLF+body arrived, or it was torn down). */
int  kl_lwr_mc_done(int idx);
/* Slot `idx` saw an HTTP "200" status. */
int  kl_lwr_mc_ok(int idx);
/* Slot `idx` connect was refused (server rejected / no slot); resolved without a 200. */
int  kl_lwr_mc_refused(int idx);
/* Body length + additive checksum over slot `idx`'s response body (0 if no body yet). */
size_t kl_lwr_mc_body(int idx, size_t *out_checksum);
/* Reset slot `idx`'s live connection now (tcp_abort: an RST to the server) and resolve the slot.
 * Returns 0, or -1 if the slot has no live pcb. */
int    kl_lwr_mc_abort(int idx);
/* Slot `idx`'s client-side local port (0 if it has no live pcb). */
uint16_t kl_lwr_mc_local_port(int idx);
/* The server-side pcb that answered slot `idx`, captured when its connect completed (NULL if it
 * never connected). An opaque address for identity comparison only: never dereference it. */
const void *kl_lwr_mc_server_pcb(int idx);

/* The live server-side pcb of the connection between server port `server_port` and client port
 * `client_port`, or NULL. An opaque address for identity comparison only (pcb reuse checks). */
const void *kl_lwr_server_pcb_of(uint16_t server_port, uint16_t client_port);

/* ── reset-on-first-data client ───────────────────────────────────────────────
 * Connects, sends `req`, and on the FIRST response bytes resets the connection from inside its
 * recv callback, without acknowledging them: the server's send that produced them never completes.
 * (A request with "Expect: 100-continue" leaves the server with the 100 Continue send AND the body
 * receive both posted when the reset arrives.) Returns 0 if the connect was issued, else -1. */
int    kl_lwr_rd_start(const uint8_t ip4[4], uint16_t port, const void *req, size_t req_len);
int    kl_lwr_rd_done(void);        /* resolved: reset sent, or the server closed/reset first */
int    kl_lwr_rd_reset_sent(void);  /* 1 if this client sent the reset after receiving data */
size_t kl_lwr_rd_head(char *dst, size_t cap);   /* the bytes received before the reset */

/* ── lwIP callback-contract guard (server-side pcbs) ──────────────────────────
 * lwIP requires a raw callback that frees its pcb (tcp_abort) to return ERR_ABRT, and one that
 * returns ERR_ABRT to have freed it: tcp_input keeps using the pcb after any other return. With the
 * stock memp pools (MEMP_MEM_MALLOC=0) a freed pcb is still mapped, so ASan cannot see that use.
 * The guard wraps the recv/sent callbacks of every live server-side pcb bound to `server_port`
 * and checks, after each call, whether the pcb is still on lwIP's active or TIME-WAIT list.
 *   - freed but not ERR_ABRT: a violation; the guard returns ERR_ABRT itself, so the run stays
 *     memory-safe and the test fails on the count instead;
 *   - ERR_ABRT but still listed: a violation (lwIP would leak the pcb).
 * Returns the number of pcbs wrapped by this call (>= 0), or -1 if the guard table is full. */
int  kl_lwr_guard_server_pcbs(uint16_t server_port);
int  kl_lwr_guard_calls(void);       /* guarded callback invocations */
int  kl_lwr_guard_aborts(void);      /* guarded calls that freed their pcb and returned ERR_ABRT */
int  kl_lwr_guard_violations(void);  /* contract violations seen */
void kl_lwr_guard_reset(void);       /* forget every wrapped pcb and zero the counters */

#endif /* KEEL_LWIP_RAW_TESTCLIENT_H */
