// The virtio-input (keyboard) device half of tapestryd (Tapestry G-3).
//
// Transport: virtio-keyboard-PCI -- the SAME co-page rule that moved the GPU
// off the MMIO bank at G-1 applies verbatim to input (virtio-input's MMIO
// slot shares the one page-exclusive 4-KiB slot page with stratumd's disk),
// so the RESIDENT input claim rides its own PCI function; the MMIO keyboard
// stays wired for the one-shot P4-K kernel-test probe (the gpu0/gpu-mmio0
// split, applied to kbd). The transport machinery mirrors gpu.rs (netdev
// VirtioNetPci lineage); the eventq layout + drain + recycle discipline are
// the audited P4-K-events probe's, verbatim.
//
// POLL-MODE, deliberately IRQ-free: tapestryd's single-threaded serve loop
// blocks in t_poll on its 9P conns (the netd shape) and cannot also block in
// SYS_IRQ_WAIT; instead the eventq is drained on every loop tick (<= the
// FRAME period, so worst-case input latency is one frame -- fine for stage
// 0). The avail ring carries VIRTQ_AVAIL_F_NO_INTERRUPT so the device
// suppresses eventq interrupts entirely; no IRQ is claimed for the function.
// (A stray assertion would also be harmless: nothing waits on the line, and
// the GPU's own INTx is a different function/pin.)

use libdriver::Error;
use libthyla_rs::handle::Rights;
use libthyla_rs::hardware::{
    mmio_read16, mmio_read32, mmio_read8, mmio_write16, mmio_write32, mmio_write64, mmio_write8,
    Dma, PciDev, PciRegion,
};
use libthyla_rs::virtio_rmb;
use libthyla_rs::{T_PROT_READ, T_PROT_WRITE};

use crate::gpu::{dsb_sy, prewarm, PAGE_SIZE};

/// The VirtIO device id for an input device (VIRTIO 1.2 section 5.8).
pub const VIRTIO_DEVICE_ID_INPUT: u32 = 18;

const STATUS_ACKNOWLEDGE: u8 = 1;
const STATUS_DRIVER: u8 = 2;
const STATUS_DRIVER_OK: u8 = 4;
const STATUS_FEATURES_OK: u8 = 8;
const STATUS_FAILED: u8 = 128;

const VIRTIO_F_VERSION_1_BIT_HI: u32 = 1 << 0;
const VIRTIO_MSI_NO_VECTOR: u16 = 0xFFFF;

const CCFG_DEVICE_FEATURE_SELECT: u64 = 0x00;
const CCFG_DEVICE_FEATURE: u64 = 0x04;
const CCFG_DRIVER_FEATURE_SELECT: u64 = 0x08;
const CCFG_DRIVER_FEATURE: u64 = 0x0C;
const CCFG_CONFIG_MSIX_VECTOR: u64 = 0x10;
const CCFG_DEVICE_STATUS: u64 = 0x14;
const CCFG_QUEUE_SELECT: u64 = 0x16;
const CCFG_QUEUE_SIZE: u64 = 0x18;
const CCFG_QUEUE_MSIX_VECTOR: u64 = 0x1A;
const CCFG_QUEUE_ENABLE: u64 = 0x1C;
const CCFG_QUEUE_DESC: u64 = 0x20;
const CCFG_QUEUE_DRIVER: u64 = 0x28;
const CCFG_QUEUE_DEVICE: u64 = 0x30;
const CCFG_MIN_LEN: u32 = 0x38;

// virtio-input specifics (VIRTIO 1.2 section 5.8).
const INPUT_QUEUE_EVENT: u16 = 0;
const VIRTIO_INPUT_EVENT_LEN: u32 = 8;

// Event types (linux/input-event-codes.h, mirrored by VIRTIO 5.8.6.2).
#[allow(dead_code)] // wire vocabulary; the drain passes SYN records through
pub const EV_SYN: u16 = 0; // and the consumer filters on EV_KEY
pub const EV_KEY: u16 = 1;

// Eventq DMA layout (single page; the P4-K probe layout, verbatim).
const QUEUE_SIZE: u16 = 16;
pub const INPUT_DMA_SIZE: usize = PAGE_SIZE as usize;

const DESC_OFF: u64 = 0x000;
const AVAIL_OFF: u64 = 0x100;
const USED_OFF: u64 = 0x200;
const EVENT_POOL_OFF: u64 = 0x300;

const VIRTQ_DESC_F_WRITE: u16 = 2;
const VIRTQ_AVAIL_F_NO_INTERRUPT: u16 = 1;

