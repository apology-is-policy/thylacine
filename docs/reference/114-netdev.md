# 114 — netdev (the net-1 virtio-net frame transport)

**Status**: net-1 LANDED. The reusable NIC driver layer net-2's `netd` owns and
smoltcp wraps (charter `docs/NET-DESIGN.md` §13/§17). Crate: `usr/lib/netdev`.

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

## Public API

```rust
// ring (pure)
TxRing::new(size) / in_flight() / can_post() / next_slot() / commit_post() / reap(cur_used, cap) / avail_idx()
RxRing::new(size) / has_used(cur_used) / take_used(cur_used) -> Option<slot> / recycle_slot() / avail_idx()

// virtio (driver feature)
VirtioNet::open() -> Result<VirtioNet, OpenError>   // probe + claim net page + init + DRIVER_OK
  .mac() -> [u8; 6]
  .mtu() -> usize                                   // 1500
  .link_up() -> bool
  .send(&[u8]) -> bool                              // false on empty / >MAX_FRAME / ring full
  .poll_rx(&mut [u8]) -> Option<usize>              // one frame (bytes copied, hdr stripped), or None
  .drain_tx()                                       // reclaim completed TX descriptors
  .wait_irq() -> bool                               // block on IRQ; true if used-ring progress
  // Drop: quiesce (QUEUE_READY=0 both queues + device reset) before DMA pages free

pub const MTU: usize = 1500;
pub const MAX_FRAME: usize = 1514;                  // 14 (Eth hdr) + MTU
pub const VIRTIO_NET_HDR_LEN: usize = 12;
```

## Implementation

- **MMIO claim scope.** `open()` claims only the net device's MMIO **page**
  (probes each of the 4 bank pages, keeps the one holding `DeviceID == 1`,
  releases the rest), NOT the whole bank — a whole-bank claim conflicts with any
  unrelated virtio-mmio claim in another page. A page held by another claimant is
  skipped (→ `NoNetDevice`).
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

## Capability

`VirtioNet::open()` calls `SYS_MMIO_CREATE` / `SYS_IRQ_CREATE` / `SYS_DMA_CREATE`,
all gated on **`CAP_HW_CREATE`**. A driver Proc MUST be spawned with that cap
(`t_spawn_with_caps(name, len, T_CAP_HW_CREATE)` / `rfork_with_caps`); without it
the create is rejected at the capability gate before `kobj_mmio_create`.

## Tests

- **Host (pure ring)**: 6 tests — TX back-pressure, capped reap, slot-in-range
  across u16 wrap; RX stop-at-cur_used, recycle-in-range, drain+recycle across
  wrap. `cargo test -p netdev --no-default-features --target aarch64-apple-darwin`.
- **Boot E2E** (`usr/netdev-test`, joey's THYLA_BOOT_PROBES ladder, PRE-stratumd):
  24 ARP round-trips against QEMU's slirp gateway through `send`/`poll_rx`
  (> `QUEUE_SIZE`, so the RX descriptors recycle past one full ring and the TX
  descriptors wrap). Verifies `PASS -- 24/24 ARP replies via VirtioNet`.

## Error paths

`OpenError`: `BankClaim`, `NoNetDevice` (no DeviceID=1 / net page held),
`LegacyDevice` (v1 MMIO), `NoVersion1`, `FeaturesRejected`, `QueueTooSmall`,
`IrqClaim`, `DmaAlloc`. `send` → `false` (empty / oversize / ring full).
`poll_rx` → `None` (no frame / bogus desc_id).

## Known caveats / the net-2 refinement

- **MMIO page co-residency (net-2 prerequisite).** QEMU packs 8 virtio-mmio
  slots per 4 KiB page; net + blk currently share one page. The page-exclusive
  `KObj_MMIO` claim means a long-lived `netd` (net) and stratumd (blk) cannot
  both hold that page. net-1 does NOT hit it (its probe runs and exits PRE-
  stratumd). net-2 must resolve it (a kernel sub-page MMIO claim, or device-page
  separation) — tracked.
- **Fixed user VAs.** `open()` maps at fixed VAs (`0x0050_0000` etc.); net-2's
  `netd` may parameterize.
- **IPv4/ARP frames at v1.0.** `MAX_FRAME = 1514` (no VLAN / jumbo).

## Spec cross-reference

No spec (pure-driver mechanics; prose-validated per the 2026-05-23 broadening).
The net arc's one reserved spec is `net_poll.tla` (the dev9p.poll readiness wake,
net-6) — unrelated to this layer.
