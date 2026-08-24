#ifndef KEEL_SRC_HTTP_SERVER_PLAT_H
#define KEEL_SRC_HTTP_SERVER_PLAT_H

/*
 * http_server_plat.h: platform-specific server services.
 *
 * One implementation per platform family (http_server_plat_posix.c /
 * http_server_plat_win.c), Makefile-selected via SERVER_PLAT_SRC: the same
 * one-platform-per-TU pattern as event_*.c / socket_*.c / platform_*.c. This
 * keeps http_server.c free of platform #ifdefs: the AF_UNIX node lifecycle, peer
 * credentials, and signal handling (all of which use POSIX-only types:
 * uid_t / sockaddr_un / struct ucred / struct sigaction) live in the
 * per-platform TU. The interface here is deliberately type-neutral so it
 * compiles on any platform.
 */

#include <stddef.h>
#include <keel/http_server.h>   /* KlHttpServer, KlPeerCred, KlSocketHandle (via handle.h) */

/* Ignore SIGPIPE and, if s->config.install_signal_handlers, install
 * SIGTERM/SIGINT graceful-stop handlers (POSIX) or a console Ctrl handler
 * (Windows). Saved handler state is process-global (kept in the platform TU).
 * kl_http_server_plat_signals_restore reverses it. */
void kl_http_server_plat_signals_install(KlHttpServer *s);
void kl_http_server_plat_signals_restore(KlHttpServer *s);

/* Bind the AF_UNIX listener into s->listen_fd. POSIX: create + unlink-stale +
 * bind (umask-guarded) + chown/chmod to the configured owner/group/mode.
 * Windows: create + unlink-stale + bind, without the POSIX node perms/ownership
 * (Win10 AF_UNIX has no filesystem-permission model). Sets s->last_error and
 * returns 0 / -1. */
int  kl_http_server_plat_bind_unix(KlHttpServer *s);

/* Best-effort removal of the owned AF_UNIX socket node on shutdown (honours
 * s->unix_socket_owned / config.unix_socket_unlink). No-op if not owned. */
void kl_http_server_plat_unlink_owned_unix(KlHttpServer *s);

/* Peer security label of a connected AF_UNIX peer (Linux SO_PEERSEC,
 * SELinux/AppArmor). Writes a NUL-terminated string to @buf; returns 0 on
 * success, -1 if unavailable (non-Linux, or no label). */
int  kl_http_server_plat_peer_label_fd(KlSocketHandle fd, char *buf, size_t buflen);

/* kl_peer_cred_fd(KlSocketHandle, KlPeerCred *) is declared in keel/http_server.h
 * (public API); its definition also lives in the platform TU. */

/* Remove an environment variable (used to stop the systemd LISTEN_* socket-
 * activation vars leaking to children). POSIX: unsetenv; Windows:
 * SetEnvironmentVariableA(name, NULL). */
void kl_http_server_plat_unsetenv(const char *name);

#endif /* KEEL_SRC_HTTP_SERVER_PLAT_H */
