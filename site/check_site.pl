#!/usr/bin/env perl
# check_site.pl - dependency-free, offline validator for the Keel landing site.
#
# Uses only core Perl (JSON::PP, Digest::SHA, File::Temp, File::Find), no network,
# and no third-party modules. Run from the site/ directory:
#
#     perl check_site.pl            # (what `make -C site check` and root `check-site` do)
#
# It validates, and exits non-zero if any category fails:
#   - required deployable assets exist
#   - no duplicate id attributes
#   - every internal #fragment link resolves to an element id
#   - every local (non-http, non-fragment) href/src target exists on disk
#   - the JSON-LD block parses as JSON
#   - required <head> metadata is present
#   - every <table> has a <caption> and every <th> carries scope="col"
#   - header/nav/main/footer landmarks exist, main is the skip-link target
#   - every decorative <svg> is aria-hidden="true" and focusable="false"
#   - DEFAULT-DENY: no external runtime subresource is loaded by any resource-bearing
#     HTML attribute (src / srcset / poster / <object data> / <use|image href> /
#     resource <link> / inline style url()) or by any CSS @import / url(); outbound
#     navigation (<a href>) and metadata URLs (meta content=, JSON-LD, canonical) are
#     explicitly allowed
#   - every class used in the HTML is defined by the committed stylesheet
#     (accounting for backslash-escaped CSS selectors)
#   - two isolated runs of the REAL build recipe (make BUILD_DIR=<fresh> build)
#     produce identical manifests + hashes, derived from the produced directory,
#     and the produced set equals the intended deployable asset set
#
# In-script self-canaries assert that the resource default-deny and the build
# reproducibility/manifest checks actually fire before the real tree is scanned.
#
# Self-cleaning: every build writes into a fresh temporary directory and never
# reads, writes, or removes a developer's existing site/build/.

use strict;
use warnings;
use JSON::PP ();
use Digest::SHA qw(sha256_hex);
use File::Temp qw(tempdir);
use File::Find ();

my $HTML = 'index.html';
my $CSS  = 'keel.css';
my @ASSETS = ($HTML, $CSS, 'mark.svg', 'og-card.svg');   # the intended deployable set

my @fail;
sub bad { push @fail, $_[0]; }

# ---- Self-canaries (must fire before the real scan) ---------------------
self_test();

# ---- Load sources -------------------------------------------------------
unless (-f $HTML) {
    print STDERR "check-site: FAIL: $HTML not found (run from the site/ directory)\n";
    exit 2;
}
my $html = slurp($HTML);
my $css  = -f $CSS ? slurp($CSS) : '';

# ---- 1. Required deployable assets --------------------------------------
for my $a (@ASSETS) {
    bad("missing deployable asset: $a") unless -f $a;
}

# ---- 2. Duplicate ids ---------------------------------------------------
my %idcount;
$idcount{$1}++ while $html =~ /\bid="([^"]+)"/g;
for my $id (sort keys %idcount) {
    bad("duplicate id: $id (x$idcount{$id})") if $idcount{$id} > 1;
}
my %ids = map { $_ => 1 } keys %idcount;

# ---- 3. Internal #fragment links resolve --------------------------------
my %frag;
while ($html =~ /\bhref="#([^"]*)"/g) {
    my $f = $1;
    next if $f eq '';         # bare "#" (scroll-to-top) is allowed
    $frag{$f} = 1;
}
for my $f (sort keys %frag) {
    bad("internal link #$f has no matching id") unless $ids{$f};
}

# ---- 4. Local asset references exist ------------------------------------
while ($html =~ /\b(?:href|src)="([^"]+)"/g) {
    my $t = $1;
    next if is_external($t) || $t =~ /^#/ || $t =~ /^mailto:/ || $t =~ /^data:/;
    (my $path = $t) =~ s/[?#].*$//;   # strip query/fragment
    bad("local asset reference not found on disk: $t") unless -e $path;
}

# ---- 5. JSON-LD parses --------------------------------------------------
if ($html =~ m{<script type="application/ld\+json">(.*?)</script>}s) {
    my $json = $1;
    my $ok = eval { JSON::PP->new->decode($json); 1 };
    bad("JSON-LD does not parse: $@") unless $ok;
} else {
    bad("no JSON-LD (application/ld+json) block found");
}

