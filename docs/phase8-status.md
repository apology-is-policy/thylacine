# Phase 8 status — Linux compat + network

Phase 8 (ROADMAP §9, §2.2): the network arc → container runner (#70) →
on-system toolchain (#67) → Linux binary shim. This doc tracks the **network
arc** (NET-DESIGN.md), the first Phase-8 work.

## TL;DR

The #68 network charter (net-0) is bound scripture; net-1 (the reusable
virtio-net frame-transport driver) is landed + proven. net-1 surfaced **#140**
(two persistent userspace drivers cannot soundly share a 4 KiB virtio-mmio
page), so net-2 is **preempted** by the **virtio-PCI transport sub-arc**
(pci-0..pci-3, `docs/VIRTIO-PCI-DESIGN.md`, the user-voted DFS fork 2026-06-15;
pci-0 scripture landed). net-2..net-8 (netd + smoltcp, listen/accept, cs/dns,
socket-compat, dev9p.poll, TLS/NTP, exit criteria) resume on the PCI NIC.

## Landed chunks

| Commit | What | Tests |
|---|---|---|
| `54a6bf6` | **net-0**: the #68 network charter (`docs/NET-DESIGN.md`) — scripture, no code; the 3 votes (shared netd + view-narrowing; pouch socket-compat; ns-restriction firewall) | n/a (scripture) |
| `ff3a621` (+audit-close) | **net-1**: `usr/lib/netdev` — the reusable `VirtioNet` RX+TX frame driver (send/poll_rx/recycle/quiesce) + host-tested `ring` + the `netdev-test` boot probe | 7 host ring tests + `netdev-test: PASS 24/24` + boot OK + SMP gate 0 corruption + Opus-4.8 audit 0/0/1/4 (F1 P2 + F2/F3 P3 fixed; F4/F5 closed) |
| `378c667` | **pci-0**: the virtio-PCI transport scripture (`docs/VIRTIO-PCI-DESIGN.md` + ARCH §13.2/§25.4/I-5 + NET-DESIGN §17 + ROADMAP) — the #140 DFS fork; net-only, INTx | n/a (scripture) |
| `<pending>` | **pci-1a**: the DTB/config primitives — `dtb_pci_intx_route` (interrupt-map → GIC INTID), `dtb_pci_mem_window`, `dtb_get_compat_prop`, `virtio_pci_cfg_write{8,16,32}`; no ABI/kobj change | 883/883 (+3: `dtb.pci_intx_route` full swizzle + `dtb.pci_mem_window` + `virtio_pci.cfg_write_bounds`) + boot OK; SMP-inert (gate at pci-1b) |

## Remaining work

### virtio-PCI transport sub-arc (pci-0..pci-3) — the #140 resolution

net-1 surfaced **#140**: QEMU packs 8 virtio-mmio slots per 4 KiB page, so net
(slot 30) + the Stratum-pool blk (slot 31) share page `0x0a003000`; the
page-exclusive `KObj_MMIO` claim over a hard 4 KiB MMU granule cannot give two
*persistent* userspace drivers (`netd` + `stratumd`) sound, isolated
co-residency (net-1 dodged it by running + exiting pre-stratumd). Per the
depth-first-dependencies principle (user-voted 2026-06-15), net-2 is preempted
to build the future-proof virtio-PCI transport (`docs/VIRTIO-PCI-DESIGN.md`):
net moves to its own page-aligned PCI BAR, dissolving the contention by
construction. net-only scope; INTx interrupts; blk→PCI + MSI-X are v1.x seams.

- **pci-0** the transport scripture (this doc + ARCH §13.2/§25.4/I-5 +
  NET-DESIGN §17 + ROADMAP). **LANDED.**
- **pci-1a** DTB/config primitives: `dtb_pci_intx_route` + `dtb_pci_mem_window`
  + `dtb_get_compat_prop` + `virtio_pci_cfg_write*`. **LANDED** (883/883;
  SMP-inert).
- **pci-1b** kernel `KObj_PCI` (exclusive per-function claim, non-transferable)
  + `SYS_PCI_CLAIM`/`MAP_BAR`/`INFO` + BAR assignment + `VIRTIO_PCI_CAP_*`
  resolve + the DTB `interrupt-map` INTx swizzle. **Audit-bearing** (the SMP
  gate runs here — the claim table is the first shared state).
- **pci-2** userspace: `libthyla-rs::hardware::PciDev` + virtio-pci-modern +
  port netdev's transport half (reuse `ring`); `netdev-test` over PCI;
  `run-vm.sh` `virtio-net-device` → `virtio-net-pci`.
- **pci-3** focused audit (§7 of the design doc) + SMP gate + reference docs.
  Then **net-2 resumes** on the PCI NIC.

### the net arc (resumes on the PCI NIC, after pci-3)

- **net-2** netd skeleton: embed smoltcp, serve `/net`, the `/net/tcp`
  clone/connect/data client path + the fid state machine (NET-DESIGN §3.4).
- **net-3** listen/accept + udp + icmp (ping).
- **net-4** cs/dns/ndb + ipconfig/DHCP. **net-5** socket-compat pouch patch.
- **net-6** dev9p.poll + reserved `net_poll.tla` + Loom-multishot accept.
- **net-7** TLS root bundle + SNTP + `SYS_CLOCK_SETTIME` + observability.
- **net-8** server + soak exit criteria + one focused audit over the arc.

## Ground truth (the DTB, read 2026-06-15)

`qemu-system-aarch64 -machine virt,gic-version={2,3},dumpdtb` + `dtc` decode of
the `pcie@10000000` node (identical for both GIC versions):

- **ECAM** PA `0x40_1000_0000`, 256 MiB (already reserved + bus-0 mapped, P4-H).
- **BAR window** PA `0x1000_0000`, ~768 MiB (32-bit, page-aligned, not reserved
  → a per-device BAR is page-aligned + userspace-claimable).
- **INTx** swizzle `SPI = 3 + ((slot + pin − 1) mod 4)` → GIC INTID **35–38**.
  A single PCI device (net-only) lands on **one unshared INTID** → no irqfwd
  multi-waiter needed.
- **MSI** present in HW (GICv3 ITS / GICv2 v2m + the pcie `msi-map`) but
  **undriven** → INTx is the v1.0 path; MSI-X is a v1.x seam.
- **BARs start unassigned** (no UEFI) → the kernel assigns them from the BAR
  window (pci-1).

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
