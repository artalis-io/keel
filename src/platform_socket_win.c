/*
 * platform_socket_win.c: the Windows half of the PAL socket-runtime seam (see platform_socket.h).
 *
 * WSAStartup once per process, under InitOnceExecuteOnce, with no WSACleanup. This is the sole
 * mechanism by which ws2_32 is brought up in a Windows Keel build; the GCC/Clang load-time
 * constructor that used to do it is gone, so MinGW/Clang and MSVC behave identically.
 */
#include "platform_socket.h"

#include <winsock2.h>
#include <errno.h>

/* Outcome of the one and only WSAStartup attempt. Written inside the InitOnce callback (which
 * publishes them to every later caller) and read afterwards.
 *
 * `volatile LONG` rather than a C11 atomic: kl_plat_socket_runtime_status() is a test seam that may
 * legitimately read these BEFORE initialisation has been attempted, so it cannot rely on the InitOnce
 * barrier. An aligned 32-bit load on Windows is indivisible, which is all the seam needs; the real
 * ordering guarantee for kl_plat_socket_runtime_init() still comes from InitOnceExecuteOnce. */
static volatile LONG g_attempted;    /* 0 until the callback has run */
static volatile LONG g_startup_err;  /* WSAStartup's return: 0 on success */

/* Translate a WSAStartup failure into the CRT errno space, matching what kl_wsa_set_errno() does for
 * ordinary Winsock errors so that callers branching on errno see one coherent error model. */
static int startup_errno(int wsa_err) {
    switch (wsa_err) {
        case WSASYSNOTREADY:     return ENETDOWN;    /* network subsystem unavailable */
        case WSAVERNOTSUPPORTED: return ENOSYS;      /* no 2.2 implementation */
        case WSAEINPROGRESS:     return EINPROGRESS; /* a blocking 1.1 op is in progress */
        case WSAEPROCLIM:        return EMFILE;      /* implementation socket/task limit */
        case WSAEFAULT:          return EFAULT;
        default:                 return EIO;         /* kl_wsa_set_errno()'s default, same taxonomy */
    }
}

/* Returns TRUE UNCONDITIONALLY, including when WSAStartup failed. That is the point: returning FALSE
 * would leave the INIT_ONCE uninitialised and let the next caller attempt WSAStartup again, which is
 * exactly the repeated-opportunistic-retry behaviour this seam is meant not to have. The outcome is
 * recorded instead, so a failed runtime fails fast and identically forever after. */
static BOOL CALLBACK runtime_init_once(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void)once; (void)param; (void)ctx;
    WSADATA wsa;
    /* 2.2 is the ceiling every supported Windows provides, and what the retired constructor asked
     * for; requesting the same version keeps behaviour identical for existing builds. */
    g_startup_err = (LONG)WSAStartup(MAKEWORD(2, 2), &wsa);
    g_attempted = 1;
    return TRUE;
}

int kl_plat_socket_runtime_init(void) {
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    (void)InitOnceExecuteOnce(&once, runtime_init_once, NULL, NULL);
    int err = (int)g_startup_err;
    if (err == 0) return 0;
    errno = startup_errno(err);
    return -1;
}

int kl_plat_socket_runtime_status(void) {
    if (!g_attempted) return 0;
    int err = (int)g_startup_err;
    return err == 0 ? 1 : -err;
}
