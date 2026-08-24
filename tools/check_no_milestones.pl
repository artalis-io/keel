#!/usr/bin/env perl
# tools/check_no_milestones.pl: permanent comments-only milestone/phase archaeology gate (C2-6).
#
# Scans every tracked first-party .c/.h (vendor/ excluded) and rejects historical milestone /
# phase / finding / step token families in COMMENTS. Real C-comment extraction (a per-file state
# machine over /* */, //, "strings", 'chars') means string literals, including the UEFI/lwIP
# self-test CI-oracle output markers ("6.4c: GO", "S-4: GO", "P9-3 PASS"), are NATURALLY ignored;
# only comment text is matched. There are NO file, line, or marker exceptions.
#
# Historical-token matching is deliberately PRECISE so legitimate local scenario catalogs pass
# (the lwIP raw_send B1..B11 index and raw_dns B1/B2), and so ordinary text is not caught:
#   - "UTF-8" is not F-8 (the F families require a word boundary before F),
#   - "E2E" is not E2, "phase10_foo.md" is not "Phase 1" (the phase family requires a separator),
#   - "two-phase" / "lifecycle phase" never reach a digit.
# Design/history docs under docs/ are NOT scanned; their recorded milestone history stands, exactly
# as the other stale-name gates treat docs.
#
# Positive AND negative self-canaries run BEFORE the tree scan and abort loudly if the extractor or
# the pattern set regresses (a false-negative that would let archaeology through, or a false-positive
# that would flag a valid catalog / a string literal). Diagnostics are file:line with the token.

use strict;
use warnings;

# ── The historical milestone/phase/finding/step token families (precise) ───────────────────────────
my $TOKEN_RE = qr{
      \bPhase[ -][0-9A-F]             # capital Phase + sep + digit/letter: Phase 8g, Phase-B, Phase 10.
                                      #   Lowercase phase / two-phase prose is intentionally not matched.
    | \bPAL\ [Pp]hase\ [0-9]          # PAL Phase 4
    | \b7[AB]-[0-9]                   # 7A-1, 7B-2a
    | \b8[a-g]-[0-9]                  # 8a-1, 8f-5a, 8g-2
    | \b6B-[0-9]                      # 6B-3
    | \bLC-[0-9]                      # LC-3a
    | \bRC-[0-9]                      # RC-2
    | \bP9-[0-9]                      # P9-3
    | \bR[0-9][a-z]\b                 # R2f, R3b
    | \bR-[0-9]\b                     # R-1
    | \bS[0-9]\.[0-9]\b               # S3.2
    | \bS-[0-9]\b                     # S-4
    | \bU-[0-9]\b                     # U-1
    | \bM[0-9]\.[0-9]                 # M5.2
    | \bD-[A-Z]{3,}                   # D-DNS
    | \b[Ss]tep[ -][0-9][a-z]\b       # Step 2a: a lettered step id. Bare step N prose is not a
                                      #   milestone; step 6B-1 / 7B-2 are already caught by the 6B/7B rules.
    | \bstep\ B[0-9]\b                # step B1
    | \bGoal\ [0-9]\b                 # Goal 8
    | \bFindings?\ [0-9A-Z]           # Finding 3, Findings A
    | \bfinding\ [0-9]                # finding 1
    | \bF-[0-9]\b                     # F-4  (\b before F excludes UTF-8's "F-8")
    | \b6\.4[a-c]\b                   # 6.4c
    | \b2b-ii\b                       # 2b-ii
    | \bE2\b                          # E2   (E2E excluded by the trailing \b)
    | \bP1(-[0-9A-Z])?\b              # P1, P1-2, P1-A
    | \bD[12]\b                       # D1, D2
}x;

