// The virtio-gpu device half of tapestryd (Tapestry G-3; TAPESTRY.md
// section 18). A port of the audited G-1 gpud device machinery (itself the
// P4-L probe's command layer over the netdev VirtioNetPci PCI transport),
// generalized from the one-baked-framebuffer shape to the compositor's
// needs: per-surface 2D resources, whole-weave ATTACH_BACKING, per-present
// offset TRANSFER_TO_HOST_2D, and the retire pair
// (DETACH_BACKING + RESOURCE_UNREF) the one-shot lineage never needed.
//
// Every command is synchronous: submit the 2-descriptor chain, kick the
// doorbell, wait the INTx IRQ, verify used.idx + the response type. That
// synchrony is load-bearing for the stage-0 I-40 present half: a present's
// TRANSFER window opens and closes INSIDE one server dispatch, so the
// in-flight set is empty at every retire decision point (the
// tapestry_present.tla quiesce obligation holds by construction; the
// pipelined controlq is the G-6+ lift and must implement the real drain).

use libdriver::Error;
use libthyla_rs::handle::Rights;
use libthyla_rs::hardware::{
    mmio_read16, mmio_read32, mmio_read8, mmio_write16, mmio_write32, mmio_write64, mmio_write8,
    Dma, Irq, PciDev, PciRegion,
};
use libthyla_rs::virtio_rmb;
use libthyla_rs::{T_PROT_READ, T_PROT_WRITE};

pub const PAGE_SIZE: u64 = 0x1000;

/// The VirtIO device id for a GPU (VIRTIO 1.2 section 5.7).
pub const VIRTIO_DEVICE_ID_GPU: u32 = 16;

// device_status bits (a u8 in common cfg).
const STATUS_ACKNOWLEDGE: u8 = 1;
const STATUS_DRIVER: u8 = 2;
const STATUS_DRIVER_OK: u8 = 4;
const STATUS_FEATURES_OK: u8 = 8;
const STATUS_FAILED: u8 = 128;

// VIRTIO_F_VERSION_1 is feature bit 32 -- bit 0 of the HIGH feature dword.
const VIRTIO_F_VERSION_1_BIT_HI: u32 = 1 << 0;

// ISR status (section 4.1.4.5): bit 0 = a virtqueue raised the IRQ; reading
// the byte clears it.
const ISR_QUEUE: u8 = 1 << 0;

const VIRTIO_MSI_NO_VECTOR: u16 = 0xFFFF;

// virtio_pci_common_cfg field offsets (section 4.1.4.3).
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
const CCFG_QUEUE_NOTIFY_OFF: u64 = 0x1E;
const CCFG_QUEUE_DESC: u64 = 0x20;
const CCFG_QUEUE_DRIVER: u64 = 0x28;
const CCFG_QUEUE_DEVICE: u64 = 0x30;
const CCFG_MIN_LEN: u32 = 0x38;

// virtio-gpu protocol constants (VIRTIO 1.2 section 5.7).
const GPU_QUEUE_CONTROL: u16 = 0;
const GPU_QUEUE_CURSOR: u16 = 1;
pub const GPU_CTRL_HDR_LEN: u32 = 24;

const VIRTIO_GPU_CMD_GET_DISPLAY_INFO: u32 = 0x0100;
const VIRTIO_GPU_CMD_RESOURCE_CREATE_2D: u32 = 0x0101;
const VIRTIO_GPU_CMD_RESOURCE_UNREF: u32 = 0x0102;
const VIRTIO_GPU_CMD_SET_SCANOUT: u32 = 0x0103;
const VIRTIO_GPU_CMD_RESOURCE_FLUSH: u32 = 0x0104;
const VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D: u32 = 0x0105;
const VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING: u32 = 0x0106;
const VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING: u32 = 0x0107;

const VIRTIO_GPU_RESP_OK_NODATA: u32 = 0x1100;
const VIRTIO_GPU_RESP_OK_DISPLAY_INFO: u32 = 0x1101;

