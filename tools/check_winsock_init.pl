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
#      Cross-TU domination is deliberately NOT expressible: it cannot be checked here and it is not
#      local enough to stay true as callers change.
#
# The API list is empirical, not theoretical: each name below was confirmed to return
# WSANOTINITIALISED (10093) when called before WSAStartup. inet_pton/inet_ntop/htons/ntohl were
# confirmed NOT to, which is why the shared parser TUs (proxy_protocol.c, sockaddr.c, dns_sys_win.c)
# need no gate and stay free of the PAL.
#
# WHAT THIS IS RUN OVER (see the check-winsock-init recipe): every Windows-compiled TU under src/,
# plus the two test TUs that make native socket calls of their own rather than going through a Keel
# seam -- tests/net_compat_win.c (the harness every Windows test binary links) and
# tests/test_datagram_public.c (whose mk_fd() models an embedder-supplied descriptor). Other test TUs
# are deliberately OUT of scope: their native calls are test-local, they are reached only after the
# suite has already driven Keel, and a violation there fails immediately and unmistakably in CI with
# WSANOTINITIALISED rather than silently. Library code has no such tight feedback loop, which is why
# src/ is checked exhaustively and the file list is wildcard-driven so a new src/*_win.c is picked up
# without anyone remembering to add it.
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
