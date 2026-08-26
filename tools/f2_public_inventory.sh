#!/bin/sh
# tools/f2_public_inventory.sh - F2-1a public-surface inventory generator and scaffold checker.
#
# Derives reproducible inventories from the installed public headers (include/keel/*.h) and seeds two
# human-owned classification scaffolds. This is DATA scaffolding for the F2 public-surface work: it
# makes no API change, edits no header, and wires no gate. The decisions that consume these
# inventories (type classification in F2-B, coverage classification in F2-4) are made by editing the
# seeded scaffolds, not this generator.
#
# Outputs (under docs/f2/):
#   public_headers.txt        - one installed header basename per line (from git ls-files).
#   public_functions.tsv      - "<function>\t<header>": distinct public kl_* function declarations.
#   public_types.tsv          - "<type>\t<kind>\t<header>": the COMPLETE public type surface, one row
#                               per type. kind is one of:
#                                 struct|union|enum  concrete body on the main (non-detail) surface
#                                 opaque             forward-declared handle; layout in a *_detail.h
#                                                    (opt-in) or private (src/), not on the main surface
#                                 callback           function-pointer typedef (an API signature, NOT a
#                                                    layout - kept distinct so F2-B never confuses a
#                                                    callback signature with an object layout)
#                                 alias              typedef alias to a primitive/other type
#   type_classification.tsv   - seeded once: "<header>\t<type>\t<kind>\t<category>\t<note>" (UNRESOLVED).
#   function_coverage.tsv     - seeded once: "<function>\t<header>\t<classification>\t<citation>".
#
# The raw inventories are regenerated on every run (authoritative, tree-derived). The two
# classification scaffolds are seeded ONLY when absent so a re-run never clobbers reviewed decisions;
# new keys are appended with default (UNRESOLVED / unreviewed) rows.
#
# Function extraction is a documented heuristic (comment-stripped, preprocessor-stripped,
# statement-split, typedefs and function-pointer vtable fields excluded). An AST-grade extractor is
# the F2-4 concern; this heuristic is the reviewable starting point, not the permanent gate. The
# --selftest mode canaries the extractor against a synthetic fixture (see below).
#
# Usage:
#   tools/f2_public_inventory.sh             # (re)generate inventories, seed scaffolds, print counts
#   tools/f2_public_inventory.sh --check     # verify the checked-in inventories reproduce AND that
#                                            # both scaffolds are exact key-set joins (missing/extra/
#                                            # duplicate/malformed/drifted keys fail); non-zero on any
#   tools/f2_public_inventory.sh --selftest  # run the extraction canaries on a synthetic fixture
set -eu

ROOT=$(git rev-parse --show-toplevel 2>/dev/null || echo .)
cd "$ROOT"
OUT=docs/f2

# Allowed classification vocabularies. During the undecided stage every row is UNRESOLVED / unreviewed;
# later gates (F2-B, F2-4) tighten by forbidding those defaults. The full vocabulary is accepted now so
# a partially-decided scaffold still passes --check.
TYPE_CATEGORIES='UNRESOLVED caller-constructed caller-inspectable opaque opt-in-unstable-layout impl-layout-installed unresolved-v3-decision api-signature type-alias'
FN_CLASSES='unreviewed direct-assertion indirect-execution compile-only example'

# --- extractors (operate on the header paths in "$@") -------------------------

emit_headers() {
    for h in "$@"; do basename "$h"; done | LC_ALL=C sort
}

