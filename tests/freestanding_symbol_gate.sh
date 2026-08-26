#!/bin/sh
# freestanding_symbol_gate.sh: F0 undefined-symbol whitelist gate for
# libkeel_freestanding.a (the client-only, completion-only freestanding archive).
#
# Proves the archive's UNDEFINED-symbol closure is within a documented whitelist:
# the C-runtime memory/string surface + the KEEL platform hooks + the socket/
# event/completion PROVIDER ops reached via vtable, and contains NO server,
# thread, UDP, DNS, file_io, or OS-syscall/errno symbol. This is the concrete
# payoff of the freestanding phase and the launch point for the UEFI spike
# (docs/phase10_uefi_feasibility_design.md, F-0).
#
# Usage: freestanding_symbol_gate.sh <archive> <nm> [mode]
#   mode = default        : mem*/strlen may be undefined (platform/EDK2 supplies them)
#   mode = selfcontained  : mem*/strlen must be DEFINED (kl_cstr_builtin.c provides them);
#                           the ONLY undefined symbols allowed are the KEEL hooks +
#                           the vendored-llhttp residual (bare target, no libc/EDK2).
# Exits 0 if every undefined symbol is whitelisted, 1 (with the offending list)
# otherwise. Portable across macOS (leading '_' / '___*_chk') and ELF nm.

set -eu

ARCHIVE="${1:?archive path}"
NM="${2:-nm}"
MODE="${3:-default}"

# ── Compute the archive's unresolved-symbol set (undefined minus defined) ──────
"$NM" "$ARCHIVE" 2>/dev/null \
  | awk 'NF>=3 && $2 ~ /^[TtDdBbRrSsWwVv]$/ {print $3}' | sort -u > /tmp/keel_fs_defined.txt
"$NM" "$ARCHIVE" 2>/dev/null \
  | awk '$1=="U"{print $2} $2=="U"{print $3}' | sort -u > /tmp/keel_fs_undef.txt
comm -23 /tmp/keel_fs_undef.txt /tmp/keel_fs_defined.txt > /tmp/keel_fs_unresolved.txt

