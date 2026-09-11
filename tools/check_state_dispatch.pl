#!/usr/bin/env perl
# check_state_dispatch.pl: every switch over a KlHttpConnState must stay exhaustive.
#
# The enforcement mechanism is the compiler: -Wall enables -Wswitch, which errors (under -Werror) on
# an enum switch that omits a member AND has no default:. A default: silences that entirely, so the
# only thing standing between Keel and a silently mis-handled new state is the absence of a default:
# on these switches. This gate keeps that absence, because it is invisible in review: nobody notices
# a default: being added.
#
# Rationale in docs/contracts/early_rejection_drain.md ("Dispatching the state"). #270 is what
# happens without it: a state added to KlHttpConnState, four consumers assuming two states, and
# already-written HTTP responses destroyed by an abortive close.
#
# Usage: check_state_dispatch.pl <file.c> ...   Exit 0 clean, 1 on a violation.
use strict;
use warnings;

my $bad = 0;
my $checked = 0;

for my $path (@ARGV) {
    open my $fh, '<', $path or die "cannot open $path: $!";
    my @lines = <$fh>;
    close $fh;

    for (my $i = 0; $i < @lines; $i++) {
        next unless $lines[$i] =~ /\bswitch\s*\(/;

        # Walk the switch body by brace depth, starting at the line's opening brace.
        my ($depth, $body, $start) = (0, '', $i + 1);
        my $seen_open = 0;
        for (my $j = $i; $j < @lines; $j++) {
            my $l = $lines[$j];
            $body .= $l;
            my $opens  = ($l =~ tr/{//);
            my $closes = ($l =~ tr/}//);
            $seen_open = 1 if $opens;
            $depth += $opens - $closes;
            last if $seen_open && $depth <= 0;
        }

        # Only switches that dispatch a connection state concern us.
        next unless $body =~ /\bcase\s+KL_HTTP_CONN_/;
        $checked++;

        if ($body =~ /^\s*default\s*:/m) {
            print STDERR
              "$path:$start: a switch over KlHttpConnState has a default:, which disables the\n" .
              "  compiler's exhaustiveness check (-Wswitch). List every state instead, so adding a\n" .
              "  member is a build error here rather than a silent mis-dispatch. See\n" .
              "  docs/contracts/early_rejection_drain.md.\n";
            $bad = 1;
        }
    }
}

if ($bad) { exit 1; }
print "state-dispatch: OK, $checked KlHttpConnState switch(es), none with a default:\n";
exit 0;