# Distinct public kl_* function declarations, attributed to the declaring header.
emit_functions() {
    for h in "$@"; do
        base=$(basename "$h")
        perl -0777 -ne '
            s{/\*.*?\*/}{ }gs; s{//[^\n]*}{ }g;        # strip comments
            my @keep; for my $l (split /\n/, $_) { next if $l =~ /^\s*#/; push @keep, $l; }
            my $s = join(" ", @keep);
            my %seen;
            for my $st (split /[;{}]/, $s) {
                next if $st =~ /\btypedef\b/;           # not a function decl
                next if $st =~ /\(\s*\*/;               # function-pointer field: (*name)(
                if ($st =~ /\b(kl_[A-Za-z0-9_]+)\s*\(/) {
                    my $n = $1;
                    next unless $st =~ /\S\s+\Q$n\E\s*\(/ || $st =~ /\*\s*\Q$n\E\s*\(/;
                    $seen{$n} = 1;
                }
            }
            print "$_\n" for keys %seen;
        ' "$h" | while IFS= read -r fn; do printf '%s\t%s\n' "$fn" "$base"; done
    done | LC_ALL=C sort -u
}

# The complete public type surface: concrete bodies (typedef-closer AND tag-form), opaque handles,
# callback typedefs, and primitive aliases; one resolved row per type. Headers are processed in
# sorted order so the canonical header of an opaque handle (alphabetically-first non-detail declarer)
# is deterministic. Runs in a single perl pass so precedence and dedup are cross-header.
emit_types() {
    for h in "$@"; do printf '%s\n' "$h"; done | LC_ALL=C sort | perl -ne '
        chomp(my $path = $_);
        push @FILES, $path;
    END {
        my (%concrete_hdr, %is_enum, %is_union, %cb_hdr, %alias_hdr, %handle_hdr);
        for my $path (@FILES) {
            local $/; open my $fh, "<", $path or next; my $src = <$fh>; close $fh;
            my $base = $path; $base =~ s{.*/}{};
            my $detail = ($base =~ /_detail\.h$/) ? 1 : 0;
            $src =~ s{/\*.*?\*/}{ }gs; $src =~ s{//[^\n]*}{ }g;   # strip comments
            if (!$detail) {
                # tag-form or typedef-with-tag bodies: "struct|union|enum KlName {"
                while ($src =~ /\b(struct|union|enum)\s+(Kl\w+)\s*\{/gs) {
                    my ($kw, $n) = ($1, $2);
                    $concrete_hdr{$n} //= $base;
                    $is_enum{$n} = 1 if $kw eq "enum";
                    $is_union{$n} = 1 if $kw eq "union";
                }
                # anonymous typedef closers: a line "} KlName;"
                for my $line (split /\n/, $src) {
                    $concrete_hdr{$1} //= $base if $line =~ /^\}\s*(Kl\w+)\s*;/;
                }
                # flat anonymous typedef bodies (single statement, no nested braces): sets the
                # concrete marker AND the kind, covering closers not anchored at column 0.
                while ($src =~ /typedef\s+(struct|union|enum)\b[^{}]*\{[^{}]*\}\s*(Kl\w+)\s*;/gs) {
                    my ($kw, $n) = ($1, $2);
                    $concrete_hdr{$n} //= $base;
                    $is_enum{$n}  = 1 if $kw eq "enum";
                    $is_union{$n} = 1 if $kw eq "union";
                }
                # multi-line enum/union closers (bodies whose "} Name;" is at column 0):
                while ($src =~ /typedef\s+enum\b[^;]*?\}\s*(Kl\w+)\s*;/gs)      { $is_enum{$1}  = 1 }
                while ($src =~ /typedef\s+union\b[^{]*\{.*?\}\s*(Kl\w+)\s*;/gs) { $is_union{$1} = 1 }
            }
            # callback typedefs (any header): "typedef ... (*KlName)( ... )"
            while ($src =~ /typedef\s+[^;{}]*\(\s*\*\s*(Kl\w+)\s*\)\s*\(/gs) { $cb_hdr{$1} //= $base }
            # primitive aliases: "typedef <rhs> KlName;" with no (* and no struct/union/enum tag.
            # Accepts the CamelCase Kl* convention plus the intentionally-public lowercase kl_* typedef
            # form (currently only kl_ssize_t). A typedef keyword is required, so struct fields and
            # arbitrary lowercase implementation identifiers are never collected.
            while ($src =~ /typedef\s+([^;{}]*?)\b((?:Kl|kl_)\w+)\s*;/gs) {
                my ($rhs, $n) = ($1, $2);
                next if $rhs =~ /\(\s*\*/;
                next if $rhs =~ /\b(struct|union|enum)\b/;
                $alias_hdr{$n} //= $base;
            }
            # opaque handle declarers (canonical header = first non-detail declarer):
            while ($src =~ /typedef\s+struct\s+(Kl\w+)\s+(Kl\w+)\s*;/gs) { $handle_hdr{$2} //= $base unless $detail }
            while ($src =~ /(?<!typedef\s)\bstruct\s+(Kl\w+)\s*;/gs)     { $handle_hdr{$1} //= $base unless $detail }
        }
        my (%row);
        # precedence: concrete > callback > alias > opaque
        for my $n (keys %concrete_hdr) {
            my $kind = $is_enum{$n} ? "enum" : $is_union{$n} ? "union" : "struct";
            $row{$n} = "$kind\t$concrete_hdr{$n}";
        }
        for my $n (keys %cb_hdr)     { next if $row{$n}; $row{$n} = "callback\t$cb_hdr{$n}" }
        for my $n (keys %alias_hdr)  { next if $row{$n}; $row{$n} = "alias\t$alias_hdr{$n}" }
        for my $n (keys %handle_hdr) { next if $row{$n}; $row{$n} = "opaque\t$handle_hdr{$n}" }
        for my $n (sort keys %row) { print "$n\t$row{$n}\n" }
    }'
}

HEADERS_LIST() { git ls-files 'include/keel/*.h'; }

# value_ok <value> <allowed-space-list>  (accepts the intentional-public-but-untested:* prefix)
value_ok() {
    case "$1" in intentional-public-but-untested:?*) return 0;; esac
    # shellcheck disable=SC2086  # $2 is a space-separated allow-list, split on purpose
    for a in $2; do [ "$1" = "$a" ] && return 0; done
    return 1
}

# --- modes --------------------------------------------------------------------

MODE=generate
case "${1:-}" in --check) MODE=check;; --selftest) MODE=selftest;; esac

if [ "$MODE" = selftest ]; then
    fx=$(mktemp -d); trap 'rm -rf "$fx"' EXIT INT TERM
    # fixture_a.h: every extraction canary in one header.
    cat > "$fx/fixture_a.h" <<'H'
#ifndef FIXTURE_A
#define FIXTURE_A
typedef struct KlCanaryStruct { int a; int b; } KlCanaryStruct;   /* concrete struct */
typedef union { int i; float f; } KlCanaryUnion;                  /* concrete union  */
typedef enum { KL_CANARY_A, KL_CANARY_B } KlCanaryEnum;           /* concrete enum   */
typedef struct KlCanaryOpaque KlCanaryOpaque;                     /* opaque handle   */
typedef int (*KlCanaryFn)(int x, void *u);                        /* callback typedef */
typedef long kl_canary_ssize_t;                                  /* lowercase public alias */
int kl_canary_normal(void);
int
kl_canary_multiline(
    int a,
    int b);
#ifdef KL_CANARY_GUARD
int kl_canary_guarded(void);
#endif
int kl_canary_dup(void);
/* int kl_canary_fake(void); struct KlCanaryFakeInComment { int x; } KlCanaryFakeInComment; */
#endif
H
    # fixture_b.h: a second umbrella re-declaring the same function (duplicate across headers).
    cat > "$fx/fixture_b.h" <<'H'
#ifndef FIXTURE_B
#define FIXTURE_B
int kl_canary_dup(void);
#endif
H
    types=$(emit_types "$fx/fixture_a.h" "$fx/fixture_b.h")
    funcs=$(emit_functions "$fx/fixture_a.h" "$fx/fixture_b.h")
    bad=0
    expect_type() {
        got=$(printf '%s\n' "$types" | awk -F'\t' -v n="$1" '$1==n{print $2}')
        if [ "$got" != "$2" ]; then echo "selftest: type $1 kind='$got' expected='$2'"; bad=1; fi
    }
    expect_no_type() {
        if printf '%s\n' "$types" | awk -F'\t' -v n="$1" '$1==n{f=1} END{exit !f}'; then
            echo "selftest: type $1 must NOT appear (fake/comment)"; bad=1; fi
    }
    expect_fn() {
        if ! printf '%s\n' "$funcs" | awk -F'\t' -v n="$1" '$1==n{f=1} END{exit !f}'; then
            echo "selftest: function $1 must appear"; bad=1; fi
    }
    expect_no_fn() {
        if printf '%s\n' "$funcs" | awk -F'\t' -v n="$1" '$1==n{f=1} END{exit !f}'; then
            echo "selftest: function $1 must NOT appear (fake/comment)"; bad=1; fi
    }
    expect_type KlCanaryStruct struct
    expect_type KlCanaryUnion  union
    expect_type KlCanaryEnum   enum
    expect_type KlCanaryOpaque opaque
    expect_type KlCanaryFn     callback          # a callback must NOT be an object layout
    expect_type kl_canary_ssize_t alias          # intentionally-public lowercase kl_* typedef
    expect_no_type KlCanaryFakeInComment
    expect_fn kl_canary_normal
    expect_fn kl_canary_multiline               # multiline declaration
    expect_fn kl_canary_guarded                 # inside a preprocessor guard
    expect_fn kl_canary_dup
    expect_no_fn kl_canary_fake
    # duplicate across two headers: one unique name, two declarations
    dups=$(printf '%s\n' "$funcs" | awk -F'\t' '$1=="kl_canary_dup"' | wc -l | tr -d ' ')
    [ "$dups" = 2 ] || { echo "selftest: kl_canary_dup expected 2 declarations, got $dups"; bad=1; }
    [ "$bad" = 0 ] && echo "selftest: all extraction canaries passed"
    exit "$bad"
fi

if [ "$MODE" = check ]; then
    tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT INT TERM
    # shellcheck disable=SC2046
    set -- $(HEADERS_LIST)
    emit_headers   "$@" > "$tmp/public_headers.txt"
    emit_functions "$@" > "$tmp/public_functions.tsv"
    emit_types     "$@" > "$tmp/public_types.tsv"
    bad=0
    for f in public_headers.txt public_functions.tsv public_types.tsv; do
        if [ ! -f "$OUT/$f" ]; then echo "f2 --check: MISSING $OUT/$f"; bad=1; continue; fi
        if ! diff -u "$OUT/$f" "$tmp/$f" >/dev/null 2>&1; then
            echo "f2 --check: STALE $OUT/$f (regenerate with tools/f2_public_inventory.sh)"
            diff -u "$OUT/$f" "$tmp/$f" | sed -n '1,12p'; bad=1
        fi
    done

    # type_classification.tsv must be an exact join over the (header,type,kind) key of public_types.tsv.
    tc="$OUT/type_classification.tsv"
    if [ ! -f "$tc" ]; then echo "f2 --check: MISSING $tc"; bad=1; else
        awk -F'\t' '{print $3"\t"$1"\t"$2}' "$tmp/public_types.tsv" | LC_ALL=C sort > "$tmp/inv_typekeys"
        awk -F'\t' 'NR>1{print $1"\t"$2"\t"$3}' "$tc" | LC_ALL=C sort > "$tmp/sc_typekeys"
        dupk=$(awk -F'\t' 'NR>1{print $1"\t"$2"\t"$3}' "$tc" | LC_ALL=C sort | uniq -d)
        [ -n "$dupk" ] && { echo "f2 --check: duplicate type_classification keys:"; printf '%s\n' "$dupk"; bad=1; }
        miss=$(comm -23 "$tmp/inv_typekeys" "$tmp/sc_typekeys")
        extra=$(comm -13 "$tmp/inv_typekeys" "$tmp/sc_typekeys")
        [ -n "$miss" ]  && { echo "f2 --check: type_classification MISSING rows (drift/new type):"; printf '%s\n' "$miss"  | sed 's/^/  /'; bad=1; }
        [ -n "$extra" ] && { echo "f2 --check: type_classification STALE/EXTRA rows (removed type or header drift):"; printf '%s\n' "$extra" | sed 's/^/  /'; bad=1; }
        # shellcheck disable=SC2034  # note is read to consume the column, not used
        while IFS='	' read -r hdr typ knd cat note; do
            [ "$hdr" = header ] && continue
            value_ok "$cat" "$TYPE_CATEGORIES" || { echo "f2 --check: type_classification bad category '$cat' for $typ"; bad=1; }
        done < "$tc"
    fi

    # function_coverage.tsv must be an exact join over the function name; header must match inventory.
    fc="$OUT/function_coverage.tsv"
    if [ ! -f "$fc" ]; then echo "f2 --check: MISSING $fc"; bad=1; else
        cut -f1 "$tmp/public_functions.tsv" | LC_ALL=C sort -u > "$tmp/inv_fn"
        awk -F'\t' 'NR>1{print $1}' "$fc" | LC_ALL=C sort > "$tmp/sc_fn_all"
        LC_ALL=C sort -u "$tmp/sc_fn_all" > "$tmp/sc_fn"
        dupf=$(uniq -d "$tmp/sc_fn_all")
        [ -n "$dupf" ] && { echo "f2 --check: duplicate function_coverage keys:"; printf '%s\n' "$dupf"; bad=1; }
        miss=$(comm -23 "$tmp/inv_fn" "$tmp/sc_fn")
        extra=$(comm -13 "$tmp/inv_fn" "$tmp/sc_fn")
        [ -n "$miss" ]  && { echo "f2 --check: function_coverage MISSING rows:"; printf '%s\n' "$miss"  | sed 's/^/  /'; bad=1; }
        [ -n "$extra" ] && { echo "f2 --check: function_coverage STALE/EXTRA rows:"; printf '%s\n' "$extra" | sed 's/^/  /'; bad=1; }
        # shellcheck disable=SC2034  # cit is read to consume the column, not used
        while IFS='	' read -r fn hdr cls cit; do
            [ "$fn" = function ] && continue
            value_ok "$cls" "$FN_CLASSES" || { echo "f2 --check: function_coverage bad classification '$cls' for $fn"; bad=1; }
            if ! awk -F'\t' -v f="$fn" -v hh="$hdr" '$1==f && $2==hh{ok=1} END{exit !ok}' "$tmp/public_functions.tsv"; then
                echo "f2 --check: function_coverage header drift for $fn (header '$hdr' not a declarer)"; bad=1; fi
        done < "$fc"
    fi

    [ "$bad" = 0 ] && echo "f2 --check: inventories reproduce and both scaffolds are exact joins"
    exit "$bad"
fi

# --- generate mode ------------------------------------------------------------

# shellcheck disable=SC2046
set -- $(HEADERS_LIST)
mkdir -p "$OUT"
emit_headers   "$@" > "$OUT/public_headers.txt"
emit_functions "$@" > "$OUT/public_functions.tsv"
emit_types     "$@" > "$OUT/public_types.tsv"

# Seed type_classification.tsv: one row per public type, category UNRESOLVED (F2-B decides).
tc="$OUT/type_classification.tsv"
[ -f "$tc" ] || printf 'header\ttype\tkind\tcategory\tnote\n' > "$tc"
while IFS='	' read -r typ knd base; do
    LC_ALL=C awk -F'\t' -v t="$typ" 'NR>1 && $2==t{f=1} END{exit !f}' "$tc" && continue
    printf '%s\t%s\t%s\tUNRESOLVED\t-\n' "$base" "$typ" "$knd" >> "$tc"
done < "$OUT/public_types.tsv"

# Seed function_coverage.tsv: one row per function, classification unreviewed (F2-4 decides).
fc="$OUT/function_coverage.tsv"
[ -f "$fc" ] || printf 'function\theader\tclassification\tcitation\n' > "$fc"
while IFS='	' read -r fn base; do
    LC_ALL=C grep -q "^$fn	" "$fc" 2>/dev/null && continue
    printf '%s\t%s\tunreviewed\t-\n' "$fn" "$base" >> "$fc"
done < "$OUT/public_functions.tsv"

echo "headers:   $(wc -l < "$OUT/public_headers.txt" | tr -d ' ')"
echo "functions: $(wc -l < "$OUT/public_functions.tsv" | tr -d ' ')  (unique names: $(cut -f1 "$OUT/public_functions.tsv" | sort -u | wc -l | tr -d ' '))"
echo "types:     $(wc -l < "$OUT/public_types.tsv" | tr -d ' ')"
for k in struct union enum opaque callback alias; do
    printf '  %-9s %s\n' "$k" "$(awk -F'\t' -v k="$k" '$2==k' "$OUT/public_types.tsv" | wc -l | tr -d ' ')"
done
echo "type_classification rows (excl header): $(awk 'NR>1' "$tc" | wc -l | tr -d ' ')"
echo "function_coverage rows (excl header):   $(awk 'NR>1' "$fc" | wc -l | tr -d ' ')"