const _: () = {
    assert!(EVENT_POOL_OFF + (QUEUE_SIZE as u64) * (VIRTIO_INPUT_EVENT_LEN as u64) <= INPUT_DMA_SIZE as u64);
};

/// A raw virtio_input_event record.
#[derive(Clone, Copy)]
pub struct RawInputEvent {
    pub etype: u16,
    pub code: u16,
    pub value: u32,
}

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

fn populate_eventq(dma_va: u64, dma_pa: u64) {
    let avail_va = dma_va + AVAIL_OFF;
    let event_pool_pa = dma_pa + EVENT_POOL_OFF;

    for i in 0..(QUEUE_SIZE as u64) * (VIRTIO_INPUT_EVENT_LEN as u64) {
        unsafe { w8(dma_va + EVENT_POOL_OFF + i, 0) };
    }

    for k in 0..QUEUE_SIZE {
        let d_off = DESC_OFF + (k as u64) * 16;
        let buf_pa = event_pool_pa + (k as u64) * (VIRTIO_INPUT_EVENT_LEN as u64);
        unsafe {
            w64(dma_va + d_off + 0, buf_pa);
            w32(dma_va + d_off + 8, VIRTIO_INPUT_EVENT_LEN);
            w16(dma_va + d_off + 12, VIRTQ_DESC_F_WRITE);
            w16(dma_va + d_off + 14, 0); // no chaining
        };
        unsafe { w16(avail_va + 4 + (k as u64) * 2, k) };
    }

    // Poll-mode: suppress eventq used-buffer interrupts (VIRTIO 1.2
    // 2.7.7.1); the serve loop drains on its own cadence.
    unsafe { w16(avail_va + 0, VIRTQ_AVAIL_F_NO_INTERRUPT) };
    dsb_sy();
    unsafe { w16(avail_va + 2, QUEUE_SIZE) };
    dsb_sy();
}

/// The claimed keyboard function: PCI transport + the poll-mode eventq.
pub struct Keyboard {
    dma_va: u64,
    last_used_idx: u16,
    avail_idx: u16,
    _pci: PciDev,
    _dma: Dma,
}

impl Keyboard {
    /// Claim + bring up the virtio-input PCI function (eventq only; the
    /// statusq -- LED writeback -- is deliberately unconfigured at stage 0).
    pub fn probe(bar_window_va: u64, dma_va: u64) -> Result<Keyboard, Error> {
        let pci = unsafe { PciDev::claim(VIRTIO_DEVICE_ID_INPUT, bar_window_va) }.map_err(|e| {
            say!("tapestryd: kbd PCI claim/map failed {:?}", e);
            Error::Hardware
        })?;

        let (common, common_len) = pci.region(PciRegion::Common).ok_or_else(|| {
            say!("tapestryd: kbd no common-cfg region");
            Error::Hardware
        })?;
        if common_len < CCFG_MIN_LEN {
            say!("tapestryd: kbd common-cfg region too small ({})", common_len);
            return Err(Error::Hardware);
        }

        let rw_map = Rights::READ | Rights::WRITE | Rights::MAP;
        let prot = T_PROT_READ | T_PROT_WRITE;
        let dma = unsafe { Dma::new(INPUT_DMA_SIZE, rw_map, dma_va, prot) }.map_err(|_| {
            say!("tapestryd: SYS_DMA_CREATE(kbd eventq) failed");
            Error::Hardware
        })?;
        prewarm(dma.base_va() as u64, INPUT_DMA_SIZE);
        let dma_pa = dma.paddr();

        unsafe {
            w8(common + CCFG_DEVICE_STATUS, 0);
            w8(common + CCFG_DEVICE_STATUS, STATUS_ACKNOWLEDGE);
            w8(common + CCFG_DEVICE_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER);
            w16(common + CCFG_CONFIG_MSIX_VECTOR, VIRTIO_MSI_NO_VECTOR);

            w32(common + CCFG_DEVICE_FEATURE_SELECT, 0);
            let _lo = r32(common + CCFG_DEVICE_FEATURE);
            w32(common + CCFG_DEVICE_FEATURE_SELECT, 1);
            let hi = r32(common + CCFG_DEVICE_FEATURE);
            if hi & VIRTIO_F_VERSION_1_BIT_HI == 0 {
                say!("tapestryd: kbd lacks VIRTIO_F_VERSION_1");
                w8(common + CCFG_DEVICE_STATUS, STATUS_FAILED);
                return Err(Error::Hardware);
            }

            w32(common + CCFG_DRIVER_FEATURE_SELECT, 0);
            w32(common + CCFG_DRIVER_FEATURE, 0);
            w32(common + CCFG_DRIVER_FEATURE_SELECT, 1);
            w32(common + CCFG_DRIVER_FEATURE, VIRTIO_F_VERSION_1_BIT_HI);

            w8(
                common + CCFG_DEVICE_STATUS,
                STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK,
            );
            if r8(common + CCFG_DEVICE_STATUS) & STATUS_FEATURES_OK == 0 {
                say!("tapestryd: kbd FEATURES_OK rejected");
                w8(common + CCFG_DEVICE_STATUS, STATUS_FAILED);
                return Err(Error::Hardware);
            }

            // eventq (queue 0) only.
            w16(common + CCFG_QUEUE_SELECT, INPUT_QUEUE_EVENT);
            if r16(common + CCFG_QUEUE_SIZE) < QUEUE_SIZE {
                say!("tapestryd: kbd eventq size below QUEUE_SIZE");
                w8(common + CCFG_DEVICE_STATUS, STATUS_FAILED);
                return Err(Error::Hardware);
            }
            w16(common + CCFG_QUEUE_SIZE, QUEUE_SIZE);
            w64(common + CCFG_QUEUE_DESC, dma_pa + DESC_OFF);
            w64(common + CCFG_QUEUE_DRIVER, dma_pa + AVAIL_OFF);
            w64(common + CCFG_QUEUE_DEVICE, dma_pa + USED_OFF);
            w16(common + CCFG_QUEUE_MSIX_VECTOR, VIRTIO_MSI_NO_VECTOR);
            // No notify doorbell: the eventq is device-filled (RX-only) and
            // virtio-input has no notify-on-RX semantic; poll-mode skips it.

            populate_eventq(dma_va, dma_pa);
            w16(common + CCFG_QUEUE_ENABLE, 1);

            w8(
                common + CCFG_DEVICE_STATUS,
                STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK | STATUS_DRIVER_OK,
            );
        }

        say!("tapestryd: kbd up (poll-mode eventq)");
        Ok(Keyboard {
            dma_va,
            last_used_idx: 0,
            avail_idx: QUEUE_SIZE,
            _pci: pci,
            _dma: dma,
        })
    }

