# dns_uefi.c Retirement Plan (post-6.4c)

**Status:** PLAN — no code. Follows the 6.4c acceptance (stock `src/dns_resolver.c` over
`KlUdp`-over-EFI_UDP4, reviewer-accepted, commits `ff06344`→`1834f18`). This document is the
dependency audit + decision the 6.4c reviewer asked for before any code lands. It preserves the
design-freeze cadence used for 6.4b: freeze the plan, then implement in reviewable steps.

## 0. One-paragraph thesis

`dns_uefi.c` is the **bespoke one-shot _synchronous_ DNS-over-EFI_UDP4 resolver** written for U-5,
before a datagram transport existed. 6.4b/6.4c replaced its reason to exist: the **stock async
`KlResolver`** (`src/dns_resolver.c`) now runs over `KlUdp`-over-EFI_UDP4 (the `socket_efi_udp4.c`
provider + `event_efi.c` completion) and is proven on firmware (DNS→GET 200 + a TC case). The clean
outcome is to **delete the bespoke resolver and retire U-5 as a superseded diagnostic**, while
keeping the **numeric `kl_resolve_sync` seam** (`resolve_uefi.c`) that U-3/U-4/U-7/S-6 still use.
Critically, we do **not** build a synchronous compatibility adapter over the async resolver — that
would re-embed a DNS protocol engine into the platform seam and violate the axis split.

## 1. The axis rule this retirement must honor

- **EFI_UDP4 is a platform (socket) provider** — `socket_efi_udp4.c` (`SOCK_DGRAM`) under the unified
  `kl_uefi_socket_provider`. It knows nothing about DNS.
- **DNS is a protocol consumer** — `src/dns_resolver.c`, reached over the provider as an **async**
  `KlResolver` injected via `cfg.resolver`. This is exactly what 6.4c does.
- Therefore DNS never rides `kl_resolve_sync` again. `kl_resolve_sync` stays a **numeric-only**
  platform seam. The `-DKL_U5_DNS` branch that folded a DNS engine into that sync seam is the precise
  thing being deleted.

## 2. Dependency audit (complete inventory)

### 2.1 `dns_uefi.c` / `dns_uefi.h` — the retirement target
Provides `kl_uefi_dns_init`, `kl_uefi_dns_resolve`, `kl_uefi_dns_last_ip` (a one-shot A-query state
machine over raw EFI_UDP4 tokens, with its own Cancel+quarantine + post-EBS refusal).

