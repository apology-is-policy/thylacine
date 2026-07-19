// /gpud -- the resident virtio-gpu driver (Tapestry build arc G-1;
// TAPESTRY.md section 18.9). The warden binds this binary to the
// `virtio-pci:16` node NARROWED (I-34: the function's bdf + its INTx
// INTID + a DMA pool, nothing else), `lifecycle = persistent` -- the
// netd precedent: probe brings the device up + presents the P4-L
// 4-quadrant pattern, serve signals READY and then runs the render loop
// forever, so the warden leaves it running.
//
// WHY RESIDENT (the G-0 finding): a scanout lives exactly as long as its
// driving Proc. At driver-Proc reap the RW-7 proc-death quiesce
// (kernel/virtio.c::virtio_mmio_reset_in_range) resets a dying driver's
// virtio devices -- required for DMA soundness -- and a virtio-gpu reset
// destroys the host-side resources and disables the scanout. The
// one-shot /virtio-gpu probe therefore leaves a blank display the
// instant it is reaped; THIS Proc holds the device (and the display) up
// for the life of the box. tools/screendump.sh -v captures + verifies
// the pattern against it at any point after READY.
//
// WHY PCI, not the virtio-mmio slot (the G-1 co-residency ground truth):
// QEMU-virt packs all six populated virtio-mmio slots into ONE 4-KiB
// page (0xa003000; slot stride 0x200), and a userspace MMIO claim is
// page-granular + exclusive (I-5). The boot survives on TEMPORAL
// sequencing -- each transient claimant (the one-shot probes, the
// netdev-driver ARP proof) releases the page before the next needs it,
// and stratumd then holds it for the box's life (its virtio-blk slots).
// A SECOND persistent claimant on that page is structurally impossible:
// a resident MMIO gpud starved netdev-driver AND stratumd's disk claim
// (boot-fatal), measured, not theorized. virtio-gpu-pci gives the GPU
// its own BAR pages -- the same move netd made for the NIC (#140) -- so
// the resident driver coexists with everyone. The virtio-mmio GPU
// device stays wired for the one-shot kernel-test probe (P4-L), whose
// scanout legitimately dies at its reap.
//
// WHAT IT DRAWS: the P4-L 4-quadrant test pattern (TL red / TR green /
// BL blue / BR white -- the screendump -v contract) with an 8x8 heartbeat
// block at the framebuffer center (rows/cols 60..68) toggling every tick.
// The heartbeat deliberately avoids the four -v quadrant sample points
// ((w/4,h/4) etc.), so -v stays deterministic while two dumps a tick
// apart differ -- the render-loop liveness witness. Each tick is a
// partial-rect TRANSFER_TO_HOST_2D + RESOURCE_FLUSH, IRQ-completed (the
// "IRQ-driven flush completion" of the G-1 charter).
//
// LINEAGE: the command machinery (single-page ring layout, 2-descriptor
// command/response chain, request builders) is the audited
// P4-L/P4-L-scanout probe's (usr/virtio-gpu); the PCI transport
// (PciDev::claim + common-cfg handshake + per-queue doorbell +
// read-to-clear ISR) mirrors the audited netdev VirtioNetPci (pci-2/3).
//
// HANDOFF: at G-3 tapestryd absorbs the GPU node into its own manifest
// (TAPESTRY.md section 18.7) and this scaffold retires; until then gpud
// is the stage-0 device-owning half -- it serves no namespace files
// (`serves` is decor; MAY_POST_SERVICE conferred but unused).
//
// Diagnostics go to the console (t_putstr); stdout is the warden's
// readiness pipe and carries EXACTLY the one READY line.

#![no_std]
#![no_main]

extern crate alloc;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use core::arch::asm;
use libdriver::driver::{run, Driver};
use libdriver::resource::BoundResources;
use libdriver::Error;
use libthyla_rs::handle::Rights;
use libthyla_rs::hardware::{
    mmio_read16, mmio_read32, mmio_read8, mmio_write16, mmio_write32, mmio_write64, mmio_write8,
    Dma, Irq, PciDev, PciRegion, PCI_BAR_VA_STRIDE,
};
use libthyla_rs::io::Write;
use libthyla_rs::time::{sleep, Duration};
use libthyla_rs::{virtio_rmb, T_PROT_READ, T_PROT_WRITE};

