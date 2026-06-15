//! `VirtioNetPci` -- the virtio-net driver over the virtio-pci-modern transport.
//!
//! The PCI sibling of `crate::virtio` (the virtio-mmio `VirtioNet`). It reuses
//! `crate::ring` VERBATIM (the audited split-virtqueue index arithmetic) and the
//! same DMA ring/pool layout; only the device-register TRANSPORT differs. On the
//! mmio path the registers are a flat bank (`REG_STATUS`, `REG_QUEUE_SEL`, ...);
//! here they live in the BAR-mapped VirtIO-PCI capability regions (VIRTIO 1.2
//! section 4.1.4): `virtio_pci_common_cfg` (feature negotiation + per-queue
//! setup), the notify region (per-queue doorbell), the ISR byte (read-to-clear),
//! and the device-config region (the net MAC + link status).
//!
//! WHY a sibling rather than a refactor of `virtio.rs`: net-1's MMIO driver is
//! audited + landed; keeping it byte-identical leaves its boot proof intact and
//! presents this PCI driver as a fresh, independently-auditable surface (pci-3).
//! The split-virtqueue DMA discipline + the audit hardenings below mirror
//! `virtio.rs` line-for-line -- the only divergences are register access,
//! queue notification, and ISR acknowledgement.
//!
//! AUDIT HARDENINGS (the RW-7/RW-8 virtio class; identical to `virtio.rs`):
//!   - `desc_id` from the device-controlled used ring is bounds-checked
//!     (`< QUEUE_SIZE`) before it scales the RX-pool base (else an OOB read); a
//!     bogus id is DROPPED, never recycled as a fabricated descriptor.
//!   - `used.len` is clamped to the largest legal frame before any buffer read.
//!   - `virtio_rmb` follows every `used.idx` load, before the used-ring entry /
//!     buffer reads (VIRTIO 1.2 section 2.7.13.2).
//!   - u16 avail/used counters wrap via `crate::ring` (`wrapping_add`).
//!   - TX back-pressure: never more than `QUEUE_SIZE` in flight.
//!   - every device-register access is a single ISV-safe `ldr`/`str`.
//!   - DEVICE-DEATH QUIESCE (`Drop`): device reset (`device_status = 0`) BEFORE
//!     the DMA pages release, so the device cannot DMA into freed memory.

use core::arch::asm;

use libthyla_rs::handle::Rights;
use libthyla_rs::hardware::{
    mmio_read16, mmio_read32, mmio_read8, mmio_write16, mmio_write32, mmio_write64, mmio_write8,
    Dma, Irq, PciDev, PciError, PciRegion, PCI_BAR_VA_STRIDE,
};
use libthyla_rs::{virtio_rmb, T_PROT_READ, T_PROT_WRITE};

use crate::ring::{RxRing, TxRing};
// Frame geometry is shared with the MMIO path -- single source of truth so the
// user-facing API (netdev::MAX_FRAME etc.) is identical across transports.
use crate::virtio::{MAX_FRAME, VIRTIO_NET_HDR_LEN};

// =============================================================================
// VirtIO device + virtio-net feature/queue identifiers.
// =============================================================================

/// The VirtIO device id for a network card (NOT the PCI device id; the kernel
/// claims by virtio_device_id, which it derives from the PCI device/subsystem
/// id per VIRTIO 1.2 section 4.1.2).
const VIRTIO_DEVICE_ID_NET: u32 = 1;

// `device_status` bits (VIRTIO 1.2 section 2.1) -- a u8 in common cfg.
const STATUS_ACKNOWLEDGE: u8 = 1;
const STATUS_DRIVER: u8 = 2;
const STATUS_DRIVER_OK: u8 = 4;
const STATUS_FEATURES_OK: u8 = 8;
const STATUS_FAILED: u8 = 128;

// VIRTIO_F_VERSION_1 is feature bit 32 -- bit 0 of the HIGH feature dword.
const VIRTIO_F_VERSION_1_BIT_HI: u32 = 1 << 0;

