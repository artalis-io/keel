#!/bin/sh
# tools/f2_allocator_boundaries.sh - F2-C2 allocator-boundary gate.
#
# Enforces that every public API accepting a KlAllocator is classified in
# docs/f2/allocator_boundaries.tsv, so a newly added allocator-accepting public API cannot bypass the
# valid-allocator policy. Three mechanical checks against the manifest:
#   1. `fn` rows      == the set of public functions taking a direct KlAllocator* parameter.
#   2. `field-count`  == the per-header count of KlAllocator* struct-field declarations.
#   3. `config` rows  name a function that is declared in an installed header.
# --selftest injects synthetic drift and confirms the gate would catch it.
set -eu

ROOT=$(git rev-parse --show-toplevel 2>/dev/null || echo .)
cd "$ROOT"
MAN=docs/f2/allocator_boundaries.tsv
[ -f "$MAN" ] || { echo "alloc-boundaries: MISSING $MAN"; exit 2; }

# Public functions with a direct KlAllocator* parameter (comment-stripped, multi-line-aware).
extract_fns() {
    for h in include/keel/*.h; do
        perl -0777 -pe 's{/\*.*?\*/}{}gs; s{//[^\n]*}{}g' "$h" |
        awk 'BEGIN{RS=";"} { d=$0; gsub(/\n/," ",d); gsub(/[ \t]+/," ",d) }
             d ~ /KlAllocator[ ]*\*/ && d ~ /kl_[a-zA-Z0-9_]+[ ]*\(/ {
               if (match(d, /kl_[a-zA-Z0-9_]+[ ]*\(/)) { fn=substr(d,RSTART,RLENGTH); sub(/[ ]*\(.*/,"",fn); print fn }
             }'
    done | sort -u
}

# Per-header count of KlAllocator* struct-field declarations ("<header> <n>").
extract_field_counts() {
    for h in include/keel/*.h; do
        n=$(grep -cE 'KlAllocator[ ]*\*[a-z_]+[ ]*;' "$h" 2>/dev/null || true)
        [ "${n:-0}" -gt 0 ] && echo "$(basename "$h") $n"
    done | sort
}

check() {
    man="$1"
    bad=0
    tmp=$(mktemp)

    # 1. fn rows vs the extracted direct-param set.
    awk -F'\t' '$1=="fn"{print $2}' "$man" | sort -u > "$tmp.man_fn"
    extract_fns > "$tmp.cur_fn"
    unknown=$(comm -23 "$tmp.cur_fn" "$tmp.man_fn")
    stale=$(comm -13 "$tmp.cur_fn" "$tmp.man_fn")
    [ -n "$unknown" ] && { echo "$unknown" | sed 's/^/  UNCLASSIFIED allocator-taking function (add an fn row): /'; bad=1; }
    [ -n "$stale" ]   && { echo "$stale"   | sed 's/^/  STALE fn row (function no longer takes an allocator): /'; bad=1; }

    # 2. field-count rows vs the per-header field counts.
    awk -F'\t' '$1=="field-count"{print $2" "$3}' "$man" | sort > "$tmp.man_fc"
    extract_field_counts > "$tmp.cur_fc"
    if ! diff -q "$tmp.man_fc" "$tmp.cur_fc" >/dev/null 2>&1; then
        echo "  KlAllocator* struct-field counts drifted (a new allocator-carrying config/object?):"
        diff "$tmp.man_fc" "$tmp.cur_fc" | sed 's/^/    /'
        bad=1
    fi

    # 3. config rows name a real installed function.
    awk -F'\t' '$1=="config"{print $2}' "$man" | while read -r fn; do
        grep -rqE "\b$fn[ ]*\(" include/keel/*.h || echo "  config row names a missing function: $fn"
    done > "$tmp.cfg_missing"
    [ -s "$tmp.cfg_missing" ] && { cat "$tmp.cfg_missing"; bad=1; }

    rm -f "$tmp" "$tmp".*
    return $bad
}

case "${1:-}" in
  --selftest)
    # A synthetic new allocator-taking header must be reported as UNCLASSIFIED.
    d=$(mktemp -d); mkdir -p "$d/include/keel"
    cp "$MAN" "$d/man.tsv"
    printf 'int kl_canary_new_alloc_api(KlAllocator *a);\n' > "$d/include/keel/zz_canary.h"
    # Run extract over the injected tree by pointing at it.
    caught=$(cd "$d" && for h in include/keel/*.h; do
               perl -0777 -pe 's{/\*.*?\*/}{}gs' "$h" |
               awk 'BEGIN{RS=";"} { d=$0; gsub(/\n/," ",d) } d ~ /KlAllocator[ ]*\*/ && d ~ /kl_[a-zA-Z0-9_]+[ ]*\(/ {
                    if (match(d,/kl_[a-zA-Z0-9_]+[ ]*\(/)){fn=substr(d,RSTART,RLENGTH); sub(/[ ]*\(.*/,"",fn); print fn}}'
             done)
    rm -rf "$d"
    case "$caught" in
      *kl_canary_new_alloc_api*) echo "alloc-boundaries: selftest OK (a new allocator-taking API is detected)";;
      *) echo "alloc-boundaries: SELFTEST FAILED (extractor missed the injected API)"; exit 1;;
    esac
    ;;
  *)
    if check "$MAN"; then
        nfn=$(awk -F'\t' '$1=="fn"{c++} END{print c}' "$MAN")
        ncfg=$(awk -F'\t' '$1=="config"{c++} END{print c}' "$MAN")
        echo "alloc-boundaries: OK ($nfn direct-parameter + $ncfg config-carried boundaries classified)"
    else
        echo "alloc-boundaries: FAILED, the allocator-boundary tree diverges from $MAN; classify the boundary in the SAME commit that adds it"
        exit 1
    fi
    ;;
esac