/// Console-direct diagnostics (T_SYS_PUTS) -- visible regardless of fd wiring;
/// stdout is reserved for the readiness line.
macro_rules! say {
    ($($a:tt)*) => {{
        let mut s = alloc::format!($($a)*);
        s.push('\n');
        let _ = libthyla_rs::t_putstr(&s);
    }};
}

// =============================================================================
// User-VA layout (driver-private): the BAR window + one ring page + the fb.
// =============================================================================

const PAGE_SIZE: u64 = 0x1000;
const BAR_WINDOW_VA: u64 = 0x0080_0000;
const RING_DMA_USER_VA: u64 = 0x0100_0000;
const FB_USER_VA: u64 = 0x0102_0000;

const _: () = {
    assert!(BAR_WINDOW_VA + 6 * PCI_BAR_VA_STRIDE <= RING_DMA_USER_VA);
    assert!(RING_DMA_USER_VA + (RING_DMA_SIZE as u64) <= FB_USER_VA);
};

// =============================================================================
// VirtIO-PCI modern transport (VIRTIO 1.2 section 4.1.4) -- the netdev
// VirtioNetPci constants, verbatim.
// =============================================================================

/// The VirtIO device id for a GPU (the kernel claims by virtio_device_id,
/// derived from the PCI device/subsystem id per VIRTIO 1.2 section 4.1.2).
const VIRTIO_DEVICE_ID_GPU: u32 = 16;

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

// MSI-X undriven (INTx only) -- park the vectors at NO_VECTOR.
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

// The driver reads through queue_device (0x30, le64) -- reject a smaller
// common region before any register access (the netdev region guard).
const CCFG_MIN_LEN: u32 = 0x38;

// =============================================================================
// virtio-gpu protocol constants (VIRTIO 1.2 section 5.7).
// =============================================================================

const GPU_QUEUE_CONTROL: u16 = 0;
const GPU_QUEUE_CURSOR: u16 = 1;
const GPU_CTRL_HDR_LEN: u32 = 24;

const VIRTIO_GPU_CMD_GET_DISPLAY_INFO: u32 = 0x0100;
const VIRTIO_GPU_CMD_RESOURCE_CREATE_2D: u32 = 0x0101;
const VIRTIO_GPU_CMD_SET_SCANOUT: u32 = 0x0103;
const VIRTIO_GPU_CMD_RESOURCE_FLUSH: u32 = 0x0104;
const VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D: u32 = 0x0105;
const VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING: u32 = 0x0106;

const VIRTIO_GPU_RESP_OK_NODATA: u32 = 0x1100;
const VIRTIO_GPU_RESP_OK_DISPLAY_INFO: u32 = 0x1101;

const VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM: u32 = 1;

const GPU_MAX_SCANOUTS: u32 = 16;
const GPU_DISPLAY_ONE_LEN: u32 = 24;
const GPU_RESP_DISPLAY_INFO_LEN: u32 =
    GPU_CTRL_HDR_LEN + GPU_MAX_SCANOUTS * GPU_DISPLAY_ONE_LEN; // 408

const VIRTQ_DESC_F_NEXT: u16 = 1;
const VIRTQ_DESC_F_WRITE: u16 = 2;

const MAX_NON_USED_BUFFER_WAKES: u32 = 16;

// =============================================================================
// Framebuffer + heartbeat geometry.
// =============================================================================

const FB_WIDTH: u32 = 128;
const FB_HEIGHT: u32 = 128;
const FB_BPP: u32 = 4;
const FB_SIZE: u64 = (FB_WIDTH as u64) * (FB_HEIGHT as u64) * (FB_BPP as u64);
const FB_RESOURCE_ID: u32 = 1;
const FB_SCANOUT_ID: u32 = 0;

