# Security Policy

## Supported Versions

Security fixes target the current stable line (2.9.x). The full version-support and maintenance policy,
including the 3.x transition and the 2.x maintenance window after 3.0.0, is the single source at
[docs/operations/platform_support.md](docs/operations/platform_support.md).

| Version | Security fixes |
|---------|----------------|
| 2.9.x (current stable) | Yes |
| < 2.0   | No |

## Reporting a Vulnerability

**Please do not open public issues for security vulnerabilities.**

Report vulnerabilities via [GitHub Security Advisories](https://github.com/artalis-io/keel/security/advisories/new) (private disclosure). You will receive an initial response within 72 hours.

Include:
- Description of the vulnerability
- Steps to reproduce
- Affected versions
- Impact assessment (if known)

## Security Measures

Keel employs multiple layers of security testing:

- **Compiler hardening**: `-Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Werror -fstack-protector-strong -fPIE`, `_FORTIFY_SOURCE=3` (release builds), `-pie`. Linux linker hardening: `-Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack`.
- **AddressSanitizer + UndefinedBehaviorSanitizer**: CI runs all tests under ASan/UBSan (`make debug-test`)
- **Fuzz testing**: libFuzzer targets for the untrusted-input parsers (HTTP request and response, multipart, WebSocket, DNS, PROXY protocol, URL, and gzip/deflate decompression)
- **Static analysis**: Clang scan-build and cppcheck in CI
- **CodeQL**: GitHub SAST scanning on every push and PR
- **W^X surface guard**: `tests/no_codegen_surface.sh` fails the build if any future commit introduces `mmap PROT_EXEC`, `MAP_JIT`, `dlopen`, `dlsym`, `memfd_create`, `pthread_jit_*`, `popen`, or `system()` under `src/` (recursively, covering `src/protocols/`).
- **Binary hardening assertion**: CI verifies the built test binaries have a non-executable stack and the `PIE` flag set (`readelf -l` / `otool -hv`).

## Architectural Guarantees

Keel is **structurally W^X**: it has no path that creates executable memory or loads native code at runtime.

| Construct | Present in Keel | Notes |
|-----------|-----------------|-------|
| `mmap` with `PROT_EXEC` | **No** | All buffers are heap+stack |
| `MAP_JIT` | **No** | No JIT layer |
| `mprotect` adding `PROT_EXEC` | **No** | No W→X transition path |
| `memfd_create` | **No** | No anonymous-fd-then-mmap pattern |
| `dlopen` / `dlsym` | **No** | No dynamic native loading |
| `popen` / `system` / `exec*` | **No** | No subprocess invocation |
| JIT (LLVM / Fast / Multi-tier) | **No** | No JIT macros recognised; `KEEL_ENABLE_JIT` / `KEEL_ENABLE_DYNAMIC_CODE` / `KEEL_ENABLE_DLOPEN` are reserved opt-in flags that fire a `#error` at the top of `keel.h` if defined. |

The HTTP/1.1 parser (llhttp), HTTP/2 framing, WebSocket framing, multipart parsing, chunked transfer encoding, URL parsing, and TLS-via-vtable layers all operate on heap and stack memory only. The libFuzzer targets in `fuzz/` exercise the untrusted network-input attack surfaces.

Keel does **not** own a process boundary. W^X enforcement at the kernel-sandbox layer (seccomp + Landlock on Linux, Seatbelt + Hardened Runtime on macOS, pledge/unveil on OpenBSD and Cosmopolitan) is the responsibility of the host application that embeds `libkeel.a`. The companion Hull project (https://github.com/artalis-io/hull) ships such a host policy; see Hull's `docs/security.md` §3.A "Inject native code at runtime" for the canonical layered model.

The guard against silent regression is twofold:

1. **At build time**, `keel.h`'s `#error` directives on `KEEL_ENABLE_JIT` / `KEEL_ENABLE_DYNAMIC_CODE` / `KEEL_ENABLE_DLOPEN` make any future opt-in fail loudly.
2. **At CI time**, `tests/no_codegen_surface.sh` greps `src/` recursively (covering `src/protocols/`) for the syscalls and APIs that could violate the invariant and fails the build if any appears.

## Vendored Dependencies

All vendored code is tracked in [`vendor/MANIFEST.json`](vendor/MANIFEST.json) with version pins and checksums. A CycloneDX SBOM is available at [`sbom.cdx.json`](sbom.cdx.json).

## Supply Chain Recommendations

For production deployments, we recommend:
- Verify signed commits on releases
- Enable 2FA on accounts with write access
- Use SSH keys for repository access
- Enable branch protection on `main`
