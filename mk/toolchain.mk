# Keel toolchain abstraction - the one place in the tree where compilers differ.
#
#     make                GCC or Clang, exactly as before
#     make CC=cl          MSVC, after `source scripts/msvc-env.sh`
#
# The Makefile composes its flags from the normalized variables below and never tests which compiler
# is in use. The GNU branch reproduces the flags the Makefile carried inline before this file existed,
# so `make -n` output is unchanged there; that is a checked property, not an intention (see
# check-toolchain-parity). Anything added here for MSVC is a deliberate, documented mapping, and flags
# are NOT translated where the semantics do not survive the trip.
#
# WHAT MSVC ACCEPTS UNCHANGED, which is why this file is small: cl takes -c, -I and -D in their GNU
# spellings. Only the standard/warning selections, the OUTPUT flags, the archiver and the library
# naming convention actually differ.
#
# NEVER name a variable LIB, INCLUDE, LINK or CL anywhere in this build. Those four are MSVC's
# environment contract, and make re-exports a variable it reassigns that also came from the
# environment -- so a makefile `LIB` replaces the linker's search path in every recipe. Keel's archive
# variable is KEEL_LIB for exactly this reason.

# Matched on the exact program name, never a substring: "clang" contains "cl".
CC_NAME := $(notdir $(CC))
ifeq ($(CC_NAME),cl)
  TOOLCHAIN := msvc
else ifeq ($(CC_NAME),cl.exe)
  TOOLCHAIN := msvc
else
  TOOLCHAIN := gnu
endif

# A trailing space that survives make's variable stripping, for the flags that take one.
KEEL_EMPTY :=
KEEL_SPACE := $(KEEL_EMPTY) $(KEEL_EMPTY)

ifeq ($(TOOLCHAIN),msvc)

# ---------------------------------------------------------------------------
# MSVC (cl.exe / lib.exe), x64, from an MSYS2 shell
# ---------------------------------------------------------------------------

# /experimental:c11atomics is how MSVC exposes C11 <stdatomic.h>; without it the header hard-errors.
# src/kl_atomic.h and src/protocols/http/http_server_plat_win.c both need it. Microsoft ships no
# stable alternative, so the flag is named here once rather than scattered. The source is NOT worked
# around to avoid it (see src/kl_atomic.h for the lock-free contract that depends on it).
# Suppresses the copyright banner AND the seven-line "experimental features" blurb that
# /experimental:c11atomics otherwise prints on every single invocation -- 83 TUs of it per build.
CC_QUIET := /nologo

CC_STD  := /std:c11 /experimental:c11atomics

# /W3 plus /WX, measured rather than assumed: at /W3 exactly three of Keel's own TUs warned, all of
# them real (an undeclared strtok_r truncating a pointer, and pointer-width socket handles narrowed
# into int), and all now fixed. /W4 added nothing over /W3 on this tree.
#
# Not a mechanical translation of Keel's GNU set: -Wpedantic, -Wshadow and -Wformat=2 have no /W
# equivalent, and MSVC diagnoses things GCC does not. The intent carried across is "a deliberate set,
# and what it reports is fatal" -- NOT "the same diagnostics", which no flag mapping can deliver.
CC_WARN   := /W3
CC_WERROR := /WX

# MSVC optimises for speed with /O2; there is no /O3. /Od is the no-optimisation form.
CC_OPT_DEFAULT := /O2

# /GS is the stack-cookie analogue of -fstack-protector-strong and /guard:cf is control-flow guard.
# There is no _FORTIFY_SOURCE counterpart, and PE images are relocatable with ASLR by construction
# (/DYNAMICBASE is the linker default), so -fPIE has nothing to map to.
CC_HARDEN := /GS /guard:cf
CC_PIE    :=
LD_PIE    :=

# _CRT_SECURE_NO_WARNINGS: MSVC deprecates snprintf/strncpy in favour of its _s variants; Keel uses
# the standard ones deliberately.
# _CRT_NONSTDC_NO_WARNINGS: <io.h> declares the POSIX spellings (close, read, _lseeki64's siblings)
# and then deprecates them in favour of underscore forms. The POSIX spellings are what the sources use.
CC_DEFS := /D_CRT_SECURE_NO_WARNINGS /D_CRT_NONSTDC_NO_WARNINGS

# strtok_r -> strtok_s is a straight rename: MSVC's 3-argument CRT strtok_s has the same signature,
# argument order and semantics as POSIX strtok_r (it is not C11 Annex K's 4-argument strtok_s).
# Done here rather than with an #ifdef in dns_resolver.c and proxy_protocol.c so a build concern stays
# in the build layer and the sources stay POSIX-spelled. Without it MSVC treats the calls as implicit
# declarations returning int, which TRUNCATES the returned pointer on a 64-bit build: a crash, not a
# warning. This is the concrete reason /WX is worth having.
CC_DEFS += /Dstrtok_r=strtok_s

