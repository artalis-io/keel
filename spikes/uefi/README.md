# U-0 spike — raw EFI_TCP4 GET under QEMU/OVMF

**Decision: GO.** A raw `EFI_TCP4` UEFI application, running under QEMU + stock Ubuntu OVMF
(TCG, no KVM), completed an HTTP/1.1 `GET / → HTTP/1.0 200 OK` over the network and printed
`U-0: GO` on the serial console. This is the go/no-go gate for Phase 10
(`docs/phase10_uefi_feasibility_design.md` §7-8, F-8 step 1).

This is a **throwaway spike** — it shapes the future `KlCompletionOps → EFI_TCP4` mapping (U-3);
it is NOT production KEEL code. Nothing under `src/`, `include/`, or the main Makefile is touched.

## Files

| File | Role |
|------|------|
| `tcp4_get.c`  | The raw EFI_TCP4 app: `efi_main` → locate SB → CreateChild → OpenProtocol → Configure(DHCP) → Connect → Transmit → Receive → print → Close/DestroyChild. |
| `efi_min.h`   | Vendored minimal UEFI base types + boot/console services (UEFI 2.10 §2/§4/§7/§12.4). Self-contained so the app builds freestanding with **no gnu-efi runtime**. |
| `efi_tcp4.h`  | Vendored `EFI_TCP4_*` + `EFI_SERVICE_BINDING_PROTOCOL` GUIDs/structs/vtables (UEFI 2.10 §28.1/§28.5). gnu-efi's TCP4 coverage is thin, so these are vendored per spec. |
| `build.sh`    | clang/lld freestanding build → `BOOTX64.EFI` (PE32+). Matches KEEL's freestanding toolchain. |
| `run.sh`      | Harness: build → FAT ESP image (mtools) → python responder → QEMU+OVMF → capture serial → grep markers. |
| `startup.nsh` | UEFI shell auto-run script (only used if booting to the shell; the ESP boots `EFI/BOOT/BOOTX64.EFI` directly). |

## Toolchain / environment

- Apple `container` (Ubuntu 24.04, **arm64**). QEMU x86_64 runs under **TCG** (software emulation)
  — no `-enable-kvm`. Packages: `qemu-system-x86 ovmf gnu-efi mtools dosfstools clang lld python3`.
- **Build path chosen: clang/lld freestanding** (`--target=x86_64-unknown-windows -ffreestanding`
  + `lld-link /subsystem:efi_application /entry:efi_main`). This was necessary *and* preferable:
  on an arm64 host the Ubuntu `gnu-efi` package ships only the **aarch64** `crt0`/`.lds` (no x86_64
  CRT), so the gnu-efi x86_64 link path is unavailable in-container — and clang/lld matches the
  KEEL freestanding toolchain (`make freestanding-link`) anyway. `efi_min.h` removes the gnu-efi
  runtime dependency entirely.

## Reproduce

```bash
container run -d --name u0spike -v "$PWD/spikes/uefi:/spike" ubuntu:24.04 sleep infinity
container exec u0spike bash -c 'export DEBIAN_FRONTEND=noninteractive; apt-get update -qq && \
  apt-get install -y -qq qemu-system-x86 ovmf gnu-efi mtools dosfstools clang lld python3 binutils file'
container exec u0spike bash -c 'cd /spike && CC=clang BOOT_TIMEOUT=110 ./run.sh'
```

## Result (verbatim serial tail, ANSI stripped)

```
U-0: EFI_TCP4 spike starting
U-0: found 1 TCP4 ServiceBinding handle(s)
U-0: TCP4 child protocol opened
U-0: Configure OK (address mapped via DHCP)
U-0: station 10.0.2.15 -> remote 10.0.2.2:18080
U-0: TCP connected
U-0: GET transmitted
U-0: --- response begin ---
HTTP/1.0 200 OK
Server: SimpleHTTP/0.6 Python/3.12.3
Date: Wed, 05 Aug 2026 16:44:24 GMT
Content-type: text/html
Content-Length: 23
Last-Modified: Wed, 05 Aug 2026 16:44:12 GMT

U-0 spike responder OK

U-0: --- response end ---
U-0: EFI_TCP4 GET status = HTTP/1.0 200 OK
U-0: GO
U-0: done
```

Host responder log confirms the request reached it: `127.0.0.1 - - "GET / HTTP/1.1" 200 -`.

## QEMU invocation (verbatim)

```
qemu-system-x86_64 -machine q35 -m 512 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file=vars_rw.fd \
  -drive format=raw,file=esp.img \
  -netdev user,id=n0 -device e1000,netdev=n0 \
  -serial stdio -display none -monitor none -no-reboot
```

OVMF fds: `/usr/share/OVMF/OVMF_CODE_4M.fd` (code) + `/usr/share/OVMF/OVMF_VARS_4M.fd` (vars,
copied writable). e1000 NIC, SLIRP user-net (guest 10.0.2.15 via DHCP, host 10.0.2.2).

