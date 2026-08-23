/*
 * errno_uefi.c — the single freestanding `errno` definition for the UEFI builds.
 *
 * The EFI socket provider (socket_efi_tcp4.c) WRITES errno = EAGAIN on a would-block
 * recv/send and EIO on error, matching the lwIP provider contract so tls_mbedtls.c's BIO
 * detects would-block correctly. That symbol must resolve in EVERY build that links the
 * provider — including builds that link the provider but not the heavier u1_link_stubs.c.
 * A dedicated minimal TU gives each link set exactly one definition regardless of which
 * other stubs it pulls. Nothing here reads it — the provider is the sole writer.
 *
 * NOT linked by the host mock harness (there, libc supplies the real errno).
 */
int errno;
