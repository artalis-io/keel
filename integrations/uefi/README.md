# KEEL — UEFI integration (U-1: platform + allocator shims)

A **bring-your-own** integration (like `integrations/platform/lwip/`, `integrations/tls/mbedtls/`):
nothing here is wired into the root `Makefile` or into `src/`/`include/`. It
supplies the freestanding platform + allocator seams a UEFI-hosted KEEL client
needs, and proves them in QEMU/OVMF.

This is **U-1**, the first stage of the real EFI provider (Phase 10, F-8),
building on the merged **U-0 spike** (`spikes/uefi/` — a raw EFI_TCP4 GET under
QEMU/OVMF). U-1 is **platform + allocator only** — no socket/TCP yet (that's
U-2/U-3).

## What the freestanding archive leaves for the platform

`libkeel_freestanding_selfcontained.a` (built by `make
freestanding-lib-selfcontained` in the repo root) leaves a small, documented set
of symbols undefined (see `tests/freestanding_symbol_gate.sh`). U-1 supplies the
**platform + allocator** members of that set:

| Undefined seam | U-1 supplies |
|----------------|--------------|
| `KlAllocator` (explicit, no global) | `kl_uefi_allocator(bs)` — `allocator_uefi.c` |
| `uint64_t kl_monotonic_ms(void)` | periodic-timer tick counter — `platform_uefi.c` |
| `void kl_plat_random(void*, size_t)` | `EFI_RNG_PROTOCOL`, fail-closed — `platform_uefi.c` |

The remaining seams (socket provider, event/completion provider, DNS, the
vendored-llhttp `abort`/`fprintf`/`stderr` residual, the PE `__chkstk`) are **not**
U-1's job; the self-test defines fail-closed link stubs for them in
`u1_link_stubs.c` purely so the image links (they never run — U-1 issues no
request). U-2/U-3 replace the socket + event/completion seams with real EFI
providers; DNS is now the async stock `src/protocols/dns/dns_resolver.c` running over the
EFI_UDP4 socket provider (6.4c), not a sync seam.

## Files

- `efi_uefi.h` — extends the spike's `spikes/uefi/efi_min.h` with the extra UEFI
  2.10 decls U-1 needs: `EfiBootServicesData` (§7.2), the `EFI_EVENT_NOTIFY`
  timer-callback prototype (§7.1), and `EFI_RNG_PROTOCOL` (§37.5). Everything else
  (boot services incl. `AllocatePool`/`FreePool`/`CreateEvent`/`SetTimer`/`Stall`/
  `LocateProtocol`, the system table, console) is reused from the spike verbatim.
