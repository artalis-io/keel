#!/usr/bin/env bash
# run_dgram_dns.sh: 6.4c harness: the STOCK dns_resolver over KlDatagram-over-EFI_UDP4 on real
# firmware, to the FROZEN acceptance (docs/phase10_efi_udp4_provider_design.md §9). Runs
# inside the Ubuntu 24.04 container. Two QEMU/OVMF boots of one EFI image:
#
#   HAPPY: python DNS answers A keel.test -> 10.0.2.2; python HTTP responder on TARGET_PORT.
#          Assert: resolver ran A+AAAA over EFI_UDP4, GET returned 200, udp_live==0/quar==0.
#   TC:    python DNS answers with TC=1 (truncated) + no records. The freestanding resolver
#          has NO TCP fallback (6.4a-2), so it must settle a clean KL_ERR_DNS; no hang.
#          Assert: NO-GO, no HTTP 200, udp_live==0, terminal DONE reached (bounded failure).
#
# The self-test only reports FACTS; THIS harness is the oracle. It exits non-zero unless BOTH
# boots pass. The guest reaches host services at 10.0.2.2 (the SLIRP host/gateway).
#
# Env:
#   KEEL_ROOT        repo root (default: ../../../..)
#   TARGET_PORT      host HTTP responder port (default 18080)
#   KL_U9_HOSTNAME   hostname the guest resolves (default keel.test)
#   KL_U9_NAMESERVER guest-visible DNS server address (default 10.0.2.2)
#   OVMF_CODE/VARS   override OVMF fds
#   BOOT_TIMEOUT     per-boot qemu safety timeout seconds (default 150; DNS/GET + TCG is slow)
set -uo pipefail
cd "$(dirname "$0")"

: "${KEEL_ROOT:=../../../..}"
TARGET_PORT="${TARGET_PORT:-18080}"
KL_U9_HOSTNAME="${KL_U9_HOSTNAME:-keel.test}"
KL_U9_NAMESERVER="${KL_U9_NAMESERVER:-10.0.2.2}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-150}"
export TARGET_PORT KL_U9_HOSTNAME KL_U9_NAMESERVER

echo "=== 6.4c run harness (stock dns_resolver over EFI_UDP4 → GET, + TC case) ==="
echo "hostname=$KL_U9_HOSTNAME nameserver=$KL_U9_NAMESERVER port=$TARGET_PORT"

# ---- 1. build both self-contained archives (client + dns/datagram), x86_64 ----
pick() { for c in "$@"; do command -v "$c" >/dev/null 2>&1 && { echo "$c"; return; }; done; }
LLVM_NM="$(pick llvm-nm llvm-nm-18 llvm-nm-17 llvm-nm-16)"
LLVM_AR="$(pick llvm-ar llvm-ar-18 llvm-ar-17 llvm-ar-16)"
ARCHIVE_ARGS=()
[ -n "$LLVM_NM" ] && ARCHIVE_ARGS+=( "NM=$LLVM_NM" )
[ -n "$LLVM_AR" ] && ARCHIVE_ARGS+=( "AR=$LLVM_AR" )
echo "building archives (${ARCHIVE_ARGS[*]:-default tools}) ..."
( cd "$KEEL_ROOT" && make freestanding-lib-selfcontained "${ARCHIVE_ARGS[@]}" ) \
  || { echo "CLIENT ARCHIVE BUILD FAILED"; exit 2; }
( cd "$KEEL_ROOT" && make freestanding-lib-dns-selfcontained "${ARCHIVE_ARGS[@]}" ) \
  || { echo "DNS ARCHIVE BUILD FAILED"; exit 2; }

# ---- 2. build the EFI app ----
TARGET_PORT="$TARGET_PORT" KL_U9_HOSTNAME="$KL_U9_HOSTNAME" KL_U9_NAMESERVER="$KL_U9_NAMESERVER" \
  ./build_dgram_dns.sh || { echo "BUILD FAILED"; exit 2; }