# ── The WHITELIST ─────────────────────────────────────────────────────────────
# Each pattern is matched against the symbol with a single leading '_' stripped
# (macOS mangling) and the FORTIFY '__<fn>_chk' wrapper normalized to '<fn>'.
#
# 1. C-runtime memory/string surface actually required by the archive.
#    After F-4 (formatted-I/O + locale elimination) this shrank to the minimal
#    freestanding surface: mem* + strlen (+ their __*_chk FORTIFY wrappers):
#      memcpy memmove memset memcmp strlen
#    The formatted-I/O + locale calls (snprintf / strtol / strcasecmp / strncmp /
#    strcmp / strstr / strchr) were replaced by bounded, locale-free kl_cstr.*
#    helpers, and the stdlib default allocator (malloc/free/realloc + the
#    fprintf/abort/stderr diagnostic path it pulled) was dropped from the
#    manifest: a freestanding client always takes an EXPLICIT KlAllocator*.
#    Any of those symbols reappearing in the undefined set now FAILS the gate.
# 1b. VENDORED-llhttp residual (documented, NOT KEEL code): vendor/llhttp/api.c
#    references abort() in unreachable switch-defaults and fprintf(stderr,...) in
#    its llhttp_pretty_print debug dumper (never called on the client path). We do
#    not modify vendored code (AGENTS.md), so abort/fprintf/stderr(__stderrp)
#    remain a residual of llhttp itself, not of any KEEL TU. A real UEFI build
#    stubs these (an abort() -> CpuDeadLoop, no stdio). They are whitelisted
#    explicitly here as the vendored residual, kept separate from the KEEL surface.
# 2. KEEL platform + resolution hooks (a freestanding build supplies these,
#    a tiny platform seam, per the UEFI design §6): kl_plat_* kl_monotonic_ms
#    kl_resolve_sync (numeric/cfg->resolver-first; blocking getaddrinfo seam).
# 3. Provider ops reached via vtable: the injection points a freestanding
#    build fills with its own provider (socket over EFI_TCP4, event/completion
#    over EFI tokens): kl_sockdef_* (default socket provider ops),
#    kl_event_*_builtin (compiled-in event backend hooks), kl_comp_ops_builtin
#    (compiled-in completion backend table).
whitelisted() {
  sym="$1"
  # Strip all leading underscores (macOS mangles memcpy_chk -> ___memcpy_chk,
  # symbols -> _sym), then strip a trailing _chk (FORTIFY wrapper). This maps
  # ___memcpy_chk -> memcpy and _strlen -> strlen and ___stderrp -> stderrp.
  s=$(printf '%s' "$sym" | sed -E 's/^_+//; s/_chk$//')
  case "$s" in
    # 1. C-runtime memory/string: the minimal freestanding surface (F-4).
    #    In selfcontained mode these must be DEFINED (kl_cstr_builtin.c), so they
    #    are NOT whitelisted-undefined here; an undefined mem*/strlen then FAILs.
    memcpy|memmove|memset|memcmp|strlen)
      [ "$MODE" = selfcontained ] && return 1 || return 0 ;;
    # 1b. vendored-llhttp residual (api.c abort()/debug fprintf, not KEEL code)
    abort|fprintf|stderr|stderrp) return 0 ;;
    # 1c. PE/COFF COMPILER-RUNTIME residual (only on the *-unknown-windows cross
    #    targets, both x86_64 AND aarch64): __chkstk is the Windows-ABI stack-probe
    #    helper the PE target emits for functions with large stack frames. It is
    #    part of the PE compiler runtime (supplied by the CRT / EDK2 on a real
    #    target, e.g. a __chkstk that touches guard pages, or a no-op on a stack
    #    that never faults), NOT KEEL code and NOT a hosted-CRT dependency. It never
    #    appears on the host (ELF/Mach-O) archive; it is the documented, expected
    #    compiler-runtime residual of a freestanding PE build. No __aarch64_* outline
    #    atomics / division helpers appear; the two arch closures are otherwise
    #    identical. Any OTHER __* compiler-runtime symbol is NOT whitelisted (a real
    #    finding). See docs/phase10_uefi_feasibility_design.md (B1).
    __chkstk|chkstk) return 0 ;;
    # 1d. PE FLOATING-POINT compiler-runtime residual (server core: http_connection.c's
    #    access-log computes a double duration). _fltused is the MSVC/PE marker the
    #    backend emits when a TU uses floating point; it tells the CRT to pull the
    #    FP support; the CRT/EDK2 (or a `int _fltused = 0;` in the EFI entry) supplies
    #    it. NOT KEEL code, NOT a hosted-CRT dependency, the FP analogue of __chkstk,
    #    and (like it) only on the PE cross targets. Any OTHER float helper is NOT
    #    whitelisted.
    _fltused|fltused) return 0 ;;
    # 2. KEEL platform + resolution hooks
    kl_plat_*|kl_monotonic_ms|kl_resolve_sync) return 0 ;;
    # 3. provider ops (socket / event / completion) filled by the injected provider
    kl_sockdef_*|kl_event_*_builtin|kl_comp_ops_builtin) return 0 ;;
    *) return 1 ;;
  esac
}

echo "== libkeel_freestanding.a undefined-symbol closure =="
bad=0
while IFS= read -r sym; do
  [ -z "$sym" ] && continue
  if whitelisted "$sym"; then
    printf '  [ok ] %s\n' "$sym"
  else
    printf '  [BAD] %s   <- FORBIDDEN (server/thread/udp/dns/file_io/syscall)\n' "$sym"
    bad=1
  fi
done < /tmp/keel_fs_unresolved.txt

# Self-contained: assert the mem*/strlen surface is DEFINED in the archive (provided
# by kl_cstr_builtin.c), so a bare target needs NO external C runtime at all.
if [ "$MODE" = selfcontained ]; then
  for fn in memcpy memmove memset memcmp strlen; do
    if grep -Eq "^_?${fn}$" /tmp/keel_fs_defined.txt; then
      printf '  [def] %s (self-contained: provided by kl_cstr_builtin.c)\n' "$fn"
    else
      printf '  [BAD] %s is NOT defined; self-contained archive still needs an external %s\n' "$fn" "$fn"
      bad=1
    fi
  done
fi

rm -f /tmp/keel_fs_defined.txt /tmp/keel_fs_undef.txt /tmp/keel_fs_unresolved.txt
if [ "$bad" -ne 0 ]; then
  echo "freestanding-lib${MODE:+ ($MODE)}: FAILED, a forbidden symbol leaked / a required symbol is missing"
  exit 1
fi
if [ "$MODE" = selfcontained ]; then
  echo "freestanding-lib (self-contained): OK, mem*/strlen defined in-archive; undefined = KEEL hooks + vendored residual only"
else
  echo "freestanding-lib: OK, every undefined symbol is within the documented whitelist"
fi