# ── Comment extractor. Returns [physical_line, comment_fragment, matched_token] for every milestone
#    match in COMMENT text (only). Implements C translation phase 2 (backslash-newline line splicing)
#    BEFORE tokenizing, so lexical state (string/char literals, // and /* */ comments) is preserved
#    across a spliced line exactly as a C compiler sees it. This closes both a false-positive (a
#    /* ... */ that lives inside a string continued over a splice) and a bypass (a banned token on the
#    continued physical line of a // comment). Diagnostics report the ORIGINAL physical line: the
#    splice deletes the backslash+newline but each surviving character keeps its source line number,
#    and a comment "run" (a maximal /* */ or // span) is scanned as one string with every match mapped
#    back to the physical line of its first character, so a token split across the splice, or one on a
#    continued line, is reported where it truly sits. ─────────────────────────────────────────────────
sub scan_text {
    my ($text) = @_;

    # Phase 2: splice. Build a character stream where each surviving char keeps its physical line.
    my @ch;                                     # [character, physical_line]
    my @c = split //, $text, -1;
    my $n = @c;
    my $line = 1;
    my $i = 0;
    while ($i < $n) {
        my $x = $c[$i];
        if ($x eq "\\") {                       # backslash + (CR)LF is deleted → the lines join
            if    ($i + 1 < $n && $c[$i + 1] eq "\n")                        { $line++; $i += 2; next; }
            elsif ($i + 2 < $n && $c[$i + 1] eq "\r" && $c[$i + 2] eq "\n")  { $line++; $i += 3; next; }
        }
        push @ch, [ $x, $line ];
        $line++ if $x eq "\n";
        $i++;
    }

    # State machine over the spliced stream. A comment run is collected with its per-char line map and
    # scanned when it ends (or at EOF for an unterminated run).
    my @hits;
    my @run;                                    # current comment run: [char, physical_line]
    my $flush = sub {
        return unless @run;
        my $str = join('', map { $_->[0] } @run);
        while ($str =~ /$TOKEN_RE/g) {
            my ($tok, $start) = ($&, $-[0]);
            my $pl = $run[$start][1];
            my $frag = join('', map { $_->[0] } grep { $_->[1] == $pl } @run);   # this line's comment text
            push @hits, [ $pl, $frag, $tok ];
        }
        @run = ();
    };

    my $state = 'code';                         # code | block | line | string | char
    my $m = @ch;
    my $j = 0;
    while ($j < $m) {
        my $x  = $ch[$j][0];
        my $x2 = ($j + 1 < $m) ? $x . $ch[$j + 1][0] : $x;
        if ($state eq 'block') {
            if ($x2 eq '*/') { $j += 2; $state = 'code'; $flush->(); }
            else             { push @run, $ch[$j]; $j++; }
        }
        elsif ($state eq 'line') {
            if ($x eq "\n")  { $j++; $state = 'code'; $flush->(); }   # a REAL newline ends // (spliced ones were removed)
            else             { push @run, $ch[$j]; $j++; }
        }
        elsif ($state eq 'string' || $state eq 'char') {
            my $q = ($state eq 'string') ? '"' : "'";
            if    ($x eq "\\") { $j += 2; }                          # escaped char (incl. \" \') never terminates
            elsif ($x eq $q)   { $j++; $state = 'code'; }
            elsif ($x eq "\n") { $j++; $state = 'code'; }            # defensive: an unterminated literal never runs away
            else               { $j++; }
        }
        else {                                                      # code
            if    ($x2 eq '/*') { $state = 'block';  $j += 2; }
            elsif ($x2 eq '//') { $state = 'line';   $j += 2; }
            elsif ($x eq '"')   { $state = 'string'; $j++; }
            elsif ($x eq "'")   { $state = 'char';   $j++; }
            else                { $j++; }
        }
    }
    $flush->();                                 # EOF: flush a trailing unterminated block/line comment
    return @hits;
}