// virtio-net feature bits (low dword): MAC = bit 5, STATUS = bit 16.
const VIRTIO_NET_F_MAC_BIT: u32 = 1 << 5;
const VIRTIO_NET_F_STATUS_BIT: u32 = 1 << 16;

// virtio-net device-config layout (VIRTIO 1.2 section 5.1.4).
const NET_CFG_MAC: u64 = 0;
const NET_CFG_STATUS: u64 = 6;
const VIRTIO_NET_S_LINK_UP: u16 = 1 << 0;

const NET_QUEUE_RX: u16 = 0;
const NET_QUEUE_TX: u16 = 1;

const VIRTQ_DESC_F_WRITE: u16 = 2;

// ISR status (VIRTIO 1.2 section 4.1.4.5): bit 0 = a virtqueue raised the IRQ;
// reading the byte clears it (no separate ACK register, unlike mmio).
const ISR_QUEUE: u8 = 1 << 0;

// MSI-X is undriven at v1.0 (INTx only) -- park both the config + per-queue
// MSI-X vectors at NO_VECTOR so the device routes interrupts through INTx.
const VIRTIO_MSI_NO_VECTOR: u16 = 0xFFFF;

// =============================================================================
// virtio_pci_common_cfg field offsets (VIRTIO 1.2 section 4.1.4.3).
// =============================================================================

const CCFG_DEVICE_FEATURE_SELECT: u64 = 0x00; // le32 RW
const CCFG_DEVICE_FEATURE: u64 = 0x04; // le32 RO
const CCFG_DRIVER_FEATURE_SELECT: u64 = 0x08; // le32 RW
const CCFG_DRIVER_FEATURE: u64 = 0x0C; // le32 RW
const CCFG_CONFIG_MSIX_VECTOR: u64 = 0x10; // le16 RW
const CCFG_DEVICE_STATUS: u64 = 0x14; // u8 RW
const CCFG_QUEUE_SELECT: u64 = 0x16; // le16 RW
const CCFG_QUEUE_SIZE: u64 = 0x18; // le16 RW
const CCFG_QUEUE_MSIX_VECTOR: u64 = 0x1A; // le16 RW
const CCFG_QUEUE_ENABLE: u64 = 0x1C; // le16 RW
const CCFG_QUEUE_NOTIFY_OFF: u64 = 0x1E; // le16 RO
const CCFG_QUEUE_DESC: u64 = 0x20; // le64 RW
const CCFG_QUEUE_DRIVER: u64 = 0x28; // le64 RW (avail ring)
const CCFG_QUEUE_DEVICE: u64 = 0x30; // le64 RW (used ring)

// The driver reads through `queue_device` (0x30, le64) -- the full common-cfg
// structure is 0x38 bytes. Reject a `common` region the device reports as
// smaller before any register access, so a malformed/undersized region is a
// clean open error rather than a read past the region into the rest of the BAR.
const CCFG_MIN_LEN: u32 = 0x38;
// The driver reads the MAC (0..6) + link status (6..8) from device-config.
const DEVICE_CFG_MIN_LEN: u32 = 8;

// =============================================================================
// Frame + queue geometry (mirrors crate::virtio; the rings live in the SAME DMA
// layout, so the index arithmetic in crate::ring carries over unchanged).
// =============================================================================

const QUEUE_SIZE: u16 = 16;
const BUF_LEN: usize = 2048; // >= VIRTIO_NET_HDR_LEN + MAX_FRAME (1526)

const RING_DMA_SIZE: usize = 4096;
const TX_DESC_OFF: u64 = 0x000;
const TX_AVAIL_OFF: u64 = 0x100;
const TX_USED_OFF: u64 = 0x200;
const RX_DESC_OFF: u64 = 0x400;
const RX_AVAIL_OFF: u64 = 0x500;
const RX_USED_OFF: u64 = 0x600;

const POOL_DMA_SIZE: usize = (QUEUE_SIZE as usize) * BUF_LEN; // 32 KiB each