# ---- 6. Required <head> metadata ----------------------------------------
my %meta = (
    'title element'    => qr{<title>[^<]+</title>},
    'meta description' => qr{<meta\s+name="description"\s+content="[^"]+"},
    'og:title'         => qr{<meta\s+property="og:title"\s+content="[^"]+"},
    'og:description'   => qr{<meta\s+property="og:description"\s+content="[^"]+"},
    'og:image'         => qr{<meta\s+property="og:image"\s+content="[^"]+"},
    'twitter:card'     => qr{<meta\s+name="twitter:card"\s+content="[^"]+"},
    'viewport'         => qr{<meta\s+name="viewport"\s+content="[^"]+"},
    'theme-color'      => qr{<meta\s+name="theme-color"\s+content="[^"]+"},
    'icon link'        => qr{<link\s+rel="icon"},
    'stylesheet link'  => qr{<link\s+rel="stylesheet"\s+href="keel\.css"},
    'lang attribute'   => qr{<html\s+lang="[^"]+"},
);
for my $name (sort keys %meta) {
    bad("required metadata missing: $name") unless $html =~ $meta{$name};
}

# ---- 7. Table captions + scope="col" ------------------------------------
my @tables = ($html =~ m{<table\b.*?</table>}gs);
bad("no <table> found (expected at least one data table)") unless @tables;
my $ti = 0;
for my $tbl (@tables) {
    $ti++;
    bad("table #$ti has no <caption>") unless $tbl =~ /<caption[\s>]/;
    for my $attrs ($tbl =~ m{<th\b([^>]*)>}g) {
        bad("table #$ti has a <th> without scope=\"col\"") unless $attrs =~ /\bscope="col"/;
    }
}

# ---- 8. Landmarks + skip link -------------------------------------------
for my $lm (qw(header nav main footer)) {
    bad("missing <$lm> landmark") unless $html =~ /<$lm[\s>]/;
}
bad("<main> has no id=\"main\"")        unless $html =~ /<main\b[^>]*\bid="main"/;
bad("skip link (href=\"#main\") absent") unless $html =~ /href="#main"/;
my @navs = ($html =~ /<nav\b([^>]*)>/g);
if (@navs > 1) {
    my $labelled = grep { /\baria-label="[^"]+"/ } @navs;
    bad("multiple <nav> landmarks but not all have aria-label") unless $labelled == @navs;
}

# ---- 9. Decorative <svg> treatment --------------------------------------
my @svgs = ($html =~ /<svg\b([^>]*)>/g);
bad("no <svg> elements found") unless @svgs;
my ($svg_i, $svg_bad) = (0, 0);
for my $attrs (@svgs) {
    $svg_i++;
    $svg_bad++ unless $attrs =~ /\baria-hidden="true"/ && $attrs =~ /\bfocusable="false"/;
}
bad("$svg_bad of $svg_i <svg> elements lack aria-hidden=\"true\" + focusable=\"false\"") if $svg_bad;

# ---- 10. No external runtime subresources (default-deny) -----------------
bad($_) for resource_violations($html, $css);

# ---- 11. Every used class is defined by the stylesheet ------------------
my %used;
while ($html =~ /\bclass="([^"]*)"/g) {
    $used{$_} = 1 for grep { length } split /\s+/, $1;
}
my %defined = css_class_set($css);
my @undef = grep { !$defined{$_} } sort keys %used;
bad("class(es) used in HTML but not defined in $CSS: @undef") if @undef;

# ---- 12. Two isolated REAL builds: identical manifest + intended set -----
{
    my $parent = tempdir("keel-site-XXXXXX", TMPDIR => 1, CLEANUP => 1);
    my ($b1, $b2) = ("$parent/b1", "$parent/b2");
    if (!run_build($b1) || !run_build($b2)) {
        bad("the site build recipe failed under an overridden BUILD_DIR");
    } else {
        my $m1 = dir_manifest($b1);
        my $m2 = dir_manifest($b2);
        bad("two isolated builds are not identical:\n"
            . "  build 1:\n    " . join("\n    ", @$m1) . "\n"
            . "  build 2:\n    " . join("\n    ", @$m2))
            if join("\n", @$m1) ne join("\n", @$m2);
        my @produced = sort map { (split /  /, $_, 2)[0] } @$m1;
        my @intended = sort @ASSETS;
        bad("produced build manifest [@produced] does not match the intended deployable set [@intended]")
            if "@produced" ne "@intended";
    }
}

