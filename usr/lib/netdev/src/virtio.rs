//! `VirtioNet` -- the native userspace virtio-net (MMIO transport) driver.
//!
//! Generalizes the proven `virtio-net-loop` probe (P4-Jc) into a reusable
//! RX+TX Ethernet frame transport: `send(&[u8])` for an arbitrary outbound
//! frame, `poll_rx(&mut [u8])` for one inbound frame, `drain_tx` to reclaim
//! completed TX descriptors, `wait_irq` to block on device activity. The
//! split-virtqueue index discipline lives in `crate::ring`; this module is the
//! device glue: MMIO init, the DMA descriptor/avail/used memory ops, and the
//! virtio audit hardenings.
//!
//! AUDIT HARDENINGS preserved from the probe (the RW-7/RW-8 virtio class):
//!   - `desc_id` from the device-controlled used ring is bounds-checked
//!     (`< QUEUE_SIZE`) before it scales the RX-pool base (else an OOB read).
//!   - `used.len` is clamped to the posted buffer length before any frame read.
//!   - `virtio_rmb` follows every `used.idx` load, before the used-ring entry /
//!     buffer reads (VIRTIO 1.2 section 2.7.13.2).
//!   - u16 avail/used counters wrap via `wrapping_add` (`ring` module).
//!   - TX back-pressure: never post more than `QUEUE_SIZE` in flight, so a
//!     buffer is never overwritten while the device may still read it.
//!   - every MMIO access is a single ISV-safe `ldr`/`str` (libthyla-rs `mmio_*`).
//!   - DEVICE-DEATH QUIESCE (`Drop`): QUEUE_READY=0 on both queues + a device
//!     reset BEFORE the DMA pages release, so the device cannot DMA into freed
//!     memory (RW-7 R3-F1 -- the discipline the single-shot probes skip).
//!
//! MMIO CLAIM SCOPE (docs/NET-DESIGN.md section 13): `open()` claims only the
//! net device's MMIO PAGE -- it probes each bank page, keeps the one holding
//! DeviceID=1, and releases the rest. It does NOT claim the whole bank: a
//! whole-bank claim conflicts with any unrelated virtio-mmio claim in another
//! page (the late-boot failure that drove this design). The net-2 co-residency
//! limit remains recorded: net + blk currently share one MMIO page, and the
//! page-exclusive KObj_MMIO claim means netd (net) and stratumd (blk) cannot
//! both hold that page -- the net-2 prerequisite (a kernel sub-page claim or a
//! device-page separation). net-1 does not hit it: its probe runs and exits
//! before stratumd claims the page.

use core::arch::asm;

use libthyla_rs::handle::Rights;
use libthyla_rs::hardware::{
    mmio_read16, mmio_read32, mmio_read8, mmio_write16, mmio_write32, mmio_write64, mmio_write8,
    Dma, Irq, Mmio,
};
use libthyla_rs::{virtio_rmb, T_PROT_READ, T_PROT_WRITE};

use crate::ring::{RxRing, TxRing};

// =============================================================================
// virtio-mmio bank (QEMU virt machine).
// =============================================================================

const VIRTIO_MMIO_BASE_PA: u64 = 0x0a00_0000;
const VIRTIO_MMIO_SLOT_STRIDE: u64 = 0x200;
const VIRTIO_MMIO_NUM_SLOTS: u64 = 32;
const PAGE_SIZE: u64 = 0x1000;
const VIRTIO_MMIO_BANK_SIZE: u64 = VIRTIO_MMIO_NUM_SLOTS * VIRTIO_MMIO_SLOT_STRIDE; // 0x4000
const VIRTIO_MMIO_BANK_PAGES: u64 = VIRTIO_MMIO_BANK_SIZE / PAGE_SIZE; // 4
const SLOTS_PER_PAGE: u64 = PAGE_SIZE / VIRTIO_MMIO_SLOT_STRIDE; // 8
const VIRTIO_MMIO_GIC_INTID_BASE: u32 = 32 + 16; // 48