**Real consumers (must change in lockstep):**
| Consumer | How it uses it | Fate |
|---|---|---|
| `resolve_uefi.c` (`#ifdef KL_U5_DNS`) | calls `kl_uefi_dns_resolve` inside `kl_resolve_sync` | delete the `KL_U5_DNS` block (revert to numeric-only, matching the file's own header) |
| `u5_selftest.c` | `kl_uefi_dns_init` + `kl_uefi_dns_last_ip` | delete (U-5 retired — see §3) |
| `mock_efi_test.c` | links `dns_uefi.c`; ~6 cases drive `kl_uefi_dns_init`/`_resolve` (delayed-write, post-EBS refuse, quarantine) | delete those cases + the `#include "dns_uefi.h"`; keep all `socket_efi_udp4` cases |
| `build_u5.sh` | compiles+links `dns_uefi.c`, defines `KL_U5_DNS` | delete (U-5 retired) |
| `build_mock_efi_test.sh` | `dns_uefi.c` in `SRCS` | drop it from `SRCS` |

**Comment-only references (no code change beyond wording):** `socket_efi_udp4.c:8` (a "same calls as
dns_uefi.c" note), `dgram_dns_selftest.c` / `build_dgram_dns.sh` (my 6.4c "contrast dns_uefi.c"
notes — keep or soften), `Makefile:1289,1401`.

### 2.2 `kl_resolve_sync` — a CORE seam that STAYS
Definitions: `src/resolve_sync.c` (hosted `getaddrinfo`), `integrations/uefi/resolve_uefi.c`
(numeric), `integrations/lwip/resolve_sync_lwip.c`, and the fail-closed `u1_link_stubs.c` stub.
Callers: `src/client_async.c` (freestanding numeric fallback + a hosted path), `src/client_sync.c`,
`src/websocket_client.c`, `src/h2_client.c`. **None of these change.** `resolve_uefi.c` continues to
serve U-3/U-4/U-7/S-6 as a numeric resolver; only its DNS `#ifdef` disappears.

### 2.3 Flags
- `KL_U5_DNS` — **fully dead** after retirement (only `resolve_uefi.c` + `build_u5.sh` mention it).
- `KEEL_UEFI_HAVE_RESOLVE` — **STAYS**. It is the `u1_link_stubs.c` "let `resolve_uefi.c`'s
  `kl_resolve_sync` win" switch used by U-3/U-4/U-7/S-6/S-6 — unrelated to DNS.
- `KL_U4_STATIC_HOST`/`KL_U4_STATIC_IP` — **STAY** (U-4 TLS-SAN dialing; not DNS).

### 2.4 Shared symbols that survive
`kl_uefi_after_ebs` is defined in `platform_uefi.c` and referenced by `socket_efi_tcp4/udp4.c`,
`event_efi.c`, `platform_uefi.c`, `u7/s7_selftest.c`, `mock_efi_test.c` — retiring `dns_uefi.c`
removes one *referencer*, not the definition. No link fallout.

### 2.5 Gating
Neither U-5 nor the mock harness is referenced by `ci.yml` or the `Makefile` build graph — so the
retirement has **zero CI blast radius**. (The mock harness runs via `build_mock_efi_test.sh`; the
firmware acceptance runs via `run_dgram_dns.sh`. Both are operator-run.)

## 3. U-5: migrate vs retire → **RETIRE**

6.4c's `dgram_dns_selftest.c` is a strict **superset** of U-5's intent:

| U-5 (`u5_selftest.c`) | 6.4c (`dgram_dns_selftest.c`) |
|---|---|
| bespoke one-shot `dns_uefi.c` behind sync `kl_resolve_sync` | **stock async `KlResolver`** via `cfg.resolver` |
| A-query only | A **+ AAAA** (RFC 8305 dual-family) |
| resolve → GET 200 | resolve → GET 200 **+ TC truncation case** |
| "GO" on 200 | GO + `udp_live==0`/`quarantined==0` teardown assert + prompt-settle timing oracle |
| parked; manual serial read | failure-proof harness, `exit 1` on any miss |

Migrating U-5 to `cfg.resolver` would only reproduce 6.4c. **Retire** U-5 (`u5_selftest.c`,
`build_u5.sh`, `run_u5.sh`). The firmware acceptance "resolve a hostname over EFI_UDP4 then GET 200"
is **preserved and exceeded** by 6.4c's happy path — no net loss of firmware coverage.

## 4. The sync/async seam — event-loop ownership & reentrancy (the crux)

The one dangerous design is a **synchronous compatibility adapter**: making `kl_resolve_sync`
internally drive the stock async resolver. **Rejected**, because:

- `kl_resolve_sync(host, port, socktype, out, max, *n)` has **no `KlEventCtx`** in its signature. To
  run the async resolver it would have to *find or own* an event loop and **pump it synchronously**.
- It is called **from inside** `client_async.c`'s own event loop (the freestanding fallback). Pumping
  a loop from within a loop callback is a **reentrancy hazard** (nested `kl_event_ctx_run`, resolver
  completion firing under an unexpected stack, double-drive of the datagram completion queue).
- It would drag the whole DNS + UDP + datagram stack behind a "numeric parser" seam — re-embedding a
  protocol engine in the platform layer (the axis violation §1 forbids).

**Resolved contract:** DNS resolution in freestanding/UEFI is **async-only**, via `cfg.resolver`.
`kl_resolve_sync` resolves **numeric literals only** (its `resolve_uefi.c` header already says so).
A freestanding consumer that needs a hostname supplies `cfg.resolver` (a stock
`kl_dns_resolver_create` over the EFI_UDP4 provider) — the 6.4c pattern. This is already the behavior
of `client_async.c`'s `#ifdef KEEL_FREESTANDING` branch; retirement just deletes the one place
(`resolve_uefi.c`'s `KL_U5_DNS`) that pretended otherwise.

## 5. Archive / link-stub / duplicate-symbol analysis

- Deleting `dns_uefi.c` removes only *provided* symbols (`kl_uefi_dns_*`); every referencer is
  removed in the same step → **no new undefined symbols**.
- `resolve_uefi.c` keeps defining `kl_resolve_sync` (numeric). No stub change; `u1_link_stubs.c`'s
  fail-closed `kl_resolve_sync` (under `!KEEL_UEFI_HAVE_RESOLVE`) is untouched.
- `build_mock_efi_test.sh`: dropping `dns_uefi.c` from `SRCS` is a pure removal — no duplicate-symbol
  risk (the mock links `libkeel.a`, which has no `kl_uefi_dns_*`).
- No change to the 6.4c archives (`freestanding-lib-dns-selfcontained` / `-selfcontained`) — they
  never contained `dns_uefi.c`.

## 6. Docs to update
- `docs/datagram_contract.md` — the §10 backend×requirement matrix: EFI column moves from "bespoke
  one-shot `dns_uefi.c`" to "stock `dns_resolver` over the `socket_efi_udp4` provider".
- `docs/phase10_uefi_feasibility_design.md` — "DNS is U-5 / `dns_uefi.c`" notes → "DNS is 6.4c stock
  resolver over EFI_UDP4".
- `docs/phase10_efi_udp4_provider_design.md` — the "seed of a future provider = `dns_uefi.c`" framing
  → retired; 6.4c is the realized provider.
- `docs/generic_datagram_audit.md`, `docs/keel_audit.md` — drop/relabel `dns_uefi.c` rows.

## 7. Proposed step sequence (each committed, then paused for review)

- **R-1 — delete the bespoke engine.** Remove `dns_uefi.c`/`.h`; delete the `KL_U5_DNS` block from
  `resolve_uefi.c` (+ its header note); delete the `dns_uefi` cases + include from `mock_efi_test.c`;
  drop `dns_uefi.c` from `build_mock_efi_test.sh`. **Validate:** `build_mock_efi_test.sh` green
  (ASan/UBSan/LSan), all surviving `socket_efi_udp4` cases pass; U-3/U-4/U-7/S-6 still *link*
  (numeric `resolve_uefi.c` unaffected — compile-check).
- **R-2 — retire U-5.** Delete `u5_selftest.c`, `build_u5.sh`, `run_u5.sh`. **Validate:** re-run the
  6.4c `run_dgram_dns.sh` (happy+TC) as the replacement firmware acceptance; confirm no CI/Makefile
  reference dangles.
- **R-3 — docs + comment sweep.** §6 doc edits; soften the `socket_efi_udp4.c`/`Makefile`/6.4c
  "contrast dns_uefi.c" comments to past tense. No code.

## 8. Risks / open questions (resolve during R-1, not blocking the freeze)
- **Lost unit coverage.** The `dns_uefi` mock cases (delayed-write settle, post-EBS refuse,
  quarantine) go away. Confirm the surviving `socket_efi_udp4` cases (6.4b) cover the *same
  disciplines* on the surviving provider (they do: dedicated quarantine Rx/Tx, post-EBS, stale-gen,
  cancel-confirm cases). If any dns_uefi case tests a behavior with no udp4 analogue, port it to a
  `socket_efi_udp4` case rather than delete it.
- **Keep U-5's static-host trick?** `KL_U4_STATIC_HOST` in `resolve_uefi.c` is a U-4 TLS-SAN feature,
  not DNS — **keep** it; only the `KL_U5_DNS` block leaves.
- **`run_u5.sh` HTTP responder reuse.** `run_dgram_dns.sh` already stands up its own responder; no
  dependency on `run_u5.sh` remains after R-2.

## 9. Recommendation
Proceed R-1 → R-2 → R-3 as above. Net effect: one duplicate DNS engine deleted, the axis split made
literal (EFI_UDP4 = provider, DNS = async consumer), `kl_resolve_sync` left as the honest numeric
seam it claims to be, and firmware DNS coverage consolidated onto the stronger 6.4c acceptance.
