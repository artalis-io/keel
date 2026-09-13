# Building Keel with native MSVC

Keel builds with Microsoft `cl.exe` / `lib.exe` from an MSYS2 shell, through the same Make graph and
the same source tree as every other toolchain. The motivating consumer is a project that builds its
whole dependency stack with one native Windows toolchain and does not want a MinGW runtime in the
result.

```sh
source scripts/msvc-env.sh
make CC=cl
```

That produces a native `libkeel.a` whose only DLL imports are `WS2_32.dll` and `KERNEL32.dll`.

| command | what it does |
|---|---|
| `make CC=cl` | the library, WSAPoll readiness backend |
| `make CC=cl BACKEND=iocp` | the library, IOCP completion backend |
| `make CC=cl test-msvc` | build and run the MSVC runtime probes |
| `make CC=cl check-msvc-no-mingw` | assert no MinGW runtime import crept in |
| `make CC=cl check-msvc-headers` | every public header compiles alone under `cl` |

The `windows-msvc` CI job runs exactly these commands, so the documented path is the tested path.

## It is a compiler mapping, not a platform

There is no MSVC source fork and no MSVC-specific runtime behaviour. Threading, Winsock startup, event
semantics, socket providers and HTTP behaviour are the PAL implementations the MinGW build uses. That
was the point of the PAL work that preceded this: `src/platform_thread_win.c` means no winpthreads, and
`src/platform_socket_win.c` means no load-time constructor to emulate.

Everything toolchain-specific lives in one file, `mk/toolchain.mk`, which maps build INTENT
(`CC_STD`, `CC_WARN`, `OBJ_OUT`, `AR_CMD`, ...) onto either GCC/Clang or MSVC. Nothing else in the
build tests which compiler is in use. The GNU expansion is byte-identical to what the Makefile spelled
inline before that file existed.

## What scripts/msvc-env.sh is for

Two MSYS2 behaviours break a naive `make CC=cl`, and both cost real debugging time:

1. **MSYS2 ships its own `/usr/bin/link.exe`** (a coreutils program). `vcvars64.bat` inherits the
   calling PATH and prepends to it, so adopting its environment still leaves `/usr/bin` ahead of the
   real linker. The script puts the VC tools directory in front explicitly.

2. **MSYS rewrites arguments that look like POSIX paths** when handing them to a native binary, which
   turns `/Fo`, `/std:c11` and `/OUT:` into Windows paths. `MSYS2_ARG_CONV_EXCL='*'` turns that off,
   and the script exports it so no developer and no CI job has to know the variable exists.

A consequence worth knowing: with argument conversion off, paths given to `cl` must be ones Windows
understands. The build uses only RELATIVE paths, which work under both. An absolute MSYS path such as
`/tmp/foo.o` reaches `cl` as the literal `C:	mpoo.o` and fails.

The script is safe to source repeatedly and reports the compiler version it found.

## Deliberate differences from the GNU build

**No header dependency tracking.** MSVC has no `-MMD`; its `/showIncludes` emits a format that would
need a parser to turn into make rules. Not worth it for this first implementation, so `cl` builds do
not track headers: run `make clean` after editing one. CI always builds clean, so it is unaffected.

**Object and archive naming is unchanged** (`.o`, `libkeel.a`, not `.obj`/`keel.lib`). `lib.exe` and
`link.exe` neither require nor care about those extensions, and keeping one naming scheme is what lets
every compile and link rule stay toolchain-neutral instead of branching on the extension.

**Object trees are per-toolchain.** `build/event_wsapoll` and `build/event_wsapoll-msvc` are separate,
because an MSVC object and a MinGW object of the same TU are not interchangeable and the archive path
is shared. Without that separation a `make CC=cl` after a `make` would mix them into one `libkeel.a`.

**Warnings.** `/W3 /WX`: warnings are fatal, as they are on GCC. This is not a translation of Keel's
GNU set -- `-Wpedantic`, `-Wshadow` and `-Wformat=2` have no `/W` equivalent, and MSVC diagnoses things
GCC does not. The intent carried across is "a deliberate set, and what it reports is fatal". Choosing
`/W3` was measured, not assumed: at `/W3` exactly three of Keel's own TUs warned, all three findings
were real, and `/W4` added nothing on this tree.

**`strtok_r` is spelled `strtok_s`** under MSVC, mapped with `/Dstrtok_r=strtok_s` in
`mk/toolchain.mk` so the sources stay POSIX-spelled. MSVC's three-argument CRT `strtok_s` has the same
signature, argument order and semantics as POSIX `strtok_r`. Without the mapping `cl` treats the calls
as implicit declarations returning `int`, which truncates the returned pointer on a 64-bit build: a
crash, not a warning, which is the concrete reason `/WX` is worth having.

## C11 atomics

`/experimental:c11atomics` is how MSVC exposes `<stdatomic.h>`; without it the header hard-errors. The
flag is named once, in `mk/toolchain.mk`. The source is not worked around to avoid it.

MSVC reports `ATOMIC_INT_LOCK_FREE == 1` ("depends on the object") where GCC reports `2` ("always").
Keel's requirement is semantic -- the flag the server-stop path touches must be operated on without a
lock, because that path runs from a signal or console-control handler -- so the build tests the
requirement rather than the macro. See `src/kl_atomic.h`: 0 fails the build, 2 is a compile-time
guarantee, and 1 triggers a runtime `atomic_is_lock_free()` query **on the actual flag**, during
`kl_http_server_init`, before any handler can be installed. A host that answered no would be refused
with `KL_ERR_UNSUPPORTED` rather than quietly running a handler that might take a lock.

## What is not built under MSVC

The full test suite. Many test TUs include `<pthread.h>` and spawn helper threads directly; the
LIBRARY never does, which is what the PAL threading seam is for, but those harnesses do and so they
need MinGW. `make CC=cl test-msvc` builds a curated set chosen to prove the things that only linking
and running can prove: the PAL socket runtime with no load-time constructor, the PAL threading seam,
adopted descriptors, and the atomics policy. They are ordinary Keel suites, so the same target also
runs under MinGW and the set cannot drift into MSVC-only test code.

Examples are also MinGW-only, for the same reason: several use `pthread_create` in the example code
itself.
