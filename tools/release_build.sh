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
git archive --format=tar --prefix="$NAME/" "$REF" | gzip -n -9 > "$OUT/$NAME.tar.gz.tmp"
mv "$OUT/$NAME.tar.gz.tmp" "$OUT/$NAME.tar.gz"

# SHA-256 manifest, relative to OUT so the manifest names the bare file (no path leakage).
( cd "$OUT" && sha256_manifest "$NAME.tar.gz" > "$NAME.sha256" )

echo "release_build: $OUT/$NAME.tar.gz"
echo "release_build: $OUT/$NAME.sha256"
