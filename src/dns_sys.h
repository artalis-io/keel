/*
 * dns_sys.h — platform config-discovery seam for the built-in DNS resolver.
 *
 * The resolver's datagram + TCP-fallback I/O already runs over the udp and
 * socket.h seams, both cross-platform, so dns_resolver.c itself is
 * platform-#ifdef-free. Only *config discovery* is OS-specific: where the
 * system nameservers, search domains, and hosts file come from. POSIX reads
 * /etc/resolv.conf + /etc/hosts (dns_sys_posix.c); Windows uses iphlpapi
 * (dns_sys_win.c). The Makefile selects one impl per platform via DNS_SYS_SRC,
 * mirroring UDP_IO_SRC / SOCKET_SRC / PLATFORM_SRC.
 */
#ifndef KL_DNS_SYS_H
#define KL_DNS_SYS_H

/* Max nameserver token, "IP" or "IP#port" (matches dns_resolver.c's nsbuf). */
#define KL_DNS_SYS_NS_STRMAX  80
/* Max search-domain string; must equal dns_resolver.c's DNS_NAME_MAX. */
#define KL_DNS_SYS_NAME_MAX   256

/* Collect up to @p max system nameserver address strings ("IP" or "IP#port")
 * into @p out. @p resolv_conf_path is the POSIX resolv.conf path, or NULL for
 * the platform default (/etc/resolv.conf); it is ignored on Windows, where the
 * nameservers come from iphlpapi. Returns the count (0 if none found). */
int kl_dns_sys_nameservers(const char *resolv_conf_path,
                           char out[][KL_DNS_SYS_NS_STRMAX], int max);

/* Fill the search-domain list + ndots for name expansion. POSIX parses
 * resolv.conf `search`/`domain` and `options ndots:`; Windows uses the iphlpapi
 * suffix list. @p resolv_conf_path is as above. @p search is a caller array of
 * @p max_s strings, each KL_DNS_SYS_NAME_MAX wide. On return *nsearch is the
 * number filled and *ndots the threshold (default 1). */
void kl_dns_sys_resolv_options(const char *resolv_conf_path,
                               char search[][KL_DNS_SYS_NAME_MAX], int max_s,
                               int *nsearch, int *ndots);

/* The platform's default hosts-file path (/etc/hosts, or the Windows
 * %SystemRoot%\System32\drivers\etc\hosts path). Returns a static string. */
const char *kl_dns_sys_default_hosts_path(void);

#endif /* KL_DNS_SYS_H */
