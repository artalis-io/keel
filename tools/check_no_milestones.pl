#!/usr/bin/env perl
# tools/check_no_milestones.pl — permanent comments-only milestone/phase archaeology gate (C2-6).
#
# Scans every tracked first-party .c/.h (vendor/ excluded) and rejects historical milestone /
# phase / finding / step token families in COMMENTS. Real C-comment extraction (a per-file state
# machine over /* */, //, "strings", 'chars') means string literals — including the UEFI/lwIP
# self-test CI-oracle output markers ("6.4c: GO", "S-4: GO", "P9-3 PASS") — are NATURALLY ignored;
# only comment text is matched. There are NO file, line, or marker exceptions.
#
# Historical-token matching is deliberately PRECISE so legitimate local scenario catalogs pass
# (the lwIP raw_send B1..B11 index and raw_dns B1/B2), and so ordinary text is not caught:
#   - "UTF-8" is not F-8 (the F families require a word boundary before F),
#   - "E2E" is not E2, "phase10_foo.md" is not "Phase 1" (the phase family requires a separator),
#   - "two-phase" / "lifecycle phase" never reach a digit.
# Design/history docs under docs/ are NOT scanned — their recorded milestone history stands, exactly
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

# ── Comment extractor: return [lineno, comment_text, matched_token] for lines whose COMMENT text
#    (only) matches $TOKEN_RE. A per-character state machine; string/char literals are skipped so
#    their contents can never match. ──────────────────────────────────────────────────────────────
sub scan_text {
    my ($text) = @_;
    my @hits;
    my $in_block = 0;                 # inside a /* ... */ that may span lines
    my $lineno = 0;
    for my $line (split /\n/, $text, -1) {
        $lineno++;
        my $comment = '';             # comment text seen on THIS line
        my $i = 0;
        my $len = length $line;
        while ($i < $len) {
            if ($in_block) {
                my $end = index($line, '*/', $i);
                if ($end >= 0) { $comment .= substr($line, $i, $end - $i); $i = $end + 2; $in_block = 0; }
                else           { $comment .= substr($line, $i); $i = $len; }
                next;
            }
            my $c  = substr($line, $i, 1);
            my $c2 = substr($line, $i, 2);
            if    ($c2 eq '/*') { $in_block = 1; $i += 2; }
            elsif ($c2 eq '//') { $comment .= substr($line, $i + 2); $i = $len; }   # to EOL
            elsif ($c eq '"' || $c eq "'") {                                        # skip literal
                my $q = $c; $i++;
                while ($i < $len) {
                    my $s = substr($line, $i, 1);
                    if    ($s eq '\\') { $i += 2; }
                    elsif ($s eq $q)   { $i++; last; }
                    else               { $i++; }
                }
            }
            else { $i++; }
        }
        if (length $comment && $comment =~ $TOKEN_RE) {
            push @hits, [ $lineno, $comment, $& ];
        }
    }
    return @hits;
}

# ── Self-canaries: MUST flag every milestone form; MUST NOT flag valid catalogs, ordinary text, or
#    string literals. Any failure means the scanner regressed — abort before trusting a clean tree. ─
sub selftest {
    my @must_flag = (
        '/* Phase 8g streaming */', '/* step 6B-1 */', '/* 7B-2a carve */', '/* 8f-5a provider */',
        '/* LC-3a PASS */', '/* P9-3 buffered */', '/* R2f review */', '/* R-1 retire */',
        '/* S-4: listening */', '/* U-1 allocator */', '/* Finding 3 */', '/* the F-4 helper */',
        '/* 6.4c GO */', '/* blocker P1-2 */', '/* D1: convenience */', '/* the E2 rules */',
        '/* Goal 8 */', '/* PAL Phase 4 */', '/* Phase-B transport */', '/* 2b-ii window */',
        '/* Step 2a rework */', '/* step 6B-1 embed */', '/* step B1 link proof */',
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
    if ($bad) { print "check-no-milestones: SELF-TEST FAILED — scanner regressed; not trusting the tree scan\n"; exit 1; }
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
    print "check-no-milestones: FAILED — a comment carries a historical milestone/phase/finding/step\n";
    print "token. State the current behavior or invariant instead (the string literals + CI oracles are\n";
    print "untouched; only the comment must change). If this is a legitimate local scenario catalog,\n";
    print "make its labels descriptive rather than a bare milestone ID.\n";
    exit 1;
}
print "check-no-milestones: OK ($scanned first-party .c/.h scanned; comments-only, string literals ignored; no exceptions)\n";
exit 0;