// User-VA layout (fixed for net-1; net-2's netd may parameterize).
const MMIO_USER_VA: u64 = 0x0050_0000;
const RING_DMA_USER_VA: u64 = 0x0061_0000;
const TXPOOL_DMA_USER_VA: u64 = 0x0062_0000;
const RXPOOL_DMA_USER_VA: u64 = 0x0063_0000;

// =============================================================================
// VirtIO MMIO register offsets (VIRTIO 1.2 section 4.2.2).
// =============================================================================

const REG_MAGIC_VALUE: u64 = 0x000;
const REG_VERSION: u64 = 0x004;
const REG_DEVICE_ID: u64 = 0x008;
const REG_DEVICE_FEATURES: u64 = 0x010;
const REG_DEVICE_FEATURES_SEL: u64 = 0x014;
const REG_DRIVER_FEATURES: u64 = 0x020;
const REG_DRIVER_FEATURES_SEL: u64 = 0x024;
const REG_QUEUE_SEL: u64 = 0x030;
const REG_QUEUE_NUM_MAX: u64 = 0x034;
const REG_QUEUE_NUM: u64 = 0x038;
const REG_QUEUE_READY: u64 = 0x044;
const REG_QUEUE_NOTIFY: u64 = 0x050;
const REG_INTERRUPT_STATUS: u64 = 0x060;
const REG_INTERRUPT_ACK: u64 = 0x064;
const REG_STATUS: u64 = 0x070;
const REG_QUEUE_DESC_LOW: u64 = 0x080;
const REG_QUEUE_DESC_HIGH: u64 = 0x084;
const REG_QUEUE_DRIVER_LOW: u64 = 0x090;
const REG_QUEUE_DRIVER_HIGH: u64 = 0x094;
const REG_QUEUE_DEVICE_LOW: u64 = 0x0a0;
const REG_QUEUE_DEVICE_HIGH: u64 = 0x0a4;
const REG_CONFIG_BASE: u64 = 0x100;

const VIRTIO_MMIO_MAGIC: u32 = 0x7472_6976;
const VIRTIO_MMIO_VERSION_MODERN: u32 = 2;
const VIRTIO_DEVICE_ID_NET: u32 = 1;

const STATUS_ACKNOWLEDGE: u32 = 1;
const STATUS_DRIVER: u32 = 2;
const STATUS_DRIVER_OK: u32 = 4;
const STATUS_FEATURES_OK: u32 = 8;
const STATUS_FAILED: u32 = 128;

const VIRTIO_F_VERSION_1_BIT_BANK1: u32 = 1 << 0; // feature bit 32

const VIRTIO_NET_F_MAC_BIT: u32 = 1 << 5;
const VIRTIO_NET_F_STATUS_BIT: u32 = 1 << 16;

const NET_CFG_MAC: u64 = 0;
const NET_CFG_STATUS: u64 = 6;
const VIRTIO_NET_S_LINK_UP: u16 = 1 << 0;

const NET_QUEUE_RX: u32 = 0;
const NET_QUEUE_TX: u32 = 1;

const VIRTQ_DESC_F_WRITE: u16 = 2;

const INT_USED_BUFFER: u32 = 1 << 0;

// =============================================================================
// Frame + queue geometry.
// =============================================================================

/// virtio-net per-frame header prepended on every RX/TX buffer (no offload /
/// no GSO at v1.0, so it is 12 zero bytes on TX and skipped on RX).
pub const VIRTIO_NET_HDR_LEN: usize = 12;
/// Ethernet payload MTU.
pub const MTU: usize = 1500;
/// Largest Ethernet frame this driver sends/receives (header + MTU). No VLAN /
/// jumbo at v1.0.
pub const MAX_FRAME: usize = 14 + MTU; // 1514

