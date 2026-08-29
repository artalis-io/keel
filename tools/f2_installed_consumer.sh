#!/bin/sh
# tools/f2_installed_consumer.sh - F2-3 installed-consumer check.
#
# Proves an out-of-tree C program can be built against the INSTALLED Keel using only pkg-config and a
# staged prefix - never the repository tree. Covers both install semantics:
#   - logical PREFIX: install into a real prefix directory; pkg-config reads it directly.
#   - DESTDIR staging: install with PREFIX=/opt/... into a DESTDIR; pkg-config reads it via
#     PKG_CONFIG_SYSROOT_DIR (the paths are rewritten under the staging root).
# For each: compile + link + RUN the consumer; confirm no repository or vendor path leaks into the
# pkg-config flags or the installed headers; and confirm keel.pc selects -lkeel from the staged libdir
# plus the platform-private link flags (the consumer pulls the thread pool, so -lpthread is required to
# link, not merely advertised).
set -eu

ROOT=$(git rev-parse --show-toplevel 2>/dev/null || echo .)
cd "$ROOT"
CC=${CC:-cc}

# pkg-config may be absent on a general local host (SKIP), but a release/CI gate must not silently
# skip: set KEEL_REQUIRE_PKGCONFIG=1 to turn a missing pkg-config into a hard failure (F2-6 enrolls it).
if ! command -v pkg-config >/dev/null 2>&1; then
    if [ "${KEEL_REQUIRE_PKGCONFIG:-0}" = 1 ]; then
        echo "installed-consumer: FAIL - pkg-config required (KEEL_REQUIRE_PKGCONFIG=1) but not found"; exit 1
    fi
    echo "installed-consumer: SKIP (no pkg-config; set KEEL_REQUIRE_PKGCONFIG=1 to require it)"; exit 0
fi

work=$(mktemp -d)
trap 'rm -rf "$work"; rm -f "$ROOT/keel.pc"' EXIT INT TERM

# The out-of-tree consumer: exercises the allocator, the event context, and the thread pool (the last
# pulls the pthread references, so the platform-private -lpthread must be present to link).
cat > "$work/consumer.c" <<'EOF'
#include <keel/keel.h>
#include <stdio.h>
int main(void) {
    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    if (kl_event_ctx_init(&ev, &a) != 0) return 1;
    KlThreadPoolConfig cfg = {0};
    cfg.num_workers = 1;
    KlThreadPool *tp = kl_thread_pool_create(&ev, &cfg);   /* pulls pthread refs */
    if (tp) kl_thread_pool_free(tp);
    kl_event_ctx_free(&ev);
    printf("keel consumer ok\n");
    return 0;
}
EOF

fail=0
bad() { echo "installed-consumer: FAIL - $1"; fail=1; }

# args: label  pkgpath  sysroot(may be empty)  incdir  libdir
run_case() {
    label=$1; pkgpath=$2; sysroot=$3; incdir=$4; libdir=$5

    cflags=$(PKG_CONFIG_PATH="$pkgpath" PKG_CONFIG_SYSROOT_DIR="$sysroot" pkg-config --cflags keel)
    libs=$(PKG_CONFIG_PATH="$pkgpath" PKG_CONFIG_SYSROOT_DIR="$sysroot" pkg-config --libs --static keel)

    # No repository / vendor leakage in the metadata flags.
    case " $cflags $libs " in
        *" $ROOT"*|*"$ROOT/"*) bad "$label: repository path leaked into pkg-config flags";;
    esac
    case " $cflags $libs " in *vendor*) bad "$label: vendor path leaked into pkg-config flags";; esac

    # keel.pc selects -lkeel from the staged libdir + the platform-private flag.
    case " $libs " in *" -lkeel "*) ;; *) bad "$label: -lkeel missing from --libs";; esac
    case " $libs " in *" -lpthread "*) ;; *) bad "$label: platform-private -lpthread missing from --libs --static";; esac
    case " $cflags " in *"-I$incdir"*) ;; *) bad "$label: --cflags does not point at the staged includedir";; esac
    [ -f "$libdir/libkeel.a" ] || bad "$label: staged libdir has no libkeel.a"

    # Build + link + run from a scratch cwd (never the repo), using ONLY pkg-config flags. A missing
    # platform-private flag would fail the static link here (undefined pthread symbols).
    if ! (cd "$work" && $CC -std=c11 consumer.c $cflags $libs -o consumer) 2>"$work/err"; then
        bad "$label: consumer failed to compile/link"; sed 's/^/    /' "$work/err" | head -6
        return
    fi
    out=$(cd "$work" && ./consumer 2>/dev/null || echo "RUN-FAILED")
    [ "$out" = "keel consumer ok" ] || bad "$label: consumer did not run correctly (got: $out)"
    [ "$fail" = 0 ] && echo "installed-consumer: $label OK (out-of-tree build+link+run via pkg-config)"
}

# Installed headers must not reach back into the repository: no quoted include resolving to a
# repo-relative path (../, or a vendor/ or src/ subtree). External integration deps behind an opt-in
# guard (e.g. "lwip/sockets.h", resolved from the consumer's own include path) are allowed.
assert_no_repo_includes() {
    inckeel=$1
    hits=$(grep -rnE '#[[:space:]]*include[[:space:]]*"(\.\./|vendor/|src/)' "$inckeel" 2>/dev/null || true)
    [ -z "$hits" ] || { echo "$hits" | sed 's/^/    /'; bad "installed header reaches into the repository tree"; }
}

# --- Case A: logical PREFIX (no DESTDIR) ---
pa="$work/prefixA"
rm -f "$ROOT/keel.pc"
make -s install PREFIX="$pa" >/dev/null 2>&1
assert_no_repo_includes "$pa/include/keel"
run_case "PREFIX" "$pa/lib/pkgconfig" "" "$pa/include" "$pa/lib"

# --- Case B: DESTDIR staging with a logical PREFIX ---
db="$work/destB"; lp=/opt/keel-staged
rm -f "$ROOT/keel.pc"
make -s install DESTDIR="$db" PREFIX="$lp" >/dev/null 2>&1
# keel.pc records the LOGICAL prefix; pkg-config rewrites it under the staging root via the sysroot.
grep -q "^prefix=$lp\$" "$db$lp/lib/pkgconfig/keel.pc" || bad "DESTDIR: keel.pc did not record the logical PREFIX"
run_case "DESTDIR" "$db$lp/lib/pkgconfig" "$db" "$db$lp/include" "$db$lp/lib"

[ "$fail" = 0 ] && echo "installed-consumer: OK (PREFIX + DESTDIR; no repo/vendor leakage; correct library + platform-private flags)"
exit "$fail"