- `allocator_uefi.c` / `.h` — `KlAllocator` over `AllocatePool(EfiBootServicesData)`
  / `FreePool` / copy-`realloc` (uses the vtable's old_size to bound the copy).
  A **factory** taking an explicit `EFI_BOOT_SERVICES*` (the design's rule — no
  global heap in the pre-boot environment). `AllocatePool` failure → `NULL`.
- `platform_uefi.c` / `.h` — `kl_monotonic_ms` + `kl_plat_random`, installed once
  via `kl_uefi_platform_init(bs, st)`. See the clock/RNG notes below.
- `u1_selftest.c` — the `efi_main` acceptance test.
- `u1_link_stubs.c` — fail-closed stubs for the non-U-1 seams (link-only).
- `build.sh` / `run.sh` / `startup.nsh` / `Makefile` — build + QEMU/OVMF harness.

## Monotonic clock — source & resolution

`kl_monotonic_ms` is backed by a **periodic `EVT_TIMER` + `EVT_NOTIFY_SIGNAL`
callback** (UEFI 2.10 §7.1): `kl_uefi_platform_init` does
`CreateEvent(EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK, uefi_timer_tick, …)`
then `SetTimer(TimerPeriodic, 100000)` — a **10 ms** period (`SetTimer`
`TriggerTime` is in 100 ns units; `100000 × 100 ns = 10 ms`). The notify function
advances a `static volatile uint64_t` counter by the period-in-ms (10);
`kl_monotonic_ms()` returns it.

- **Monotonic by construction**: a free-running count that only increases —
  unlike `GetTime`/`EFI_TIME`, which is RTC **wall-clock** and can jump backward
  (Phase-10 finding 8). This is why we do **not** use `GetTime`.
- **Resolution: 10 ms.** EDK2's timer architectural protocol coalesces very short
  periods up to its own tick granularity (~10 ms under OVMF): a 1 ms request only
  fires every ~10 ms, so counting each notify as 1 ms undercounts ~10x (observed
  dt=5 for a 50 ms Stall in an early revision). Arming at 10 ms — at/above that
  granularity — fires reliably once per period, and advancing by the period gives
  an accurate elapsed-ms count (the 50 ms wait reads ~50 ms). 10 ms resolution is
  ample for KEEL's uses (timeout deadlines, Happy-Eyeballs delay).
- **Why a callback, not `CheckEvent` polling**: a bare polled timer event carries
  a single *signaled* bit, not a count — it cannot measure a gap across a blocking
  `Stall`. The NOTIFY_SIGNAL callback is the only way to accumulate elapsed periods.
  The callback is trivial (one add) so it is safe on UEFI's bounded stack.

## RNG fail-closed policy

`kl_plat_random` prefers `EFI_RNG_PROTOCOL->GetRNG` (UEFI 2.10 §37.5), located
once at init. If `EFI_RNG_PROTOCOL` is **absent** (or `GetRNG` fails at call
time), it **fails closed**: it **zeroes** the output buffer and clears the
internal source so `kl_uefi_have_entropy()` returns 0. It does **not** invent a
weak address-/clock-derived fallback. Security-sensitive callers (TLS, later)
**must** check `kl_uefi_have_entropy()` and refuse to proceed when it returns 0.
Zeroing (vs. leaving the buffer uninitialized) makes the failure deterministic
and obvious rather than silently seeding with stack garbage.

> Note: the plaintext client archive does **not** pull `kl_plat_random` (no
> DNS/TLS in that manifest), but the shim + policy are in place for the moment a
> security-sensitive caller does.

## Build & run (Apple container, Linux)

Requires `clang` + `lld` (PE cross target) to build; `qemu-system-x86` + `ovmf`
+ `mtools` + `dosfstools` to boot. Per the U-0 pattern, run under Apple's
`container` CLI (dedicated Linux VM), QEMU under **TCG** (no `-enable-kvm`):

```bash
container run --rm --cpus 6 -m 4g -v "$PWD:/src" docker.io/library/ubuntu:24.04 bash -c '
  apt-get update -qq && apt-get install -y -qq qemu-system-x86 ovmf mtools dosfstools clang lld llvm python3 file make
  cp -a /src/. /work/ && cd /work/integrations/uefi
  BOOT_TIMEOUT=180 KEEL_ROOT=/work bash run.sh'
```

Or, with the toolchain already present:

```bash
make build   # freestanding-lib-selfcontained + BOOTX64.EFI (PE32+ EFI application)
make run     # + ESP image + QEMU/OVMF boot, capture serial
```

### Expected serial

```
=== U-1 UEFI platform+allocator self-test ===
  [plat] CreateEvent...
  [plat] SetTimer...
  [plat] LocateProtocol(RNG)...
  [plat] init done
  [alloc] size=16 ... size=262144
  [alloc] over-large (expect NULL)
U-1: allocator OK
U-1: monotonic OK (dt=~50)
U-1: rng <available|fail-closed>
U-1: GO
U-1: (parked; harness will terminate QEMU)
```

`U-1: GO` is printed only if the allocator round-trip **and** the monotonic delta
both pass. After GO the app **parks** (infinite `Stall` loop) so the markers stay
the last serial output — the harness ends QEMU via its `BOOT_TIMEOUT`. (Returning
to the boot manager instead let some OVMF builds execute an unsupported opcode on
`reset`, muddying the capture.)

`rng` reports whichever branch the firmware took. The stock OVMF used here has
**no** `EFI_RNG_PROTOCOL`, so the shim correctly reports `fail-closed` (it zeroes
the buffer and `kl_uefi_have_entropy()` returns 0). On firmware that publishes
`EFI_RNG_PROTOCOL` it reports `available`.
