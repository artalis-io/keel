/*
 * win_prelude.h — Windows-only test prelude (force-included before utest.h via
 * `-include` on the Windows test build; see the Makefile `test-win` target).
 *
 * vendor/utest.h self-declares QueryPerformanceCounter/Frequency on MinGW UNLESS
 * <windows.h> was already included (it keys on `_WINDOWS_`). Test files include
 * utest.h first, then Keel headers pull in <winsock2.h>/<windows.h> afterward —
 * so utest.h's own prototype clashes with the real one ("conflicting types for
 * 'QueryPerformanceCounter'"). Including the Win32 network+base headers here,
 * before utest.h, makes utest.h use the real LARGE_INTEGER declarations and skip
 * its conflicting fallback. winsock2.h must precede windows.h (the winsock v1
 * clash) — the same ordering src/sockcompat.h enforces.
 *
 * This is a build-only harness shim: it changes no test logic and is never
 * compiled into the library.
 */
#ifndef KEEL_TESTS_WIN_PRELUDE_H
#define KEEL_TESTS_WIN_PRELUDE_H

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#endif

#endif /* KEEL_TESTS_WIN_PRELUDE_H */