pub const VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM: u32 = 1;

const GPU_MAX_SCANOUTS: u32 = 16;
const GPU_DISPLAY_ONE_LEN: u32 = 24;
const GPU_RESP_DISPLAY_INFO_LEN: u32 = GPU_CTRL_HDR_LEN + GPU_MAX_SCANOUTS * GPU_DISPLAY_ONE_LEN;

const VIRTQ_DESC_F_NEXT: u16 = 1;
const VIRTQ_DESC_F_WRITE: u16 = 2;

const MAX_NON_USED_BUFFER_WAKES: u32 = 16;

/// The QEMU-virt virtio-gpu default scanout geometry, used when
/// GET_DISPLAY_INFO reports no enabled scanout (never observed on QEMU;
/// fail-soft rather than fail-closed -- the display size is policy, not
/// soundness).
const DEFAULT_DISPLAY_W: u32 = 1024;
const DEFAULT_DISPLAY_H: u32 = 768;
/// A sanity clamp on the device-reported geometry (an absurd size would
/// size every fullscreen weave allocation).
const MAX_DISPLAY_DIM: u32 = 8192;

// Single-page DMA ring layout (the gpud/probe audited layout, verbatim).
const QUEUE_SIZE: u16 = 16;
pub const RING_DMA_SIZE: usize = PAGE_SIZE as usize;

const CTRL_DESC_OFF: u64 = 0x000;
const CTRL_AVAIL_OFF: u64 = 0x100;
const CTRL_USED_OFF: u64 = 0x200;
const CURSOR_DESC_OFF: u64 = 0x300;
const CURSOR_AVAIL_OFF: u64 = 0x400;
const CURSOR_USED_OFF: u64 = 0x500;
const REQ_OFF: u64 = 0x600;
const RESP_OFF: u64 = 0x700;

const REQ_REGION_LEN: u32 = 0x100;
const RESP_REGION_LEN: u32 = 0x300;

const _: () = {
    assert!(CTRL_DESC_OFF + (QUEUE_SIZE as u64) * 16 <= CTRL_AVAIL_OFF);
    assert!(CTRL_AVAIL_OFF + 4 + (QUEUE_SIZE as u64) * 2 <= CTRL_USED_OFF);
    assert!(CTRL_USED_OFF + 4 + (QUEUE_SIZE as u64) * 8 <= CURSOR_DESC_OFF);
    assert!(REQ_OFF + (REQ_REGION_LEN as u64) <= RESP_OFF);
    assert!(RESP_OFF + (RESP_REGION_LEN as u64) <= RING_DMA_SIZE as u64);
    assert!(GPU_RESP_DISPLAY_INFO_LEN <= RESP_REGION_LEN);
};

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
pub fn dsb_sy() {
    unsafe { core::arch::asm!("dsb sy", options(nostack, preserves_flags)) }
}

pub fn prewarm(va: u64, size: usize) {
    let mut off = 0u64;
    while (off as usize) < size {
        unsafe { w8(va + off, 0) };
        off += PAGE_SIZE;
    }
}

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

