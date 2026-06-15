# Phase 8 status — Linux compat + network

Phase 8 (ROADMAP §9, §2.2): the network arc → container runner (#70) →
on-system toolchain (#67) → Linux binary shim. This doc tracks the **network
arc** (NET-DESIGN.md), the first Phase-8 work.

## TL;DR

The #68 network charter (net-0) is bound scripture; net-1 (the reusable
virtio-net frame-transport driver) is landed + proven. net-1 surfaced **#140**
(two persistent userspace drivers cannot soundly share a 4 KiB virtio-mmio
page), so net-2 is **preempted** by the **virtio-PCI transport sub-arc**
(pci-0..pci-3, `docs/VIRTIO-PCI-DESIGN.md`, the user-voted DFS fork 2026-06-15).
**pci-0..pci-3 are LANDED** — the scripture, the kernel `KObj_PCI` + the 3
syscalls, the userspace `PciDev` + `VirtioNetPci` driver (a PCI virtio-net ARP
round-trip passes on its own page-aligned BAR), and the focused soundness audit
(Opus 4.8 max + self-audit, CLEAN 0/0/0/4 — all P3, F2/F3/F4 fixed + F1 tracked).
**The virtio-PCI transport sub-arc is COMPLETE.** net-2..net-8 (netd + smoltcp,
listen/accept, cs/dns, socket-compat, dev9p.poll, TLS/NTP, exit criteria) now
resume on the PCI NIC.

## Landed chunks

| Commit | What | Tests |
|---|---|---|
| `54a6bf6` | **net-0**: the #68 network charter (`docs/NET-DESIGN.md`) — scripture, no code; the 3 votes (shared netd + view-narrowing; pouch socket-compat; ns-restriction firewall) | n/a (scripture) |
| `ff3a621` (+audit-close) | **net-1**: `usr/lib/netdev` — the reusable `VirtioNet` RX+TX frame driver (send/poll_rx/recycle/quiesce) + host-tested `ring` + the `netdev-test` boot probe | 7 host ring tests + `netdev-test: PASS 24/24` + boot OK + SMP gate 0 corruption + Opus-4.8 audit 0/0/1/4 (F1 P2 + F2/F3 P3 fixed; F4/F5 closed) |
| `378c667` | **pci-0**: the virtio-PCI transport scripture (`docs/VIRTIO-PCI-DESIGN.md` + ARCH §13.2/§25.4/I-5 + NET-DESIGN §17 + ROADMAP) — the #140 DFS fork; net-only, INTx | n/a (scripture) |
| `5a1b36c` | **pci-1a**: the DTB/config primitives — `dtb_pci_intx_route` (interrupt-map → GIC INTID), `dtb_pci_mem_window`, `dtb_get_compat_prop`, `virtio_pci_cfg_write{8,16,32}`; no ABI/kobj change | 883/883 (+3: `dtb.pci_intx_route` full swizzle + `dtb.pci_mem_window` + `virtio_pci.cfg_write_bounds`) + boot OK; SMP-inert (gate at pci-1b) |
| `89a6c69` | **pci-1b**: the `KObj_PCI` kernel object — `kernel/pci_handle.{c,h}` (exclusive `g_pci_claims` per-(bus,dev,fn) claim; bump-assign BARs from `dtb_pci_mem_window` with width-correct 32/64-bit sizing; `VIRTIO_PCI_CAP_*` cap-walk into `regions[]`, bounded + OOB-validated; INTx swizzle; quiesce-before-free) + `KOBJ_PCI=11` joining `KOBJ_KIND_HW_MASK` (I-5 non-transferable + NoHwDup for free) + `kobj_pci_init` at boot | 889/889 (+6: `pci.bar_decode_size` + `pci.claim_rng`/`claim_unknown`/`claim_exclusive`/`unref_releases_bars`/`live_count_balances`, claiming the live idle rng-pci) + boot OK + SMP gate; **self-found+fixed**: 32-bit BAR size mask must invert in 32-bit width (a 64-bit invert yields a bogus exabyte size) |
| `11eb559` | **pci-1c**: the 3 syscalls — `SYS_PCI_CLAIM=76` / `SYS_PCI_MAP_BAR=77` / `SYS_PCI_INFO=78` (`kernel/syscall.c` handlers + dispatch) + the 208-byte `struct t_pci_info` ABI (`_Static_assert`-pinned in the kernel header, the libt mirror, and the libthyla-rs `TPciInfo` with `offset_of!` asserts) + libt (`t_pci_claim`/`t_pci_map_bar`/`t_pci_info`) + libthyla-rs raw wrappers. CLAIM: `CAP_HW_CREATE` gate → `kobj_pci_claim` → mint fixed R\|W\|MAP (no TRANSFER). MAP_BAR: mirrors `sys_mmio_map` via the `kobj_pci_bar_mmio` seam (bar_index `>= 6` rejected before the u32 narrowing). INFO: zero-init + fill + byte copy-out (no implicit padding → no stack leak). | 891/891 (+2: `pci.syscall_reject` + `pci.syscall_claim_info`) + boot OK + SMP gate |
| `2908e0e` | **pci-2**: the userspace PCI transport. `libthyla-rs::hardware::PciDev` (claim + info + map-BARs over the pci-1c syscalls; per-BAR VA window `PCI_BAR_VA_STRIDE`; `region()` bounds each cap-region within its BAR; non-transferable I-5 like Mmio/Irq/Dma) + `netdev::virtio_pci::VirtioNetPci` (the PCI sibling of `VirtioNet`: reuses `ring` VERBATIM + the same RX/TX API + audit hardenings; the virtio-pci-modern `common`/`notify`/`isr`/`device` register transport; `CCFG_MIN_LEN`/`DEVICE_CFG_MIN_LEN` region-size guards [self-audit]; device-reset quiesce-on-drop) + `usr/netdev-pci-test` boot probe + `run-vm.sh` adds `virtio-net-pci,disable-legacy=on` (separate `net1` slirp; the MMIO net + `netdev-test` kept). `virtio.rs` left byte-identical (net-1 proof intact). | host ring 7/7 + 891/891 + boot OK (both `netdev-test` + `netdev-pci-test` PASS 24/24) + 0 EXTINCTION + login E2E + SMP gate |
| `44fc3fb` | **pci-3**: the focused soundness audit (Opus 4.8 max prosecutor + concurrent self-audit) over pci-1b/1c/2 — **CLEAN 0 P0 / 0 P1 / 0 P2 / 4 P3**. F1=R1 partial-map leak **tracked** (the suggested `t_burrow_detach` fix is a verified no-op: `SYS_BURROW_DETACH` is confined above 4 GiB, the driver-VA windows live below it by design → proc-exit-bounded like every virtio mapping; single-BAR NIC never triggers it). F2=R2 notify-doorbell bound **fixed** (`off*mul+2 <= notify_len` → `NotifyRegionTooSmall`). F3 `TPciInfo` `offset_of!` asserts completed. F4 `pci.walk_caps_hostile` test added (cap-loop / OOB-bar / unassigned-bar / oversized-region / valid-control over a synthetic config). + the `KObj_PCI` reference doc (`115-pci-claim.md`) + the `37` cross-reference. | 892/892 (+`pci.walk_caps_hostile`) + both NIC probes PASS 24/24 + 0 EXTINCTION + boot OK + login E2E + SMP gate (default+UBSan × smp4/smp8 N=10, 0 corruption) |

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
  + BAR assignment + `VIRTIO_PCI_CAP_*` resolve + the DTB `interrupt-map` INTx
  swizzle. **Audit-bearing** (the claim table is the first shared state).
  **LANDED** (kobj-only; the 3 syscalls split out to pci-1c). 889/889 + boot OK.
- **pci-1c** the 3 syscalls: `SYS_PCI_CLAIM=76` / `MAP_BAR=77` / `INFO=78` +
  `struct t_pci_info` ABI + libt/libthyla-rs wrappers + syscall tests.
  **LANDED** (891/891; the syscall envelope over the pci-1b kobj). The success
  copy-out (SYS_PCI_INFO) + the BAR burrow-map (SYS_PCI_MAP_BAR) need a user
  buffer + address space, so they are proven by the pci-2 userspace probe; the
  in-kernel tests cover the cap gate / mint / rights / reject paths.
- **pci-2** userspace: `libthyla-rs::hardware::PciDev` + virtio-pci-modern +
  port netdev's transport half (reuse `ring`); `netdev-pci-test` over PCI;
  `run-vm.sh` adds `virtio-net-pci`. **LANDED** (`VirtioNetPci` PASS 24/24
  alongside the kept MMIO `netdev-test`; host ring 7/7 + 891/891 + boot OK).
- **pci-3** focused audit + SMP gate + the kernel reference docs. **LANDED**
  (CLEAN 0/0/0/4; Opus 4.8 max + self-audit). The §7 surfaces all verified sound
  (KObj_PCI lifecycle / I-5 / exclusive-claim / BAR-assign / config-mediation /
  cap-walk / INTx); the 4 P3s: F1 partial-map leak tracked (no v1.0 detach path —
  proc-exit-bounded by design), F2 notify-doorbell bound fixed, F3 ABI asserts
  completed, F4 `pci.walk_caps_hostile` test added. New `115-pci-claim.md` +
  `37-virtio_pci.md` cross-reference. **The pci sub-arc is COMPLETE → net-2
  resumes** on the PCI NIC.

### the net arc (resumes on the PCI NIC, after pci-3)

- **net-2** netd skeleton: embed smoltcp, serve `/net`, the `/net/tcp`
  clone/connect/data client path + the fid state machine (NET-DESIGN §3.4).
- **net-3** listen/accept + udp + icmp (ping).
- **net-4** cs/dns/ndb + ipconfig/DHCP. **net-5** socket-compat pouch patch.
- **net-6** dev9p.poll + reserved `net_poll.tla` + Loom-multishot accept.
- **net-7** TLS root bundle + SNTP + `SYS_CLOCK_SETTIME` + observability.
- **net-8** server + soak exit criteria + one focused audit over the arc.

## Ground truth (the DTB + the live device, read 2026-06-15)

The DTB dump (`qemu-system-aarch64 -machine virt,... ,dumpdtb` + `dtc`) of the
`pcie` node, reconciled with the **actual** in-boot enumeration (the kobj is
DTB-derived at runtime, so it is config-agnostic):

- **ECAM** PA varies by machine: `0x3f00_0000` (16 MiB) on the **default
  `gic-version=2`** machine that `tools/test.sh` boots; `0x40_1000_0000`
  (256 MiB, highmem) on the `gic-version=3` variant my pci-1a manual dump used.
  The kobj never hardcodes it — `kernel/virtio_pci.c` maps it from the DTB.
- **BAR window** PA `0x1000_0000`, ~768 MiB (32-bit, page-aligned, not reserved
  → a per-device BAR is page-aligned + userspace-claimable). Stable across both
  machines (`dtb.pci_mem_window` asserts the base).
- **INTx** swizzle `SPI = 3 + ((slot + pin − 1) mod 4)` → GIC INTID **35–38**.
  A single PCI device (net-only) lands on **one unshared INTID** → no irqfwd
  multi-waiter needed. The live rng-pci at bdf 0:1.0 routes to INTID 36.
- **MSI** present in HW but **undriven** → INTx is the v1.0 path; MSI-X v1.x.
- **BARs start unassigned** (no UEFI) → the kernel assigns them from the BAR
  window (pci-1b).
- **The live test device (rng-pci) is TRANSITIONAL**: it presents the *legacy*
  device_id `0x1005` (`is_modern=0`) but carries the *full* modern interface —
  a 32-bit legacy MMIO BAR1 (4 KiB) **and** a 64-bit modern BAR4 (16 KiB) with
  all four `VIRTIO_PCI_CAP_*` structures (common/notify/isr/device) packed into
  BAR4. So `pci.claim_rng` exercises BOTH BAR widths + the full cap-walk. **For
  pci-2/net-2**: prefer `virtio-net-pci,disable-legacy=on` for a clean
  modern-only NIC (device_id `0x1041`, no I/O BAR) — the kobj handles either.

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
