# Contributing to Keel

## Getting Started

```bash
git clone https://github.com/artalis-io/keel.git
cd keel
make        # build libkeel.a
make test   # run all tests
```

## Coding Standards

- **C11**: compiled with `-Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Werror -fstack-protector-strong`
- **No direct malloc/free**: all allocation through the `KlAllocator` interface
- **Public API prefix**: all public functions use `kl_` (e.g. `kl_http_router_init`)
- **Datagram APIs**: portable message protocols use `KlDatagram` (the canonical Tier-1 message transport). It covers the full datagram surface: batching, GSO/GRO, multicast/broadcast, per-packet TOS, source-pinned send, and confirmed-detachment close. See [`include/keel/datagram.h`](include/keel/datagram.h) + [docs/contracts/datagram.md](docs/contracts/datagram.md)
- **Error handling**: return `-1` on failure, `0` on success (or positive value); set `last_error` at the point of failure
- **Resource cleanup**: every `_init` has a corresponding `_free`
- **Overflow guards**: check against `SIZE_MAX/2` or `INT_MAX/2` before arithmetic
- **Header-only code**: `static inline` in headers (see `http_request.h`)
- **Vendor code**: compiled with `-w`; do not modify files under `vendor/`

## Project Layout

Where code lives, and where its tests go:

- **Substrate** (event/completion, sockets, allocator, timer, the datagram machine, ...): `src/`; tests at `tests/`.
- **Protocols** (`http`, `http2`, `websocket`, `dns`, `proxy_protocol`): `src/protocols/<family>/`; tests mirror at `tests/protocols/<family>/`.
- **Integrations** (optional, bring-your-own: TLS, HTTP/2 session, compression, lwIP/UEFI platforms): `integrations/<role>/<backend>/`; tests at `integrations/<role>/<backend>/tests/`.
- **Public headers**: flat under `include/keel/`.

The substrate never includes a protocol header; protocols never include an integration header; integrations reach core only through `include/keel/` or the frozen substrate seam. See `CLAUDE.md` and `AGENTS.md` for the full module, layout, and API guidance.

## Before Submitting

All of these must pass cleanly:

```bash
make test                  # the unit + integration suites
make debug-test            # tests under ASan + UBSan
make analyze               # Clang scan-build
make cppcheck              # cppcheck static analysis
make check-no-milestones   # no milestone/phase archaeology in C/H comments
make check-no-em-dash      # no em-dash (U+2014) in any tracked text
```

The layout and seam gates (`make check-old-layout`, `check-test-layout`, `check-integration-seam`, ...) also run in CI; the full list is under "Local Gates" in `CLAUDE.md`. Which toolchains, backends, providers, and integrations are covered by standing CI (versus a strict compile gate, local-only, or bring-your-own) is documented in [docs/operations/platform_support.md](docs/operations/platform_support.md).

## Pull Request Workflow

1. Branch off `main`
2. Make your changes, add tests for new functionality
3. Ensure all checks above pass
4. Open a PR against `main`; CI runs automatically
5. Required status checks (Linux epoll, ASan+UBSan, Static Analysis, CodeQL) must pass

## Reporting Bugs

Open a GitHub issue with steps to reproduce, expected behavior, and actual behavior.

## Security Vulnerabilities

**Do not open public issues for security vulnerabilities.** See [SECURITY.md](SECURITY.md) for responsible disclosure instructions.