const QUEUE_SIZE: u16 = 16;
const BUF_LEN: usize = 2048; // >= VIRTIO_NET_HDR_LEN + MAX_FRAME (1526)

// DMA ring page (4 KiB): the six split-virtqueue structures, no buffers (the
// frame pools are separate MTU-sized DMA regions).
const RING_DMA_SIZE: usize = 4096;
const TX_DESC_OFF: u64 = 0x000;
const TX_AVAIL_OFF: u64 = 0x100;
const TX_USED_OFF: u64 = 0x200;
const RX_DESC_OFF: u64 = 0x400;
const RX_AVAIL_OFF: u64 = 0x500;
const RX_USED_OFF: u64 = 0x600;

const POOL_DMA_SIZE: usize = (QUEUE_SIZE as usize) * BUF_LEN; // 32 KiB each

// Compile-time layout sanity: every ring region is non-overlapping in monotonic
// order and fits the ring page; a buffer holds a full frame + the net header.
const _: () = {
    assert!(BUF_LEN >= VIRTIO_NET_HDR_LEN + MAX_FRAME, "buffer too small for MTU frame");
    assert!(TX_DESC_OFF + (QUEUE_SIZE as u64) * 16 <= TX_AVAIL_OFF);
    assert!(TX_AVAIL_OFF + 4 + (QUEUE_SIZE as u64) * 2 <= TX_USED_OFF);
    assert!(TX_USED_OFF + 4 + (QUEUE_SIZE as u64) * 8 <= RX_DESC_OFF);
    assert!(RX_DESC_OFF + (QUEUE_SIZE as u64) * 16 <= RX_AVAIL_OFF);
    assert!(RX_AVAIL_OFF + 4 + (QUEUE_SIZE as u64) * 2 <= RX_USED_OFF);
    assert!(RX_USED_OFF + 4 + (QUEUE_SIZE as u64) * 8 <= RING_DMA_SIZE as u64);
};

// =============================================================================
// Single-instruction accessors (ISV-safe MMIO; same primitives for the normal
// DMA ring memory -- a bare ldr/str is correct there too, paired with the
// explicit dsb / virtio_rmb barriers).
// =============================================================================

#[inline(always)]
unsafe fn r32(addr: u64) -> u32 {
    mmio_read32(addr)
}
#[inline(always)]
unsafe fn w32(addr: u64, v: u32) {
    mmio_write32(addr, v)
}
#[inline(always)]
unsafe fn r16(addr: u64) -> u16 {
    mmio_read16(addr)
}
#[inline(always)]
unsafe fn w16(addr: u64, v: u16) {
    mmio_write16(addr, v)
}
#[inline(always)]
unsafe fn w64(addr: u64, v: u64) {
    mmio_write64(addr, v)
}
#[inline(always)]
unsafe fn r8(addr: u64) -> u8 {
    mmio_read8(addr)
}
#[inline(always)]
unsafe fn w8(addr: u64, v: u8) {
    mmio_write8(addr, v)
}
#[inline(always)]
fn dsb_sy() {
    unsafe { asm!("dsb sy", options(nostack, preserves_flags)) }
}

// =============================================================================
// Open errors.
// =============================================================================

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum OpenError {
    /// `SYS_MMIO_CREATE`/`MAP` for the virtio-mmio bank failed (claim conflict
    /// or no `CAP_HW_CREATE`).
    BankClaim,
    /// No virtio-mmio slot reports `DeviceID == 1` (no net device attached).
    NoNetDevice,
    /// The net slot reports the legacy (v1) MMIO layout; only modern (v2) is
    /// supported.
    LegacyDevice,
    /// The device does not advertise `VIRTIO_F_VERSION_1`.
    NoVersion1,
    /// The device rejected `FEATURES_OK` after negotiation.
    FeaturesRejected,
    /// `QueueNumMax` is below `QUEUE_SIZE` on a required queue.
    QueueTooSmall,
    /// `SYS_IRQ_CREATE` for the device INTID failed.
    IrqClaim,
    /// `SYS_DMA_CREATE`/`MAP` for a ring or pool failed.
    DmaAlloc,
}