// B8G8R8A8 little-endian u32 pixels.
const COLOR_RED: u32 = 0xFFFF_0000;
const COLOR_GREEN: u32 = 0xFF00_FF00;
const COLOR_BLUE: u32 = 0xFF00_00FF;
const COLOR_WHITE: u32 = 0xFFFF_FFFF;
const COLOR_BLACK: u32 = 0xFF00_0000;
const COLOR_YELLOW: u32 = 0xFFFF_FF00;

// The heartbeat block: 8x8 at the framebuffer center. Rows/cols 60..68
// stay clear of the -v quadrant sample points ((32,32)/(96,32)/(32,96)/
// (96,96)), so the -v assertion is tick-phase-independent.
const HB_X: u32 = 60;
const HB_Y: u32 = 60;
const HB_W: u32 = 8;
const HB_H: u32 = 8;
const _: () = {
    assert!(HB_X + HB_W <= 96 && HB_X >= 33); // clear of x=32 and x=96
    assert!(HB_Y + HB_H <= 96 && HB_Y >= 33); // clear of y=32 and y=96
};

/// Render-loop tick period. 2 Hz: cheap (4 controlq commands/s) yet fast
/// enough that two consecutive screendumps straddle a toggle.
const TICK_MS: u64 = 500;

// =============================================================================
// Single-page DMA ring layout (the probe's audited layout, verbatim).
// =============================================================================

const QUEUE_SIZE: u16 = 16;
const RING_DMA_SIZE: usize = PAGE_SIZE as usize;

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

// =============================================================================
// ISV-safe accessors (#890) + barrier.
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

fn prewarm(va: u64, size: usize) {
    let mut off = 0u64;
    while (off as usize) < size {
        unsafe { w8(va + off, 0) };
        off += PAGE_SIZE;
    }
}

// =============================================================================
// PCI transport bring-up (netdev VirtioNetPci-derived).
// =============================================================================

/// Configure one queue over common cfg; returns its notify doorbell offset.
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

/// The VIRTIO 1.2 modern-PCI init handshake for the GPU: reset -> ACK ->
/// DRIVER -> negotiate (VERSION_1 only) -> FEATURES_OK readback -> both
/// queues (controlq + cursorq; the cursorq is configured but unused) ->
/// DRIVER_OK. Returns the controlq's notify doorbell VA.
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
            say!("gpud: device lacks VIRTIO_F_VERSION_1");
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
            say!("gpud: FEATURES_OK rejected");
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
                say!("gpud: controlq size below QUEUE_SIZE");
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
                say!("gpud: cursorq size below QUEUE_SIZE");
                w8(common + CCFG_DEVICE_STATUS, STATUS_FAILED);
                return Err(Error::Hardware);
            }
        };

        // Bound each device-supplied doorbell offset within the notify region
        // (the pci-3 F2 guard).
        for off in [ctrl_off, cursor_off] {
            if u64::from(off) * notify_mul + 2 > notify_len {
                say!("gpud: notify doorbell past the notify region");
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

// =============================================================================
// Controlq: per-command submit + IRQ-wait + verify (probe-derived; PCI
// notify/ISR in place of the mmio QueueNotify/InterruptStatus).
// =============================================================================

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

        // Kick the controlq doorbell (the queue's notify word takes the
        // queue index).
        unsafe { w16(self.notify_va, GPU_QUEUE_CONTROL) };

        // Wait for completion; the ISR byte is read-to-clear (bit 0 = queue).
        let mut wakes = 0u32;
        loop {
            if wakes >= MAX_NON_USED_BUFFER_WAKES {
                say!("gpud: too many non-queue IRQ wakes");
                return Err(());
            }
            if self.irq.wait().is_err() {
                say!("gpud: SYS_IRQ_WAIT returned error");
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
            say!("gpud: used.idx {} != expected {}", used_idx, next_seq);
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
                say!("gpud: {} resp_type={:#x} (expected {:#x})", label, t, expected);
                Err(Error::Hardware)
            }
            Err(()) => {
                say!("gpud: {} submit_and_wait error", label);
                Err(Error::Hardware)
            }
        }
    }
}