# ---- Report -------------------------------------------------------------
if (@fail) {
    print STDERR "check-site: FAIL (" . scalar(@fail) . " issue(s)):\n";
    print STDERR "  - $_\n" for @fail;
    exit 1;
}
print "check-site: OK ("
    . scalar(@ASSETS) . " assets, "
    . scalar(keys %ids) . " unique ids, "
    . scalar(keys %frag) . " internal links, "
    . scalar(@tables) . " captioned tables, "
    . $svg_i . " decorative svgs, "
    . scalar(keys %used) . " class tokens defined, "
    . "default-deny resource scan clean, "
    . "real build reproduced)\n";
exit 0;

# =========================================================================
# Helpers
# =========================================================================
sub slurp {
    my ($f) = @_;
    open my $fh, '<', $f or die "cannot open $f: $!";
    local $/;
    my $c = <$fh>;
    close $fh;
    return $c;
}

# True for any absolute-scheme or protocol-relative URL (http/https/ws/ftp/...//host).
# False for relative paths, "#frag", "data:", and "mailto:" (handled by callers).
sub is_external {
    my ($u) = @_;
    return 0 unless defined $u;
    return 1 if $u =~ m{^\s*//};                     # protocol-relative
    return 1 if $u =~ m{^\s*[a-zA-Z][a-zA-Z0-9+.\-]*://};  # scheme://host
    return 0;
}

# Default-deny scan for external runtime subresources in HTML + CSS.
# Returns a list of human-readable violations (empty when clean).
sub resource_violations {
    my ($h, $c) = @_;
    my @v;

    # src= on any element (script/img/source/iframe/video/audio/track/embed/input/...)
    while ($h =~ /<([a-zA-Z][\w-]*)\b[^>]*?\bsrc\s*=\s*"([^"]*)"/g) {
        push @v, "external <$1 src>: $2" if is_external($2);
    }
    # srcset= (img/source): any candidate URL external
    while ($h =~ /<([a-zA-Z][\w-]*)\b[^>]*?\bsrcset\s*=\s*"([^"]*)"/g) {
        my ($el, $val) = ($1, $2);
        push @v, "external <$el srcset>: $val" if grep { is_external($_) } split /[\s,]+/, $val;
    }
    # poster= (video)
    while ($h =~ /\bposter\s*=\s*"([^"]*)"/g) {
        push @v, "external poster: $1" if is_external($1);
    }
    # <object data=...>
    while ($h =~ /<object\b[^>]*?\bdata\s*=\s*"([^"]*)"/gi) {
        push @v, "external <object data>: $1" if is_external($1);
    }
    # SVG external refs: <use>/<image> href or xlink:href
    while ($h =~ /<(?:use|image)\b[^>]*?\b(?:xlink:href|href)\s*=\s*"([^"]*)"/gi) {
        push @v, "external svg ref: $1" if is_external($1);
    }
    # <link> resource loads: external href is denied unless the rel is metadata-only.
    my %meta_rel = map { $_ => 1 } qw(canonical alternate author license prev next help search);
    while ($h =~ /<link\b([^>]*)>/gi) {
        my $attrs = $1;
        my ($href) = $attrs =~ /\bhref\s*=\s*"([^"]*)"/;
        next unless defined $href && is_external($href);
        my ($rel) = $attrs =~ /\brel\s*=\s*"([^"]*)"/;
        $rel = defined $rel ? lc $rel : '';
        my $is_meta = ($rel ne '') && !(grep { !$meta_rel{$_} } split /\s+/, $rel);
        push @v, "external <link rel=\"$rel\" href>: $href" unless $is_meta;
    }
    # inline style="...url(external)..."
    while ($h =~ /\bstyle\s*=\s*"([^"]*)"/gi) {
        my $s = $1;
        while ($s =~ /url\(\s*['"]?([^'")]+)['"]?\s*\)/gi) {
            push @v, "external inline-style url(): $1" if is_external($1);
        }
    }

    # CSS: any @import, and any external url()
    if (defined $c && length $c) {
        (my $cn = $c) =~ s{/\*.*?\*/}{}gs;   # strip comments
        push @v, "CSS \@import present (no external stylesheet imports allowed)" if $cn =~ /\@import\b/i;
        while ($cn =~ /url\(\s*['"]?([^'")]+)['"]?\s*\)/gi) {
            push @v, "CSS external url(): $1" if is_external($1);
        }
    }
    return @v;
}