// =============================================================================
// VirtioNet.
// =============================================================================

/// A claimed + initialized virtio-net device. RX buffers are pre-posted at
/// `open()`; `send` / `poll_rx` move arbitrary Ethernet frames; `Drop`
/// quiesces the device before its DMA pages release.
pub struct VirtioNet {
    // RAII handles. Held for the driver's lifetime; their `Drop` (handle close)
    // runs AFTER `VirtioNet::drop` quiesces the device (Rust runs the outer
    // `Drop::drop` before dropping fields), so the device is reset before any
    // page release.
    _mmio: Mmio,
    _irq: Irq,
    ring: Dma,
    txpool: Dma,
    rxpool: Dma,

    slot_va: u64,
    tx: TxRing,
    rx: RxRing,
    mac: [u8; 6],
    negotiated_lo: u32,
}

impl VirtioNet {
    /// Claim the virtio-mmio bank, locate the net device, allocate its rings +
    /// frame pools, run the VIRTIO 1.2 init handshake, pre-post the RX buffers,
    /// and arm the device (`DRIVER_OK`).
    pub fn open() -> Result<Self, OpenError> {
        let rw_map = Rights::READ | Rights::WRITE | Rights::MAP;
        let prot = T_PROT_READ | T_PROT_WRITE;

        // Find + claim ONLY the net device's MMIO page (not the whole bank).
        // Probe each page in turn: claim it, scan its slots for DeviceID=1, keep
        // the page that holds the net device, release the rest. Claiming one
        // page -- not the bank -- avoids conflicting with unrelated virtio-mmio
        // claims in OTHER pages (the late-boot failure the whole-bank claim hit),
        // and is the minimal claim net-2's netd needs. A page already held by
        // another claimant is skipped, so a net device whose page is held (the
        // net-2 co-residency case vs stratumd) yields NoNetDevice / SKIP, never a
        // hard failure.
        let mut found: Option<(Mmio, u32, u64)> = None;
        for p in 0..VIRTIO_MMIO_BANK_PAGES {
            let pa = VIRTIO_MMIO_BASE_PA + p * PAGE_SIZE;
            let va = MMIO_USER_VA + p * PAGE_SIZE;
            let m = match unsafe { Mmio::new(pa, PAGE_SIZE as usize, rw_map, va, prot) } {
                Ok(m) => m,
                Err(_) => continue, // page held / unmappable -- skip it
            };
            let mut hit: Option<(u32, u64)> = None;
            for s in 0..SLOTS_PER_PAGE {
                let slot_va = va + s * VIRTIO_MMIO_SLOT_STRIDE;
                if unsafe { r32(slot_va + REG_MAGIC_VALUE) } != VIRTIO_MMIO_MAGIC {
                    continue;
                }
                if unsafe { r32(slot_va + REG_DEVICE_ID) } == VIRTIO_DEVICE_ID_NET {
                    hit = Some(((p * SLOTS_PER_PAGE + s) as u32, slot_va));
                    break;
                }
            }
            if let Some((slot, slot_va)) = hit {
                found = Some((m, slot, slot_va));
                break;
            }
            // No net device in this page; `m` drops here, releasing the claim.
        }
        let (mmio, slot, slot_va) = found.ok_or(OpenError::NoNetDevice)?;
        if unsafe { r32(slot_va + REG_VERSION) } != VIRTIO_MMIO_VERSION_MODERN {
            return Err(OpenError::LegacyDevice);
        }
        let intid = VIRTIO_MMIO_GIC_INTID_BASE + slot;

        let irq = Irq::new(intid, Rights::SIGNAL).map_err(|_| OpenError::IrqClaim)?;

        let ring =
            unsafe { Dma::new(RING_DMA_SIZE, rw_map, RING_DMA_USER_VA, prot) }.map_err(|_| OpenError::DmaAlloc)?;
        let txpool = unsafe { Dma::new(POOL_DMA_SIZE, rw_map, TXPOOL_DMA_USER_VA, prot) }
            .map_err(|_| OpenError::DmaAlloc)?;
        let rxpool = unsafe { Dma::new(POOL_DMA_SIZE, rw_map, RXPOOL_DMA_USER_VA, prot) }
            .map_err(|_| OpenError::DmaAlloc)?;

        // Pre-warm (demand-page) each DMA region before the device touches it.
        prewarm(ring.base_va() as u64, RING_DMA_SIZE);
        prewarm(txpool.base_va() as u64, POOL_DMA_SIZE);
        prewarm(rxpool.base_va() as u64, POOL_DMA_SIZE);

        let mut mac = [0u8; 6];
        let negotiated_lo = init_device(
            slot_va,
            ring.base_va() as u64,
            ring.paddr(),
            txpool.paddr(),
            rxpool.paddr(),
            &mut mac,
        )?;

        Ok(Self {
            _mmio: mmio,
            _irq: irq,
            ring,
            txpool,
            rxpool,
            slot_va,
            tx: TxRing::new(QUEUE_SIZE),
            rx: RxRing::new(QUEUE_SIZE),
            mac,
            negotiated_lo,
        })
    }

