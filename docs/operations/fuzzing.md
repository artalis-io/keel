# Fuzzing KEEL

KEEL ships libFuzzer targets over its **untrusted-input attack surface** — the parsers that touch
bytes straight off the network. Every command below is real; the sample output is from an actual run.
For the full test story see [testing.md](testing.md).

## Targets

Seven targets build with `make fuzz`; an eighth (`fuzz_decompress`) needs the optional miniz codec and
is built on demand.

| Target | Attack surface | Seed corpus |
|---|---|---|
| `fuzz_parser` | HTTP/1.1 request parser + chunked transfer-encoding decoder | `fuzz/corpus_parser/` |
| `fuzz_response_parser` | client-side HTTP response parser | `fuzz/corpus_response_parser/` |
| `fuzz_multipart` | `multipart/form-data` state machine | `fuzz/corpus_multipart/` |
| `fuzz_websocket` | WebSocket frame decoder (RFC 6455) | `fuzz/corpus_websocket/` |
| `fuzz_dns` | DNS response parser (`kl_dns_parse_response`) | `fuzz/corpus_dns/` |
| `fuzz_proxy` | PROXY protocol v1/v2 header + CIDR match | `fuzz/corpus_proxy/` |
| `fuzz_url` | URL parser (redirect `Location`, CRLF-injection guard) | `fuzz/corpus_url/` |
| `fuzz_decompress` *(on demand)* | gzip/deflate decompression + decompression-bomb cap | `fuzz/corpus_decompress/` |

## Building

libFuzzer needs clang. The targets link a **separately-instrumented** build of the library
(`libkeel_fuzz.a`, built from `.fuzz.o` objects): every library object is compiled with
SanitizerCoverage (`-fsanitize=fuzzer-no-link`) plus ASan + UBSan, so libFuzzer both *explores* and
*memory-checks* the parsers. (Linking the plain `libkeel.a` would fuzz only the harness — no coverage,
no ASan on library code.)

```sh
# Linux
make fuzz CC=clang

# macOS (Homebrew LLVM — the system clang lacks libFuzzer)
make fuzz CC=/opt/homebrew/opt/llvm@18/bin/clang
```

The optional decompression target is built separately with a miniz checkout:

```sh
make fuzz-decompress KEEL_COMPRESS=miniz MINIZ_DIR=/path/to/miniz CC=clang
```

## Running

libFuzzer treats the corpus directory as **read-write**: it mutates from the seeds *and* writes every
new coverage-increasing input back into that directory. Pointing it at the tracked `fuzz/corpus_*/`
therefore **adds files to your working tree** — a short run can add hundreds. Keep the tracked corpus
clean one of two ways:

```sh
# Preferred: explore against a scratch copy of the seeds — the tracked corpus is untouched.
mkdir -p /tmp/kc && cp fuzz/corpus_parser/* /tmp/kc/
./fuzz/fuzz_parser /tmp/kc/ -max_total_time=60 -artifact_prefix=/tmp/

# Or run against the tracked corpus, then review and drop the generated additions before committing:
#   git status fuzz/corpus_parser/      # shows what libFuzzer added (all untracked)
#   ...remove the untracked additions you do not intend to keep.
```

Bound a run with `-max_total_time=<seconds>` (or `-runs=<n>`), and use `-artifact_prefix=<dir>/` so
crash/timeout reproducers land there (e.g. `/tmp/`) rather than the current directory / repo root.

A clean run ends with a `Done … runs` line and exits `0`. Representative short local runs (macOS,
Homebrew clang 18, 8 s each) — no crashes, no sanitizer reports:

```
fuzz_parser : Done 1026491 runs in 9 second(s)
fuzz_dns    : Done 2894497 runs in 9 second(s)
fuzz_url    : Done 4992462 runs in 9 second(s)
```

A crash (ASan report, assertion, or timeout) is written as `<prefix>crash-<sha1>` /
`<prefix>timeout-<sha1>`; re-run the target on that file to reproduce deterministically:

```sh
./fuzz/fuzz_parser /tmp/crash-0123abcd…      # replay a single reproducer
```

## Continuous integration

The **fuzz** CI job (`.github/workflows/ci.yml`) builds every target with `make fuzz CC=clang` and
runs each for 60 seconds against its seed corpus: `fuzz_parser`, `fuzz_multipart`, `fuzz_websocket`,
`fuzz_response_parser`, `fuzz_dns`, `fuzz_proxy`, `fuzz_url`. A new crash fails the job. `fuzz_decompress`
is not in the standing job (it needs the miniz dependency) and is run on demand.

## Adding a target

1. Write `fuzz/fuzz_<name>.c` with a `LLVMFuzzerTestOneInput(const uint8_t *, size_t)` that drives the
   parser and frees everything it allocates (so ASan/LSan can prove no leak per input).
2. Seed `fuzz/corpus_<name>/` with a few valid inputs.
3. Add the target to the `fuzz:` list in the Makefile, and add a 60 s step to the CI fuzz job.

Keep each harness to a single parser with no I/O — the point is to explore that parser's byte handling,
not the network stack.