// =============================================================================
// Request builders (probe-derived; VIRTIO 1.2 section 5.7.6 layouts).
// =============================================================================

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

fn req_get_display_info(ring_va: u64) -> u32 {
    unsafe { write_ctrl_hdr(ring_va + REQ_OFF, VIRTIO_GPU_CMD_GET_DISPLAY_INFO) };
    GPU_CTRL_HDR_LEN
}

fn req_resource_create_2d(ring_va: u64, resource_id: u32, format: u32, width: u32, height: u32) -> u32 {
    let req_va = ring_va + REQ_OFF;
    unsafe {
        write_ctrl_hdr(req_va, VIRTIO_GPU_CMD_RESOURCE_CREATE_2D);
        w32(req_va + 24, resource_id);
        w32(req_va + 28, format);
        w32(req_va + 32, width);
        w32(req_va + 36, height);
    };
    GPU_CTRL_HDR_LEN + 16
}

fn req_resource_attach_backing(ring_va: u64, resource_id: u32, entry_addr: u64, entry_len: u32) -> u32 {
    let req_va = ring_va + REQ_OFF;
    unsafe {
        write_ctrl_hdr(req_va, VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING);
        w32(req_va + 24, resource_id);
        w32(req_va + 28, 1); // nr_entries
        w64(req_va + 32, entry_addr);
        w32(req_va + 40, entry_len);
        w32(req_va + 44, 0);
    };
    GPU_CTRL_HDR_LEN + 8 + 16
}

fn req_set_scanout(ring_va: u64, scanout_id: u32, resource_id: u32, x: u32, y: u32, w: u32, h: u32) -> u32 {
    let req_va = ring_va + REQ_OFF;
    unsafe {
        write_ctrl_hdr(req_va, VIRTIO_GPU_CMD_SET_SCANOUT);
        write_rect(req_va + 24, x, y, w, h);
        w32(req_va + 40, scanout_id);
        w32(req_va + 44, resource_id);
    };
    GPU_CTRL_HDR_LEN + 24
}

fn req_transfer_to_host_2d(ring_va: u64, resource_id: u32, offset: u64, x: u32, y: u32, w: u32, h: u32) -> u32 {
    let req_va = ring_va + REQ_OFF;
    unsafe {
        write_ctrl_hdr(req_va, VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D);
        write_rect(req_va + 24, x, y, w, h);
        w64(req_va + 40, offset);
        w32(req_va + 48, resource_id);
        w32(req_va + 52, 0);
    };
    GPU_CTRL_HDR_LEN + 32
}

fn req_resource_flush(ring_va: u64, resource_id: u32, x: u32, y: u32, w: u32, h: u32) -> u32 {
    let req_va = ring_va + REQ_OFF;
    unsafe {
        write_ctrl_hdr(req_va, VIRTIO_GPU_CMD_RESOURCE_FLUSH);
        write_rect(req_va + 24, x, y, w, h);
        w32(req_va + 40, resource_id);
        w32(req_va + 44, 0);
    };
    GPU_CTRL_HDR_LEN + 24
}

// =============================================================================
// Framebuffer painting.
// =============================================================================

#[inline]
fn px_off(x: u32, y: u32) -> u64 {
    (y as u64 * FB_WIDTH as u64 + x as u64) * FB_BPP as u64
}

fn fill_framebuffer(fb_va: u64) {
    let half_w = FB_WIDTH / 2;
    let half_h = FB_HEIGHT / 2;
    for y in 0..FB_HEIGHT {
        for x in 0..FB_WIDTH {
            let c = match (x < half_w, y < half_h) {
                (true, true) => COLOR_RED,
                (false, true) => COLOR_GREEN,
                (true, false) => COLOR_BLUE,
                (false, false) => COLOR_WHITE,
            };
            unsafe { w32(fb_va + px_off(x, y), c) };
        }
    }
}