    /// The device MAC (from config space, or the QEMU default if the device did
    /// not advertise `VIRTIO_NET_F_MAC`).
    #[inline]
    pub fn mac(&self) -> [u8; 6] {
        self.mac
    }

    /// The Ethernet payload MTU.
    #[inline]
    pub fn mtu(&self) -> usize {
        MTU
    }

    /// Link state. True if the device advertised `VIRTIO_NET_F_STATUS` and the
    /// link bit is set; if STATUS was not negotiated, assumes up (the QEMU
    /// slirp NIC has no link-down state).
    pub fn link_up(&self) -> bool {
        if self.negotiated_lo & VIRTIO_NET_F_STATUS_BIT == 0 {
            return true;
        }
        let st = unsafe { r16(self.slot_va + REG_CONFIG_BASE + NET_CFG_STATUS) };
        st & VIRTIO_NET_S_LINK_UP != 0
    }

    /// Post one Ethernet frame for transmission. Returns false if `frame` is
    /// empty, exceeds `MAX_FRAME`, or the TX ring is full after a reclaim
    /// attempt (back-pressure -- the caller retries after `wait_irq`).
    pub fn send(&mut self, frame: &[u8]) -> bool {
        if frame.is_empty() || frame.len() > MAX_FRAME {
            return false;
        }
        if !self.tx.can_post() {
            self.drain_tx();
            if !self.tx.can_post() {
                return false;
            }
        }
        let slot = self.tx.next_slot();
        let ring_va = self.ring.base_va() as u64;
        let buf_va = self.txpool.base_va() as u64 + (slot * BUF_LEN) as u64;

        // virtio_net_hdr: 12 zero bytes, then the frame.
        unsafe {
            for i in 0..VIRTIO_NET_HDR_LEN as u64 {
                w8(buf_va + i, 0);
            }
            for (i, &b) in frame.iter().enumerate() {
                w8(buf_va + VIRTIO_NET_HDR_LEN as u64 + i as u64, b);
            }
            // descriptor[slot].len = hdr + frame (addr/flags set once at init).
            let desc = ring_va + TX_DESC_OFF + (slot as u64) * 16;
            w32(desc + 8, (VIRTIO_NET_HDR_LEN + frame.len()) as u32);
        }

        let new_idx = self.tx.commit_post();
        unsafe {
            dsb_sy();
            w16(ring_va + TX_AVAIL_OFF + 2, new_idx);
            dsb_sy();
            w32(self.slot_va + REG_QUEUE_NOTIFY, NET_QUEUE_TX);
        }
        true
    }