fn init_device(
    common: u64,
    notify_base: u64,
    notify_mul: u64,
    notify_len: u64,
    ring_pa: u64,
) -> Result<u64, Error> {
    unsafe {
        w8(common + CCFG_DEVICE_STATUS, 0);
        w8(common + CCFG_DEVICE_STATUS, STATUS_ACKNOWLEDGE);
        w8(common + CCFG_DEVICE_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER);
        w16(common + CCFG_CONFIG_MSIX_VECTOR, VIRTIO_MSI_NO_VECTOR);

        w32(common + CCFG_DEVICE_FEATURE_SELECT, 0);
        let _dev_feat_lo = r32(common + CCFG_DEVICE_FEATURE);
        w32(common + CCFG_DEVICE_FEATURE_SELECT, 1);
        let dev_feat_hi = r32(common + CCFG_DEVICE_FEATURE);
        if dev_feat_hi & VIRTIO_F_VERSION_1_BIT_HI == 0 {
            say!("tapestryd: gpu lacks VIRTIO_F_VERSION_1");
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
            say!("tapestryd: gpu FEATURES_OK rejected");
            w8(common + CCFG_DEVICE_STATUS, STATUS_FAILED);
            return Err(Error::Hardware);
        }

        let ctrl_off = match setup_queue(
            common,
            GPU_QUEUE_CONTROL,
            ring_pa + CTRL_DESC_OFF,
            ring_pa + CTRL_AVAIL_OFF,
            ring_pa + CTRL_USED_OFF,
        ) {
            Some(o) => o,
            None => {
                say!("tapestryd: gpu controlq size below QUEUE_SIZE");
                w8(common + CCFG_DEVICE_STATUS, STATUS_FAILED);
                return Err(Error::Hardware);
            }
        };
        let cursor_off = match setup_queue(
            common,
            GPU_QUEUE_CURSOR,
            ring_pa + CURSOR_DESC_OFF,
            ring_pa + CURSOR_AVAIL_OFF,
            ring_pa + CURSOR_USED_OFF,
        ) {
            Some(o) => o,
            None => {
                say!("tapestryd: gpu cursorq size below QUEUE_SIZE");
                w8(common + CCFG_DEVICE_STATUS, STATUS_FAILED);
                return Err(Error::Hardware);
            }
        };

        // Bound each device-supplied doorbell offset within the notify region
        // (the pci-3 F2 guard).
        for off in [ctrl_off, cursor_off] {
            if u64::from(off) * notify_mul + 2 > notify_len {
                say!("tapestryd: gpu notify doorbell past the notify region");
                w8(common + CCFG_DEVICE_STATUS, STATUS_FAILED);
                return Err(Error::Hardware);
            }
        }

        w8(
            common + CCFG_DEVICE_STATUS,
            STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK | STATUS_DRIVER_OK,
        );
        Ok(notify_base + u64::from(ctrl_off) * notify_mul)
    }
}

struct Controlq {
    ring_va: u64,
    ring_pa: u64,
    notify_va: u64,
    isr_va: u64,
    irq: Irq,
    seq: u16,
}

impl Controlq {
    fn submit_and_wait(&mut self, req_len: u32, resp_len: u32) -> Result<u32, ()> {
        let next_seq = self.seq.wrapping_add(1);

        for i in 0..(GPU_CTRL_HDR_LEN as u64) {
            unsafe { w8(self.ring_va + RESP_OFF + i, 0) };
        }

        let desc_va = self.ring_va + CTRL_DESC_OFF;
        unsafe {
            w64(desc_va + 0, self.ring_pa + REQ_OFF);
            w32(desc_va + 8, req_len);
            w16(desc_va + 12, VIRTQ_DESC_F_NEXT);
            w16(desc_va + 14, 1);

            w64(desc_va + 16, self.ring_pa + RESP_OFF);
            w32(desc_va + 24, resp_len);
            w16(desc_va + 28, VIRTQ_DESC_F_WRITE);
            w16(desc_va + 30, 0);
        };

        let avail_va = self.ring_va + CTRL_AVAIL_OFF;
        let ring_slot = (self.seq % QUEUE_SIZE) as u64;
        unsafe {
            w16(avail_va + 0, 0);
            w16(avail_va + 4 + ring_slot * 2, 0);
        };

        // VIRTIO 1.2 2.7.13.1: descriptor + ring writes visible before the
        // idx bump.
        dsb_sy();
        unsafe { w16(avail_va + 2, next_seq) };
        dsb_sy();

        unsafe { w16(self.notify_va, GPU_QUEUE_CONTROL) };

        let mut wakes = 0u32;
        loop {
            if wakes >= MAX_NON_USED_BUFFER_WAKES {
                say!("tapestryd: gpu too many non-queue IRQ wakes");
                return Err(());
            }
            if self.irq.wait().is_err() {
                say!("tapestryd: gpu SYS_IRQ_WAIT returned error");
                return Err(());
            }
            let isr = unsafe { r8(self.isr_va) };
            if isr & ISR_QUEUE != 0 {
                break;
            }
            wakes += 1;
        }

        let used_va = self.ring_va + CTRL_USED_OFF;
        let used_idx = unsafe { r16(used_va + 2) };
        // VIRTIO 1.2 2.7.13.2: order the used.idx observation before the
        // response-buffer read.
        virtio_rmb();
        if used_idx != next_seq {
            say!("tapestryd: gpu used.idx {} != expected {}", used_idx, next_seq);
            return Err(());
        }

        let resp_type = unsafe { r32(self.ring_va + RESP_OFF) };
        self.seq = next_seq;
        Ok(resp_type)
    }

