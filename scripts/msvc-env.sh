# msvc-env.sh - put an MSVC toolchain into an MSYS2 bash session.
#
#   source scripts/msvc-env.sh
#   make CC=cl
#
# Keel builds with MSVC through the same Make graph as every other toolchain (see mk/toolchain.mk).
# What that needs is cl.exe, link.exe and lib.exe on PATH plus the INCLUDE/LIB/LIBPATH that
# vcvars64.bat sets. MSYS makes it awkward in three specific ways, all of which have cost real
# debugging time here and in the sibling OTTO tree this is modelled on:
#
#   1. MSYS2 ships its own /usr/bin/link.exe (a coreutils program). vcvars inherits the calling PATH
#      and prepends to it, so adopting the environment it produces still leaves /usr/bin ahead of the
#      real linker. The VC tools directory is therefore put in front EXPLICITLY, not merged.
#
#   2. MSYS rewrites ARGUMENTS that look like POSIX paths when handing them to a native binary, which
#      mangles /Fo, /std:c11 and /OUT: into Windows paths. MSYS2_ARG_CONV_EXCL turns that off.
#
#   3. MSYS2 also rewrites ENVIRONMENT variables that look like path lists, which corrupts
#      INCLUDE/LIB/LIBPATH on the way to cl and link. That is a separate mechanism with a separate
#      switch, MSYS2_ENV_CONV_EXCL, and MSYS_NO_PATHCONV does NOT cover it -- that one is Git Bash
#      only. Missing this is why the first CI run compiled every TU (INCLUDE survived far enough) and
#      then failed at link with LNK1181: cannot open input file ws2_32.lib.
#
# Both conversion switches are exported here so neither developers nor CI have to know they exist.
#
# Make still needs MSYS own tools -- the Makefile calls uname, rm, mkdir, perl and sh for recipes --
# so /usr/bin stays on PATH, just behind the VC tools.
#
# Safe to source repeatedly; the work is skipped once it has succeeded.

_keel_msvc_env_setup() {
    local vswhere vsroot vcvars dump line key toolsdir tmpbat

    if [ -n "${KEEL_MSVC_ENV_READY:-}" ]; then
        return 0
    fi

    vswhere="/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
    if [ ! -x "$vswhere" ]; then
        echo "msvc-env.sh: vswhere.exe not found; is Visual Studio or the Build Tools installed?" >&2
        return 1
    fi

    vsroot=$("$vswhere" -latest -products '*' \
                 -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 \
                 -property installationPath 2>/dev/null | tr -d '\r')
    if [ -z "$vsroot" ]; then
        echo "msvc-env.sh: no Visual Studio install with the x64 C++ tools." >&2
        return 1
    fi

    vcvars="$(cygpath -u "$vsroot")/VC/Auxiliary/Build/vcvars64.bat"
    if [ ! -f "$vcvars" ]; then
        echo "msvc-env.sh: vcvars64.bat missing under $vsroot" >&2
        return 1
    fi

    # Capture the environment vcvars produces, through a throwaway batch file so there is no nested
    # quoting to get wrong.
    #
    # Note `cmd.exe /c` rather than the usual MSYS `cmd //c` idiom: //c only collapses to /c while MSYS
    # path conversion is ON, and this needs it off. Left as //c with conversion disabled, cmd never
    # sees a switch at all and blocks forever waiting on stdin.
    tmpbat="${TMPDIR:-/tmp}/keel-vcvars-$$.bat"
    {
        echo '@echo off'
        echo "call \"$(cygpath -w "$vcvars")\" >nul 2>&1"
        echo 'set'
    } > "$tmpbat"
    dump=$(MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*' \
               cmd.exe /c "$(cygpath -w "$tmpbat")" 2>/dev/null | tr -d '\r')
    rm -f "$tmpbat"
    if [ -z "$dump" ]; then
        echo "msvc-env.sh: vcvars64.bat produced no environment." >&2
        return 1
    fi

    while IFS= read -r line; do
        key="${line%%=*}"
        case "$key" in
            INCLUDE|LIB|LIBPATH|VCToolsInstallDir|WindowsSdkDir)
                export "$key=${line#*=}" ;;
        esac
    done <<EOF
$dump
EOF

    if [ -z "${VCToolsInstallDir:-}" ] || [ -z "${INCLUDE:-}" ] || [ -z "${LIB:-}" ]; then
        echo "msvc-env.sh: vcvars did not set VCToolsInstallDir/INCLUDE/LIB." >&2
        return 1
    fi

    # Explicitly in front of everything, so MSYS link.exe cannot win.
    toolsdir="$(cygpath -u "$VCToolsInstallDir")"
    toolsdir="${toolsdir%/}/bin/HostX64/x64"
    if [ ! -x "$toolsdir/cl.exe" ]; then
        echo "msvc-env.sh: no cl.exe under $toolsdir" >&2
        return 1
    fi
    export PATH="$toolsdir:$PATH"

    export MSYS_NO_PATHCONV=1                          # Git Bash: argument conversion
    export MSYS2_ARG_CONV_EXCL='*'                     # MSYS2: argument conversion
    export MSYS2_ENV_CONV_EXCL='INCLUDE;LIB;LIBPATH'   # MSYS2: environment conversion
    export KEEL_MSVC_ENV_READY=1
}

# The contract is not "cl is on PATH", it is "a link will succeed". Checking that here means a broken
# environment reports itself in one line at source time, instead of surfacing later as an LNK1181 in
# the middle of a build.
_keel_msvc_env_verify() {
    local d tool found=0
    for tool in cl link lib; do
        case "$(command -v "$tool")" in
            /usr/bin/*|/bin/*|/mingw*/*|/ucrt64/*|/clang*/*)
                echo "msvc-env.sh: $tool resolves to $(command -v "$tool"), not MSVC own." >&2
                return 1 ;;
        esac
    done
    # ws2_32.lib lives in the Windows SDK half of LIB, so finding it proves the whole list survived.
    IFS=";"
    for d in $LIB; do
        [ -n "$d" ] || continue
        if [ -f "$(cygpath -u "$d")/ws2_32.lib" ]; then found=1; break; fi
    done
    unset IFS
    if [ "$found" -ne 1 ]; then
        echo "msvc-env.sh: ws2_32.lib not found in LIB; a link would fail with LNK1181." >&2
        echo "msvc-env.sh: LIB=$LIB" >&2
        return 1
    fi
    return 0
}

if _keel_msvc_env_setup && _keel_msvc_env_verify; then
    echo "msvc-env.sh: MSVC ready ($(cl 2>&1 | head -1 | tr -d '\r')); build with: make CC=cl"
else
    echo "msvc-env.sh: setup failed; CC=cl builds will not work." >&2
    unset KEEL_MSVC_ENV_READY
fi