    /// Drain one received frame into `out` (non-blocking). Returns the number of
    /// frame bytes written (the device's frame minus the virtio_net_hdr, clamped
    /// to `out.len()`), or `None` if no frame is ready. Pass `out.len() >=
    /// MAX_FRAME` to never truncate. The consumed RX descriptor is recycled
    /// before returning, so `out` holds a private copy.
    pub fn poll_rx(&mut self, out: &mut [u8]) -> Option<usize> {
        let ring_va = self.ring.base_va() as u64;
        let rx_used_va = ring_va + RX_USED_OFF;

        let cur_used = unsafe { r16(rx_used_va + 2) };
        // Barrier between the used.idx observation and the used-ring entry /
        // buffer reads (VIRTIO 1.2 section 2.7.13.2).
        virtio_rmb();

        let used_slot = self.rx.take_used(cur_used)?;
        let entry = rx_used_va + 4 + (used_slot as u64) * 8;
        let desc_id = unsafe { r32(entry) };
        let raw_len = unsafe { r32(entry + 4) };

        // desc_id is device-controlled: reject out-of-range before it scales the
        // RX-pool base (the critical OOB guard).
        if desc_id >= QUEUE_SIZE as u32 {
            // A bogus, out-of-range id means the device misbehaved: which buffer
            // it actually filled is unknowable, so we must NOT republish a
            // fabricated descriptor -- that could double-post a still-live buffer
            // (audit F1). The used entry is already consumed (take_used advanced
            // seen_used); drop it without recycling. A device that keeps lying
            // starves its OWN rx (the avail ring shrinks) -- safe: no double-post,
            // no OOB. Trusted QEMU never emits this.
            return None;
        }
        // Clamp the device-reported length to the largest legal frame (header +
        // MTU, <= BUF_LEN) before any read, then strip the net header. The clamp
        // is the memory-safety bound -- any raw_len, including a lying
        // 0xFFFF_FFFF, is pinned inside buffer desc_id; the copy is then further
        // bounded by out.len().
        let len = (raw_len as usize).min(VIRTIO_NET_HDR_LEN + MAX_FRAME);
        let frame_len = len.saturating_sub(VIRTIO_NET_HDR_LEN);
        let n = frame_len.min(out.len());

        let base = self.rxpool.base_va() as u64 + (desc_id as u64) * BUF_LEN as u64;
        let src = base + VIRTIO_NET_HDR_LEN as u64;
        for (i, b) in out.iter_mut().enumerate().take(n) {
            *b = unsafe { r8(src + i as u64) };
        }

        self.recycle_rx(desc_id);
        Some(n)
    }

    /// Re-publish a drained RX descriptor to the avail ring + notify the device.
    fn recycle_rx(&mut self, desc_id: u32) {
        let ring_va = self.ring.base_va() as u64;
        let rx_avail_va = ring_va + RX_AVAIL_OFF;
        let avail_slot = self.rx.recycle_slot();
        let new_idx = self.rx.avail_idx();
        unsafe {
            w16(rx_avail_va + 4 + (avail_slot as u64) * 2, desc_id as u16);
            dsb_sy();
            w16(rx_avail_va + 2, new_idx);
            dsb_sy();
            w32(self.slot_va + REG_QUEUE_NOTIFY, NET_QUEUE_RX);
        }
    }

    /// Reclaim completed TX descriptors (frees back-pressure for `send`).
    pub fn drain_tx(&mut self) {
        let ring_va = self.ring.base_va() as u64;
        let cur_used = unsafe { r16(ring_va + TX_USED_OFF + 2) };
        virtio_rmb();
        self.tx.reap(cur_used, QUEUE_SIZE);
    }

