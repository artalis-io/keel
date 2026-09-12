#!/bin/sh
# tools/release_build.sh <outdir> [ref] - build a deterministic source release archive + SHA-256 manifest.
#
# Source-first release bundle: the tracked tree at <ref> (default HEAD), named from the root VERSION file
# (keel-<VERSION>.tar.gz), plus a SHA-256 manifest. Two builds from the same clean commit are
# byte-identical: git archive emits tracked files only (no .git, no untracked/build/editor/local state),
# in stable tree order, with git mode bits, uid/gid 0, and file mtimes derived from the commit (not
# wall-clock); gzip -n drops the name/timestamp from the gzip header. This builds artifacts only; it does
# not sign, tag, or publish.
set -eu

ROOT=$(git rev-parse --show-toplevel 2>/dev/null || echo .)
cd "$ROOT"

OUT=${1:?usage: release_build.sh <outdir> [ref]}
REF=${2:-HEAD}
V=$(cat VERSION)
NAME="keel-$V"

# portable SHA-256 (Linux sha256sum, macOS shasum). Output is the standard "<hash>  <file>" manifest,
# verifiable with the same tool's -c.
sha256_manifest() { if command -v sha256sum >/dev/null 2>&1; then sha256sum "$@"; else shasum -a 256 "$@"; fi; }

mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)

# Deterministic tar.gz. --format=tar then `gzip -n -9` (not --format=tar.gz) so the gzip header carries
# no filename/timestamp. Write to a temp then rename so a partial file is never left behind.
git archive --format=tar --prefix="$NAME/" "$REF" > "$OUT/$NAME.tar.tmp"

# Line-ending guard. `git archive` applies core.autocrlf, so on a Windows checkout without the
# repo's .gitattributes every text file went into the archive with CRLF: 2356 CR bytes in the
# Makefile alone, and a tarball 174080 bytes larger than the one CI builds from the same commit,
# with a different SHA-256. Nothing failed; the archive was simply not the canonical one, and the
# only thing that caught it was comparing two checksums by hand. Fail loudly instead.
#
# The fuzz corpora are excluded because CR is deliberate there: those seeds carry CRLF as the
# protocol syntax under test (HTTP request/response, PROXY v1). .gitattributes marks them -text so
# they pass through byte for byte, and this check must agree with that.
CRLF_SCAN=$(mktemp -d)
trap 'rm -rf "$CRLF_SCAN"' EXIT
tar xf "$OUT/$NAME.tar.tmp" -C "$CRLF_SCAN"
# `tr -dc` keeps only CR, so any file containing CR yields non-empty output and `grep -q .`
# matches. NOT `grep "$(printf \\r)"`: command substitution collapses that to an EMPTY
# pattern, and an empty pattern matches every file, which made the first draft of this guard flag
# whatever it happened to scan first.
offenders=$(find "$CRLF_SCAN" -type f ! -path '*/fuzz/corpus_*' ! -name '*.bin' \
              -exec sh -c 'tr -dc "\\r" < "$1" | grep -q . && echo "$1"' _ {} \; 2>/dev/null \
            | sed "s|^$CRLF_SCAN/$NAME/||" | sort | head -20)
if [ -n "$offenders" ]; then
  echo "release_build: FAIL - CRLF in the archive, so it is not the canonical artifact:" >&2
  echo "$offenders" | sed 's|^|  |' >&2
  echo "  Check .gitattributes is present and declares 'eol=lf'; an explicit eol attribute is what" >&2
  echo "  overrides core.autocrlf. Refusing to produce a non-reproducible release archive." >&2
  rm -f "$OUT/$NAME.tar.tmp"
  exit 1
fi

gzip -n -9 < "$OUT/$NAME.tar.tmp" > "$OUT/$NAME.tar.gz.tmp"
rm -f "$OUT/$NAME.tar.tmp"
mv "$OUT/$NAME.tar.gz.tmp" "$OUT/$NAME.tar.gz"

# SHA-256 manifest, relative to OUT so the manifest names the bare file (no path leakage).
( cd "$OUT" && sha256_manifest "$NAME.tar.gz" > "$NAME.sha256" )

echo "release_build: $OUT/$NAME.tar.gz"
echo "release_build: $OUT/$NAME.sha256"