## Key finding — the design doc's "likely blocker" did NOT materialize

`docs/phase10_uefi_feasibility_design.md` §7 flagged the likely blocker as *"stock Ubuntu OVMF
built without the EDK2 network stack → no `EFI_TCP4_SERVICE_BINDING_PROTOCOL`"*. **Not so here:**
stock Ubuntu 24.04 `ovmf` (`OVMF_CODE_4M.fd`, package `2024.02-2ubuntu0.9`) **ships a working
TCP4 stack** — `LocateHandleBuffer(EFI_TCP4_SERVICE_BINDING_PROTOCOL_GUID)` returned 1 handle,
DHCP settled (10.0.2.15), and Connect/Transmit/Receive all completed. No custom edk2 build, no
iPXE, no NIC option-ROM juggling was needed. (The `strings` grep for `Tcp4Dxe`/`Ip4Dxe` finds
nothing because the drivers live compressed inside the firmware volume; the authoritative test is
running the app, which succeeded.)

## EFI_TCP4 lifecycle exercised (→ informs U-3 `KlCompletionOps`)

CreateChild → OpenProtocol → Configure/DHCP → Connect → Transmit → Receive → Close → DestroyChild.
Surprises vs the design doc §4 mapping:

1. **Token events must be bare (type 0), NOT `EVT_NOTIFY_WAIT`.** The first run failed at
   `CreateEvent(EVT_NOTIFY_WAIT, …, NULL, NULL, …)` with `EFI_INVALID_PARAMETER`
   (`0x8000000000000002`): `EVT_NOTIFY_WAIT`/`EVT_NOTIFY_SIGNAL` **require** a non-NULL
   NotifyFunction (UEFI 2.10 §7.1.1). For `CheckEvent`/`WaitForEvent`-polled completion tokens,
   create the event with type `0`. **U-3 note:** the completion-backend `drain` should create its
   per-op token events as type-0 events (optionally add ONE `EVT_TIMER` event to the
   `WaitForEvent` set for the loop tick / deadlines, per §4's `drain` row).

2. **The pump is `Poll()` + `CheckEvent()`, not just `WaitForEvent()`.** Progress required calling
   `Tcp4->Poll(This)` each spin to advance the stack, then `CheckEvent(token.Event)`. This is
   exactly KEEL's "the event loop **is** the firmware pump" model (lwip-raw `NO_SYS=1` precedent).
   `drain(ctx, out, max, timeout_ms)` → loop `Poll()` + `CheckEvent()` over pending tokens,
   translate `token->Status` → Keel completion category, bounded by an `EVT_TIMER`.

3. **The child connection is an opened protocol, not a returned pointer** (design-doc correction
   confirmed): `ServiceBinding->CreateChild(&childHandle)` yields a *handle*; the usable
   `EFI_TCP4_PROTOCOL*` comes from `OpenProtocol(childHandle, EFI_TCP4_PROTOCOL_GUID, …)`.
   `BY_DRIVER` was accepted here; `GET_PROTOCOL` is the fallback. This is the `KlSocketHandle`
   carrying an `EFI_TCP4_PROTOCOL*` (handle.h) the design describes.

4. **Configure is synchronous but DHCP is not** — `Configure()` returns `EFI_NO_MAPPING` until the
   default address is bound; retry with `Poll()`+`Stall()` (or watch `GetModeData`). Here it bound
   on the first retry. **U-3 note:** the socket provider's `configure`/`connect` must tolerate
   `EFI_NO_MAPPING` and pump until the address settles (or surface it as a would-block/retry).

5. **`EFI_STATUS` → Keel category** is a clean switch: `EFI_CONNECTION_FIN` = EOF/read-0,
   `EFI_CONNECTION_RESET/REFUSED` = connection error, `EFI_NOT_READY` = would-block/keep-pumping,
   `EFI_TIMEOUT` = deadline. No `errno` anywhere (matches the freestanding contract, §6).

6. **Teardown ordering matches §5**: `Configure(NULL)` to reset → `Close(&CloseToken)` (async, pump
   its event) → `CloseEvent` every event → `CloseProtocol` → `DestroyChild` → `FreePool` the handle
   buffer. Exactly-once terminal-result + no leak.

## Scope / honesty notes

- This is the **raw EFI app** leg of U-0 (design §7 step 2) — it proves the EFI_TCP4
  token/event/pump model end-to-end. The **host EFI_TCP4 mock skeleton** (§7 step 4 / U-0's other
  half, the ASan-gated CI double) is NOT built here; that is the next artifact and can proceed on
  the host with the lifecycle characterized above.
- Runs under **TCG** (software emulation) since the container VM has no nested KVM — slower but
  functionally identical; the completion came back well within the 110s bound.
- IPv4 only (design's first cut); IPv6/`EFI_TCP6` is a family switch.
