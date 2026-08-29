#!/bin/sh
# tools/release_verify.sh [--verify|--strict|--selftest] - build and verify the deterministic release
# artifacts (tools/release_build.sh). Builds and verifies only; never signs, tags, or publishes.
#
#   --verify   (default) untagged verification: the full pipeline, no tag requirement.
#   --strict   release mode: additionally require the Git tag v$(VERSION) to point at HEAD.
#   --selftest negative canaries: prove the detectors reject version disagreement, a missing/unexpected
#              archive file, nondeterministic gzip, and checksum corruption.
set -eu

ROOT=$(git rev-parse --show-toplevel 2>/dev/null || echo .)
cd "$ROOT"
MODE=${1:---verify}
V=$(cat VERSION)
NAME="keel-$V"

sha256_manifest() { if command -v sha256sum >/dev/null 2>&1; then sha256sum "$@"; else shasum -a 256 "$@"; fi; }
sha256_check()    { if command -v sha256sum >/dev/null 2>&1; then sha256sum -c "$@"; else shasum -a 256 -c "$@"; fi; }
sha256_hash()     { sha256_manifest "$1" | cut -d' ' -f1; }
fail() { echo "check-release-artifacts: FAIL - $1" >&2; exit 1; }

TMPS=""
cleanup() { for d in $TMPS; do rm -rf "$d"; done; }
trap cleanup EXIT INT TERM
mk() { d=$(mktemp -d); TMPS="$TMPS $d"; echo "$d"; }

run_verify() {
    strict=$1

    # 1. Refuse a dirty tracked tree and a malformed / drifted VERSION.
    git diff --quiet && git diff --cached --quiet || fail "tracked tree is dirty (commit or stash first)"
    case "$V" in
        *.*.*) : ;;
        *) fail "malformed VERSION '$V'" ;;
    esac
    sh tools/version_sync.sh --check >/dev/null 2>&1 || fail "version drift: derived values disagree with VERSION"

    # strict mode: the Git tag v$V must exist and point at HEAD.
    if [ "$strict" = "strict" ]; then
        git rev-parse --verify "refs/tags/v$V" >/dev/null 2>&1 || fail "strict: tag v$V does not exist"
        [ "$(git rev-parse "v$V^{commit}")" = "$(git rev-parse HEAD^{commit})" ] \
            || fail "strict: HEAD is not tagged v$V"
    fi

    # 2. Build the artifacts twice in isolated temp dirs; the archives and checksums must match.
    d1=$(mk); d2=$(mk)
    sh tools/release_build.sh "$d1" >/dev/null
    sh tools/release_build.sh "$d2" >/dev/null
    cmp -s "$d1/$NAME.tar.gz" "$d2/$NAME.tar.gz" || fail "nondeterministic: two builds are not byte-identical"
    [ "$(sha256_hash "$d1/$NAME.tar.gz")" = "$(sha256_hash "$d2/$NAME.tar.gz")" ] || fail "nondeterministic: hashes differ"
    ( cd "$d1" && sha256_check "$NAME.sha256" >/dev/null 2>&1 ) || fail "checksum manifest does not verify"
    H=$(sha256_hash "$d1/$NAME.tar.gz")

    # 3. Extract into a clean temp dir.
    ex=$(mk); tar -xzf "$d1/$NAME.tar.gz" -C "$ex"
    T="$ex/$NAME"
    [ -d "$T" ] || fail "extract: expected top-level dir $NAME/ is missing"

    # 4. Extracted file set == the intended tracked release manifest (git ls-tree at HEAD).
    ( cd "$T" && find . -type f | sed 's|^\./||' | LC_ALL=C sort ) > "$ex/extracted.lst"
    git ls-tree -r --name-only HEAD | LC_ALL=C sort > "$ex/tracked.lst"
    if ! cmp -s "$ex/tracked.lst" "$ex/extracted.lst"; then
        echo "  extracted-vs-tracked diff:" >&2; diff "$ex/tracked.lst" "$ex/extracted.lst" | head >&2
        fail "extracted file set does not match the tracked release manifest"
    fi
    # required inclusions (files + dirs).
    for req in VERSION include/keel/version.h keel.pc.in sbom.cdx.json LICENSE CHANGELOG.md \
               docs/migrations/2.x-to-3.0.md Makefile tools/version_sync.sh tools/release_build.sh; do
        [ -f "$T/$req" ] || fail "archive is missing a required file: $req"
    done
    for d in include/keel src integrations examples tests tools; do
        [ -d "$T/$d" ] || fail "archive is missing a required directory: $d"
    done

    # 5. Agreement: VERSION, version.h, SBOM version + purl, archive name, checksum manifest.
    [ "$NAME" = "keel-$(cat "$T/VERSION")" ] || fail "archive name does not match the archived VERSION"
    grep -q "^#define KL_VERSION_STRING \"$V\"\$" "$T/include/keel/version.h" || fail "archived version.h != VERSION"
    grep -q "\"version\": \"$V\"" "$T/sbom.cdx.json" || fail "archived SBOM component version != VERSION"
    grep -q "pkg:github/artalis-io/keel@$V" "$T/sbom.cdx.json" || fail "archived SBOM purl != VERSION"
    grep -q "  $NAME.tar.gz\$" "$d1/$NAME.sha256" || fail "checksum manifest does not name $NAME.tar.gz"
    [ "$(cut -d' ' -f1 < "$d1/$NAME.sha256")" = "$H" ] || fail "checksum manifest hash != archive hash"

    # 9 (structure part, done early): SBOM is valid JSON; license + release docs present.
    if command -v python3 >/dev/null 2>&1; then
        python3 -c "import json,sys; json.load(open('$T/sbom.cdx.json'))" 2>/dev/null || fail "SBOM is not valid JSON"
    fi
    for f in LICENSE CHANGELOG.md docs/migrations/2.x-to-3.0.md; do
        [ -s "$T/$f" ] || fail "release document missing or empty: $f"
    done

    # 6. Build and test from the EXTRACTED tree, with no reference to the original repo.
    ( cd "$T" && make -s >/dev/null 2>&1 ) || fail "extracted tree does not build"
    # compiled kl_version() agreement.
    cat > "$ex/ver.c" <<EOF2