    fn step(&mut self, label: &str, req_len: u32, resp_len: u32, expected: u32) -> Result<(), Error> {
        match self.submit_and_wait(req_len, resp_len) {
            Ok(t) if t == expected => Ok(()),
            Ok(t) => {
                say!("tapestryd: gpu {} resp_type={:#x} (expected {:#x})", label, t, expected);
                Err(Error::Hardware)
            }
            Err(()) => {
                say!("tapestryd: gpu {} submit error", label);
                Err(Error::Hardware)
            }
        }
    }
}

// Request builders (VIRTIO 1.2 section 5.7.6 layouts; probe-derived).

unsafe fn write_ctrl_hdr(req_va: u64, cmd_type: u32) {
    w32(req_va + 0, cmd_type);
    w32(req_va + 4, 0);
    w64(req_va + 8, 0);
    w32(req_va + 16, 0);
    w8(req_va + 20, 0);
    w8(req_va + 21, 0);
    w8(req_va + 22, 0);
    w8(req_va + 23, 0);
}

unsafe fn write_rect(va: u64, x: u32, y: u32, w: u32, h: u32) {
    w32(va + 0, x);
    w32(va + 4, y);
    w32(va + 8, w);
    w32(va + 12, h);
}

/// The GPU device: the claimed PCI function, the command ring, and the
/// display geometry. Owns the RAII handles for the Proc's lifetime
/// (persistent driver; the RW-7 quiesce at reap is the teardown).
pub struct Gpu {
    ctrl: Controlq,
    ring_va: u64,
    pub width: u32,
    pub height: u32,
    _pci: PciDev,
    _ring: Dma,
}

impl Gpu {
    /// Claim + bring up the virtio-gpu PCI function: transport handshake,
    /// both queues, then GET_DISPLAY_INFO for the scanout geometry.
    pub fn probe(bar_window_va: u64, ring_va: u64) -> Result<Gpu, Error> {
        let pci = unsafe { PciDev::claim(VIRTIO_DEVICE_ID_GPU, bar_window_va) }.map_err(|e| {
            say!("tapestryd: gpu PCI claim/map failed {:?}", e);
            Error::Hardware
        })?;

        let (common_va, common_len) = pci.region(PciRegion::Common).ok_or_else(|| {
            say!("tapestryd: gpu no common-cfg region");
            Error::Hardware
        })?;
        if common_len < CCFG_MIN_LEN {
            say!("tapestryd: gpu common-cfg region too small ({})", common_len);
            return Err(Error::Hardware);
        }
        let (notify_base, notify_len) = pci.region(PciRegion::Notify).ok_or_else(|| {
            say!("tapestryd: gpu no notify region");
            Error::Hardware
        })?;
        let isr_va = pci
            .region(PciRegion::Isr)
            .ok_or_else(|| {
                say!("tapestryd: gpu no ISR region");
                Error::Hardware
            })?
            .0;
        let notify_mul = u64::from(pci.notify_off_multiplier());
        let intid = pci.intid().ok_or_else(|| {
            say!("tapestryd: gpu no INTx INTID resolved");
            Error::Hardware
        })?;

        let irq = Irq::new(intid, Rights::SIGNAL).map_err(|_| {
            say!("tapestryd: SYS_IRQ_CREATE failed for gpu intid {}", intid);
            Error::Hardware
        })?;

        let rw_map = Rights::READ | Rights::WRITE | Rights::MAP;
        let prot = T_PROT_READ | T_PROT_WRITE;
        let ring = unsafe { Dma::new(RING_DMA_SIZE, rw_map, ring_va, prot) }.map_err(|_| {
            say!("tapestryd: SYS_DMA_CREATE(gpu ring) failed");
            Error::Hardware
        })?;
        prewarm(ring.base_va() as u64, RING_DMA_SIZE);
        let ring_pa = ring.paddr();

        let notify_va = init_device(common_va, notify_base, notify_mul, u64::from(notify_len), ring_pa)?;

        let mut gpu = Gpu {
            ctrl: Controlq {
                ring_va,
                ring_pa,
                notify_va,
                isr_va,
                irq,
                seq: 0,
            },
            ring_va,
            width: DEFAULT_DISPLAY_W,
            height: DEFAULT_DISPLAY_H,
            _pci: pci,
            _ring: ring,
        };

        gpu.read_display_info()?;
        say!("tapestryd: gpu up -- {}x{}, pci intid={}", gpu.width, gpu.height, intid);
        Ok(gpu)
    }

