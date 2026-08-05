#!/bin/sh
# freestanding_symbol_gate.sh — F0 undefined-symbol whitelist gate for
# libkeel_freestanding.a (the client-only, completion-only freestanding archive).
#
# Proves the archive's UNDEFINED-symbol closure is within a documented whitelist:
# the C-runtime memory/string surface + the KEEL platform hooks + the socket/
# event/completion PROVIDER ops reached via vtable — and contains NO server,
# thread, UDP, DNS, file_io, or OS-syscall/errno symbol. This is the concrete
# payoff of the freestanding phase and the launch point for the UEFI spike
# (docs/phase10_uefi_feasibility_design.md, F-0).
#
# Usage: freestanding_symbol_gate.sh <archive> <nm>
# Exits 0 if every undefined symbol is whitelisted, 1 (with the offending list)
# otherwise. Portable across macOS (leading '_' / '___*_chk') and ELF nm.

set -eu

ARCHIVE="${1:?archive path}"
NM="${2:-nm}"

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
# 1. C-runtime memory/string surface actually required by the archive:
#      memcpy memmove memset memcmp strlen strcmp strncmp strcasecmp strchr
#      strstr strtol snprintf   (+ their __*_chk FORTIFY wrappers)
#    and the stdlib allocator + minimal error/abort path the default stdlib
#    allocator wrapper and assert/format-diagnostic paths pull:
#      malloc free realloc   fprintf abort stderr(__stderrp)
# 2. KEEL platform + resolution hooks (a freestanding build supplies these —
#    a tiny platform seam, per the UEFI design §6): kl_plat_* kl_monotonic_ms
#    kl_resolve_sync (numeric/cfg->resolver-first; blocking getaddrinfo seam).
# 3. Provider ops reached via vtable — the injection points a freestanding
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
    # 1. C-runtime memory/string
    memcpy|memmove|memset|memcmp|strlen|strcmp|strncmp|strcasecmp|strchr|strstr|strtol|snprintf) return 0 ;;
    # 1. stdlib allocator + diagnostic path (stderrp is macOS's stdio __stderrp)
    malloc|free|realloc|fprintf|abort|stderr|stderrp) return 0 ;;
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

rm -f /tmp/keel_fs_defined.txt /tmp/keel_fs_undef.txt /tmp/keel_fs_unresolved.txt
if [ "$bad" -ne 0 ]; then
  echo "freestanding-lib: FAILED — a forbidden symbol leaked into the client archive"
  exit 1
fi
echo "freestanding-lib: OK — every undefined symbol is within the documented whitelist"