#include <keel/keel.h>
#include <string.h>
int main(void){ return strcmp(kl_version(), "$V") == 0 ? 0 : 1; }
EOF2
    cc -std=c11 -I"$T/include" "$ex/ver.c" "$T/libkeel.a" -o "$ex/ver" -lpthread 2>/dev/null \
        || fail "kl_version() consumer failed to link against the extracted libkeel.a"
    "$ex/ver" || fail "compiled kl_version() does not equal VERSION"
    ( cd "$T" && make -s test >/dev/null 2>&1 ) || fail "extracted tree tests fail"

    # 7. Staged install + out-of-tree C and C++ compile-link-run consumers from the extracted tree.
    stage=$(mk)
    ( cd "$T" && make -s install PREFIX="$stage" >/dev/null 2>&1 ) || fail "staged install from extracted tree failed"
    # In CI, a missing pkg-config is a hard failure (the installed-consumer path must not silently fall
    # back to direct flags); local hosts without pkg-config use the documented fallback.
    if [ "${RELEASE_REQUIRE_PKGCONFIG:-0}" = 1 ] && ! command -v pkg-config >/dev/null 2>&1; then
        fail "pkg-config required (RELEASE_REQUIRE_PKGCONFIG=1) but not found"
    fi
    if command -v pkg-config >/dev/null 2>&1; then
        pcver=$(PKG_CONFIG_PATH="$stage/lib/pkgconfig" pkg-config --modversion keel 2>/dev/null || true)
        [ "$pcver" = "$V" ] || fail "pkg-config modversion '$pcver' != VERSION"
        CF=$(PKG_CONFIG_PATH="$stage/lib/pkgconfig" pkg-config --cflags keel)
        LF=$(PKG_CONFIG_PATH="$stage/lib/pkgconfig" pkg-config --libs keel)
    else
        CF="-I$stage/include"; LF="-L$stage/lib -lkeel -lpthread"
    fi
    # C consumer
    cat > "$ex/cons.c" <<EOF2
#include <keel/keel.h>
#include <string.h>
int main(void){ return strcmp(kl_version(), "$V") == 0 ? 0 : 1; }
EOF2
    ( cd "$ex" && cc -std=c11 $CF cons.c $LF -o cons_c ) 2>/dev/null || fail "out-of-tree C consumer failed to build"
    "$ex/cons_c" || fail "out-of-tree C consumer: kl_version() != VERSION"
    # C++ consumer (C API via extern "C")
    cat > "$ex/cons.cpp" <<EOF2
