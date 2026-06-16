# 114 — netdev (the virtio-net frame transport: MMIO + PCI)

**Status**: net-1 (MMIO `VirtioNet`) + pci-2 (PCI `VirtioNetPci`) LANDED; 5d-3
retrofitted `VirtioNet` to the warden-bound, grant-driven `open_slot` (the MMIO
driver is now `usr/netdev-driver`, bound by `virtio:1`). The
reusable NIC driver layer net-2's `netd` owns and smoltcp wraps (charter
`docs/NET-DESIGN.md` §13/§17). Crate: `usr/lib/netdev`. net-2 resumes on the
**PCI** transport (`VirtioNetPci`) — the #140 resolution: a PCI function carries
its own page-aligned BAR, so a long-lived `netd` and stratumd no longer contend
for one virtio-mmio page (`docs/VIRTIO-PCI-DESIGN.md`).

## Purpose

netdev is the native (libthyla-rs, `no_std`) Ethernet frame transport. net-1
generalizes the proven `virtio-net-loop` probe (P4-Jc: RX descriptor recycling +
TX reuse, with the virtio audit hardenings) into a reusable `VirtioNet` driver
with a `send(&[u8])` / `poll_rx(&mut [u8])` API over arbitrary Ethernet frames,
plus the device-death **quiesce** the single-shot probes skip. net-2's `netd`
embeds this driver; smoltcp's `phy::Device` wraps `send`/`poll_rx`.

## Layering (the kaua pattern)

- **`ring`** — pure split-virtqueue index arithmetic (`TxRing` / `RxRing`: the
  avail/used counters, wrap, back-pressure). Terminal-free + HOST-TESTABLE; the
  audit-bearing arithmetic in isolation (`cargo test -p netdev
  --no-default-features --target <host>`).
- **`virtio`** (feature `driver`, default) — `VirtioNet`, the device glue over
  the libthyla-rs MMIO/IRQ/DMA substrate. The only libthyla-rs + audit-bearing
  layer; built for aarch64-thylacine, not the host.
