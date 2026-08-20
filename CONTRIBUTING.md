# Contributing to Keel

## Getting Started

```bash
git clone https://github.com/artalis-io/keel.git
cd keel
make        # build libkeel.a
make test   # run all tests
```

## Coding Standards

- **C11** — compiled with `-Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Werror -fstack-protector-strong`
- **No direct malloc/free** — all allocation through the `KlAllocator` interface
- **Public API prefix** — all public functions use `kl_` (e.g. `kl_router_init`)
- **Datagram APIs** — portable message protocols use `KlDatagram` (the canonical Tier-1 message transport). It covers the full datagram surface: batching, GSO/GRO, multicast/broadcast, per-packet TOS, source-pinned send, and confirmed-detachment close. See [`include/keel/datagram.h`](include/keel/datagram.h) + [docs/datagram_contract.md](docs/datagram_contract.md)
- **Error handling** — return `-1` on failure, `0` on success (or positive value); set `last_error` at the point of failure
- **Resource cleanup** — every `_init` has a corresponding `_free`
- **Overflow guards** — check against `SIZE_MAX/2` or `INT_MAX/2` before arithmetic
- **Header-only code** — `static inline` in headers (see `request.h`)
- **Vendor code** — compiled with `-w`; do not modify files under `vendor/`

## Before Submitting

All of these must pass cleanly:

```bash
make test           # all unit tests
make debug-test     # tests under ASan + UBSan
make analyze        # Clang scan-build
make cppcheck       # cppcheck static analysis
```

## Pull Request Workflow

1. Branch off `main`
2. Make your changes, add tests for new functionality
3. Ensure all checks above pass
4. Open a PR against `main` — CI runs automatically
5. Required status checks (Linux epoll, ASan+UBSan, Static Analysis, CodeQL) must pass

## Reporting Bugs

Open a GitHub issue with steps to reproduce, expected behavior, and actual behavior.

## Security Vulnerabilities

**Do not open public issues for security vulnerabilities.** See [SECURITY.md](SECURITY.md) for responsible disclosure instructions.