# MSVC's /showIncludes emits a format that would need a parser to turn into make rules. Not worth it
# for this first implementation, so MSVC builds do not track header dependencies: after editing a
# header, `make clean`. CI always builds clean, so it is unaffected. Documented in docs/build.md.
CC_DEPFLAGS :=

# cl's output flags are unlike anyone else's, and -o is deprecated (D9035). These carry the whole
# difference. No trailing space: /Fo and /Fe take the path glued on.
OBJ_OUT := /Fo
EXE_OUT := /Fe
AR_CMD  := lib /nologo /OUT:

# Object and archive NAMING is deliberately unchanged (.o, libkeel.a) rather than switched to
# .obj/keel.lib. lib.exe and link.exe neither require nor care about those extensions, and keeping one
# naming scheme is what lets every compile and link rule stay toolchain-neutral instead of each one
# branching on the extension. cl itself does warn D9024 on an unfamiliar extension, which is why link
# inputs are passed after LD_SEP: past that point they go straight to link.exe, which does not care.

# Everything after this point on a cl command line goes to the linker. Without it cl treats the
# archive as a source file of unknown type and warns D9024 before handing it over anyway; with it the
# archive reaches link.exe as the input file it is. Empty on GNU, where the driver needs no separator.
LD_SEP := /link

# MSVC takes a static library as a plain input file; there is no -L/-l. The .a extension is immaterial
# to link.exe, which identifies an archive by its header.
LD_KEEL     := libkeel.a
# The Windows system libraries, spelled for this linker. REFERENCED ONLY from the Makefile's Windows
# branch -- it is defined unconditionally here because only the spelling is toolchain-dependent, not
# the list, and a POSIX build never reads it.
LD_WIN_PLATFORM := ws2_32.lib mswsock.lib bcrypt.lib iphlpapi.lib advapi32.lib shell32.lib
# No pthreads: the PAL threading seam (src/platform_thread_win.c) uses CreateThread + SRWLOCK +
# CONDITION_VARIABLE, which is the whole reason winpthreads is not needed here.
LD_THREAD   :=

# MSVC gives an executable a 1 MB stack where MinGW gives 2 MB and Linux 8 MB. Code written against a
# larger default overflows on entry, before its first statement runs, dying with no output and an exit
# code that says nothing. Match the MinGW default rather than leave that trap set.
LD_STACK := /F2097152

# Force-including a header before the TU (the test harness prelude). Resolved against the include
# path, so callers pass a bare filename and -Itests finds it.
CC_FORCE_INC := /FI
# Relaxations the test harness build asks for on GNU. MSVC's /W3 set does not contain analogues of
# -Wpedantic / -Wsign-compare / -Wunused-result, so there is nothing to relax and nothing is emitted
# rather than a guess at equivalent /wd codes.
CC_TEST_RELAX :=
# cl writes the intermediate object beside the exe unless told otherwise, which would litter tests/
# with .obj files. A directory (trailing slash) so one setting serves every test rule.
TEST_OBJ_TMP = /Fo$(OBJDIR)/tests/

else

# ---------------------------------------------------------------------------
# GCC / Clang - byte-for-byte what the Makefile spelled inline
# ---------------------------------------------------------------------------

CC_QUIET  :=
CC_STD    := -std=c11
CC_WARN   := -Wall -Wextra -Wpedantic -Wshadow -Wformat=2
CC_WERROR := -Werror
CC_OPT_DEFAULT := -O2
CC_HARDEN := -fstack-protector-strong
CC_PIE    := -fPIE
LD_PIE    := -Wl,-pie
CC_DEFS   :=
CC_DEPFLAGS := -MMD -MP

OBJ_OUT := -o$(KEEL_SPACE)
EXE_OUT := -o$(KEEL_SPACE)
AR_CMD  := $(AR) rcs$(KEEL_SPACE)

LD_SEP      :=
LD_KEEL     := -L. -lkeel
LD_WIN_PLATFORM := -lws2_32 -lmswsock -lbcrypt -liphlpapi -ladvapi32 -lshell32
LD_THREAD   := -lpthread
LD_STACK    :=

CC_FORCE_INC := -include$(KEEL_SPACE)
CC_TEST_RELAX := -Wno-pedantic -Wno-sign-compare -Wno-unused-result
# GNU puts the intermediate where it likes and cleans up after itself.
TEST_OBJ_TMP :=

endif