fn paint_heartbeat(fb_va: u64, on: bool) {
    let c = if on { COLOR_YELLOW } else { COLOR_BLACK };
    for y in HB_Y..(HB_Y + HB_H) {
        for x in HB_X..(HB_X + HB_W) {
            unsafe { w32(fb_va + px_off(x, y), c) };
        }
    }
}

// =============================================================================
// The driver.
// =============================================================================

struct Gpud {
    ctrl: Controlq,
    // RAII: held for the Proc's lifetime (persistent); dropped only if probe
    // errors after acquisition, where their Drop releases cleanly.
    _pci: PciDev,
    _ring: Dma,
    _fb: Dma,
    hb_on: bool,
}

impl Gpud {
    /// One heartbeat tick: toggle the block, then partial-rect TRANSFER +
    /// FLUSH, each IRQ-completed.
    fn tick(&mut self) -> Result<(), Error> {
        self.hb_on = !self.hb_on;
        paint_heartbeat(FB_USER_VA, self.hb_on);
        dsb_sy();

        let off = px_off(HB_X, HB_Y);
        let req = req_transfer_to_host_2d(RING_DMA_USER_VA, FB_RESOURCE_ID, off, HB_X, HB_Y, HB_W, HB_H);
        self.ctrl
            .step("TRANSFER(hb)", req, GPU_CTRL_HDR_LEN, VIRTIO_GPU_RESP_OK_NODATA)?;

        let req = req_resource_flush(RING_DMA_USER_VA, FB_RESOURCE_ID, HB_X, HB_Y, HB_W, HB_H);
        self.ctrl
            .step("FLUSH(hb)", req, GPU_CTRL_HDR_LEN, VIRTIO_GPU_RESP_OK_NODATA)
    }
}

