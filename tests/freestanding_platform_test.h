/*
 * freestanding_platform_test.h — the mock-clock test API exposed by
 * freestanding_host_platform.c (F-5). Lets the harness drive the advanceable
 * monotonic clock so timer deadlines (Happy-Eyeballs delay, request deadline)
 * fire deterministically without sleeping. Test/harness-only.
 */
#ifndef KEEL_FREESTANDING_PLATFORM_TEST_H
#define KEEL_FREESTANDING_PLATFORM_TEST_H

#include <stdint.h>

/* Set the mock monotonic clock to an absolute millisecond value. */
void     fs_clock_set(uint64_t ms);
/* Advance the mock monotonic clock by a millisecond delta (fires due timers on
 * the next event-loop tick). */
void     fs_clock_advance(uint64_t ms);
/* Read the current mock monotonic value (== kl_monotonic_ms()). */
uint64_t fs_clock_now(void);

#endif /* KEEL_FREESTANDING_PLATFORM_TEST_H */
