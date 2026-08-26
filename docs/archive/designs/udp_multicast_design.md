# UDP Multicast + Broadcast: Design

Status: **implemented (2026-07-19).**
Decisions: interface selected **by index** (0 = default, portable); **runtime
join/leave API** on `KlUdp` plus a config convenience group; `KlUdpServer` gets
**config fields + join/leave passthrough**.

Adds link-local service discovery and one-to-many datagram services to the
existing single-loop UDP substrate (`KlUdp` + `KlUdpServer`): mDNS
(`224.0.0.251:5353` / `ff02::fb`), SSDP/UPnP (`239.255.255.250:1900`), and
broadcast beacons/telemetry. Everything rides the current recv/send machinery:
this adds only socket-option + group-membership plumbing. No event-loop changes.

---

## Background: what the OS actually requires

- **Broadcast (IPv4 only):** `SO_BROADCAST` to *send* to `255.255.255.255` or a
  subnet-directed broadcast. *Receiving* broadcast needs nothing beyond a normal
  bind to the port. IPv6 has no broadcast (uses multicast).
- **Multicast receive:** join a group (`IP_ADD_MEMBERSHIP` /
  `IPV6_JOIN_GROUP`); leave (`IP_DROP_MEMBERSHIP` / `IPV6_LEAVE_GROUP`). The
  socket binds the port (typically wildcard) with `SO_REUSEADDR`/`SO_REUSEPORT`
  so multiple listeners can share it. Membership is auto-dropped when the socket
  closes.
- **Multicast send tuning:** `IP_MULTICAST_TTL` / `IPV6_MULTICAST_HOPS` (scope;
  kernel default 1 = link-local), `IP_MULTICAST_LOOP` (loop locally-sent
  datagrams back to local members; default on), `IP_MULTICAST_IF` (egress
  interface). The send itself is the existing `kl_udp_send_to`, unchanged.

## Surface

### `KlUdpConfig` additions (socket-setup knobs, "0 = default")

```c
int       broadcast;              /* SO_BROADCAST: allow broadcast sends (IPv4)        */
int       multicast_ttl;          /* IP_MULTICAST_TTL / v6 HOPS; 0 = kernel default (1) */
int       multicast_disable_loop; /* 1 = disable local loopback (default: enabled)    */
unsigned  multicast_iface;        /* egress ifindex for multicast sends; 0 = default  */
const char *multicast_group;      /* optional: join this group at init (convenience)  */
```

### `KlUdp` runtime primitive

```c
/* Join/leave an any-source multicast group. `group` is a numeric multicast
 * address matching the socket family; `iface_index` is an interface index
 * (0 = default/kernel-chosen). Multiple groups may be joined. Returns 0 / -1
 * (last_error). Membership is auto-dropped on kl_udp_free (socket close). */
int kl_udp_multicast_join (KlUdp *udp, const char *group, unsigned iface_index);
int kl_udp_multicast_leave(KlUdp *udp, const char *group, unsigned iface_index);
```

### `KlUdpServer`

Config passthrough (same five fields) so a discovery responder is one init call,
plus dynamic membership without reaching into the underlying `KlUdp`:

```c
int kl_udp_server_multicast_join (KlUdpServer *s, const char *group, unsigned iface_index);
int kl_udp_server_multicast_leave(KlUdpServer *s, const char *group, unsigned iface_index);
```

## Implementation notes

- **`KlUdp` gains an `int family;`** field (stored from `kl_udp_init`), needed to
  build the correct `mreq` and pick the v4/v6 option level in join/leave.
- **Ordering in `kl_udp_init`:** TX options (`SO_BROADCAST`, TTL, loop, egress
  iface) are set alongside the existing `setsockopt` block. A config
  `multicast_group` join happens **after** a successful `bind()` (you join on a
  bound socket).
- **Group validation:** parse `group` with `inet_pton` for the socket's family;
  reject a family mismatch or a non-multicast address (IPv4 `224.0.0.0/4`; IPv6
  `ff00::/8`) with `KL_ERR_INVALID_ARG`. `setsockopt` failure → `KL_ERR_SOCKET`.
- **IPv4 membership + egress by index:** where `struct ip_mreqn` is available
  (Linux) use `imr_ifindex`; otherwise (BSD/macOS `struct ip_mreq`) a non-zero
  index cannot be honored for IPv4 and falls back to the default interface
  (`INADDR_ANY`): **documented best-effort**. IPv6 uses `ipv6mr_interface` /
  `IPV6_MULTICAST_IF` = index on all platforms.
- **Option value types:** `IP_MULTICAST_TTL` / `IP_MULTICAST_LOOP` take a
  `u_char` on BSD/macOS and an `int` on Linux; the v6 `HOPS`/`LOOP` take `int`.
  Set with the platform-correct type behind `#if` guards; all `setsockopt` calls
  are best-effort for TX tuning (a failure to set TTL doesn't fail init).
- **No membership tracking needed** for cleanup: socket close drops all groups;
  `kl_udp_free` is unchanged. `leave` is an explicit user action.

## Non-goals (v1)

Source-specific multicast (SSM: `IP_ADD_SOURCE_MEMBERSHIP` /
`MCAST_JOIN_SOURCE_GROUP`), IGMP/MLD version pinning, and per-packet TTL
override. Add later if a consumer needs them.

## Tests (`tests/test_udp_multicast.c`)

Uses the `KlEventCtx` pump pattern; timing/environment-sensitive cases probe and
`UTEST_SKIP` (some CI sandboxes block multicast on `lo`, like the Happy Eyeballs
timing tests):

- **v4 loopback round-trip**: two sockets on `SO_REUSEPORT`, both join a
  `239.x` group on the loopback; send to the group; the receiver gets it. Probe
  a self-send first; SKIP if the sandbox drops loopback multicast.
- **join / leave / silence**: after `leave`, a send to the group is no longer
  delivered; double-join / double-leave are idempotent-safe (no crash).
- **IPv6**: `ff02::` join/leave + round-trip on `lo` where supported.
- **broadcast**: `SO_BROADCAST` send to `255.255.255.255:port` succeeds with the
  flag; without it the send fails (`EACCES`), asserted via `kl_udp_last_error`.
- **error paths**: join a non-multicast address, or a wrong-family group →
  `KL_ERR_INVALID_ARG`; join on an unbound socket behaves sanely.
- **server passthrough**: `KlUdpServer` config-group join + a dynamic
  `kl_udp_server_multicast_join` both receive.

## Delivery

One feature commit (KlUdp primitive + server passthrough are tightly coupled and
the server side is thin). Full gauntlet before handoff: `make test`, poll
backend, ASan+UBSan, scan-build, cppcheck, gcc-14; CI-green.
