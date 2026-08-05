/*
 * mbedtls_config_uefi.h — a FREESTANDING TLS 1.2 CLIENT mbedTLS config for the
 * U-4 UEFI spike.
 *
 * Goal: the smallest mbedTLS build that can complete a TLS 1.2 client handshake
 * against a stock openssl `s_server` / python TLS responder and read app data,
 * compiled with clang --target=x86_64-unknown-windows -ffreestanding -nostdlib
 * (NO hosted libc, NO OS, NO filesystem, NO wall-clock time).
 *
 * ── Deliberate freestanding shape ──────────────────────────────────────────
 *  - NO NET_C / FS_IO         : all BIO I/O goes through Keel's memory/socket BIO
 *                               (tls_mbedtls.c), which routes ciphertext through
 *                               the U-2 EFI_TCP4 socket provider. mbedTLS never
 *                               touches a host socket or file.
 *  - NO HAVE_TIME / TIMING_C  : no wall clock in UEFI pre-boot. Cert validity
 *                               (not-before/not-after) is therefore NOT checked.
 *                               That is acceptable here ONLY because we also run
 *                               verify-none (see the client-ctx path in
 *                               tls_mbedtls.c: NULL CA => MBEDTLS_SSL_VERIFY_NONE).
 *                               *** SPIKE SHORTCUT — NOT production-safe. ***
 *  - NO THREADING_C           : single-threaded pre-boot environment.
 *  - NO PSA / TLS 1.3         : stay on the classic TLS 1.2 stack — PSA drags in
 *                               a much larger surface and its own entropy/driver
 *                               init we don't want freestanding.
 *  - PLATFORM_C + PLATFORM_MEMORY : lets us install EFI AllocatePool/FreePool as
 *                               mbedTLS's calloc/free (mbedtls_platform_set_calloc_free)
 *                               so heap goes through the UEFI pool, not a libc malloc.
 *  - NO_PLATFORM_ENTROPY + ENTROPY_HARDWARE_ALT : we provide mbedtls_hardware_poll()
 *                               (mbedtls_platform_uefi.c) backed by EFI_RNG (or a
 *                               loudly-documented weak fallback when EFI_RNG is
 *                               absent — see mbedtls_platform_uefi.c).
 *  - NO ERROR_C / DEBUG / SELF_TEST : keep the code + string tables small and avoid
 *                               pulling snprintf/printf-family libc dependencies.
 */
#ifndef MBEDTLS_CONFIG_UEFI_H
#define MBEDTLS_CONFIG_UEFI_H

/* ── System / platform abstraction ──────────────────────────────────────────
 * PLATFORM_C + PLATFORM_MEMORY: route mbedTLS heap through our EFI calloc/free
 * (installed at runtime via mbedtls_platform_set_calloc_free). We do NOT enable
 * PLATFORM_NO_STD_FUNCTIONS — mbedTLS defaults its (unused-here) printf/snprintf
 * macros to the libc names, which the freestanding shim + platform TU satisfy,
 * and enabling NO_STD_FUNCTIONS would instead REQUIRE us to register every one. */
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY

/* No OS entropy source; we drive the entropy pool from a hardware alt callback
 * (mbedtls_hardware_poll) over EFI_RNG. */
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT

/* ── Protocol: TLS 1.2 client only ──────────────────────────────────────────*/
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2

/* Handshake robustness extensions a modern server may require / prefer. */
#define MBEDTLS_SSL_ENCRYPT_THEN_MAC
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET
#define MBEDTLS_SSL_SERVER_NAME_INDICATION   /* SNI (mbedtls_ssl_set_hostname) */
#define MBEDTLS_SSL_KEEP_PEER_CERTIFICATE    /* mbedtls_ssl_get_peer_cert (tls_peer_cert) */
#define MBEDTLS_SSL_ALPN                     /* the adapter references ALPN unconditionally */

/* ── Key exchange suites ────────────────────────────────────────────────────
 * ECDHE-RSA + ECDHE-ECDSA cover a stock openssl s_server default cert; plain RSA
 * kept as a broad fallback. */
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_RSA_ENABLED

/* ── X.509 (parse the server certificate chain) ─────────────────────────────*/
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C

/* ── Public-key + asymmetric crypto ─────────────────────────────────────────*/
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21
#define MBEDTLS_BIGNUM_C

/* Elliptic curve (ECDHE key exchange + ECDSA cert verify). */
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C

/* Curves: the common server/client set. */
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_SECP521R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED

/* ── Symmetric crypto + AEAD ────────────────────────────────────────────────*/
#define MBEDTLS_CIPHER_C
#define MBEDTLS_AES_C
#define MBEDTLS_AES_ROM_TABLES        /* smaller/no runtime table init */
#define MBEDTLS_GCM_C
#define MBEDTLS_CCM_C                 /* optional AEAD some suites use */

/* ── Hashes / MAC ───────────────────────────────────────────────────────────*/
#define MBEDTLS_MD_C
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_SHA1_C                /* older cert signatures / compat */

/* ── RNG ────────────────────────────────────────────────────────────────────*/
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_CTR_DRBG_C

/* ── Explicitly DISABLED (documented above) ─────────────────────────────────
 * We do NOT define: MBEDTLS_NET_C, MBEDTLS_FS_IO, MBEDTLS_TIMING_C,
 * MBEDTLS_HAVE_TIME, MBEDTLS_HAVE_TIME_DATE, MBEDTLS_THREADING_C,
 * MBEDTLS_SELF_TEST, MBEDTLS_ERROR_C, MBEDTLS_DEBUG_C, MBEDTLS_PSA_CRYPTO_C,
 * MBEDTLS_SSL_PROTO_TLS1_3. Leaving them undefined is how mbedTLS disables them. */

#endif /* MBEDTLS_CONFIG_UEFI_H */
