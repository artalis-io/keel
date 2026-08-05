#!/usr/bin/env bash
# run_u5.sh — U-5 harness. Runs inside the Ubuntu 24.04 container.
#   1. build the freestanding self-contained archive (make freestanding-lib-selfcontained)
#   2. build BOOTX64.EFI (build_u5.sh) with the hostname + nameserver defines
#   3. build a FAT ESP image with EFI/BOOT/BOOTX64.EFI + startup.nsh
#   4. start a tiny host DNS server on 0.0.0.0:53 answering A <HOSTNAME> -> 10.0.2.2
#   5. start a python HTTP responder on 0.0.0.0:PORT (guest reaches it at 10.0.2.2:PORT)
#   6. boot QEMU x86_64 + OVMF (TCG, no KVM), e1000 NIC, SLIRP user-net, serial to stdout
#   7. capture serial, grep for U-5: markers / the resolved / status lines
#
# The guest reaches the host services at 10.0.2.2 (the SLIRP host/gateway address), so
# KL_U5_NAMESERVER defaults to 10.0.2.2 and the DNS server answers with 10.0.2.2 (the
# same host, where the HTTP responder listens).
#
# Env:
#   KEEL_ROOT        repo root (default: ../..)
#   TARGET_PORT      host responder port (default 18080)
#   KL_U5_HOSTNAME   hostname the guest resolves (default keel.test)
#   KL_U5_NAMESERVER guest-visible DNS server address (default 10.0.2.2)
#   OVMF_CODE/VARS   override OVMF fds
#   BOOT_TIMEOUT     qemu timeout seconds (default 200 — DNS + TCG is slow)
set -uo pipefail
cd "$(dirname "$0")"

: "${KEEL_ROOT:=../..}"
TARGET_PORT="${TARGET_PORT:-18080}"
KL_U5_HOSTNAME="${KL_U5_HOSTNAME:-keel.test}"
KL_U5_NAMESERVER="${KL_U5_NAMESERVER:-10.0.2.2}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-200}"
export TARGET_PORT KL_U5_HOSTNAME KL_U5_NAMESERVER

echo "=== U-5 run harness ==="
echo "hostname=$KL_U5_HOSTNAME nameserver=$KL_U5_NAMESERVER port=$TARGET_PORT"

# ---- 1. build the freestanding self-contained archive (x86_64) ----
pick() { for c in "$@"; do command -v "$c" >/dev/null 2>&1 && { echo "$c"; return; }; done; }
LLVM_NM="$(pick llvm-nm llvm-nm-18 llvm-nm-17 llvm-nm-16)"
LLVM_AR="$(pick llvm-ar llvm-ar-18 llvm-ar-17 llvm-ar-16)"
ARCHIVE_ARGS=()
[ -n "$LLVM_NM" ] && ARCHIVE_ARGS+=( "NM=$LLVM_NM" )
[ -n "$LLVM_AR" ] && ARCHIVE_ARGS+=( "AR=$LLVM_AR" )
echo "building libkeel_freestanding_selfcontained.a (${ARCHIVE_ARGS[*]:-default tools}) ..."
( cd "$KEEL_ROOT" && make freestanding-lib-selfcontained "${ARCHIVE_ARGS[@]}" ) \
  || { echo "ARCHIVE BUILD FAILED"; exit 2; }

# ---- 2. build the EFI app ----
TARGET_PORT="$TARGET_PORT" KL_U5_HOSTNAME="$KL_U5_HOSTNAME" \
  KL_U5_NAMESERVER="$KL_U5_NAMESERVER" ./build_u5.sh || { echo "BUILD FAILED"; exit 2; }

# ---- 3. ESP FAT image ----
ESP=esp.img
rm -f "$ESP"
dd if=/dev/zero of="$ESP" bs=1M count=48 status=none
mformat -i "$ESP" -F ::
mmd    -i "$ESP" ::/EFI
mmd    -i "$ESP" ::/EFI/BOOT
mcopy  -i "$ESP" BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
mcopy  -i "$ESP" startup.nsh ::/startup.nsh
echo "ESP image built:"
mdir -i "$ESP" ::/EFI/BOOT/

# ---- 4. host DNS server on 0.0.0.0:53 ----
# Answers an A query for $KL_U5_HOSTNAME with 10.0.2.2 (the SLIRP host the guest
# reaches). Ignores non-A / other-name queries (returns NOERROR/no-answer or nothing).
# Binding :53 needs root — the container runs as root, so this is fine.
DNS_ANSWER_IP="10.0.2.2"
cat > /tmp/u5_dns.py <<PYEOF
import socket, struct, sys

HOSTNAME = ${KL_U5_HOSTNAME@Q}
ANSWER_IP = "${DNS_ANSWER_IP}"

def encode_name(name):
    out = b""
    for label in name.split("."):
        if label:
            out += bytes([len(label)]) + label.encode("ascii")
    return out + b"\x00"

