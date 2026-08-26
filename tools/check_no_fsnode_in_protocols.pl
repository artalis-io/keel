#!/usr/bin/env perl
# check_no_fsnode_in_protocols.pl - ownership guard (axis boundary).
#
# The AF_UNIX listener's filesystem-node lifecycle (directory traversal, stale-node reclamation,
# ownership/mode mutation, teardown) is transport/socket-axis work and lives in the substrate module
# src/unix_socket_node*.c. It must NOT return to the protocol tree. This gate fails if a pathname
# open / mutation / node-identity syscall appears in code under src/protocols/.
#
# It uses a splice-aware C lexer (translation phase 2 line splicing, then a state machine over
# /* */, //, "strings", 'chars'), the same lexical model as tools/check_no_milestones.pl, so a call
# name that appears only inside a comment or a string literal never trips the gate, and a token
# inside a literal cannot bypass it. Uses only core Perl; git ls-files for the file set.

use strict;
use warnings;

# Pathname-open / mutation / node-identity syscall families that belong in the substrate node
# module, not protocols. The "at" and l-/f- variants are covered explicitly so the inventory matches
# the gate's "no filesystem-node syscalls" claim.
my @forbidden = qw(
    open openat creat
    remove unlink unlinkat rmdir
    rename renameat renameat2
    mkdir mkdirat mknod mknodat mkfifo mkfifoat
    link linkat symlink symlinkat
    chmod fchmod fchmodat lchmod
    chown fchown fchownat lchown
    stat lstat statx fstatat
);
my $re = qr/\b(?:@{[ join '|', @forbidden ]})\s*\(/;

# Self-canaries: the pattern detects a call, ignores a bare mention, and (via the lexer) ignores a
# call name that appears only inside a comment or a string literal.
die "check-no-fsnode: SELF-TEST FAILED (pattern misses a call)\n"      unless "x = unlinkat(d,b,0);" =~ $re;
die "check-no-fsnode: SELF-TEST FAILED (pattern hits a bare word)\n"   if     "the unlink happens later" =~ $re;
die "check-no-fsnode: SELF-TEST FAILED (lexer keeps a comment call)\n" if     code_only("/* chmod(x) */ int y;") =~ $re;
die "check-no-fsnode: SELF-TEST FAILED (lexer keeps a string call)\n"  if     code_only("s = \"symlink(a,b)\";") =~ $re;
die "check-no-fsnode: SELF-TEST FAILED (lexer drops real code)\n"      unless code_only("v = open(p); // note") =~ $re;

my @files = grep { m{^src/protocols/.*\.(?:c|h)$} } split /\0/, `git ls-files -z src/protocols`;
my @hits;
for my $f (@files) {
    open my $fh, '<', $f or die "cannot open $f: $!";
    local $/;
    my $raw = <$fh>;
    close $fh;
    my @code = code_only_lines($raw);   # [physical_line, code_text] per line, comments/strings blanked
    for my $e (@code) {
        push @hits, "$f:$e->[0]: $e->[1]" if $e->[1] =~ $re;
    }
}

if (@hits) {
    print STDERR "check-no-fsnode-in-protocols: FAILED -- filesystem-node syscall(s) in the protocol tree\n";
    print STDERR "  (these belong in the substrate module src/unix_socket_node*.c, not src/protocols/)\n";
    print STDERR "  $_\n" for @hits;
    exit 1;
}
print "check-no-fsnode-in-protocols: OK (" . scalar(@files) . " protocol files; no filesystem-node syscalls)\n";
exit 0;

# ── Splice-aware C lexer: return code-only text with comment/string/char content blanked. ──────
# Phase 2 line splicing (backslash-newline removed, each surviving char keeps its physical line),
# then a state machine that emits code chars verbatim and replaces comment/literal content with a
# space, preserving line numbers.
sub code_only_lines {
    my ($src) = @_;
    # Phase 2 splice: char stream with per-char physical line.
    my @s;                       # [char, line]
    my $line = 1;
    my @ch = split //, $src;
    for (my $i = 0; $i < @ch; $i++) {
        my $c = $ch[$i];
        if ($c eq "\\" && $i + 1 < @ch && $ch[$i + 1] eq "\n") { $line++; $i++; next; }  # spliced newline
        push @s, [$c, $line];
        $line++ if $c eq "\n";
    }
    # State machine.
    my @out;                     # [line, code_char]
    my $state = 'code';
    for (my $j = 0; $j < @s; $j++) {
        my ($c, $pl) = @{ $s[$j] };
        my $c2 = $c . (($j + 1 < @s) ? $s[$j + 1][0] : '');
        if ($state eq 'block') {
            if ($c2 eq '*/') { $j++; push @out, [$pl, ' ']; push @out, [$pl, ' ']; $state = 'code'; }
            else             { push @out, [$pl, ($c eq "\n" ? "\n" : ' ')]; }
        }
        elsif ($state eq 'line') {
            if ($c eq "\n") { push @out, [$pl, "\n"]; $state = 'code'; }
            else            { push @out, [$pl, ' ']; }
        }
        elsif ($state eq 'string' || $state eq 'char') {
            my $q = ($state eq 'string') ? '"' : "'";
            if    ($c eq "\\" && $j + 1 < @s) { push @out, [$pl, ' ']; push @out, [$s[$j+1][1], ' ']; $j++; }
            elsif ($c eq $q)                  { push @out, [$pl, ' ']; $state = 'code'; }
            elsif ($c eq "\n")                { push @out, [$pl, "\n"]; $state = 'code'; }  # defensive
            else                              { push @out, [$pl, ' ']; }
        }
        else {   # code
            if    ($c2 eq '/*') { push @out, [$pl, ' ']; push @out, [$pl, ' ']; $j++; $state = 'block'; }
            elsif ($c2 eq '//') { push @out, [$pl, ' ']; push @out, [$pl, ' ']; $j++; $state = 'line'; }
            elsif ($c eq '"')   { push @out, [$pl, ' ']; $state = 'string'; }
            elsif ($c eq "'")   { push @out, [$pl, ' ']; $state = 'char'; }
            else                { push @out, [$pl, $c]; }
        }
    }
    # Reassemble per physical line.
    my %byline;
    for my $e (@out) { $byline{$e->[0]} .= $e->[1] unless $e->[1] eq "\n"; }
    return map { [$_, $byline{$_}] } sort { $a <=> $b } keys %byline;
}

# Convenience for the self-canaries: code-only text of a snippet as a single string.
sub code_only {
    my ($src) = @_;
    return join("\n", map { $_->[1] } code_only_lines($src));
}