    /// Block until the device raises an interrupt; ACK it and report whether it
    /// signalled used-ring progress (vs a config-change-only wake). The caller
    /// then drains TX completions + `poll_rx`es inbound frames.
    pub fn wait_irq(&self) -> bool {
        let _ = self._irq.wait();
        let status = unsafe { r32(self.slot_va + REG_INTERRUPT_STATUS) };
        unsafe { w32(self.slot_va + REG_INTERRUPT_ACK, status) };
        status & INT_USED_BUFFER != 0
    }

    /// Stop the device: QUEUE_READY=0 on both queues + a device reset. After
    /// this the device performs no further DMA, so its buffers are safe to free.
    fn quiesce(&self) {
        unsafe {
            w32(self.slot_va + REG_QUEUE_SEL, NET_QUEUE_RX);
            w32(self.slot_va + REG_QUEUE_READY, 0);
            w32(self.slot_va + REG_QUEUE_SEL, NET_QUEUE_TX);
            w32(self.slot_va + REG_QUEUE_READY, 0);
            dsb_sy();
            w32(self.slot_va + REG_STATUS, 0);
            dsb_sy();
        }
    }
}

impl Drop for VirtioNet {
    fn drop(&mut self) {
        // Quiesce BEFORE the Dma/Mmio fields drop (Rust runs this before field
        // drops), so the device is reset before any page release (RW-7 R3-F1).
        self.quiesce();
    }
}

// =============================================================================
// Init helpers (operate on raw VAs before the struct exists).
// =============================================================================

fn prewarm(va: u64, size: usize) {
    let mut off = 0u64;
    while (off as usize) < size {
        unsafe { w8(va + off, 0) };
        off += PAGE_SIZE;
    }
}

fn setup_queue(slot_va: u64, queue_idx: u32, desc_pa: u64, avail_pa: u64, used_pa: u64) -> bool {
    unsafe {
        w32(slot_va + REG_QUEUE_SEL, queue_idx);
        if r32(slot_va + REG_QUEUE_NUM_MAX) < QUEUE_SIZE as u32 {
            return false;
        }
        w32(slot_va + REG_QUEUE_NUM, QUEUE_SIZE as u32);
        w32(slot_va + REG_QUEUE_DESC_LOW, (desc_pa & 0xFFFF_FFFF) as u32);
        w32(slot_va + REG_QUEUE_DESC_HIGH, (desc_pa >> 32) as u32);
        w32(slot_va + REG_QUEUE_DRIVER_LOW, (avail_pa & 0xFFFF_FFFF) as u32);
        w32(slot_va + REG_QUEUE_DRIVER_HIGH, (avail_pa >> 32) as u32);
        w32(slot_va + REG_QUEUE_DEVICE_LOW, (used_pa & 0xFFFF_FFFF) as u32);
        w32(slot_va + REG_QUEUE_DEVICE_HIGH, (used_pa >> 32) as u32);
        w32(slot_va + REG_QUEUE_READY, 1);
    }
    true
}

/// Pre-post all 16 RX descriptors (device-writable, one buffer each) and
/// announce them (avail.ring[k]=k, avail.idx=16).
fn populate_rx(ring_va: u64, rxpool_pa: u64) {
    unsafe {
        for k in 0..QUEUE_SIZE as u64 {
            let d = ring_va + RX_DESC_OFF + k * 16;
            w64(d, rxpool_pa + k * BUF_LEN as u64);
            w32(d + 8, BUF_LEN as u32);
            w16(d + 12, VIRTQ_DESC_F_WRITE);
            w16(d + 14, 0);
        }
        let avail = ring_va + RX_AVAIL_OFF;
        w16(avail, 0); // flags
        for k in 0..QUEUE_SIZE {
            w16(avail + 4 + (k as u64) * 2, k);
        }
        dsb_sy();
        w16(avail + 2, QUEUE_SIZE); // idx = 16, all posted
        dsb_sy();
    }
}

