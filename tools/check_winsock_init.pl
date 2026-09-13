#!/usr/bin/env perl
# check_winsock_init.pl: every native socket-runtime boundary states the PAL invariant.
#
# On Windows, ws2_32 must be started before any of its functions will do anything but return
# WSANOTINITIALISED. Keel used to arrange that with a GCC/Clang load-time constructor; MSVC has no
# equivalent, so the invariant is now stated in the code, once per native boundary, by calling
# kl_plat_socket_runtime_init() (see src/platform_socket.h).
#
# That turns a guarantee the compiler used to provide into a convention, and a convention nobody
# enforces is one somebody will forget: a new Winsock call added to any of these TUs would compile,
# link, pass review, and then fail only on the one path that reaches it before anything else has
# opened a socket. That failure does not look like a missing initialisation either, it looks like the
# wrong errno on an unrelated operation. This gate is what keeps the convention true.
#
# A function that calls an init-requiring ws2_32 API must either:
#   1. call kl_plat_socket_runtime_init() itself, or
#   2. live in a TU whose header declares   PAL-gate: dominated-by <function>
#      where <function> is a gated function IN THAT TU that every path here must pass through first.
#
# THE HARD RULE FOR RULE 2, which this gate enforces structurally and which must survive future edits:
# a dominated boundary is dominated within the SAME TU, through an invariant obvious enough to state in
# one sentence beside it -- "these all operate on the state that <function> allocated", "this only
# accepts the listener that <function> created". Cross-TU domination, or anything resting on a long
# call-chain assumption, is NOT expressible here and must not be argued for in review: it cannot be
# checked, it stops being true the moment a caller moves, and the exceptions would decay into
# archaeology that nobody can re-derive. When in doubt, take rule 1. The InitOnce fast path is an
# interlocked read in front of a call that is already entering the kernel, so rule 1 is never the
# expensive choice -- rule 2 exists to keep hot paths and internal helpers honest about WHY they are
# safe, not to buy back cycles.
#
# The API list is empirical, not theoretical: each name below was confirmed to return
# WSANOTINITIALISED (10093) when called before WSAStartup. inet_pton/inet_ntop/htons/ntohl were
# confirmed NOT to, which is why the shared parser TUs (proxy_protocol.c, sockaddr.c, dns_sys_win.c)
# need no gate and stay free of the PAL.
#
# WHAT THIS IS RUN OVER, AND WHY THAT IS NOT EXHAUSTIVE (see the check-winsock-init recipe).
#
# Scanned: every Windows-compiled TU under src/, via a wildcard so a new src/*_win.c is covered
# without anyone remembering to add it. Plus exactly three test TUs, which are scanned because they
# are test INFRASTRUCTURE and were each capable of masking the product invariant:
#
#   tests/net_compat_win.c        the harness every Windows test binary links
#   tests/loopback_listener.h     the shared loopback listener many client suites start with
#   tests/test_datagram_public.c  whose mk_fd() models an embedder-supplied descriptor
#
# All three call socket() themselves rather than going through a Keel seam, and all three had been
# living off the retired load-time constructor. They were found by this gate, after the constructor
# was deleted, which is the clearest evidence available that deleting it was right: a constructor
# hides exactly this class of lifecycle dependency.
#
# NOT scanned, and this is a DELIBERATE ASYMMETRY rather than an omission -- do not "finish the job"
# later by adding PAL calls to every test that touches a socket. Roughly sixty isolated test-local
# native calls exist across the suites. They are not scanned because:
#
#   - src/ is product code shipped to embedders, so it must be protected STRUCTURALLY; a gap there
#     reaches consumers and surfaces as a wrong errno on an unrelated operation, which is close to
#     undebuggable from the outside.
#   - an isolated test-local call fails immediately, loudly and unambiguously in the very environment
#     built to exercise it. The feedback loop that library code lacks is precisely what tests have.
#   - requiring PAL setup in every networking test would bloat each one with ceremony that tests
#     nothing, to re-state a guarantee the suite itself already demonstrates.
#
# So: exhaustive and wildcard-driven over src/, deliberately narrow over tests/. If a test suite ever
# does fail with WSANOTINITIALISED, the fix is a gate in that suite's own helper, not a policy change
# here.
#
# Usage: check_winsock_init.pl <file.c> ...   Exit 0 clean, 1 on a violation.
use strict;
use warnings;

