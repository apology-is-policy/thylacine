# Phase 8 status — Linux compat + network

Phase 8 (ROADMAP §9, §2.2): the network arc → container runner (#70) →
on-system toolchain (#67) → Linux binary shim. This doc tracks the **network
arc** (NET-DESIGN.md), the first Phase-8 work.

## TL;DR

The #68 network charter (net-0) is bound scripture; net-1 (the reusable
virtio-net frame-transport driver) is landed + proven. net-2..net-8 (netd +
smoltcp, listen/accept, cs/dns, socket-compat, dev9p.poll, TLS/NTP, exit
criteria) remain.

## Landed chunks

| Commit | What | Tests |
|---|---|---|
| `54a6bf6` | **net-0**: the #68 network charter (`docs/NET-DESIGN.md`) — scripture, no code; the 3 votes (shared netd + view-narrowing; pouch socket-compat; ns-restriction firewall) | n/a (scripture) |
| `ff3a621` (+audit-close) | **net-1**: `usr/lib/netdev` — the reusable `VirtioNet` RX+TX frame driver (send/poll_rx/recycle/quiesce) + host-tested `ring` + the `netdev-test` boot probe | 7 host ring tests + `netdev-test: PASS 24/24` + boot OK + SMP gate 0 corruption + Opus-4.8 audit 0/0/1/4 (F1 P2 + F2/F3 P3 fixed; F4/F5 closed) |

## Remaining work (the net arc)

- **net-2** netd skeleton: embed smoltcp, serve `/net`, the `/net/tcp`
  clone/connect/data client path + the fid state machine (NET-DESIGN §3.4).
  **BLOCKED on the net-2 MMIO co-residency prerequisite** (below).
- **net-3** listen/accept + udp + icmp (ping).
- **net-4** cs/dns/ndb + ipconfig/DHCP. **net-5** socket-compat pouch patch.
- **net-6** dev9p.poll + reserved `net_poll.tla` + Loom-multishot accept.
- **net-7** TLS root bundle + SNTP + `SYS_CLOCK_SETTIME` + observability.
- **net-8** server + soak exit criteria + one focused audit over the arc.

## net-2 PREREQUISITE (must resolve before netd)

QEMU packs 8 virtio-mmio slots per 4 KiB page; **net + blk share one page**
(`0x0a003000`). The page-exclusive `KObj_MMIO` claim
(`kernel/mmio_handle.c`, page-aligned + `ranges_overlap`) means a long-lived
`netd` (net) and stratumd (blk) cannot both hold that page. net-1 does not hit
it (its probe runs + exits pre-stratumd). Resolutions: (a) a kernel **sub-page
MMIO claim** (recommended — general; any two userspace virtio drivers), (b)
QEMU device-page separation, (c) a virtio-mmio multiplexer. Escalation-worthy
(kernel surface).

## Exit criteria (refined per W4-F8; see NET-DESIGN §16)

- [ ] Client: `ping`, `curl`/`wget` (TLS), `ssh`-client, a native `TcpStream`.
- [ ] Server: native `TcpListener` echo (≥2 conns) + the Loom-multishot accept
      loop + a ported `listen`/`accept` server.
- [ ] Soak: N conns × M s, no fd/connection/Burrow leak, under the SMP gate.
- [ ] No Utopia regressions; no P0/P1 on the reserved net audit surfaces.

## Build + verify

```bash
tools/build.sh kernel              # kernel + userspace + ramfs
tools/test.sh                      # boot + netdev-test PASS + boot OK
cargo test -p netdev --no-default-features --target aarch64-apple-darwin   # host ring tests
tools/ci-smp-gate.sh               # SMP soundness gate
```

## Trip hazards

- A virtio driver Proc needs `CAP_HW_CREATE` (spawn via `t_spawn_with_caps` /
  `rfork_with_caps`); plain `t_spawn` → `SYS_MMIO_CREATE` rejected at the cap
  gate (net-1's root-cause).
- netdev-test runs PRE-stratumd in the joey ladder (the net page must be free).
- `THYLACINE_NO_QMP=1` makes the virtio-input probe fail (#34, unrelated).

## References

- `docs/NET-DESIGN.md` (the #68 charter), `docs/reference/114-netdev.md`.
- ARCH §10.1 (network is 9P), §28 (no new net invariant), ROADMAP §9.