def parse_qname(data, off):
    labels = []
    while True:
        if off >= len(data):
            return None, off
        l = data[off]
        if l == 0:
            off += 1
            break
        if (l & 0xC0):  # no compression in a question we generate; bail
            return None, off
        off += 1
        labels.append(data[off:off+l].decode("ascii", "replace"))
        off += l
    return ".".join(labels), off

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind(("0.0.0.0", 53))
sys.stderr.write("u5 DNS server up on 0.0.0.0:53 -> %s A %s\n" % (HOSTNAME, ANSWER_IP))
sys.stderr.flush()

while True:
    try:
        data, addr = sock.recvfrom(1500)
    except Exception:
        continue
    if len(data) < 12:
        continue
    tid = data[0:2]
    qname, off = parse_qname(data, 12)
    if qname is None or off + 4 > len(data):
        continue
    qtype = struct.unpack(">H", data[off:off+2])[0]
    sys.stderr.write("u5 DNS query: %s type=%d from %s\n" % (qname, qtype, addr))
    sys.stderr.flush()
    # header: id, flags QR=1 RD=1 RA=1, qd=1, an=(1 if A match else 0)
    is_a = (qtype == 1 and qname.lower() == HOSTNAME.lower())
    ancount = 1 if is_a else 0
    flags = struct.pack(">H", 0x8180)  # QR|RD|RA, RCODE=0
    header = tid + flags + struct.pack(">HHHH", 1, ancount, 0, 0)
    question = data[12:off+4]  # echo the exact question (name+type+class)
    resp = header + question
    if is_a:
        # answer: name(ptr to 0x0c), type A, class IN, ttl 60, rdlen 4, rdata
        resp += b"\xc0\x0c" + struct.pack(">HHIH", 1, 1, 60, 4)
        resp += socket.inet_aton(ANSWER_IP)
    try:
        sock.sendto(resp, addr)
    except Exception:
        pass
PYEOF
python3 /tmp/u5_dns.py >/tmp/u5_dns.log 2>&1 &
DNS_PID=$!
sleep 1
echo "u5 DNS server pid=$DNS_PID on 0.0.0.0:53 (answers $KL_U5_HOSTNAME -> $DNS_ANSWER_IP)"
cat /tmp/u5_dns.log || true

# ---- 5. host HTTP responder ----
mkdir -p wwwroot
printf 'U-5 KlClient responder OK\n' > wwwroot/index.html
( cd wwwroot && python3 -m http.server "$TARGET_PORT" --bind 0.0.0.0 ) >/tmp/httpd_u5.log 2>&1 &
HTTPD_PID=$!
sleep 1
echo "python http.server pid=$HTTPD_PID on 0.0.0.0:$TARGET_PORT"

cleanup() { kill "$HTTPD_PID" "$DNS_PID" 2>/dev/null || true; }
trap cleanup EXIT

# ---- 6. locate OVMF ----
find_ovmf() {
  for c in \
    "${OVMF_CODE:-}" \
    /usr/share/OVMF/OVMF_CODE_4M.fd \
    /usr/share/OVMF/OVMF_CODE.fd \
    /usr/share/ovmf/OVMF.fd ; do
    [ -n "$c" ] && [ -f "$c" ] && { echo "$c"; return; }
  done
}
find_vars() {
  for v in \
    "${OVMF_VARS:-}" \
    /usr/share/OVMF/OVMF_VARS_4M.fd \
    /usr/share/OVMF/OVMF_VARS.fd ; do
    [ -n "$v" ] && [ -f "$v" ] && { echo "$v"; return; }
  done
}

OVMF_CODE_FD="$(find_ovmf)"
OVMF_VARS_FD="$(find_vars)"
if [ -z "$OVMF_CODE_FD" ]; then echo "ERROR: no OVMF code fd found"; exit 3; fi
echo "OVMF code: $OVMF_CODE_FD"
echo "OVMF vars: ${OVMF_VARS_FD:-<none>}"

VARS_RW=vars_rw.fd
if [ -n "$OVMF_VARS_FD" ]; then cp "$OVMF_VARS_FD" "$VARS_RW"; fi

# ---- 7. boot QEMU (TCG; no KVM in container) ----
QEMU_ARGS=(
  -machine q35
  -m 512
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE_FD"
)
[ -n "$OVMF_VARS_FD" ] && QEMU_ARGS+=( -drive if=pflash,format=raw,file="$VARS_RW" )
QEMU_ARGS+=(
  -drive format=raw,file="$ESP"
  -netdev "user,id=n0"
  -device e1000,netdev=n0
  -serial stdio
  -display none
  -monitor none
  -no-reboot
)

echo "=== qemu cmd ==="
echo "timeout ${BOOT_TIMEOUT} qemu-system-x86_64 ${QEMU_ARGS[*]}"
echo "=== serial output begin ==="
timeout "${BOOT_TIMEOUT}" qemu-system-x86_64 "${QEMU_ARGS[@]}" 2>&1 | tee /tmp/serial_u5.log
echo "=== serial output end ==="

echo "=== DNS server log ==="
cat /tmp/u5_dns.log || true

echo "=== grep markers ==="
grep -E 'U-5:|HTTP/1\.[01]' /tmp/serial_u5.log || echo "(no U-5 markers found)"