# ws2_32 entry points that fail with WSANOTINITIALISED when the runtime is not up.
my @API = qw(
    socket bind listen accept connect shutdown closesocket ioctlsocket
    send sendto recv recvfrom select getsockname getpeername setsockopt getsockopt
    getaddrinfo freeaddrinfo getnameinfo GetAddrInfoW
    WSAPoll WSASocket WSASocketA WSASocketW WSAIoctl WSAConnect WSAAccept
    WSARecv WSARecvFrom WSASend WSASendTo WSADuplicateSocket WSAEventSelect
    WSAGetOverlappedResult AcceptEx ConnectEx TransmitFile
);
my $API = join '|', map { quotemeta } @API;

my $GATE = 'kl_plat_socket_runtime_init';
my ($bad, $checked, $gated, $exempt) = (0, 0, 0, 0);

for my $path (@ARGV) {
    open my $fh, '<', $path or die "cannot open $path: $!";
    my $src = do { local $/; <$fh> };
    close $fh;

    # A file-level domination declaration, if any, plus the gated function it names.
    my $dominator = ($src =~ /PAL-gate:\s*dominated-by\s+([A-Za-z_][A-Za-z0-9_]*)/) ? $1 : undef;

    # Strip comments and string/char literals so neither a prose mention of send() nor a name inside
    # a literal can satisfy or trip the check.
    my $code = $src;
    $code =~ s{/\*.*?\*/}{ }gs;
    $code =~ s{//[^\n]*}{}g;
    $code =~ s{"(?:\\.|[^"\\])*"}{""}g;
    $code =~ s{'(?:\\.|[^'\\])*'}{''}g;
    # Preprocessor directives too: `#if defined(IP_PKTINFO)` otherwise parses as a function
    # named defined() whose body is whatever follows.
    $code =~ s{^[ \t]*#[^\n]*}{}gm;

    # Split into top-level function bodies by brace depth.
    my @fns;
    while ($code =~ /(^|\n)[^\n;{}]*?\b([A-Za-z_][A-Za-z0-9_]*)\s*\([^;{}]*\)\s*\{/gs) {
        my ($name, $open) = ($2, pos($code) - 1);
        next if $name =~ /^(if|for|while|switch|return|sizeof|do|else)$/;
        my ($depth, $i) = (0, $open);
        while ($i < length $code) {
            my $c = substr $code, $i, 1;
            $depth++ if $c eq '{';
            if ($c eq '}') { $depth--; last if $depth == 0; }
            $i++;
        }
        push @fns, { name => $name, body => substr($code, $open, $i - $open + 1) };
        pos($code) = $open + 1;   # allow nested/subsequent definitions to be found
    }

    for my $fn (@fns) {
        # A vtable call (tls->shutdown(), dg_ops(dg)->send(), ops.recv()) is not a native call:
        # the protocol and datagram layers reach the stack only through Keel seams, and those are
        # gated at the seam implementation, not at every caller.
        my @calls = ($fn->{body} =~ /(?<![.>\w])($API)\s*\(/g);
        next unless @calls;
        $checked++;
        if ($fn->{body} =~ /\b\Q$GATE\E\s*\(/) { $gated++; next; }
        if (defined $dominator && $dominator ne $fn->{name}) {
            # The declared dominator must itself be gated, in this same TU.
            my ($dom) = grep { $_->{name} eq $dominator } @fns;
            if ($dom && $dom->{body} =~ /\b\Q$GATE\E\s*\(/) { $exempt++; next; }
            print STDERR "$path: PAL-gate declares dominated-by $dominator, but that function is "
                       . "not present in this TU or does not call $GATE()\n";
            $bad = 1;
            next;
        }
        my %seen; my @uniq = grep { !$seen{$_}++ } @calls;
        print STDERR "$path: $fn->{name}() calls " . join(', ', map { "$_()" } @uniq)
                   . " without $GATE()\n";
        $bad = 1;
    }
}

if ($bad) {
    print STDERR "\nEvery native socket-runtime boundary must state the PAL invariant:\n"
               . "    if (kl_plat_socket_runtime_init() != 0) return <this op did not happen>;\n"
               . "as its first statement (src/platform_socket.h). If the call is genuinely dominated by\n"
               . "another gated function IN THE SAME TU, declare it in the file header comment:\n"
               . "    PAL-gate: dominated-by <that function>\n";
    exit 1;
}
printf "check-winsock-init: %d native boundaries (%d gated, %d dominated)\n", $checked, $gated, $exempt;
exit 0;