impl Driver for Gpud {
    fn probe(res: &BoundResources) -> Result<Self, Error> {
        say!(
            "gpud: grant compat={} pci={:?} irq={} dma={:#x}",
            res.compatible,
            res.pci,
            res.irq.len(),
            res.dma_max
        );

        // Claim the GPU function + map its BARs. The warden's conferred PCI
        // allowance (the function's bdf) is what the kernel SYS_PCI_CLAIM gate
        // enforces (I-34); the claim resolves the same first-match function.
        let pci = unsafe { PciDev::claim(VIRTIO_DEVICE_ID_GPU, BAR_WINDOW_VA) }.map_err(|e| {
            say!("gpud: PCI claim/map failed {:?}", e);
            Error::Hardware
        })?;

        let (common_va, common_len) = pci.region(PciRegion::Common).ok_or_else(|| {
            say!("gpud: no common-cfg region");
            Error::Hardware
        })?;
        if common_len < CCFG_MIN_LEN {
            say!("gpud: common-cfg region too small ({})", common_len);
            return Err(Error::Hardware);
        }
        let (notify_base, notify_len) = pci.region(PciRegion::Notify).ok_or_else(|| {
            say!("gpud: no notify region");
            Error::Hardware
        })?;
        let isr_va = pci
            .region(PciRegion::Isr)
            .ok_or_else(|| {
                say!("gpud: no ISR region");
                Error::Hardware
            })?
            .0;
        let notify_mul = u64::from(pci.notify_off_multiplier());
        let intid = pci.intid().ok_or_else(|| {
            say!("gpud: no INTx INTID resolved");
            Error::Hardware
        })?;

        let irq = Irq::new(intid, Rights::SIGNAL).map_err(|_| {
            say!("gpud: SYS_IRQ_CREATE failed for intid {}", intid);
            Error::Hardware
        })?;

        let rw_map = Rights::READ | Rights::WRITE | Rights::MAP;
        let prot = T_PROT_READ | T_PROT_WRITE;
        let ring = unsafe { Dma::new(RING_DMA_SIZE, rw_map, RING_DMA_USER_VA, prot) }.map_err(|_| {
            say!("gpud: SYS_DMA_CREATE(ring) failed");
            Error::Hardware
        })?;
        let fb = unsafe { Dma::new(FB_SIZE as usize, rw_map, FB_USER_VA, prot) }.map_err(|_| {
            say!("gpud: SYS_DMA_CREATE(fb) failed");
            Error::Hardware
        })?;

        // Pre-warm (demand-page) so the Normal-WB PTEs are installed before
        // any device-side access races a CPU store.
        prewarm(ring.base_va() as u64, RING_DMA_SIZE);
        prewarm(fb.base_va() as u64, FB_SIZE as usize);

        let ring_pa = ring.paddr();
        let fb_pa = fb.paddr();

        let notify_va = init_device(
            common_va,
            notify_base,
            notify_mul,
            u64::from(notify_len),
            ring_pa,
        )?;

        let mut ctrl = Controlq {
            ring_va: RING_DMA_USER_VA,
            ring_pa,
            notify_va,
            isr_va,
            irq,
            seq: 0,
        };

        // Bring the scanout up: display info (informational), resource, the
        // pattern, backing, scanout bind, full transfer, full flush.
        let req = req_get_display_info(RING_DMA_USER_VA);
        ctrl.step(
            "GET_DISPLAY_INFO",
            req,
            GPU_RESP_DISPLAY_INFO_LEN,
            VIRTIO_GPU_RESP_OK_DISPLAY_INFO,
        )?;

        let req = req_resource_create_2d(
            RING_DMA_USER_VA,
            FB_RESOURCE_ID,
            VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM,
            FB_WIDTH,
            FB_HEIGHT,
        );
        ctrl.step("RESOURCE_CREATE_2D", req, GPU_CTRL_HDR_LEN, VIRTIO_GPU_RESP_OK_NODATA)?;

        fill_framebuffer(FB_USER_VA);
        paint_heartbeat(FB_USER_VA, false);
        dsb_sy();

        let req = req_resource_attach_backing(RING_DMA_USER_VA, FB_RESOURCE_ID, fb_pa, FB_SIZE as u32);
        ctrl.step("RESOURCE_ATTACH_BACKING", req, GPU_CTRL_HDR_LEN, VIRTIO_GPU_RESP_OK_NODATA)?;

        let req = req_set_scanout(RING_DMA_USER_VA, FB_SCANOUT_ID, FB_RESOURCE_ID, 0, 0, FB_WIDTH, FB_HEIGHT);
        ctrl.step("SET_SCANOUT", req, GPU_CTRL_HDR_LEN, VIRTIO_GPU_RESP_OK_NODATA)?;

        let req = req_transfer_to_host_2d(RING_DMA_USER_VA, FB_RESOURCE_ID, 0, 0, 0, FB_WIDTH, FB_HEIGHT);
        ctrl.step("TRANSFER_TO_HOST_2D", req, GPU_CTRL_HDR_LEN, VIRTIO_GPU_RESP_OK_NODATA)?;

        let req = req_resource_flush(RING_DMA_USER_VA, FB_RESOURCE_ID, 0, 0, FB_WIDTH, FB_HEIGHT);
        ctrl.step("RESOURCE_FLUSH", req, GPU_CTRL_HDR_LEN, VIRTIO_GPU_RESP_OK_NODATA)?;

        say!(
            "gpud: scanout up -- {}x{} B8G8R8A8, pci intid={} cmds={}",
            FB_WIDTH,
            FB_HEIGHT,
            intid,
            ctrl.seq
        );
        Ok(Gpud {
            ctrl,
            _pci: pci,
            _ring: ring,
            _fb: fb,
            hb_on: false,
        })
    }

    fn serve(mut self, _res: &BoundResources) -> Result<(), Error> {
        // Persistent service: signal READY last (all console output precedes
        // it), then hold the device -- and the display -- up forever. The
        // warden leaves us running (lifecycle = persistent); teardown is a
        // warden group-terminate -> kernel reap.
        let mut out = libthyla_rs::io::stdout();
        let _ = out.write_all(b"READY\n");

        loop {
            let _ = sleep(Duration::from_millis(TICK_MS));
            self.tick()?;
        }
    }
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run::<Gpud>()
}