# ---- 3. ESP FAT image ----
ESP=esp.img
rm -f "$ESP"
dd if=/dev/zero of="$ESP" bs=1M count=48 status=none
mformat -i "$ESP" -F ::
mmd    -i "$ESP" ::/EFI
mmd    -i "$ESP" ::/EFI/BOOT
mcopy  -i "$ESP" BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
mcopy  -i "$ESP" startup.nsh ::/startup.nsh
echo "ESP image built:"; mdir -i "$ESP" ::/EFI/BOOT/

# ---- 4. host HTTP responder (happy path GET target) ----
mkdir -p wwwroot
printf '6.4c EFI_UDP4 responder OK\n' > wwwroot/index.html
( cd wwwroot && python3 -m http.server "$TARGET_PORT" --bind 0.0.0.0 ) >/tmp/httpd_u9.log 2>&1 &
HTTPD_PID=$!
sleep 1
echo "python http.server pid=$HTTPD_PID on 0.0.0.0:$TARGET_PORT"

# ---- 5. DNS server (mode-switchable) on 0.0.0.0:53 ----
# HAPPY: answer A keel.test -> 10.0.2.2, AAAA -> empty NOERROR (A wins, no hang).
# TC:    every response has TC=1 (truncated) and no records; exercises the freestanding
#        no-TCP-fallback branch, which must settle KL_ERR_DNS.
DNS_ANSWER_IP="10.0.2.2"
cat > /tmp/u9_dns.py <<PYEOF
import socket, struct, sys, os

HOSTNAME  = ${KL_U9_HOSTNAME@Q}
ANSWER_IP = "${DNS_ANSWER_IP}"
MODE      = os.environ.get("DNS_MODE", "happy")

def parse_qname(data, off):
    labels = []
    while True:
        if off >= len(data): return None, off
        l = data[off]
        if l == 0: off += 1; break
        if (l & 0xC0): return None, off
        off += 1
        labels.append(data[off:off+l].decode("ascii", "replace")); off += l
    return ".".join(labels), off

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind(("0.0.0.0", 53))
sys.stderr.write("6.4c DNS server up on :53 mode=%s -> %s A %s\n" % (MODE, HOSTNAME, ANSWER_IP))
sys.stderr.flush()

while True:
    try:
        data, addr = sock.recvfrom(1500)
    except Exception:
        continue
    if len(data) < 12: continue
    tid = data[0:2]
    qname, off = parse_qname(data, 12)
    if qname is None or off + 4 > len(data): continue
    qtype = struct.unpack(">H", data[off:off+2])[0]
    sys.stderr.write("6.4c DNS query: %s type=%d mode=%s from %s\n" % (qname, qtype, MODE, addr))
    sys.stderr.flush()
    question = data[12:off+4]
    is_tc = (MODE == "tc")
    if is_tc:
        # QR|RD|RA|TC, RCODE=0, no records: a truncated answer.
        flags = struct.pack(">H", 0x8380)
        resp = tid + flags + struct.pack(">HHHH", 1, 0, 0, 0) + question
    else:
        is_a = (qtype == 1 and qname.lower() == HOSTNAME.lower())
        ancount = 1 if is_a else 0
        flags = struct.pack(">H", 0x8180)  # QR|RD|RA, RCODE=0
        resp = tid + flags + struct.pack(">HHHH", 1, ancount, 0, 0) + question
        if is_a:
            resp += b"\xc0\x0c" + struct.pack(">HHIH", 1, 1, 60, 4) + socket.inet_aton(ANSWER_IP)
    try:
        n = sock.sendto(resp, addr)
        # Prove the (truncated) response was actually delivered to the guest BEFORE the
        # harness attributes the resolver failure to the rejection (6.3 negative-test rule).
        if is_tc:
            sys.stderr.write("6.4c DNS TC-SENT %d bytes (TC=1, 0 records) type=%d to %s\n" % (n, qtype, addr))
            sys.stderr.flush()
    except Exception as e:
        sys.stderr.write("6.4c DNS sendto FAILED: %s\n" % e); sys.stderr.flush()
PYEOF

DNS_PID=""
start_dns() {  # $1 = mode
  [ -n "$DNS_PID" ] && kill "$DNS_PID" 2>/dev/null || true
  DNS_MODE="$1" python3 /tmp/u9_dns.py >"/tmp/u9_dns_$1.log" 2>&1 &
  DNS_PID=$!
  sleep 1
  echo "6.4c DNS server pid=$DNS_PID mode=$1"
}

