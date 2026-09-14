/*
 * win_prelude.h: Windows-only test prelude (force-included before utest.h via
 * `-include` on the Windows test build; see the Makefile `test-win` target).
 *
 * vendor/utest.h self-declares QueryPerformanceCounter/Frequency on MinGW UNLESS
 * <windows.h> was already included (it keys on `_WINDOWS_`). Test files include
 * utest.h first, then Keel headers pull in <winsock2.h>/<windows.h> afterward:
 * so utest.h's own prototype clashes with the real one ("conflicting types for
 * 'QueryPerformanceCounter'"). Including the Win32 network+base headers here,
 * before utest.h, makes utest.h use the real LARGE_INTEGER declarations and skip
 * its conflicting fallback. winsock2.h must precede windows.h (the winsock v1
 * clash): the same ordering src/sockcompat.h enforces.
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

/* MSVC spells the POSIX case-insensitive compares with a leading underscore.
 * MinGW provides the POSIX names, so key on the compiler, not the platform.
 * Harness-only: the library never calls these. */
#if defined(_MSC_VER)
#define strcasecmp  _stricmp
#define strncasecmp _strnicmp
#endif

/* The UCRT TERMINATES the process when a CRT call is handed an invalid file descriptor:
 * _close(10) on an unopened fd never returns, the process dies with 0xC0000409, and utest
 * never flushes, so the suite reports nothing at all. glibc and MinGW return -1/EBADF.
 * Several suites hand deliberately-fabricated descriptors to error paths and expect that
 * POSIX behaviour; a no-op invalid-parameter handler restores it.
 *
 * The handler has to be installed before any test runs. utest.h generates main() from
 * UTEST_MAIN(), and vendor/ is not ours to modify, so the generated entry point is renamed
 * here -- this header is force-included ahead of utest.h -- and the real main() lives in
 * tests/net_compat_win.c, which is built WITHOUT this prelude and so keeps the name. Every
 * Windows test binary links that TU. A suite that writes its own main() (test_http_redirect)
 * is renamed identically and works the same way. No CRT-section constructor tricks: this is
 * one ordinary function calling another. */
#if defined(_MSC_VER)
#define main keel_utest_main
#endif

#endif /* KEEL_TESTS_WIN_PRELUDE_H */