- **`virtio_pci`** (feature `driver`) — `VirtioNetPci`, the PCI sibling of
  `virtio` over the libthyla-rs `PciDev` substrate (the #140 transport). Reuses
  `ring` VERBATIM; the same `send`/`poll_rx` API + audit hardenings. The transport
  diverges only in register access (BAR-mapped `virtio_pci_common_cfg` / notify /
  isr / device-cfg regions vs the flat mmio bank), queue notification, and ISR
  acknowledgement. `virtio.rs` is left byte-identical (its net-1 audit + boot
  proof intact); the PCI driver is a fresh, independently-auditable surface (pci-3).

## Public API

```rust
// ring (pure)
TxRing::new(size) / in_flight() / can_post() / next_slot() / commit_post() / reap(cur_used, cap) / avail_idx()
RxRing::new(size) / has_used(cur_used) / take_used(cur_used) -> Option<slot> / recycle_slot() / avail_idx()

// virtio (driver feature)
VirtioNet::open_slot(slot_pa, intid) -> Result<VirtioNet, OpenError>  // map the warden-granted slot + init + DRIVER_OK
  .mac() -> [u8; 6]
  .mtu() -> usize                                   // 1500
  .link_up() -> bool
  .send(&[u8]) -> bool                              // false on empty / >MAX_FRAME / ring full
  .poll_rx(&mut [u8]) -> Option<usize>              // one frame (bytes copied, hdr stripped), or None
  .drain_tx()                                       // reclaim completed TX descriptors
  .wait_irq() -> bool                               // block on IRQ; true if used-ring progress
  // Drop: quiesce (QUEUE_READY=0 both queues + device reset) before DMA pages free

// virtio_pci (driver feature) — same API surface, PCI transport
VirtioNetPci::open() -> Result<VirtioNetPci, PciOpenError>  // claim PCI fn + map BARs + modern init + DRIVER_OK
  .mac() / .mtu() / .link_up() / .send(&[u8]) / .poll_rx(&mut [u8]) / .drain_tx() / .wait_irq()
  // identical contracts to VirtioNet; Drop: device reset (device_status=0) before DMA pages free

pub const MTU: usize = 1500;
pub const MAX_FRAME: usize = 1514;                  // 14 (Eth hdr) + MTU
pub const VIRTIO_NET_HDR_LEN: usize = 12;
```

## Implementation

- **MMIO claim scope (5d-3: grant-driven).** `open_slot(slot_pa, intid)` claims
  only the **page** holding the warden-granted net slot — there is no bank-probe
  loop (the warden's virtio-mmio bus source already identified the slot and
  conferred its window + INTID; 5d retired the old hardcoded `VIRTIO_MMIO_BASE_PA`
  + the page scan). `open_slot` re-validates `MAGIC` + `DeviceID == 1` + modern
  version at the granted slot before driving it. A page held by another claimant
  fails `BankClaim`. The conferred MMIO allowance is page-rounded (`libdriver::
  to_allowance`), so the page map is covered even though the slot is sub-page; the
  shared net/blk page is the documented #140 / net-2 co-residency over-grant.
- **Geometry.** `QUEUE_SIZE = 16`; one 4 KiB ring DMA page (TX/RX desc+avail+used)
  + two 32 KiB frame pools (16 × `BUF_LEN = 2048`, holding hdr + a full MTU
  frame). Compile-time `_Static_assert`-style `const _` checks pin the ring
  layout non-overlap + buffer sizing.
- **Init** (`init_device`): VIRTIO 1.2 status handshake → require
  `VIRTIO_F_VERSION_1`, negotiate `VIRTIO_NET_F_MAC` + `_STATUS` → set up RX
  (queue 0) + TX (queue 1) before `DRIVER_OK` → read MAC → pre-post all 16 RX
  buffers (`VIRTQ_DESC_F_WRITE`, `avail.idx = 16`) + pre-init TX descriptors
  (`addr = txpool[k]`, `len` per-send) → `DRIVER_OK`.
- **send**: back-pressure (`drain_tx` + recheck) → copy a zeroed 12-byte
  `virtio_net_hdr` + the frame into `txpool[slot]` → write `descriptor[slot].len`
  → `dsb` → publish `avail.idx` → `dsb` → notify TX.
- **poll_rx**: read `used.idx` → `virtio_rmb` → `take_used` → read `desc_id` +
  `len` from the used entry → **bounds-check `desc_id < QUEUE_SIZE`** → clamp
  `len` to `BUF_LEN` → strip the 12-byte header → copy `min(frame_len, out.len())`
  → recycle the descriptor (`dsb` → `avail.idx` → `dsb` → notify) → return the
  copied length.

## Audit hardenings (the RW-7/RW-8 virtio class)

- `desc_id` from the device-controlled used ring is bounds-checked before it
  scales the RX-pool base (the OOB-read guard); a bogus id recycles a masked
  valid slot + returns `None` (drops the frame, never an OOB).
- `used.len` clamped to the posted buffer length before any frame read.
- `virtio_rmb` (`dmb ishld`) after every `used.idx` load, before the used-ring
  entry / buffer reads (VIRTIO 1.2 §2.7.13.2).
- u16 avail/used counters wrap via `wrapping_add` (the `ring` module; host-tested
  across > 5 full u16 cycles).
- TX back-pressure: never more than `QUEUE_SIZE` in flight, so a buffer is never
  overwritten while the device may still read it.
- Every MMIO access is a single ISV-safe `ldr`/`str` (libthyla-rs `mmio_*`; #890).
- **Device-death quiesce** (`Drop`): `QUEUE_READY = 0` on both queues + a device
  reset BEFORE the DMA pages release, so the device cannot DMA into freed memory
  (RW-7 R3-F1 — the discipline the single-shot probes skip).

## PCI transport (`VirtioNetPci`, pci-2)

The split-virtqueue DMA discipline + every audit hardening above are identical
(same `ring`, same RX/TX memory ops, same `desc_id` bound / `used.len` clamp /
`virtio_rmb` placement / back-pressure). The transport diverges only in:

- **Registers.** `virtio_pci_common_cfg` (VIRTIO 1.2 §4.1.4.3) in the BAR-mapped
  `Common` region: `device_feature[_select]` / `driver_feature[_select]` /
  `device_status` (a **u8**, not the mmio u32) / per-queue `queue_select` /
  `queue_size` / `queue_desc` / `queue_driver` / `queue_device` (le64 ring PAs) /
  `queue_enable` / `queue_notify_off`. `VirtioNetPci::open` claims the function
  via `PciDev::claim(virtio_device_id=1, bar_window)`, resolves the four regions
  (`Common`/`Notify`/`Isr`/`Device`), and parks every MSI-X vector at
  `NO_VECTOR` (INTx only — MSI-X is undriven at v1.0).
- **Notification.** Per queue, read `queue_notify_off` after `queue_select`; the
  doorbell VA is `notify_base + queue_notify_off * notify_off_multiplier`. `send`
  / `recycle_rx` write the queue index (u16) there (vs the mmio `REG_QUEUE_NOTIFY`).
- **ISR.** `wait_irq` reads the `Isr` region byte (read-to-clear, bit 0 = queue
  IRQ) — no separate ACK register.
- **Device-config.** The MAC + link status live in the `Device` region (vs the
  mmio `REG_CONFIG_BASE`). `open` guards `CCFG_MIN_LEN` (0x38) on `Common` and
  `DEVICE_CFG_MIN_LEN` (8) on `Device` so a malformed/undersized region is a
  clean open error rather than a read past the resolved region.
- **Quiesce.** `Drop` resets the device (`device_status = 0`) before the DMA
  pages free (the modern-transport equivalent of the mmio `QUEUE_READY=0` + reset).

`open` maps DMA + BARs at a distinct VA window from the mmio path
(`BAR_WINDOW_VA = 0x0080_0000`, DMA from `0x0100_0000`), so a future `netd` could
hold both transports without a VA collision.

## Capability

`VirtioNet::open_slot()` calls `SYS_MMIO_CREATE` / `SYS_IRQ_CREATE` / `SYS_DMA_CREATE`,
and `VirtioNetPci::open()` calls `SYS_PCI_CLAIM` / `SYS_PCI_MAP_BAR` (+ `IRQ` /
`DMA`) — all gated on **`CAP_HW_CREATE`**. A driver Proc MUST be spawned with that
cap (`t_spawn_with_caps(name, len, T_CAP_HW_CREATE)` / `rfork_with_caps`); without
it the create/claim is rejected at the capability gate.

## Tests

- **Host (pure ring)**: 6 tests — TX back-pressure, capped reap, slot-in-range
  across u16 wrap; RX stop-at-cur_used, recycle-in-range, drain+recycle across
  wrap. `cargo test -p netdev --no-default-features --target aarch64-apple-darwin`.
- **Boot E2E** (`usr/netdev-driver`, the warden-bound `virtio:1` driver, MENAGERIE
  5d-3 — in joey's THYLA_BOOT_PROBES ladder PRE-stratumd, bound NARROWED by the
  warden rather than a standalone probe): 24 ARP round-trips against QEMU's slirp
  gateway through `send`/`poll_rx` (> `QUEUE_SIZE`, so the RX descriptors recycle
  past one full ring and the TX descriptors wrap). Verifies `PASS -- 24/24 ARP
  replies via VirtioNet (grant-driven open_slot)`. Retires the old standalone
  `netdev-test`.
- **PCI boot E2E** (`usr/netdev-pci-test`, same ladder, PRE-stratumd, spawned
  WITH `CAP_HW_CREATE`): the identical 24-ARP round-trip through `VirtioNetPci`
  over a `virtio-net-pci,disable-legacy=on` NIC (its own slirp backend `net1`).
  Verifies `PASS -- 24/24 ARP replies via VirtioNetPci`. Both probes run every
  boot — the MMIO net-1 proof + the PCI pci-2 proof, independently.

## Error paths

`OpenError` (MMIO): `BankClaim`, `NoNetDevice` (no DeviceID=1 / net page held),
`LegacyDevice` (v1 MMIO), `NoVersion1`, `FeaturesRejected`, `QueueTooSmall`,
`IrqClaim`, `DmaAlloc`. `send` → `false` (empty / oversize / ring full).
`poll_rx` → `None` (no frame / bogus desc_id).

`PciOpenError` (PCI): `NoNetDevice` (claim failed — treated as SKIP by the
probe), `BarMap` (info/map-bar failed or a BAR exceeds the VA stride),
`MissingRegion` (no/undersized `Common`/`Notify`/`Isr` region), `NoIntid`,
`NoVersion1`, `FeaturesRejected`, `QueueTooSmall`, `NotifyRegionTooSmall`
(pci-3 F2: a queue's `queue_notify_off * notify_off_multiplier` doorbell would
land past the notify region — a malformed/hostile device), `IrqClaim`, `DmaAlloc`.

## Known caveats / the net-2 refinement

- **MMIO page co-residency (#140) — RESOLVED by the PCI transport.** QEMU packs
  8 virtio-mmio slots per 4 KiB page; net + blk share one page, and the page-
  exclusive `KObj_MMIO` claim cannot give a long-lived `netd` (net) and stratumd
  (blk) sound co-residency. The MMIO `VirtioNet` does NOT hit it (its probe runs
  + exits PRE-stratumd), but a persistent `netd` would. **pci-2 dissolves it**:
  `VirtioNetPci` runs on a PCI function whose BAR is page-aligned, so the existing
  page-exclusive claim isolates net from blk at the MMU granule. net-2 builds on
  `VirtioNetPci`. (Retiring the MMIO `virtio-net-device` from the boot config is
  a trivial v1.x cleanup once `netd`-on-PCI lands; it is kept now so the
  warden-bound `netdev-driver` (5d-3) keeps the MMIO proof. The MMIO net-1 driver
  still runs + exits PRE-stratumd, so the page-rounded grant over the shared
  net/blk page is released before stratumd claims its blk.)
- **Fixed user VAs.** Both transports map at fixed VAs (`VirtioNet` at
  `0x0050_0000`+; `VirtioNetPci` at `0x0080_0000`/`0x0100_0000`+); net-2's `netd`
  may parameterize.
- **IPv4/ARP frames at v1.0.** `MAX_FRAME = 1514` (no VLAN / jumbo).

## Spec cross-reference

No spec (pure-driver mechanics; prose-validated per the 2026-05-23 broadening).
The net arc's one reserved spec is `net_poll.tla` (the dev9p.poll readiness wake,
net-6) — unrelated to this layer.