cleanup() { kill "$HTTPD_PID" "$DNS_PID" 2>/dev/null || true; }
trap cleanup EXIT

# ---- 6. locate OVMF ----
find_ovmf() { for c in "${OVMF_CODE:-}" /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF_CODE.fd /usr/share/ovmf/OVMF.fd; do [ -n "$c" ] && [ -f "$c" ] && { echo "$c"; return; }; done; }
find_vars() { for v in "${OVMF_VARS:-}" /usr/share/OVMF/OVMF_VARS_4M.fd /usr/share/OVMF/OVMF_VARS.fd; do [ -n "$v" ] && [ -f "$v" ] && { echo "$v"; return; }; done; }
OVMF_CODE_FD="$(find_ovmf)"; OVMF_VARS_FD="$(find_vars)"
if [ -z "$OVMF_CODE_FD" ]; then echo "ERROR: no OVMF code fd found"; exit 3; fi
echo "OVMF code: $OVMF_CODE_FD ; vars: ${OVMF_VARS_FD:-<none>}"

# ---- 7. one boot: $1 = mode, writes serial to /tmp/serial_<mode>.log ----
boot() {
  local mode="$1" serial="/tmp/serial_$1.log"
  local vars_rw="vars_$1.fd"
  [ -n "$OVMF_VARS_FD" ] && cp "$OVMF_VARS_FD" "$vars_rw"
  local args=( -machine q35 -m 512 -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE_FD" )
  [ -n "$OVMF_VARS_FD" ] && args+=( -drive if=pflash,format=raw,file="$vars_rw" )
  args+=( -drive format=raw,file="$ESP" -netdev user,id=n0 -device e1000,netdev=n0
          -serial stdio -display none -monitor none -no-reboot )
  echo "=== boot mode=$mode (timeout ${BOOT_TIMEOUT}s) ==="
  timeout "${BOOT_TIMEOUT}" qemu-system-x86_64 "${args[@]}" 2>&1 | tee "$serial"
  echo "(qemu exit: ${PIPESTATUS[0]}; 124=safety-timeout, expected only if firmware ignored reset -s)"
}

# ---- 8. oracles ----
FAILS=0
has() { grep -qE "$2" "$1"; }

assert_happy() {
  local s=/tmp/serial_happy.log dns=/tmp/u9_dns_happy.log ok=1
  echo "--- HAPPY oracle ---"
  has "$s" '6\.4c: DONE'                     && echo "  [ok] terminal DONE reached"        || { echo "  [FAIL] no DONE (crash/hang)"; ok=0; }
  has "$s" '6\.4c: GO'                        && echo "  [ok] GO"                            || { echo "  [FAIL] no GO"; ok=0; }
  has "$s" 'http status = 200'               && echo "  [ok] HTTP 200 over EFI_TCP4"        || { echo "  [FAIL] no HTTP 200"; ok=0; }
  has "$s" 'udp_live = 0 udp_quarantined = 0' && echo "  [ok] udp_live=0 quarantined=0"     || { echo "  [FAIL] dirty UDP teardown"; ok=0; }
  has "$dns" 'type=1 '                        && echo "  [ok] A query seen over EFI_UDP4"    || { echo "  [FAIL] no A query"; ok=0; }
  has "$dns" 'type=28 '                       && echo "  [ok] AAAA query seen over EFI_UDP4" || { echo "  [FAIL] no AAAA query"; ok=0; }
  [ "$ok" = 1 ] && echo "  HAPPY: PASS" || { echo "  HAPPY: FAIL"; FAILS=$((FAILS+1)); }
}

assert_tc() {
  local s=/tmp/serial_tc.log dns=/tmp/u9_dns_tc.log ok=1
  echo "--- TC oracle (truncated response, no TCP fallback → clean KL_ERR_DNS) ---"
  has "$s" '6\.4c: DONE'                 && echo "  [ok] terminal DONE reached (bounded, no hang)" || { echo "  [FAIL] no DONE (hang on TC?)"; ok=0; }
  has "$s" '6\.4c: NO-GO'                && echo "  [ok] NO-GO (did not falsely succeed)"          || { echo "  [FAIL] unexpected GO on TC"; ok=0; }
  ! has "$s" 'http status = 200'         && echo "  [ok] no HTTP 200 (resolve failed as required)" || { echo "  [FAIL] got HTTP 200 on TC"; ok=0; }
  # Attribute the failure to the truncation, not a timeout/socket/malformed reject. Per the
  # 6.3 negative-test rule the DNS server must PROVE it put a truncated response on the wire
  # for BOTH legs (TC-SENT type=1 AND type=28, after sendto), and the resolver must report
  # KL_ERR_DNS specifically (error = 10).
  has "$dns" 'TC-SENT.*type=1 '          && echo "  [ok] A-leg truncated response delivered"       || { echo "  [FAIL] no TC response for the A leg"; ok=0; }
  has "$dns" 'TC-SENT.*type=28 '         && echo "  [ok] AAAA-leg truncated response delivered"    || { echo "  [FAIL] no TC response for the AAAA leg"; ok=0; }
  has "$s" '6\.4c: resolve-error = KL_ERR_DNS' && echo "  [ok] failure is KL_ERR_DNS (the TC rejection)" || { echo "  [FAIL] failure not attributable to TC rejection"; ok=0; }
  has "$s" 'error = 10'                  && echo "  [ok] error = 10 (KL_ERR_DNS numeric)"          || { echo "  [FAIL] error != 10"; ok=0; }
  has "$s" 'udp_live = 0 udp_quarantined = 0' && echo "  [ok] udp_live=0 quarantined=0 (clean teardown on TC too)" || { echo "  [FAIL] dirty UDP teardown on TC (live or quarantine != 0)"; ok=0; }
  has "$dns" 'type=1 '                   && echo "  [ok] guest issued the query (TC path exercised)" || { echo "  [FAIL] no query reached DNS"; ok=0; }
  # Timing: response-driven TC settlement is prompt (leg fails immediately on TC, no retransmit),
  # so it lands well below the per-leg timeout. A DROPPED TC response would only settle after
  # the ≥leg_timeout_ms timer, indistinguishable from this test's other markers WITHOUT this
  # check. Require completion under 2/3 of the leg timeout (derived from the serial, not magic).
  local legto ms thr
  legto=$(grep -oE 'leg_timeout_ms = [0-9]+' "$s" | grep -oE '[0-9]+' | head -1)
  ms=$(grep -oE 'elapsed_ms = [0-9]+' "$s" | grep -oE '[0-9]+' | head -1)
  if [ -n "$legto" ] && [ -n "$ms" ]; then
    thr=$(( legto * 2 / 3 ))
    if [ "$ms" -lt "$thr" ]; then
      echo "  [ok] TC settled in ${ms}ms < ${thr}ms (2/3 of ${legto}ms leg timeout) → response-driven, not timeout"
    else
      echo "  [FAIL] TC settled in ${ms}ms (>= ${thr}ms); cannot rule out a timeout-driven failure"; ok=0
    fi
  else
    echo "  [FAIL] missing leg_timeout_ms/elapsed_ms markers (cannot prove prompt settlement)"; ok=0
  fi
  [ "$ok" = 1 ] && echo "  TC: PASS" || { echo "  TC: FAIL"; FAILS=$((FAILS+1)); }
}

# ---- 9. run both boots ----
start_dns happy; boot happy
echo "=== HAPPY DNS log ==="; cat /tmp/u9_dns_happy.log || true
assert_happy

start_dns tc;    boot tc
echo "=== TC DNS log ==="; cat /tmp/u9_dns_tc.log || true
assert_tc

echo "======================================================"
if [ "$FAILS" -eq 0 ]; then
  echo "RESULT: PASS, stock dns_resolver over EFI_UDP4: GET 200 (happy) + clean KL_ERR_DNS (TC), no UDP leak"
  exit 0
else
  echo "RESULT: FAIL, $FAILS boot(s) did not meet the frozen 6.4c acceptance"
  exit 1
fi
