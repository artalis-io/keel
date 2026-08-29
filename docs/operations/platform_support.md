# Keel platform and compatibility support

This is the single source of truth for what Keel supports: toolchains, event backends, socket/platform
providers, integrations, and the version/compatibility policy. Other documents link here rather than
repeat a matrix. Every claim below is backed by a `make` target or a standing CI job name; where a
capability is only checked locally, that is stated explicitly.

Audited against the current Makefile and the standing CI workflows (`.github/workflows/`). Toolchain and
suite counts drift, so this document cites `make` commands and CI job names as evidence rather than
version numbers or test totals, unless the workflow pins them (it does not pin compiler versions, so no
minimum compiler version is promised beyond C11 conformance).

## Support levels

- **Standing-CI tested**: built AND exercised (tests/smokes/roundtrips) on every push by a named CI job.
- **Strict-gate supported**: not executed in CI, but held to a strict compile / static-analysis gate
  (for example a freestanding cross-compile with `-Werror`) on every push.
- **Locally tested (not standing CI)**: verified by a maintainer with a documented command, but not run
  on every push.
- **BYO with standing CI**: a bring-your-own integration (you add the adapter to your build) whose
  adapter is exercised by a named CI job.
- **BYO without standing CI**: a shipped adapter you wire in yourself; compiled by its own tests but not
  in the standing matrix.
- **Experimental or planned (not supported)**: present as a design direction or spike, with no support
  promise.

## C11 compiler and tested toolchains

Keel is C11. The public headers use only C11 and expose no compiler-specific type in the API. No minimum
compiler version is promised beyond C11 conformance (the workflows track the runner-provided compilers
and do not pin a version).

| Toolchain | Level | Evidence |
|-----------|-------|----------|
| GCC (Linux) | Standing-CI tested | jobs `Linux (epoll)`, `Linux (poll fallback)`, `Linux (no completion)` (`make`, `make BACKEND=poll`, `make KEEL_NO_COMPLETION=1`) |
| GCC (musl / Alpine) | Standing-CI tested | job `Linux (musl/Alpine)` |
| AppleClang (macOS) | Standing-CI tested | job `macOS (kqueue)` (`make`) |
| MinGW-w64 GCC (Windows) | Standing-CI tested | job `Windows (MinGW, full core)` (`make OS=windows CC=gcc`) |
| Cosmopolitan `cosmocc` | Standing-CI tested | job `Cosmopolitan (APE)` (`make CC=cosmocc`) |
| Clang (Linux) | Standing-CI for fuzz + static analysis; full build+test is locally tested | `make fuzz CC=clang` (job `Fuzz Testing`), `make analyze` / `make cppcheck` (job `Static Analysis`); a full `make test CC=clang` is a documented local run, not a standing job |

## C++11 consumer linkage

The installed C headers are consumable from C++11: every `keel/*.h` compiles standalone under C11 and
C++11 with strict flags and guards its declarations with `extern "C"`, so a C++11 translation unit can
include them and link `libkeel.a`. Evidence: `make check-public-headers` (standalone C11/C++11 compile
of every installed header, the opacity probe, and a C++-links-the-C-archive canary), run in the
`Static Analysis` job.

Limits: there is no C++ wrapper API, no templates or exceptions in the surface, and no C++ (name-mangled)
ABI promise. C++ is a consumption guarantee for the C API only. Opt-in `*_detail.h` layouts are not
ABI-stable in either language.

## Event backends

| Backend | Axis | Level | Evidence |
|---------|------|-------|----------|
| epoll (Linux default) | readiness | Standing-CI tested | job `Linux (epoll)` (`make`) |
| kqueue (macOS default) | readiness | Standing-CI tested | job `macOS (kqueue)` |
| poll (universal fallback) | readiness | Standing-CI tested | job `Linux (poll fallback)` (`make BACKEND=poll`) |
| WSAPoll (Windows default) | readiness | Standing-CI tested | job `Windows (MinGW, full core)` (`make OS=windows CC=gcc`) |
| io_uring (Linux) | completion | Standing-CI tested | jobs `Completion (io_uring)`, `Completion (io_uring) unit suite` (`make BACKEND=iouring`) |
| IOCP (Windows) | completion | Standing-CI tested | job `Windows (IOCP)` (`make OS=windows BACKEND=iocp CC=gcc`) |
| pollcomp (portable poll()-based completion double) | completion (test) | Standing-CI tested | job `Completion (poll)` (`make BACKEND=pollcomp`); a CI/ASan double, never a production backend |

## Socket and platform providers

| Provider | Level | Evidence |
|----------|-------|----------|
| POSIX sockets | Standing-CI tested | the Linux/macOS jobs above |
| Winsock | Standing-CI tested | `Windows (MinGW, full core)` and `Windows (IOCP)` |
| lwIP (BSD sockets + raw NO_SYS completion provider) | BYO with standing CI | job `Integration (lwIP)` (`make -C integrations/platform/lwip loopback` / `loopback-raw` / `loopback-raw-asan`, with a bring-your-own lwIP checkout) |
| UEFI EFI_TCP4 / EFI_UDP4 | Strict-gate supported (compile); execution locally tested | see UEFI below |

### Windows AF_UNIX capability boundary

The AF_UNIX filesystem-node lifecycle on Windows is an identity-anchored, fail-closed capability. It is
supported on modern Windows on a local NTFS volume (open-by-file-id + reparse handling + POSIX-semantics
delete), the boundary verified in a maintainer spike on Windows 10 build 26100 / local NTFS
(`src/unix_socket_node_win.c`, `docs/archive/designs/unix_socket_cleanup_windows_spike.md`). On any other
surface (non-NTFS, SMB, ReFS, FAT, or unknown volumes, or a system missing the required file APIs) the
provider returns `KL_UNIX_NODE_ERR_UNSUPPORTED` and refuses rather than offering a weaker guarantee.