const PAGE_SIZE: u64 = 0x1000;

// User-VA layout (distinct window from the mmio path's 0x0050_0000.. so a
// future netd that ran both transports would not collide). The PCI BAR window
// holds up to six PCI_BAR_VA_STRIDE-spaced BARs.
const BAR_WINDOW_VA: u64 = 0x0080_0000;
const RING_DMA_USER_VA: u64 = 0x0100_0000;
const TXPOOL_DMA_USER_VA: u64 = 0x0102_0000;
const RXPOOL_DMA_USER_VA: u64 = 0x0104_0000;

// Compile-time layout sanity (mirrors crate::virtio) + the BAR window must fit
// below the first DMA region so a six-BAR map never reaches RING_DMA_USER_VA.
const _: () = {
    assert!(BUF_LEN >= VIRTIO_NET_HDR_LEN + MAX_FRAME, "buffer too small for MTU frame");
    assert!(TX_DESC_OFF + (QUEUE_SIZE as u64) * 16 <= TX_AVAIL_OFF);
    assert!(TX_AVAIL_OFF + 4 + (QUEUE_SIZE as u64) * 2 <= TX_USED_OFF);
    assert!(TX_USED_OFF + 4 + (QUEUE_SIZE as u64) * 8 <= RX_DESC_OFF);
    assert!(RX_DESC_OFF + (QUEUE_SIZE as u64) * 16 <= RX_AVAIL_OFF);
    assert!(RX_AVAIL_OFF + 4 + (QUEUE_SIZE as u64) * 2 <= RX_USED_OFF);
    assert!(RX_USED_OFF + 4 + (QUEUE_SIZE as u64) * 8 <= RING_DMA_SIZE as u64);
    assert!(BAR_WINDOW_VA + 6 * PCI_BAR_VA_STRIDE <= RING_DMA_USER_VA, "BAR window overruns DMA");
    assert!(RING_DMA_USER_VA + RING_DMA_SIZE as u64 <= TXPOOL_DMA_USER_VA);
    assert!(TXPOOL_DMA_USER_VA + POOL_DMA_SIZE as u64 <= RXPOOL_DMA_USER_VA);
};

// =============================================================================
// Single-instruction accessors (ISV-safe; same primitives as crate::virtio).
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
pub enum PciOpenError {
    /// `SYS_PCI_CLAIM` for a virtio-net function failed: no net-PCI device, the
    /// function is already claimed, BAR assignment failed, or no `CAP_HW_CREATE`.
    /// The boot probe treats this as SKIP (a config without a net-PCI device).
    NoNetDevice,
    /// `SYS_PCI_INFO` / `SYS_PCI_MAP_BAR` failed, or a BAR exceeds the VA window.
    BarMap,
    /// The function exposes no `common`, `notify`, `isr`, or `device` region --
    /// not a modern virtio device.
    MissingRegion,
    /// The DTB interrupt-map did not resolve an INTx GIC INTID.
    NoIntid,
    /// The device does not advertise `VIRTIO_F_VERSION_1`.
    NoVersion1,
    /// The device rejected `FEATURES_OK` after negotiation.
    FeaturesRejected,
    /// `queue_size` is below `QUEUE_SIZE` on a required queue.
    QueueTooSmall,
    /// A queue's `queue_notify_off * notify_off_multiplier` doorbell would land
    /// past the notify region's reported length (a malformed / hostile device).
    NotifyRegionTooSmall,
    /// `SYS_IRQ_CREATE` for the device INTID failed.
    IrqClaim,
    /// `SYS_DMA_CREATE` / `MAP` for a ring or pool failed.
    DmaAlloc,
}

// =============================================================================
// VirtioNetPci.
// =============================================================================

