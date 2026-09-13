# msvc-env.sh - put an MSVC toolchain into an MSYS2 bash session.
#
#   source scripts/msvc-env.sh
#   make CC=cl
#
# Keel builds with MSVC through the same Make graph as every other toolchain (see mk/toolchain.mk).
# What that needs is cl.exe, link.exe and lib.exe on PATH plus the INCLUDE/LIB/LIBPATH that
# vcvars64.bat sets. MSYS2 makes it awkward in two specific ways, both of which have cost real
# debugging time here and in the sibling OTTO tree this is modelled on:
#
#   1. MSYS2 ships its own /usr/bin/link.exe (a coreutils program). vcvars inherits the calling PATH
#      and prepends to it, so adopting the environment it produces still leaves /usr/bin ahead of the
#      real linker. The VC tools directory is therefore put in front EXPLICITLY, not merged.
#
#   2. MSYS rewrites anything that looks like a POSIX path when handing arguments to a native binary,
#      which mangles /Fo, /std:c11, /OUT: and every other MSVC flag into a Windows path.
#      MSYS2_ARG_CONV_EXCL turns that off. Exported here so neither developers nor CI have to know
#      the variable exists; a build that forgets it fails in confusing ways rather than loudly.
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

    vsroot=$("$vswhere" -latest -products '*'                  -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64                  -property installationPath 2>/dev/null | tr -d '')
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
    # path conversion is ON, and this function needs it off. Left as //c with conversion disabled, cmd
    # never sees a switch at all and blocks forever waiting on stdin.
    tmpbat="${TMPDIR:-/tmp}/keel-vcvars-$$.bat"
    {
        echo '@echo off'
        echo "call \"$(cygpath -w "$vcvars")\" >nul 2>&1"
        echo 'set'
    } > "$tmpbat"
    dump=$(MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'                cmd.exe /c "$(cygpath -w "$tmpbat")" 2>/dev/null | tr -d '')
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

    if [ -z "${VCToolsInstallDir:-}" ] || [ -z "${INCLUDE:-}" ]; then
        echo "msvc-env.sh: vcvars did not set VCToolsInstallDir/INCLUDE." >&2
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

    export MSYS_NO_PATHCONV=1
    export MSYS2_ARG_CONV_EXCL='*'
    export KEEL_MSVC_ENV_READY=1
}

if _keel_msvc_env_setup; then
    for _keel_msvc_tool in cl link lib; do
        case "$(command -v "$_keel_msvc_tool")" in
            /usr/bin/*|/bin/*|/mingw*/*|/ucrt64/*|/clang*/*)
                echo "msvc-env.sh: $_keel_msvc_tool resolves to $(command -v "$_keel_msvc_tool"), not MSVC own." >&2
                ;;
        esac
    done
    unset _keel_msvc_tool
    echo "msvc-env.sh: MSVC ready ($(cl 2>&1 | head -1 | tr -d '')); build with: make CC=cl"
else
    echo "msvc-env.sh: setup failed; CC=cl builds will not work." >&2
fi