# Collect the set of class selectors defined by the stylesheet, comments
# stripped and backslash-escapes removed (".md\:hidden" -> "md:hidden").
sub css_class_set {
    my ($c) = @_;
    (my $cn = $c) =~ s{/\*.*?\*/}{}gs;
    my %set;
    while ($cn =~ /\.((?:\\.|[A-Za-z0-9_-])+)/g) {
        (my $sel = $1) =~ s/\\(.)/$1/g;
        $set{$sel} = 1;
    }
    return %set;
}

# Invoke the REAL build recipe into a fresh directory (never touches build/).
# MAKE-flags are cleared so a parent `make check` does not leak its jobserver.
sub run_build {
    my ($builddir) = @_;
    local $ENV{MAKEFLAGS} = '';
    local $ENV{MAKELEVEL} = '';
    local $ENV{MFLAGS}    = '';
    my $rc = system("make BUILD_DIR='$builddir' build >/dev/null 2>&1");
    return $rc == 0 && -d $builddir;
}

# Derive a sorted "relpath  sha256" manifest from a produced directory.
sub dir_manifest {
    my ($dir) = @_;
    my @files;
    File::Find::find({ wanted => sub { push @files, $File::Find::name if -f $_ }, no_chdir => 1 }, $dir);
    my @rows;
    for my $f (sort @files) {
        (my $rel = $f) =~ s{^\Q$dir\E/?}{};
        push @rows, sprintf("%s  %s", $rel, sha256_hex(slurp($f)));
    }
    return \@rows;
}

# ---- self-canaries ------------------------------------------------------
sub self_test {
    # (a) resource default-deny MUST flag external subresources, even unknown hosts.
    my $bad_html = join("\n",
        '<img src="https://unknown.example/x.png">',
        '<source srcset="https://unknown.example/y.png 2x">',
        '<link rel="preconnect" href="https://unknown.example">',
        '<use href="https://unknown.example/sprite.svg#i"></use>',
        '<div style="background:url(https://unknown.example/z.png)"></div>',
    );
    my $bad_css = '@import url(https://unknown.example/a.css); .x{background:url(https://unknown.example/b.png)}';
    my @bv = resource_violations($bad_html, $bad_css);
    die "check-site: SELF-TEST FAILED: resource scanner missed an external subresource (saw " . scalar(@bv) . ")\n"
        if @bv < 6;

    # (b) allowed navigation + metadata + local refs MUST NOT be flagged.
    my $ok_html = join("\n",
        '<a href="https://github.com/artalis-io/keel">repo</a>',
        '<meta property="og:image" content="https://raw.githubusercontent.com/x/og-card.svg">',
        '<meta name="twitter:image" content="https://raw.githubusercontent.com/x/og-card.svg">',
        '<link rel="icon" href="mark.svg">',
        '<link rel="canonical" href="https://example.com/">',
        '<img src="mark.svg">',
    );
    my $ok_css = '.g{mask-image:radial-gradient(black,transparent)} .u{background:url(#frag)}';
    my @ov = resource_violations($ok_html, $ok_css);
    die "check-site: SELF-TEST FAILED: resource scanner flagged an allowed reference: @ov\n" if @ov;

    # (c) the manifest / reproducibility LOGIC must detect corruption. This canaries
    # the checker's own comparison paths; it does not assert the real recipe is correct
    # (a broken recipe is caught by the main section-12 check with a clear message).
    my $t = tempdir("keel-site-selftest-XXXXXX", TMPDIR => 1, CLEANUP => 1);
    die "check-site: SELF-TEST FAILED: real build recipe did not run\n" unless run_build($t);
    my $base = dir_manifest($t);
    die "check-site: SELF-TEST FAILED: build produced no files to hash\n" unless @$base;
    # inject a stray deployable: the produced-set-vs-intended check must diverge.
    open my $sf, '>', "$t/stray.js" or die "selftest: $!";
    print $sf "console.log('stray')";
    close $sf;
    my @stray_set = sort map { (split /  /, $_, 2)[0] } @{ dir_manifest($t) };
    die "check-site: SELF-TEST FAILED: manifest-set check did not catch a stray produced file\n"
        if "@stray_set" eq "@{[ sort @ASSETS ]}";
    # mutate a file: the content hash must change (reproducibility divergence).
    open my $af, '>>', "$t/$ASSETS[0]" or die "selftest: $!";
    print $af "\n<!-- mutated -->";
    close $af;
    die "check-site: SELF-TEST FAILED: content hash did not change after mutation\n"
        if join("\n", @$base) eq join("\n", @{ dir_manifest($t) });
}