/// A claimed + initialized virtio-net-pci device. The API mirrors
/// `crate::virtio::VirtioNet` exactly (`send` / `poll_rx` / `drain_tx` /
/// `wait_irq` / `mac` / `link_up`); only the transport underneath differs.
pub struct VirtioNetPci {
    // RAII handles. Held for the driver's lifetime; their `Drop` runs AFTER
    // `VirtioNetPci::drop` quiesces the device (Rust runs the outer drop before
    // field drops), so the device is reset before any page release.
    _pci: PciDev,
    _irq: Irq,
    ring: Dma,
    txpool: Dma,
    rxpool: Dma,

    common_va: u64,
    isr_va: u64,
    device_cfg_va: Option<u64>,
    rx_notify_va: u64,
    tx_notify_va: u64,

    tx: TxRing,
    rx: RxRing,
    mac: [u8; 6],
    negotiated_lo: u32,
}

impl VirtioNetPci {
    /// Claim the virtio-net-pci function, map its BARs, allocate its rings +
    /// frame pools, run the VIRTIO 1.2 modern-PCI init handshake, pre-post the
    /// RX buffers, and arm the device (`DRIVER_OK`).
    pub fn open() -> Result<Self, PciOpenError> {
        let rw_map = Rights::READ | Rights::WRITE | Rights::MAP;
        let prot = T_PROT_READ | T_PROT_WRITE;

        // Claim the net function + map its BARs. A claim failure is treated as
        // "no net-PCI device" (SKIP) -- the bare syscall cannot distinguish
        // not-found from already-claimed, and the probe is spawned WITH
        // CAP_HW_CREATE so cap-missing is not the cause.
        let pci = unsafe { PciDev::claim(VIRTIO_DEVICE_ID_NET, BAR_WINDOW_VA) }.map_err(|e| match e {
            PciError::Claim => PciOpenError::NoNetDevice,
            _ => PciOpenError::BarMap,
        })?;

        let (common_va, common_len) = pci.region(PciRegion::Common).ok_or(PciOpenError::MissingRegion)?;
        if common_len < CCFG_MIN_LEN {
            return Err(PciOpenError::MissingRegion);
        }
        let (notify_base, notify_len) = pci.region(PciRegion::Notify).ok_or(PciOpenError::MissingRegion)?;
        let isr_va = pci.region(PciRegion::Isr).ok_or(PciOpenError::MissingRegion)?.0;
        // Device-config is optional; treat a too-small region as absent so the
        // MAC/status reads never run past it. Without it we fall back to a
        // default MAC + assume-link-up (the same posture as the mmio path).
        let device_cfg_va = pci
            .region(PciRegion::Device)
            .filter(|&(_, len)| len >= DEVICE_CFG_MIN_LEN)
            .map(|(va, _)| va);
        let notify_mul = u64::from(pci.notify_off_multiplier());
        let intid = pci.intid().ok_or(PciOpenError::NoIntid)?;

        let irq = Irq::new(intid, Rights::SIGNAL).map_err(|_| PciOpenError::IrqClaim)?;

        let ring = unsafe { Dma::new(RING_DMA_SIZE, rw_map, RING_DMA_USER_VA, prot) }
            .map_err(|_| PciOpenError::DmaAlloc)?;
        let txpool = unsafe { Dma::new(POOL_DMA_SIZE, rw_map, TXPOOL_DMA_USER_VA, prot) }
            .map_err(|_| PciOpenError::DmaAlloc)?;
        let rxpool = unsafe { Dma::new(POOL_DMA_SIZE, rw_map, RXPOOL_DMA_USER_VA, prot) }
            .map_err(|_| PciOpenError::DmaAlloc)?;

        prewarm(ring.base_va() as u64, RING_DMA_SIZE);
        prewarm(txpool.base_va() as u64, POOL_DMA_SIZE);
        prewarm(rxpool.base_va() as u64, POOL_DMA_SIZE);

        let mut mac = [0u8; 6];
        let mut rx_notify_va = 0u64;
        let mut tx_notify_va = 0u64;
        let negotiated_lo = init_device(
            common_va,
            notify_base,
            notify_mul,
            u64::from(notify_len),
            device_cfg_va,
            ring.base_va() as u64,
            ring.paddr(),
            txpool.paddr(),
            rxpool.paddr(),
            &mut mac,
            &mut rx_notify_va,
            &mut tx_notify_va,
        )?;

        Ok(Self {
            _pci: pci,
            _irq: irq,
            ring,
            txpool,
            rxpool,
            common_va,
            isr_va,
            device_cfg_va,
            rx_notify_va,
            tx_notify_va,
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
        crate::virtio::MTU
    }

    /// Link state. True if STATUS was not negotiated or the device-config link
    /// bit is set.
    pub fn link_up(&self) -> bool {
        if self.negotiated_lo & VIRTIO_NET_F_STATUS_BIT == 0 {
            return true;
        }
        match self.device_cfg_va {
            Some(va) => (unsafe { r16(va + NET_CFG_STATUS) } & VIRTIO_NET_S_LINK_UP) != 0,
            None => true,
        }
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

        unsafe {
            for i in 0..VIRTIO_NET_HDR_LEN as u64 {
                w8(buf_va + i, 0);
            }
            for (i, &b) in frame.iter().enumerate() {
                w8(buf_va + VIRTIO_NET_HDR_LEN as u64 + i as u64, b);
            }
            let desc = ring_va + TX_DESC_OFF + (slot as u64) * 16;
            w32(desc + 8, (VIRTIO_NET_HDR_LEN + frame.len()) as u32);
        }

        let new_idx = self.tx.commit_post();
        unsafe {
            dsb_sy();
            w16(ring_va + TX_AVAIL_OFF + 2, new_idx);
            dsb_sy();
            w16(self.tx_notify_va, NET_QUEUE_TX);
        }
        true
    }

    /// Drain one received frame into `out` (non-blocking). Returns the frame
    /// byte count (device frame minus the virtio_net_hdr, clamped to
    /// `out.len()`), or `None` if no frame is ready. The consumed RX descriptor
    /// is recycled before returning.
    pub fn poll_rx(&mut self, out: &mut [u8]) -> Option<usize> {
        let ring_va = self.ring.base_va() as u64;
        let rx_used_va = ring_va + RX_USED_OFF;

        let cur_used = unsafe { r16(rx_used_va + 2) };
        virtio_rmb();

        let used_slot = self.rx.take_used(cur_used)?;
        let entry = rx_used_va + 4 + (used_slot as u64) * 8;
        let desc_id = unsafe { r32(entry) };
        let raw_len = unsafe { r32(entry + 4) };

        // desc_id is device-controlled: reject out-of-range before it scales the
        // RX-pool base (the critical OOB guard). A bogus id is dropped without
        // recycling -- never republished as a fabricated descriptor (audit F1
        // from net-1). Trusted QEMU never emits this.
        if desc_id >= QUEUE_SIZE as u32 {
            return None;
        }
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
            w16(self.rx_notify_va, NET_QUEUE_RX);
        }
    }

    /// Reclaim completed TX descriptors (frees back-pressure for `send`).
    pub fn drain_tx(&mut self) {
        let ring_va = self.ring.base_va() as u64;
        let cur_used = unsafe { r16(ring_va + TX_USED_OFF + 2) };
        virtio_rmb();
        self.tx.reap(cur_used, QUEUE_SIZE);
    }

    /// Block until the device raises an interrupt; report whether it signalled
    /// virtqueue progress (vs a config-change-only wake). Reading the ISR byte
    /// clears it -- no separate ACK register on the PCI transport.
    pub fn wait_irq(&self) -> bool {
        let _ = self._irq.wait();
        let isr = unsafe { r8(self.isr_va) };
        isr & ISR_QUEUE != 0
    }

    /// Stop the device: a full reset (`device_status = 0`). After this the
    /// device performs no further DMA, so its buffers are safe to free.
    fn quiesce(&self) {
        unsafe {
            w8(self.common_va + CCFG_DEVICE_STATUS, 0);
            dsb_sy();
        }
    }
}

impl Drop for VirtioNetPci {
    fn drop(&mut self) {
        // Reset BEFORE the Dma/PciDev fields drop, so the device stops DMA
        // before any page release (RW-7 R3-F1).
        self.quiesce();
    }
}

// =============================================================================
// Init helpers (operate on the mapped region/ring VAs before the struct exists).
// =============================================================================

fn prewarm(va: u64, size: usize) {
    let mut off = 0u64;
    while (off as usize) < size {
        unsafe { w8(va + off, 0) };
        off += PAGE_SIZE;
    }
}

/// Configure one queue over common cfg: select it, bound-check + set its size,
/// install the ring PAs, park its MSI-X vector (INTx), read its notify offset,
/// then enable it. Returns the queue's `queue_notify_off` (the doorbell index),
/// or `None` if the device's max queue size is below `QUEUE_SIZE`.
fn setup_queue(common: u64, queue: u16, desc_pa: u64, avail_pa: u64, used_pa: u64) -> Option<u16> {
    unsafe {
        w16(common + CCFG_QUEUE_SELECT, queue);
        if r16(common + CCFG_QUEUE_SIZE) < QUEUE_SIZE {
            return None;
        }
        w16(common + CCFG_QUEUE_SIZE, QUEUE_SIZE);
        w64(common + CCFG_QUEUE_DESC, desc_pa);
        w64(common + CCFG_QUEUE_DRIVER, avail_pa);
        w64(common + CCFG_QUEUE_DEVICE, used_pa);
        w16(common + CCFG_QUEUE_MSIX_VECTOR, VIRTIO_MSI_NO_VECTOR);
        let notify_off = r16(common + CCFG_QUEUE_NOTIFY_OFF);
        w16(common + CCFG_QUEUE_ENABLE, 1);
        Some(notify_off)
    }
}

/// Pre-post all 16 RX descriptors (device-writable, one buffer each) and
/// announce them. Identical to the mmio path.
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
        w16(avail, 0);
        for k in 0..QUEUE_SIZE {
            w16(avail + 4 + (k as u64) * 2, k);
        }
        dsb_sy();
        w16(avail + 2, QUEUE_SIZE);
        dsb_sy();
    }
}