# ── Self-canaries: MUST flag every milestone form; MUST NOT flag valid catalogs, ordinary text, or
#    string literals. Any failure means the scanner regressed; abort before trusting a clean tree. ─
sub selftest {
    my @must_flag = (
        '/* Phase 8g streaming */', '/* step 6B-1 */', '/* 7B-2a carve */', '/* 8f-5a provider */',
        '/* LC-3a PASS */', '/* P9-3 buffered */', '/* R2f review */', '/* R-1 retire */',
        '/* S-4: listening */', '/* U-1 allocator */', '/* Finding 3 */', '/* the F-4 helper */',
        '/* 6.4c GO */', '/* blocker P1-2 */', '/* D1: convenience */', '/* the E2 rules */',
        '/* Goal 8 */', '/* PAL Phase 4 */', '/* Phase-B transport */', '/* 2b-ii window */',
        '/* Step 2a rework */', '/* step 6B-1 embed */', '/* step B1 link proof */',
        # line-splicing: a banned token on the CONTINUED physical line of a // comment must be caught,
        # and reported on its true physical line (2).
        "// intro note \\\nLC-3a on the next physical line\nint x;\n",
    );
    my @must_pass = (
        '/* B1 small response; B11 no-alloc; raw-DNS B1/B2 catalog */',   # local scenario catalogs
        '/* Incremental UTF-8 validator; HTTP/2 path; RFC 6455; SHA-256 */',  # standards / not F-8
        '/* an E2E loopback test; two-phase proof; lifecycle phase 1 of close */',  # ordinary words
        '/* the first step, step 3, reads headers; a two-phase handshake */',        # bare step/phase prose
        '/* see docs/archive/phases/phase10_uefi_feasibility_design.md */',  # doc-path citation
    );
    my @must_ignore_string = (
        'print_line("S-4: GO");',            # CI-oracle output marker in a string literal
        'const char *m = "6.4c: DONE Phase 8g";',
        "char q = '\"'; /* an escaped quote must not swallow the next token: fine */",
        # line-splicing: a milestone-looking /* ... */ INSIDE a string continued over a backslash-newline
        # is still string content; the splice must not let it be seen as a comment.
        "const char *s = \"start \\\n/* Phase 8g */ still inside the string\";\n",
        # line-splicing: an escaped quote formed across a continuation must not terminate the string, so
        # the following /* ... */ stays string content and is ignored.
        "const char *s = \"abc\\\n\\\" /* LC-3a */ still string\";\n",
    );
    my $bad = 0;
    for my $t (@must_flag) {
        unless (scan_text($t)) { print "  SELF-TEST FAILED: milestone token NOT flagged: $t\n"; $bad = 1; }
    }
    for my $t (@must_pass) {
        if (my @h = scan_text($t)) { print "  SELF-TEST FAILED: valid text WRONGLY flagged ($h[0][2]): $t\n"; $bad = 1; }
    }
    for my $t (@must_ignore_string) {
        if (my @h = scan_text($t)) { print "  SELF-TEST FAILED: string-literal token WRONGLY flagged ($h[0][2]): $t\n"; $bad = 1; }
    }
    if ($bad) { print "check-no-milestones: SELF-TEST FAILED; scanner regressed; not trusting the tree scan\n"; exit 1; }
}

# ── Main ───────────────────────────────────────────────────────────────────────────────────────────
selftest();

my @files = grep { $_ ne '' && !m{^vendor/} } split /\n/, `git ls-files '*.c' '*.h'`;
my $fail = 0;
my $scanned = 0;
for my $f (@files) {
    open my $fh, '<', $f or next;
    local $/;
    my $text = <$fh>;
    close $fh;
    $scanned++;
    for my $h (scan_text($text)) {
        my ($ln, $comment, $tok) = @$h;
        $comment =~ s/^\s+//; $comment =~ s/\s+$//; $comment =~ s/\s+/ /g;
        $comment = substr($comment, 0, 100);
        print "  $f:$ln: [$tok] $comment\n";
        $fail = 1;
    }
}

if ($fail) {
    print "check-no-milestones: FAILED; a comment carries a historical milestone/phase/finding/step\n";
    print "token. State the current behavior or invariant instead (the string literals + CI oracles are\n";
    print "untouched; only the comment must change). If this is a legitimate local scenario catalog,\n";
    print "make its labels descriptive rather than a bare milestone ID.\n";
    exit 1;
}
print "check-no-milestones: OK ($scanned first-party .c/.h scanned; comments-only, string literals ignored; no exceptions)\n";
exit 0;