Support is claimed only for local NTFS. ReFS, SMB, and FAT are explicitly NOT supported for AF_UNIX node
lifecycle; the fail-closed behavior is the contract there. The provider itself is compiled in the
standing Windows build; the filesystem-behavior verification is the local spike, not a standing job.

### UEFI

UEFI is a freestanding target: the EFI_TCP4/EFI_UDP4 providers and the freestanding library subset are
held to strict PE cross-compile gates on every push (both x86_64 and aarch64, `-Werror`):
`make uefi-dgram-gate UEFI_GATE_STRICT=1`, `make freestanding-lib-selfcontained`,
`make freestanding-lib-server-selfcontained`, `make freestanding-lib-dns-selfcontained`, plus the
freestanding header/DNS gates, all in the `Static Analysis` job. Actual firmware EXECUTION under
QEMU/OVMF is locally tested only (`integrations/platform/uefi/tests/run_dgram_dns.sh`,
`integrations/platform/uefi/tests/run_dgram_public.sh`); it is not a standing CI job.

## Protocol integrations: built-in contract vs bring-your-own

Keel defines the protocol/transport contract in core (a vtable); concrete backends are bring-your-own
adapters under `integrations/`, selected at build time or passed in by the caller. Nothing is
dynamically loaded.

| Area | Core contract (built in) | Adapter | Level |
|------|--------------------------|---------|-------|
| TLS | `KlTls` vtable (`keel/tls.h`), mTLS peer-cert hook | mbedTLS (`integrations/tls/mbedtls/`) | BYO with standing CI (job `Integrations (mbedTLS + nghttp2)`: peer-cert unit tests, real handshake, ALPN e2e) |
| TLS | same | OpenSSL, BoringSSL, LibreSSL (`integrations/tls/{openssl,boringssl,libressl}/`) | BYO without standing CI (OpenSSL also appears only as an interop peer in the ALPN e2e, not as the Keel adapter under test) |
| HTTP/2 | `KlHttp2ClientSession` / `KlHttp2ServerSession` vtables | nghttp2 (`integrations/http2/nghttp2/`) | BYO with standing CI (h2spec conformance, h2load, curl/nghttpd interop) |
| Compression | `KlCompress` / `KlDecompress` vtables (`keel/http_compress.h`, `keel/decompress.h`) | miniz (`integrations/codec/miniz/`) | BYO with standing CI (gzip-trailer integrity regression under ASan+UBSan) |
| DNS resolution | `KlResolver` vtable (`keel/resolver.h`) plus a built-in async resolver over `KlDatagram` (`keel/dns_resolver.h`) | caller-supplied resolvers | Built-in resolver: Standing-CI tested (unit suites; the freestanding DNS truncation harness under ASan+UBSan+LSan). Custom resolvers: BYO |

## Sanitizer, fuzz, static-analysis, and CodeQL coverage

- **ASan + UBSan**: standing. Job `ASan + UBSan`; also completion roundtrips under ASan+UBSan
  (`Completion (poll)`, `Completion (io_uring)`), and `make debug-test` locally. LeakSanitizer runs in
  the freestanding DNS harness and the lwIP raw ASan run.
- **Fuzzing**: standing. Job `Fuzz Testing` builds libFuzzer targets with `make fuzz CC=clang` and runs
  the HTTP request parser, multipart parser, WebSocket frame parser, response parser, DNS response
  parser, PROXY protocol parser, and URL parser (a decompression-bomb fuzzer builds on demand with the
  miniz backend).
- **Static analysis**: standing. Job `Static Analysis` runs `make analyze` (Clang scan-build) and
  `make cppcheck`, alongside the structural/seam/version gates.
- **CodeQL**: standing. Workflow `CodeQL`, job `Analyze`, language `cpp`.

An OpenSSF Scorecard workflow and a Benchmark workflow also run; this document makes no performance
claim (benchmark numbers are environment-specific and are not a support guarantee).

## Distribution and ABI

Keel ships a static library `libkeel.a` plus a pkg-config module named `keel`
(`pkg-config --cflags --libs keel`). There is no shared object and no soname: consumers link statically
and there is no cross-version binary ABI promise. The installed header set is exactly a reviewed manifest
(`make check-public-headers` / `make check-install`), and the pkg-config `Version` is single-sourced from
the root `VERSION` file and always matches the compiled `kl_version()` and `keel/version.h`
(`make check-version-drift`).

## Version compatibility and maintenance

- Within the 3.x major version: source compatibility plus recompile / static-relink; not cross-version
  binary ABI. Public configs and vtables evolve append-only with zero-default optional tails; callbacks
  and factories are frozen; `*_detail.h` layouts are not ABI-stable. Full contract:
  `docs/contracts/compatibility.md`. Migration from 2.x: `docs/migrations/2.x-to-3.0.md`.
- 2.x maintenance: while 3.0 release candidates are active, the 2.9.x line remains the supported stable
  line. After 3.0.0 final, 2.x receives critical security and correctness fixes for 90 days, then is
  marked end-of-life; no general feature backports are promised (see
  `docs/v3_release_version_policy_freeze.md`).

## Unsupported or planned surfaces

- **QUIC / HTTP-3**: not supported. The `KlDatagram` primitive is designed to be a base for future
  portable message protocols (see `docs/archive/designs/udp_design.md`), but no QUIC/HTTP-3 support is
  implemented or promised.
- **MQTT**: not supported, not in-tree.
- **AF_XDP, DPDK**: not supported. These are kernel-bypass data planes with no in-tree provider; the
  socket-provider seam could host such an adapter in principle, but none exists and none is promised.