/// Pre-init the 16 TX descriptors (addr = txpool[k], static; `len` is rewritten
/// per `send`) and the TX avail ring (ring[k]=k, idx=0 -- nothing in flight).
fn populate_tx(ring_va: u64, txpool_pa: u64) {
    unsafe {
        for k in 0..QUEUE_SIZE as u64 {
            let d = ring_va + TX_DESC_OFF + k * 16;
            w64(d, txpool_pa + k * BUF_LEN as u64);
            w32(d + 8, 0); // len set per send
            w16(d + 12, 0); // single descriptor, no NEXT, device-readable
            w16(d + 14, 0);
        }
        let avail = ring_va + TX_AVAIL_OFF;
        w16(avail, 0); // flags
        for k in 0..QUEUE_SIZE {
            w16(avail + 4 + (k as u64) * 2, k);
        }
        dsb_sy();
        w16(avail + 2, 0); // idx = 0
        dsb_sy();
    }
}

fn init_device(
    slot_va: u64,
    ring_va: u64,
    ring_pa: u64,
    txpool_pa: u64,
    rxpool_pa: u64,
    mac: &mut [u8; 6],
) -> Result<u32, OpenError> {
    unsafe {
        w32(slot_va + REG_STATUS, 0);
        w32(slot_va + REG_STATUS, STATUS_ACKNOWLEDGE);
        w32(slot_va + REG_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER);

        w32(slot_va + REG_DEVICE_FEATURES_SEL, 0);
        let dev_feat_lo = r32(slot_va + REG_DEVICE_FEATURES);
        w32(slot_va + REG_DEVICE_FEATURES_SEL, 1);
        let dev_feat_hi = r32(slot_va + REG_DEVICE_FEATURES);

        if dev_feat_hi & VIRTIO_F_VERSION_1_BIT_BANK1 == 0 {
            w32(slot_va + REG_STATUS, STATUS_FAILED);
            return Err(OpenError::NoVersion1);
        }

        let want_lo = dev_feat_lo & (VIRTIO_NET_F_MAC_BIT | VIRTIO_NET_F_STATUS_BIT);
        w32(slot_va + REG_DRIVER_FEATURES_SEL, 0);
        w32(slot_va + REG_DRIVER_FEATURES, want_lo);
        w32(slot_va + REG_DRIVER_FEATURES_SEL, 1);
        w32(slot_va + REG_DRIVER_FEATURES, VIRTIO_F_VERSION_1_BIT_BANK1);

        w32(slot_va + REG_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK);
        if r32(slot_va + REG_STATUS) & STATUS_FEATURES_OK == 0 {
            w32(slot_va + REG_STATUS, STATUS_FAILED);
            return Err(OpenError::FeaturesRejected);
        }

        // Configure both queues before DRIVER_OK.
        if !setup_queue(slot_va, NET_QUEUE_RX, ring_pa + RX_DESC_OFF, ring_pa + RX_AVAIL_OFF, ring_pa + RX_USED_OFF)
        {
            w32(slot_va + REG_STATUS, STATUS_FAILED);
            return Err(OpenError::QueueTooSmall);
        }
        if !setup_queue(slot_va, NET_QUEUE_TX, ring_pa + TX_DESC_OFF, ring_pa + TX_AVAIL_OFF, ring_pa + TX_USED_OFF)
        {
            w32(slot_va + REG_STATUS, STATUS_FAILED);
            return Err(OpenError::QueueTooSmall);
        }

        if want_lo & VIRTIO_NET_F_MAC_BIT != 0 {
            for (i, b) in mac.iter_mut().enumerate() {
                *b = r8(slot_va + REG_CONFIG_BASE + NET_CFG_MAC + i as u64);
            }
        } else {
            *mac = [0x52, 0x54, 0x00, 0x12, 0x34, 0x56];
        }

        populate_rx(ring_va, rxpool_pa);
        populate_tx(ring_va, txpool_pa);

        w32(slot_va + REG_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK | STATUS_DRIVER_OK);
        Ok(want_lo)
    }
}