    /// GET_DISPLAY_INFO: adopt scanout 0's enabled rect as the display
    /// geometry (fail-soft to the default when absent or absurd).
    fn read_display_info(&mut self) -> Result<(), Error> {
        unsafe { write_ctrl_hdr(self.ring_va + REQ_OFF, VIRTIO_GPU_CMD_GET_DISPLAY_INFO) };
        self.ctrl.step(
            "GET_DISPLAY_INFO",
            GPU_CTRL_HDR_LEN,
            GPU_RESP_DISPLAY_INFO_LEN,
            VIRTIO_GPU_RESP_OK_DISPLAY_INFO,
        )?;
        // struct virtio_gpu_display_one { rect r; u32 enabled; u32 flags; }
        // starting right after the response header; scanout 0 first.
        let d0 = self.ring_va + RESP_OFF + GPU_CTRL_HDR_LEN as u64;
        let w = unsafe { r32(d0 + 8) };
        let h = unsafe { r32(d0 + 12) };
        let enabled = unsafe { r32(d0 + 16) };
        if enabled != 0 && (1..=MAX_DISPLAY_DIM).contains(&w) && (1..=MAX_DISPLAY_DIM).contains(&h) {
            self.width = w;
            self.height = h;
        } else {
            say!(
                "tapestryd: scanout0 absent/absurd (en={} {}x{}); default {}x{}",
                enabled,
                w,
                h,
                self.width,
                self.height
            );
        }
        Ok(())
    }

    pub fn resource_create_2d(&mut self, resource_id: u32, w: u32, h: u32) -> Result<(), Error> {
        let req_va = self.ring_va + REQ_OFF;
        unsafe {
            write_ctrl_hdr(req_va, VIRTIO_GPU_CMD_RESOURCE_CREATE_2D);
            w32(req_va + 24, resource_id);
            w32(req_va + 28, VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM);
            w32(req_va + 32, w);
            w32(req_va + 36, h);
        };
        self.ctrl
            .step("RESOURCE_CREATE_2D", GPU_CTRL_HDR_LEN + 16, GPU_CTRL_HDR_LEN, VIRTIO_GPU_RESP_OK_NODATA)
    }