    /// Drain every pending eventq record, invoking `f` per record; recycle
    /// each descriptor back to the device. Bounded by the ring size per call
    /// (a device cannot outrun the recycle within one drain pass).
    pub fn drain(&mut self, mut f: impl FnMut(RawInputEvent)) {
        let used_va = self.dma_va + USED_OFF;
        let avail_va = self.dma_va + AVAIL_OFF;
        let pool_va = self.dma_va + EVENT_POOL_OFF;

        let cur_used = unsafe { r16(used_va + 2) };
        if cur_used == self.last_used_idx {
            return;
        }
        // VIRTIO 1.2 2.7.13.2: order the used.idx observation before the
        // used.ring / event-pool reads (the P4-K phantom-EV_SYN lesson).
        virtio_rmb();

        let mut idx = self.last_used_idx;
        let entry_avail = self.avail_idx;
        let mut consumed = 0u16;
        while idx != cur_used && consumed < QUEUE_SIZE {
            let slot = (idx % QUEUE_SIZE) as u64;
            let elem_va = used_va + 4 + slot * 8;
            let desc_id = unsafe { r32(elem_va + 0) };
            let used_len = unsafe { r32(elem_va + 4) };

            if desc_id < QUEUE_SIZE as u32 && used_len == VIRTIO_INPUT_EVENT_LEN {
                let evt_va = pool_va + (desc_id as u64) * (VIRTIO_INPUT_EVENT_LEN as u64);
                let ev = RawInputEvent {
                    etype: unsafe { r16(evt_va + 0) },
                    code: unsafe { r16(evt_va + 2) },
                    value: unsafe { r32(evt_va + 4) },
                };
                f(ev);

                // Recycle: re-publish this descriptor to the avail ring.
                let avail_slot = (self.avail_idx % QUEUE_SIZE) as u64;
                unsafe { w16(avail_va + 4 + avail_slot * 2, desc_id as u16) };
                self.avail_idx = self.avail_idx.wrapping_add(1);
            } else {
                say!("tapestryd: kbd malformed used elem id={} len={}", desc_id, used_len);
            }
            idx = idx.wrapping_add(1);
            consumed += 1;
        }
        self.last_used_idx = idx;

        if self.avail_idx != entry_avail {
            // Publications visible before the idx bump (VIRTIO 1.2 2.7.13.1).
            dsb_sy();
            unsafe { w16(avail_va + 2, self.avail_idx) };
            dsb_sy();
            // No notify: virtio-input's eventq has no notify-on-RX semantic.
        }
    }
}