#include <keel/keel.h>
#include <string>
int main(){ return std::string(kl_version()) == "$V" ? 0 : 1; }
EOF2
    ( cd "$ex" && c++ -std=c++11 $CF cons.cpp $LF -o cons_cpp ) 2>/dev/null || fail "out-of-tree C++ consumer failed to build/link"
    "$ex/cons_cpp" || fail "out-of-tree C++ consumer: kl_version() != VERSION"

    # 8. No source-tree / absolute-build-path / DESTDIR / vendor-path leakage in installed metadata.
    pc="$stage/lib/pkgconfig/keel.pc"
    for bad in "$ROOT" "$T" "DESTDIR"; do
        grep -qF "$bad" "$pc" && fail "installed keel.pc leaks a path: $bad"
    done
    grep -qE 'vendor/' "$pc" && fail "installed keel.pc leaks a vendor path"
    # installed headers must not embed the source tree or build path either.
    if grep -rqF "$ROOT" "$stage/include" 2>/dev/null; then fail "installed headers leak the source tree path"; fi

    # staged uninstall (ownership-safe, from the extracted tree).
    ( cd "$T" && make -s uninstall PREFIX="$stage" >/dev/null 2>&1 ) || fail "staged uninstall from extracted tree failed"

    echo "check-release-artifacts: OK ($NAME.tar.gz deterministic; sha256 $H; extracted build+test+install+C/C++ consumers verified; no path leakage)"
}

run_selftest() {
    st=0
    base=$(mk)
    sh tools/release_build.sh "$base" >/dev/null
    A="$base/$NAME.tar.gz"

    # canary: checksum corruption must be caught by sha256 -c.
    c=$(mk); cp "$A" "$c/$NAME.tar.gz"; cp "$base/$NAME.sha256" "$c/"
    printf 'x' >> "$c/$NAME.tar.gz"
    if ( cd "$c" && sha256_check "$NAME.sha256" >/dev/null 2>&1 ); then echo "selftest: checksum corruption NOT caught"; st=1; fi

    # canary: nondeterministic gzip (default gzip embeds a timestamp) must differ from the -n build,
    # and byte-compare must distinguish them (this is why the builder uses gzip -n).
    nd=$(mk)
    git archive --format=tar --prefix="$NAME/" HEAD | gzip -9 > "$nd/nondet.tar.gz"   # NO -n
    if cmp -s "$A" "$nd/nondet.tar.gz"; then echo "selftest: nondeterministic gzip NOT distinguished"; st=1; fi

    # canary: version disagreement in version.h must fail the agreement grep.
    vd=$(mk); tar -xzf "$A" -C "$vd"
    sed 's/#define KL_VERSION_STRING "[^"]*"/#define KL_VERSION_STRING "9.9.9"/' \
        "$vd/$NAME/include/keel/version.h" > "$vd/vh"; mv "$vd/vh" "$vd/$NAME/include/keel/version.h"
    if grep -q "^#define KL_VERSION_STRING \"$V\"\$" "$vd/$NAME/include/keel/version.h"; then
        echo "selftest: version-disagreement canary did not diverge"; st=1
    fi

    # canary: a missing file and an unexpected file must both show in the file-set diff.
    fs=$(mk); tar -xzf "$A" -C "$fs"
    ( cd "$fs/$NAME" && find . -type f | sed 's|^\./||' | LC_ALL=C sort ) > "$fs/orig.lst"
    rm -f "$fs/$NAME/LICENSE"                       # missing
    : > "$fs/$NAME/UNEXPECTED_FILE"                 # unexpected
    ( cd "$fs/$NAME" && find . -type f | sed 's|^\./||' | LC_ALL=C sort ) > "$fs/mut.lst"
    if cmp -s "$fs/orig.lst" "$fs/mut.lst"; then echo "selftest: missing/unexpected file NOT detected"; st=1; fi
    grep -q '^UNEXPECTED_FILE$' "$fs/mut.lst" || { echo "selftest: unexpected file not present in list"; st=1; }
    grep -q '^LICENSE$' "$fs/mut.lst" && { echo "selftest: missing-file canary still lists LICENSE"; st=1; }

    [ "$st" = 0 ] && echo "release_verify selftest: OK (checksum, nondeterminism, version, file-set canaries all detected)"
    return "$st"
}

case "$MODE" in
    --verify)   run_verify no ;;
    --strict)   run_verify strict ;;
    --selftest) run_selftest ;;
    *) echo "usage: $0 [--verify|--strict|--selftest]" >&2; exit 2 ;;
esac