    /// Attach the whole weave (one physically-contiguous KObj_DMA chunk) as
    /// the resource's guest backing. Per-present slot selection rides the
    /// TRANSFER offset, so one attach serves all slots.
    pub fn attach_backing(&mut self, resource_id: u32, pa: u64, len: u32) -> Result<(), Error> {
        let req_va = self.ring_va + REQ_OFF;
        unsafe {
            write_ctrl_hdr(req_va, VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING);
            w32(req_va + 24, resource_id);
            w32(req_va + 28, 1); // nr_entries
            w64(req_va + 32, pa);
            w32(req_va + 40, len);
            w32(req_va + 44, 0);
        };
        self.ctrl
            .step("ATTACH_BACKING", GPU_CTRL_HDR_LEN + 8 + 16, GPU_CTRL_HDR_LEN, VIRTIO_GPU_RESP_OK_NODATA)
    }

    pub fn detach_backing(&mut self, resource_id: u32) -> Result<(), Error> {
        let req_va = self.ring_va + REQ_OFF;
        unsafe {
            write_ctrl_hdr(req_va, VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING);
            w32(req_va + 24, resource_id);
            w32(req_va + 28, 0);
        };
        self.ctrl
            .step("DETACH_BACKING", GPU_CTRL_HDR_LEN + 8, GPU_CTRL_HDR_LEN, VIRTIO_GPU_RESP_OK_NODATA)
    }

    pub fn resource_unref(&mut self, resource_id: u32) -> Result<(), Error> {
        let req_va = self.ring_va + REQ_OFF;
        unsafe {
            write_ctrl_hdr(req_va, VIRTIO_GPU_CMD_RESOURCE_UNREF);
            w32(req_va + 24, resource_id);
            w32(req_va + 28, 0);
        };
        self.ctrl
            .step("RESOURCE_UNREF", GPU_CTRL_HDR_LEN + 8, GPU_CTRL_HDR_LEN, VIRTIO_GPU_RESP_OK_NODATA)
    }

    /// Bind scanout 0 to a resource (resource_id 0 = disable the scanout).
    pub fn set_scanout(&mut self, resource_id: u32, w: u32, h: u32) -> Result<(), Error> {
        let req_va = self.ring_va + REQ_OFF;
        unsafe {
            write_ctrl_hdr(req_va, VIRTIO_GPU_CMD_SET_SCANOUT);
            write_rect(req_va + 24, 0, 0, w, h);
            w32(req_va + 40, 0); // scanout_id
            w32(req_va + 44, resource_id);
        };
        self.ctrl
            .step("SET_SCANOUT", GPU_CTRL_HDR_LEN + 24, GPU_CTRL_HDR_LEN, VIRTIO_GPU_RESP_OK_NODATA)
    }

    /// TRANSFER_TO_HOST_2D: host-DMA-read the backing at `offset` into the
    /// resource rect. Rows advance by the RESOURCE stride (w*4), so `offset`
    /// = slot_base + (y*res_w + x)*4 selects both the slot and the rect
    /// origin within it.
    pub fn transfer(&mut self, resource_id: u32, offset: u64, x: u32, y: u32, w: u32, h: u32) -> Result<(), Error> {
        let req_va = self.ring_va + REQ_OFF;
        unsafe {
            write_ctrl_hdr(req_va, VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D);
            write_rect(req_va + 24, x, y, w, h);
            w64(req_va + 40, offset);
            w32(req_va + 48, resource_id);
            w32(req_va + 52, 0);
        };
        self.ctrl
            .step("TRANSFER", GPU_CTRL_HDR_LEN + 32, GPU_CTRL_HDR_LEN, VIRTIO_GPU_RESP_OK_NODATA)
    }

    pub fn flush(&mut self, resource_id: u32, x: u32, y: u32, w: u32, h: u32) -> Result<(), Error> {
        let req_va = self.ring_va + REQ_OFF;
        unsafe {
            write_ctrl_hdr(req_va, VIRTIO_GPU_CMD_RESOURCE_FLUSH);
            write_rect(req_va + 24, x, y, w, h);
            w32(req_va + 40, resource_id);
            w32(req_va + 44, 0);
        };
        self.ctrl
            .step("FLUSH", GPU_CTRL_HDR_LEN + 24, GPU_CTRL_HDR_LEN, VIRTIO_GPU_RESP_OK_NODATA)
    }
}
