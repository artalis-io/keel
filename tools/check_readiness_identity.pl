#!/usr/bin/env perl
#
# check_readiness_identity.pl: Phase-B step 6B-2 audit gate.
#
# Every readiness kl_event_add/mod() call must register the raw KlStream (&conn->stream) as its
# udata, NOT a bare KlConn. Because KlStream is the leading member of KlConn (offset 0), a bare
# KlConn compiles and runs identically, so behavioral tests cannot catch such a regression; this
# gate does, statically. It parses whole call expressions (balanced parens), so it catches BOTH
# single-line and multiline calls, unlike a per-line grep.
#
# Usage: check_readiness_identity.pl <file.c> ...   (exit 1 on any contract violation)
#
# CONTRACT-BASED (allowlist, not a name denylist): the last positional argument of each
# kl_event_add/mod() call (the udata) MUST be one of:
#   - NULL                           (the listen socket, or a genuinely conn-less event), or
#   - &<expr>->stream / &<expr>.stream   (the raw KlStream identity of a connection).
# ANY other final argument is rejected; a bare KlConn under any variable name (c, nc, conn,
# connection, accepted, ...) or any other expression. A legitimately conn-less future event may
# opt out by placing a  /* readiness-identity-exempt */  comment on the udata argument.
use strict;
use warnings;

my $allowed  = qr/^ (?: NULL | & .*? (?: -> | \. ) stream ) $/x;   # the only accepted udata forms
my $exempt   = qr{/\*\s*readiness-identity-exempt\s*\*/};          # explicit opt-out marker
my $violations = 0;

for my $file (@ARGV) {
    open my $fh, '<', $file or die "check_readiness_identity: cannot open $file: $!\n";
    local $/;                     # slurp
    my $src = <$fh>;
    close $fh;

    # Match kl_event_add( ... ) / kl_event_mod( ... ) with a balanced-paren argument list.
    while ($src =~ /kl_event_(?:add|mod)\s*(\((?:[^()]++|(?1))*\))/g) {
        my $end  = pos($src);
        my $call = $1;
        my $args = substr($call, 1, -1);      # strip the outer parentheses

        # Split into top-level arguments (commas not nested in parens).
        my @args;
        my $cur   = '';
        my $depth = 0;
        for my $ch (split //, $args) {
            if    ($ch eq '(') { $depth++; $cur .= $ch; }
            elsif ($ch eq ')') { $depth--; $cur .= $ch; }
            elsif ($ch eq ',' && $depth == 0) { push @args, $cur; $cur = ''; }
            else  { $cur .= $ch; }
        }
        push @args, $cur if length $cur;
        next unless @args;

        my $last_raw = $args[-1];              # keep comments for the exemption check
        next if $last_raw =~ $exempt;          # explicit, documented opt-out

        my $last = $last_raw;
        $last =~ s{/\*.*?\*/}{}gs;             # strip inline comments
        $last =~ s/^\s+//; $last =~ s/\s+$//;  # trim

        unless ($last =~ $allowed) {           # allowlist: NULL or &...stream only
            my $line = (substr($src, 0, $end) =~ tr/\n//) + 1;
            print STDERR "$file:$line: readiness kl_event_* udata '$last' is not a KlStream ",
                         "identity; use &conn->stream (or NULL / readiness-identity-exempt)\n";
            $violations++;
        }
    }
}

exit($violations ? 1 : 0);
