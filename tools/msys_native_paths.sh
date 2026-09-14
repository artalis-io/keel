# Sourced (not executed) by gates that drive a NATIVE-Windows tool -- mingw32-make, pkg-config,
# a MinGW compiler -- from an MSYS shell. No-ops off Windows.
#
# Two different path problems, which need opposite treatment:
#
#   REAL filesystem paths (a mktemp staging root) must reach the native tool in a spelling it
#   understands. MSYS normally rewrites them for you, but it cannot be relied on once anything
#   opts out, so convert them explicitly with keel_native_path().
#
#   LOGICAL paths (PREFIX=/usr/local, recorded verbatim in keel.pc) must NOT be rewritten. They
#   name nothing on disk; converting one turns it into a literal directory and DESTDIR$PREFIX
#   becomes nonsense like ".../rootC:/Program Files/...". keel_keep_logical() opts those specific
#   strings out, by value.
#
# Opting out wholesale (MSYS2_ARG_CONV_EXCL='*') fixes the second and breaks the first, which is
# why this is scoped rather than global.
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*) KEEL_WINDOWS_HOST=1 ;;
    *)                    KEEL_WINDOWS_HOST=0 ;;
esac

# A real path in the spelling native tools use (C:/...). Identity off Windows.
keel_native_path() {
    if [ "$KEEL_WINDOWS_HOST" = 1 ] && command -v cygpath >/dev/null 2>&1; then
        cygpath -m "$1"
    else
        printf %s "$1"
    fi
}

# Logical paths that must survive verbatim into the native tool's argv.
keel_keep_logical() {
    [ "$KEEL_WINDOWS_HOST" = 1 ] || return 0
    _v=''
    for _p in "$@"; do _v="$_v;$_p"; done
    MSYS2_ARG_CONV_EXCL="${_v#;}"
    export MSYS2_ARG_CONV_EXCL
}
