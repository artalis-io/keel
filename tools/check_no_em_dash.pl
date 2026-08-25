#!/usr/bin/env perl
# tools/check_no_em_dash.pl - permanent gate: no em-dash (U+2014) in any tracked text (C3-7).
#
# The em-dash is a stylistic tell; the house style uses ASCII punctuation. This gate rejects the
# exact codepoint U+2014 anywhere in a tracked, non-binary file, so it cannot return.
#
# Robustness:
#   - The file list comes from `git ls-files -z` (NUL-delimited), so a legal filename containing a
#     newline (or |, &, backslash, spaces) is handled correctly.
#   - Files are read as RAW BYTES. A file containing a NUL byte is treated as binary and skipped (and
#     not counted); only scanned text files are counted.
#   - Only vendor/ is excluded (.git/ is untracked and never appears). There is NO format allowlist
#     and NO other exception - a future tracked text format is covered automatically.
#   - Matches are reported as file:line printed directly (filenames are never routed through sed or a
#     shell), so odd filenames cannot corrupt the diagnostic.
#
# Byte-exact and self-referential-safe: this script contains NO non-ASCII bytes. The four glyphs it
# reasons about are written as \x byte escapes, so the gate scanning its own source never trips on
# itself and its diagnostics are em-dash-free. Positive + negative self-canaries run before the tree
# scan: U+2014 MUST match; box-drawing U+2500, en-dash U+2013 and minus U+2212 MUST NOT - proving it
# targets exactly one codepoint and leaves the glyphs C3 deliberately keeps in place.

use strict;
use warnings;

my $EM    = "\xE2\x80\x94";   # U+2014 EM DASH (UTF-8 bytes)
my $BOX   = "\xE2\x94\x80";   # U+2500 BOX DRAWINGS LIGHT HORIZONTAL
my $EN    = "\xE2\x80\x93";   # U+2013 EN DASH
my $MINUS = "\xE2\x88\x92";   # U+2212 MINUS SIGN

sub has_em { return index($_[0], $EM) >= 0 }

# -- self-canaries --------------------------------------------------------------------------------
has_em("before${EM}after")
    or die "check-no-em-dash: SELF-TEST FAILED - U+2014 was not detected\n";
for my $g ($BOX, $EN, $MINUS) {
    if (has_em("before${g}after")) {
        die "check-no-em-dash: SELF-TEST FAILED - a protected glyph (U+2500 / U+2013 / U+2212) "
          . "was matched as an em-dash\n";
    }
}

# -- scan every tracked non-binary file (vendor/ excluded) ----------------------------------------
open(my $ls, '-|', 'git', 'ls-files', '-z') or die "check-no-em-dash: cannot run git ls-files: $!\n";
local $/ = "\0";

my $scanned = 0;
my @hits;
while (defined(my $path = <$ls>)) {
    $path =~ s/\0\z//;                       # strip the NUL delimiter
    next if $path eq '' || $path =~ m{^vendor/};
    open(my $fh, '<:raw', $path) or next;    # unreadable -> skip (not counted)
    my $data;
    { local $/; $data = <$fh>; }             # slurp raw bytes; $/ restored for the next <$ls>
    close $fh;
    $data = '' unless defined $data;
    next if index($data, "\x00") >= 0;       # binary (NUL) -> skip, not counted
    $scanned++;
    next unless has_em($data);
    my $lineno = 0;
    for my $line (split /\n/, $data, -1) {
        $lineno++;
        push @hits, "  $path:$lineno: $line" if has_em($line);
    }
}
close $ls;

if (@hits) {
    print "$_\n" for @hits;
    print "check-no-em-dash: FAILED - an em-dash (U+2014) is present in tracked text. Replace it with\n";
    print "context-appropriate ASCII punctuation: a period or semicolon for a clause break, a colon for\n";
    print "a definition or list introduction, parentheses or commas for an aside, an ASCII hyphen for a\n";
    print "genuine label or range.\n";
    exit 1;
}
print "check-no-em-dash: OK ($scanned tracked non-binary files scanned; no U+2014; "
    . "vendor/ excluded, no other exceptions)\n";
exit 0;