/// Pre-init the 16 TX descriptors (addr static, len rewritten per send) + the
/// TX avail ring. Identical to the mmio path.
fn populate_tx(ring_va: u64, txpool_pa: u64) {
    unsafe {
        for k in 0..QUEUE_SIZE as u64 {
            let d = ring_va + TX_DESC_OFF + k * 16;
            w64(d, txpool_pa + k * BUF_LEN as u64);
            w32(d + 8, 0);
            w16(d + 12, 0);
            w16(d + 14, 0);
        }
        let avail = ring_va + TX_AVAIL_OFF;
        w16(avail, 0);
        for k in 0..QUEUE_SIZE {
            w16(avail + 4 + (k as u64) * 2, k);
        }
        dsb_sy();
        w16(avail + 2, 0);
        dsb_sy();
    }
}

#[allow(clippy::too_many_arguments)]
fn init_device(
    common: u64,
    notify_base: u64,
    notify_mul: u64,
    notify_len: u64,
    device_cfg: Option<u64>,
    ring_va: u64,
    ring_pa: u64,
    txpool_pa: u64,
    rxpool_pa: u64,
    mac: &mut [u8; 6],
    rx_notify_va: &mut u64,
    tx_notify_va: &mut u64,
) -> Result<u32, PciOpenError> {
    unsafe {
        // Reset, then ACKNOWLEDGE + DRIVER.
        w8(common + CCFG_DEVICE_STATUS, 0);
        w8(common + CCFG_DEVICE_STATUS, STATUS_ACKNOWLEDGE);
        w8(common + CCFG_DEVICE_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER);
        // Park the device-config MSI-X vector (INTx).
        w16(common + CCFG_CONFIG_MSIX_VECTOR, VIRTIO_MSI_NO_VECTOR);

        // Feature negotiation: low dword (net features), then high (VERSION_1).
        w32(common + CCFG_DEVICE_FEATURE_SELECT, 0);
        let dev_feat_lo = r32(common + CCFG_DEVICE_FEATURE);
        w32(common + CCFG_DEVICE_FEATURE_SELECT, 1);
        let dev_feat_hi = r32(common + CCFG_DEVICE_FEATURE);

        if dev_feat_hi & VIRTIO_F_VERSION_1_BIT_HI == 0 {
            w8(common + CCFG_DEVICE_STATUS, STATUS_FAILED);
            return Err(PciOpenError::NoVersion1);
        }

        let want_lo = dev_feat_lo & (VIRTIO_NET_F_MAC_BIT | VIRTIO_NET_F_STATUS_BIT);
        w32(common + CCFG_DRIVER_FEATURE_SELECT, 0);
        w32(common + CCFG_DRIVER_FEATURE, want_lo);
        w32(common + CCFG_DRIVER_FEATURE_SELECT, 1);
        w32(common + CCFG_DRIVER_FEATURE, VIRTIO_F_VERSION_1_BIT_HI);

        w8(common + CCFG_DEVICE_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK);
        if r8(common + CCFG_DEVICE_STATUS) & STATUS_FEATURES_OK == 0 {
            w8(common + CCFG_DEVICE_STATUS, STATUS_FAILED);
            return Err(PciOpenError::FeaturesRejected);
        }

        // Configure both queues; capture each queue's notify doorbell VA.
        let rx_off = match setup_queue(
            common,
            NET_QUEUE_RX,
            ring_pa + RX_DESC_OFF,
            ring_pa + RX_AVAIL_OFF,
            ring_pa + RX_USED_OFF,
        ) {
            Some(o) => o,
            None => {
                w8(common + CCFG_DEVICE_STATUS, STATUS_FAILED);
                return Err(PciOpenError::QueueTooSmall);
            }
        };
        let tx_off = match setup_queue(
            common,
            NET_QUEUE_TX,
            ring_pa + TX_DESC_OFF,
            ring_pa + TX_AVAIL_OFF,
            ring_pa + TX_USED_OFF,
        ) {
            Some(o) => o,
            None => {
                w8(common + CCFG_DEVICE_STATUS, STATUS_FAILED);
                return Err(PciOpenError::QueueTooSmall);
            }
        };
        // Bound each device-supplied doorbell offset within the notify region
        // (pci-3 F2): a hostile queue_notify_off * multiplier must not place the
        // 2-byte doorbell write past the region. Same posture as the CCFG_MIN_LEN
        // / DEVICE_CFG_MIN_LEN region guards; off*mul is u16*u32 -> no u64 wrap.
        for off in [rx_off, tx_off] {
            if u64::from(off) * notify_mul + 2 > notify_len {
                w8(common + CCFG_DEVICE_STATUS, STATUS_FAILED);
                return Err(PciOpenError::NotifyRegionTooSmall);
            }
        }
        *rx_notify_va = notify_base + u64::from(rx_off) * notify_mul;
        *tx_notify_va = notify_base + u64::from(tx_off) * notify_mul;

        if want_lo & VIRTIO_NET_F_MAC_BIT != 0 {
            match device_cfg {
                Some(cfg) => {
                    for (i, b) in mac.iter_mut().enumerate() {
                        *b = r8(cfg + NET_CFG_MAC + i as u64);
                    }
                }
                None => *mac = [0x52, 0x54, 0x00, 0x12, 0x34, 0x56],
            }
        } else {
            *mac = [0x52, 0x54, 0x00, 0x12, 0x34, 0x56];
        }

        populate_rx(ring_va, rxpool_pa);
        populate_tx(ring_va, txpool_pa);

        w8(
            common + CCFG_DEVICE_STATUS,
            STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK | STATUS_DRIVER_OK,
        );
        Ok(want_lo)
    }
}
