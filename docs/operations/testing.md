# Testing KEEL

How to build and run KEEL's tests, sanitizers, smoke tests, boundary gates, and static analysis.
Every command below is real; the sample output shown is from an actual run (yours will differ in
counts as suites are added). For *what runs on which backend*, see
[capability_matrix.md](capability_matrix.md); for the parser fuzzers, see [fuzzing.md](fuzzing.md).

## Quick start

```sh
make test        # build libkeel.a + every unit suite, run them all (default backend)
```

The default backend is the platform readiness engine — **epoll** on Linux, **kqueue** on macOS. On a
clean tree `make test` builds the library and each test binary, then runs every suite:

```
--- tests/test_router ---
[==========] 12 test cases ran.
[  PASSED  ] 12 tests.
...
```

A passing run exits `0`; a representative macOS/kqueue run reports **89 suites, 1318 individual tests,
0 failures**. Any failure prints `SOME TESTS FAILED` and exits non-zero.

## Test layout

Tests use Sheredom's [utest.h](../../vendor/utest.h) (single header). Each `tests/test_*.c` compiles
to a **standalone binary** and is auto-discovered by the Makefile (`$(wildcard ...)`):

- `tests/test_*.c` — substrate, shared, and backend-axis unit suites (allocator, event, event_ctx,
  event_caps, socket_provider, stream, listener, datagram, connect_op, timer, drain, url, …).
- `tests/protocols/<family>/test_*.c` — protocol suites, mirroring `src/protocols/<family>/`
  (`http/`, `http2/`, `websocket/`, `dns/`, `proxy_protocol/`) — including the HTTP client/server
  suites under `tests/protocols/http/`.

Run one suite directly:

```sh
./tests/test_stream           # a single substrate suite
./tests/test_datagram_socket
```

Suite/test naming is `UTEST(suite, test_name)`.

## Sanitizers (ASan + UBSan)

```sh
make debug         # clean rebuild with -fsanitize=address,undefined -g -O0
make test          # run the suite against the sanitizer build
# or, in one step:
make debug-test    # debug build + full suite under ASan/UBSan
```

`DEBUG_CFLAGS` adds `-fsanitize=address,undefined -fno-omit-frame-pointer` to the standard
`-Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Werror` set. ASan catches heap/stack overflow,
use-after-free, double-free, and leaks; UBSan catches signed overflow, null deref, misaligned access,
and shift overflow. The production build (`make`) uses `-O2 -fstack-protector-strong`.

## The two axes: readiness and completion

The unit suites are model-independent by design, but the two axes are **not** covered the same way.
Readiness runs the full suite on the default build. The completion axis is covered by a **curated
suite set plus dedicated smokes** — not a full-suite parity run: some suites are readiness-shaped by
design (they drive `kl_event_wait`, or assert readiness-capability negotiation) and are deliberately
excluded from the completion gate (see [capability_matrix.md §3](capability_matrix.md)).

Note that `make` variables do not persist across invocations — pass the **same** `BACKEND=`/`OS=`/`CC=`
on every command that builds or runs, or you may test a differently-configured library.

| Backend | Command | Coverage |
|---|---|---|
| epoll / kqueue (readiness, default) | `make test` | full suite — Linux / macOS locally + CI |
| poll (universal readiness fallback) | `make BACKEND=poll test` | full suite — any POSIX host + CI |
| **pollcomp** (portable completion double) | `make BACKEND=pollcomp smoke-pollcomp` (+ `-tls`/`-ws`/`-async`/`-client`); `make smoke-pollcomp-asan` | dedicated completion smokes + the stream single-shot oracle + an ASan/UBSan/LSan leak run (CI). `make BACKEND=pollcomp test` builds and runs the completion driver locally, but the *standing* coverage is the smokes + the io_uring curated suite below. |
| **io_uring** (Linux completion) | `make BACKEND=iouring test-iouring`; `make BACKEND=iouring smoke-iouring` (+ `-async`/`-client`/`-asan`) | the curated `IOURING_TEST_SUITES` (readiness-shaped suites excluded) + the io_uring smokes. Best in an Apple/Linux container (unrestricted io_uring) |
| **IOCP** (Windows completion) | `make OS=windows BACKEND=iocp CC=gcc` then `make OS=windows BACKEND=iocp CC=gcc test-win-iocp` | the IOCP lifecycle + negotiation suite + HTTP / TLS / async / KlDatagram smokes — Windows CI |

The **pollcomp** double lets the completion driver (and its ASan/leak checks) run on macOS/Linux
without io_uring or Windows:

```sh
make smoke-pollcomp-asan     # completion driver + roundtrips under ASan/UBSan/LSan — leak-clean
```

## Smoke tests

Per-backend end-to-end roundtrips (real sockets / loopback), beyond the unit suites:

```sh
make smoke-tcp                                  # plaintext TCP HTTP roundtrip (default readiness)
make smoke-datagram                             # public KlDatagram send/recv roundtrip (socket-seam)
make smoke-dns                                  # DNS resolver init + resolve
make BACKEND=pollcomp smoke-pollcomp            # HTTP over the completion double (+ -tls / -ws / -async / -client)
make BACKEND=iouring  smoke-iouring             # HTTP over io_uring, Linux       (+ -async / -client / -asan)
make OS=windows BACKEND=iocp CC=gcc smoke-iocp  # HTTP over IOCP, Windows         (+ -tls / -async)
make KEEL_TLS=mbedtls smoke-tls                 # real TLS handshake (needs a TLS backend)
```

## Boundary and stale-name gates

Structural gates enforce the three-axis architecture and keep deleted layouts from resurfacing. They
run in CI and locally; each is fast and self-canaried:

```sh
make check-substrate-purity          # G1: src/ never includes a protocol header
make check-protocol-no-integration   # G2: protocols never include an integration adapter header
make check-integration-seam          # G3: integrations reach src/ only via the frozen seam allowlist
make check-protocol-home             # G4: protocol impl .c lives only under src/protocols/<fam>/
make check-old-layout                # G5: no resurrected protocols// spikes/ / parsers/ / flat-home
make check-test-layout               # tests match tests/test-layout.manifest (no drift)
make check-no-kludp                  # the deleted KlUdp object API does not reappear
make check-no-httplegacy             # no legacy KlServer/KlClient/KlH2 / old module names
make check-doc-refs                  # living-architecture doc links resolve
make check-tier1-boundary            # protocol TUs stay above KlListener/KlStream/KlDatagram
make check-sockaddr-neutral          # protocol TUs are KlSockAddr-only (no host sockaddr)
```

Freestanding gates additionally prove the client / server / datagram / DNS subsets compile and link
with no hosted libc (`make freestanding-headers`, `freestanding-dgram`, `freestanding-dns`, the
`uefi-dgram-gate` PE gate), and `make wx-guard` asserts the W^X surface (no JIT / dlopen / exec /
`mmap+PROT_EXEC`).

## Static analysis and coverage

```sh
make analyze     # Clang static analyzer via scan-build (--status-bugs fails on findings)
make cppcheck    # cppcheck --enable=all (--error-exitcode=1 fails on findings)
make coverage    # lcov/genhtml HTML report (Linux; needs lcov) → coverage_html/index.html
```

Both `analyze` and `cppcheck` should exit cleanly before merging.

## Continuous integration

CI (`.github/workflows/ci.yml`) runs on push / PR to `main`. Standing jobs:

| Job | Covers |
|---|---|
| **build** (matrix) | Linux epoll, Linux poll fallback, macOS kqueue, Linux `KEEL_NO_COMPLETION` — build + full suite + example smokes |
| **completion** | the pollcomp double: HTTP/TLS/WebSocket/async roundtrips, the stream single-shot oracle, ASan+UBSan leak run, runtime-injected provider |
| **completion-iouring** / **-suite** | io_uring roundtrips + ASan/LSan, and the `IOURING_TEST_SUITES` unit gate |
| **windows** / **windows-iocp** | Winsock/WSAPoll full-core build + `WIN_TEST_SUITES` subset; IOCP lifecycle suite + HTTP/TLS/async/KlDatagram roundtrips |
| **sanitizers** | build + suite under ASan+UBSan |
| **fuzz** | the parser fuzzers, 60 s each (see [fuzzing.md](fuzzing.md)) |
| **analyze** | scan-build, cppcheck, all boundary/stale-name gates, the freestanding + EFI PE gates, W^X guard, hardening-flag assertion |
| **musl** / **cosmo** | Alpine/musl build + suite; Cosmopolitan APE build + example smokes |
| **integrations** | mbedTLS + nghttp2 — roundtrip, real-socket e2e, h2spec (pass-floor 130/146), h2load, curl + nghttpd interop, ALPN, real-TLS smoke |
| **lwip** | lwIP providers — loopback (server/client/UDP echo), HTTPS-over-lwIP, raw-completion backend under ASan+UBSan+LSan |

The io_uring and IOCP axes and the integration jobs are CI-gated, not merely buildable — see the
[capability matrix](capability_matrix.md) for the per-backend, per-feature detail.
