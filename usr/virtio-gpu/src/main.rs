// /virtio-gpu — fourth composed userspace driver.
//
// Two-stage scope:
//
//   P4-L (probe; previously landed): RESET → DRIVER_OK → controlq
//   GET_DISPLAY_INFO → verify OK_DISPLAY_INFO. Proves the substrate
//   generalizes to DeviceID=16 + two-virtqueue MMIO config + the
//   controlq command/response chain shape.
//
//   P4-L-scanout (this chunk): adds the full 2D resource lifecycle
//   that puts pixels on a scanout (Halcyon-prep gate):
//
//     RESOURCE_CREATE_2D     → allocate host-side resource
//     RESOURCE_ATTACH_BACKING → bind guest framebuffer pages
//     SET_SCANOUT            → bind scanout 0 to the resource
//     TRANSFER_TO_HOST_2D    → copy guest backing into host resource
//     RESOURCE_FLUSH         → present the resource on the scanout
//
//   Each of the five new commands returns OK_NODATA (0x1100); the
//   probe asserts this for every step. The verifying signal is the
//   chain of OK_NODATAs — QEMU's virtio-gpu device validates resource
//   ID + format + dimensions + backing length + scanout id + rect
//   bounds and answers with ERR_INVALID_* on any mismatch, so the
//   five OK responses constitute a tight contract that the host
//   actually built, backed, bound, transferred, and flushed.
//
// Visual verification (pixels reaching a real framebuffer) is not in
// CI scope — `tools/run-vm.sh` runs `-nographic` so QEMU's gl-on-egl
// back-end isn't active. A future P4-L-screencap chunk could wire QMP
// `screendump` + a Python verifier for pixel-perfect verification.
//
// What this chunk specifically guards against (beyond the probe):
//   - Multi-command controlq flow: each command bumps avail.idx by 1;
//     each waits on its own IRQ; each ACKs InterruptStatus. used.idx
//     tracks monotonically; the per-command resp.hdr.type is read at
//     RESP_OFF and validated against OK_NODATA.
//   - Second DMA allocation pattern: 4 KiB ring + 64 KiB framebuffer
//     as two distinct KObj_DMA handles. Each maps into its own
//     user-VA window; the framebuffer's PA is the value handed to the
//     device in the ATTACH_BACKING mem_entry.
//   - virtio_gpu_mem_entry layout: { le64 addr; le32 length; le32
//     padding } = 16 B. Single entry suffices because the kernel-side
//     buddy allocator backs each kobj_dma_create with one physically
//     contiguous chunk (KOBJ_DMA_MAX_SIZE = 1 MiB; we use 64 KiB).
//   - Format encoding: VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM = 1; pixel
//     bytes [B, G, R, A] in memory = u32 little-endian 0xAARRGGBB on
//     AArch64.
//
// Framebuffer test pattern: 128×128, four solid-color quadrants
// (red TL, green TR, blue BL, white BR). Deterministic and
// orientation-revealing if anyone screencaps later.

#![no_std]
#![no_main]

// libthyla-rs convention (Phase 7 U-2b): every native Rust binary opts
// in to ThylaAlloc as its global allocator. Required because libthyla-rs
// links the alloc crate at its root; the symbol resolves here.
#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use core::arch::asm;
use libthyla_rs::{
    T_PROT_READ, T_PROT_WRITE, T_RIGHT_MAP, T_RIGHT_READ, T_RIGHT_SIGNAL,
    T_RIGHT_WRITE, t_dma_create, t_dma_map, t_exits, t_irq_create, t_irq_wait,
    t_mmio_create, t_mmio_map, t_puts, t_putstr, virtio_rmb,
};

// =============================================================================
// virtio-mmio bank constants (QEMU virt machine).
// =============================================================================

const VIRTIO_MMIO_BASE_PA: u64 = 0x0a00_0000;
const VIRTIO_MMIO_SLOT_STRIDE: u64 = 0x200;
const VIRTIO_MMIO_NUM_SLOTS: u64 = 32;
const PAGE_SIZE: u64 = 0x1000;

const VIRTIO_MMIO_NUM_PAGES: u64 = (VIRTIO_MMIO_NUM_SLOTS * VIRTIO_MMIO_SLOT_STRIDE) / PAGE_SIZE;

// SPI base per hw/arm/virt.c::vms->irqmap[VIRT_MMIO]. GIC SPI offset 32.
const VIRTIO_MMIO_GIC_INTID_BASE: u32 = 32 + 16; // = 48

// =============================================================================
// User-VA layout (probe-private; distinct from blk/net/input).
// =============================================================================

const MMIO_USER_VA: u64 = 0x0090_0000;
const DMA_USER_VA: u64  = 0x00a0_0000;
const FB_USER_VA: u64   = 0x00b0_0000;

// =============================================================================
// VirtIO MMIO register offsets (per VIRTIO 1.2 §4.2.2).
// =============================================================================

const REG_MAGIC_VALUE: u64       = 0x000;
const REG_VERSION: u64           = 0x004;
const REG_DEVICE_ID: u64         = 0x008;
const REG_DEVICE_FEATURES: u64   = 0x010;
const REG_DEVICE_FEATURES_SEL: u64 = 0x014;
const REG_DRIVER_FEATURES: u64   = 0x020;
const REG_DRIVER_FEATURES_SEL: u64 = 0x024;
const REG_QUEUE_SEL: u64         = 0x030;
const REG_QUEUE_NUM_MAX: u64     = 0x034;
const REG_QUEUE_NUM: u64         = 0x038;
const REG_QUEUE_READY: u64       = 0x044;
const REG_QUEUE_NOTIFY: u64      = 0x050;
const REG_INTERRUPT_STATUS: u64  = 0x060;
const REG_INTERRUPT_ACK: u64     = 0x064;
const REG_STATUS: u64            = 0x070;
const REG_QUEUE_DESC_LOW: u64    = 0x080;
const REG_QUEUE_DESC_HIGH: u64   = 0x084;
const REG_QUEUE_DRIVER_LOW: u64  = 0x090;
const REG_QUEUE_DRIVER_HIGH: u64 = 0x094;
const REG_QUEUE_DEVICE_LOW: u64  = 0x0a0;
const REG_QUEUE_DEVICE_HIGH: u64 = 0x0a4;

// Device-specific config space begins at offset 0x100 in modern MMIO.
const REG_CONFIG_BASE: u64 = 0x100;

const VIRTIO_MMIO_MAGIC: u32     = 0x7472_6976; // "virt"
const VIRTIO_MMIO_VERSION_MODERN: u32 = 2;
const VIRTIO_DEVICE_ID_GPU: u32  = 16;

// Status bits (§2.1).
const STATUS_ACKNOWLEDGE: u32 = 1;
const STATUS_DRIVER: u32      = 2;
const STATUS_DRIVER_OK: u32   = 4;
const STATUS_FEATURES_OK: u32 = 8;
const STATUS_FAILED: u32      = 128;

// VIRTIO_F_VERSION_1 = bit 32 → bank-1 bit 0.
const VIRTIO_F_VERSION_1_BIT_BANK1: u32 = 1 << 0;

// VIRTIO 1.2 §4.2.5 InterruptStatus bits.
const INT_USED_BUFFER:  u32 = 1 << 0;
const INT_CONFIG_CHANGE: u32 = 1 << 1;

// Cap on consecutive non-used-buffer wakes (config-change only) we
// tolerate per submit_and_wait call. QEMU's virtio-gpu can issue
// config-change events on display geometry shifts (rare in -nographic
// CI, possible interactively); 16 is well above any reasonable burst.
const MAX_NON_USED_BUFFER_WAKES: u32 = 16;

// Descriptor flags (§2.7.5).
const VIRTQ_DESC_F_NEXT: u16  = 1;
const VIRTQ_DESC_F_WRITE: u16 = 2;

// =============================================================================
// virtio-gpu specifics (VIRTIO 1.2 §5.7).
// =============================================================================
//
// Config space layout (§5.7.4) at REG_CONFIG_BASE:
//   offset  type      field
//   0       le32      events_read
//   4       le32      events_clear
//   8       le32      num_scanouts
//   12      le32      num_capsets
const GPU_CFG_NUM_SCANOUTS: u64 = 8;
const GPU_CFG_NUM_CAPSETS: u64  = 12;

// Virtqueue indices (§5.7.2).
const GPU_QUEUE_CONTROL: u32 = 0;
const GPU_QUEUE_CURSOR: u32  = 1;

// controlq command types (§5.7.6.7).
const VIRTIO_GPU_CMD_GET_DISPLAY_INFO: u32        = 0x0100;
const VIRTIO_GPU_CMD_RESOURCE_CREATE_2D: u32      = 0x0101;
const VIRTIO_GPU_CMD_SET_SCANOUT: u32             = 0x0103;
const VIRTIO_GPU_CMD_RESOURCE_FLUSH: u32          = 0x0104;
const VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D: u32     = 0x0105;
const VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING: u32 = 0x0106;

// controlq response types (§5.7.6.7).
const VIRTIO_GPU_RESP_OK_NODATA: u32       = 0x1100;
const VIRTIO_GPU_RESP_OK_DISPLAY_INFO: u32 = 0x1101;

// Pixel format (§5.7.3). B8G8R8A8_UNORM = bytes [B, G, R, A] in memory
// = u32 little-endian 0xAARRGGBB on AArch64.
const VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM: u32 = 1;

// struct virtio_gpu_ctrl_hdr (§5.7.6.6):
//   le32 type;        // offset 0
//   le32 flags;       // offset 4
//   le64 fence_id;    // offset 8
//   le32 ctx_id;      // offset 16
//   u8   ring_idx;    // offset 20
//   u8   padding[3];  // offset 21..24
// Total: 24 bytes.
const GPU_CTRL_HDR_LEN: u32 = 24;

// struct virtio_gpu_resp_display_info (§5.7.6.1):
//   struct virtio_gpu_ctrl_hdr hdr;          // 24 bytes
//   struct virtio_gpu_display_one pmodes[VIRTIO_GPU_MAX_SCANOUTS];
//
// struct virtio_gpu_display_one (§5.7.6.1):
//   struct virtio_gpu_rect r;  // 4 le32 = 16 bytes (x, y, width, height)
//   le32 enabled;              // offset 16
//   le32 flags;                // offset 20
// Total: 24 bytes per scanout.
const GPU_MAX_SCANOUTS: u32 = 16;
const GPU_DISPLAY_ONE_LEN: u32 = 24;
const GPU_RESP_DISPLAY_INFO_LEN: u32 =
    GPU_CTRL_HDR_LEN + GPU_MAX_SCANOUTS * GPU_DISPLAY_ONE_LEN; // 24 + 384 = 408

// =============================================================================
// Framebuffer dimensions + DMA size.
// =============================================================================
//
// 128 × 128 × 4 B = 64 KiB. Comfortably below KOBJ_DMA_MAX_SIZE
// (1 MiB at v1.0) and exactly 16 contiguous 4-KiB pages — single
// mem_entry suffices in ATTACH_BACKING.

const FB_WIDTH: u32 = 128;
const FB_HEIGHT: u32 = 128;
const FB_BPP: u32 = 4;
const FB_SIZE: u64 = (FB_WIDTH as u64) * (FB_HEIGHT as u64) * (FB_BPP as u64);
const FB_RESOURCE_ID: u32 = 1; // any non-zero u32 (0 is reserved)
const FB_SCANOUT_ID: u32 = 0;

// =============================================================================
// DMA layout (single 4 KiB ring page).
// =============================================================================
//
//   0x000..0x100   controlq desc[0..16]   (16 × 16 B)
//   0x100..0x200   controlq avail         (header + ring + used_event)
//   0x200..0x300   controlq used          (header + ring + avail_event)
//   0x300..0x400   cursorq desc[0..16]    (configured but unused)
//   0x400..0x500   cursorq avail
//   0x500..0x600   cursorq used
//   0x600..0x700   request region         (256 B; largest body = TRANSFER_TO_HOST_2D
//                                          at 24 + 32 = 56 B)
//   0x700..0xa00   response region        (768 B; covers display_info at 408 B
//                                          + 24-B OK_NODATA replies)
//   0xa00..0x1000  unused

const QUEUE_SIZE: u16 = 16;
const DMA_BUFSIZE: u64 = PAGE_SIZE;

const CTRL_DESC_OFF: u64   = 0x000;
const CTRL_AVAIL_OFF: u64  = 0x100;
const CTRL_USED_OFF: u64   = 0x200;
const CURSOR_DESC_OFF: u64 = 0x300;
const CURSOR_AVAIL_OFF: u64 = 0x400;
const CURSOR_USED_OFF: u64 = 0x500;
const REQ_OFF: u64         = 0x600;
const RESP_OFF: u64        = 0x700;

const REQ_REGION_LEN: u32 = 0x100;   // 256 B
const RESP_REGION_LEN: u32 = 0x300;  // 768 B

// Compile-time sanity: regions don't overlap and fit within DMA buffer.
const _: () = {
    assert!(REQ_OFF + (REQ_REGION_LEN as u64) <= RESP_OFF);
    assert!(RESP_OFF + (RESP_REGION_LEN as u64) <= DMA_BUFSIZE);
    assert!(GPU_RESP_DISPLAY_INFO_LEN <= RESP_REGION_LEN);
};

// =============================================================================
// Volatile MMIO + DMA access helpers.
// =============================================================================

#[inline(always)]
unsafe fn read32(addr: u64) -> u32 { libthyla_rs::hardware::mmio_read32(addr) }

#[inline(always)]
unsafe fn write32(addr: u64, val: u32) { libthyla_rs::hardware::mmio_write32(addr, val) }

#[inline(always)]
unsafe fn write16(addr: u64, val: u16) { libthyla_rs::hardware::mmio_write16(addr, val) }

#[inline(always)]
unsafe fn read16(addr: u64) -> u16 { libthyla_rs::hardware::mmio_read16(addr) }

#[inline(always)]
unsafe fn write64(addr: u64, val: u64) { libthyla_rs::hardware::mmio_write64(addr, val) }

#[inline(always)]
unsafe fn write_u8(addr: u64, val: u8) { libthyla_rs::hardware::mmio_write8(addr, val) }

#[inline(always)]
fn dsb_sy() {
    unsafe { asm!("dsb sy", options(nostack, preserves_flags)) }
}

// =============================================================================
// Diagnostics — integer formatters mirror virtio-blk/net/input.
// =============================================================================

fn write_hex_u32(buf: &mut [u8; 10], val: u32) {
    buf[0] = b'0';
    buf[1] = b'x';
    for i in 0..8 {
        let nibble = (val >> ((7 - i) * 4)) & 0xf;
        buf[2 + i] = if nibble < 10 { b'0' + nibble as u8 } else { b'a' + (nibble - 10) as u8 };
    }
}

fn write_dec_u32(buf: &mut [u8; 11], val: u32) -> usize {
    let mut n = val;
    let mut i = 11;
    if n == 0 { i -= 1; buf[i] = b'0'; return i; }
    while n > 0 {
        i -= 1;
        buf[i] = b'0' + (n % 10) as u8;
        n /= 10;
    }
    i
}

fn log(s: &str) { t_putstr(s); }

fn log_dec(val: u32) {
    let mut buf = [0u8; 11];
    let start = write_dec_u32(&mut buf, val);
    unsafe { t_puts(buf.as_ptr().add(start), 11 - start); }
}

fn log_hex(val: u32) {
    let mut buf = [0u8; 10];
    write_hex_u32(&mut buf, val);
    unsafe { t_puts(buf.as_ptr(), 10) };
}

// =============================================================================
// MMIO bank claim + slot scan.
// =============================================================================

fn claim_virtio_mmio_bank() -> Option<u64> {
    for i in 0..VIRTIO_MMIO_NUM_PAGES {
        let pa = VIRTIO_MMIO_BASE_PA + i * PAGE_SIZE;
        let va = MMIO_USER_VA + i * PAGE_SIZE;
        let rights = T_RIGHT_READ | T_RIGHT_WRITE | T_RIGHT_MAP;
        let prot = T_PROT_READ | T_PROT_WRITE;

        let handle = unsafe { t_mmio_create(pa, PAGE_SIZE, rights) };
        if handle < 0 {
            log("virtio-gpu: SYS_MMIO_CREATE failed for page ");
            log_dec(i as u32);
            log("\n");
            return None;
        }
        let map_rc = unsafe { t_mmio_map(handle, va, prot) };
        if map_rc < 0 {
            log("virtio-gpu: SYS_MMIO_MAP failed for page ");
            log_dec(i as u32);
            log("\n");
            return None;
        }
    }
    Some(MMIO_USER_VA)
}

fn find_gpu_slot(mmio_base_va: u64) -> Option<(u32, u64)> {
    for slot in 0..VIRTIO_MMIO_NUM_SLOTS {
        let slot_va = mmio_base_va + slot * VIRTIO_MMIO_SLOT_STRIDE;
        let magic = unsafe { read32(slot_va + REG_MAGIC_VALUE) };
        if magic != VIRTIO_MMIO_MAGIC { continue; }
        let dev_id = unsafe { read32(slot_va + REG_DEVICE_ID) };
        if dev_id == VIRTIO_DEVICE_ID_GPU {
            return Some((slot as u32, slot_va));
        }
    }
    None
}

// =============================================================================
// Configure a single virtqueue (controlq or cursorq).
// =============================================================================

fn configure_queue(slot_va: u64, dma_pa: u64,
                   desc_off: u64, avail_off: u64, used_off: u64) -> bool {
    let num_max = unsafe { read32(slot_va + REG_QUEUE_NUM_MAX) };
    if num_max < QUEUE_SIZE as u32 {
        log("virtio-gpu: QueueNumMax=");
        log_dec(num_max);
        log(" below QUEUE_SIZE\n");
        return false;
    }
    unsafe { write32(slot_va + REG_QUEUE_NUM, QUEUE_SIZE as u32) };

    let desc_pa = dma_pa + desc_off;
    let driver_pa = dma_pa + avail_off;
    let device_pa = dma_pa + used_off;
    unsafe {
        write32(slot_va + REG_QUEUE_DESC_LOW,    (desc_pa & 0xFFFF_FFFF) as u32);
        write32(slot_va + REG_QUEUE_DESC_HIGH,   (desc_pa >> 32) as u32);
        write32(slot_va + REG_QUEUE_DRIVER_LOW,  (driver_pa & 0xFFFF_FFFF) as u32);
        write32(slot_va + REG_QUEUE_DRIVER_HIGH, (driver_pa >> 32) as u32);
        write32(slot_va + REG_QUEUE_DEVICE_LOW,  (device_pa & 0xFFFF_FFFF) as u32);
        write32(slot_va + REG_QUEUE_DEVICE_HIGH, (device_pa >> 32) as u32);
    };
    unsafe { write32(slot_va + REG_QUEUE_READY, 1) };
    true
}

// =============================================================================
// VirtIO 1.2 device initialization for the GPU device.
// =============================================================================

fn init_device(slot_va: u64, dma_pa: u64) -> bool {
    unsafe { write32(slot_va + REG_STATUS, 0) };
    unsafe { write32(slot_va + REG_STATUS, STATUS_ACKNOWLEDGE) };
    unsafe { write32(slot_va + REG_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER) };

    unsafe { write32(slot_va + REG_DEVICE_FEATURES_SEL, 1) };
    let dev_feat_hi = unsafe { read32(slot_va + REG_DEVICE_FEATURES) };
    if dev_feat_hi & VIRTIO_F_VERSION_1_BIT_BANK1 == 0 {
        log("virtio-gpu: device lacks VIRTIO_F_VERSION_1\n");
        unsafe { write32(slot_va + REG_STATUS, STATUS_FAILED) };
        return false;
    }

    unsafe { write32(slot_va + REG_DRIVER_FEATURES_SEL, 0) };
    unsafe { write32(slot_va + REG_DRIVER_FEATURES, 0) };
    unsafe { write32(slot_va + REG_DRIVER_FEATURES_SEL, 1) };
    unsafe { write32(slot_va + REG_DRIVER_FEATURES, VIRTIO_F_VERSION_1_BIT_BANK1) };

    unsafe {
        write32(slot_va + REG_STATUS,
                STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK);
    };
    let status = unsafe { read32(slot_va + REG_STATUS) };
    if status & STATUS_FEATURES_OK == 0 {
        log("virtio-gpu: FEATURES_OK rejected; status=");
        log_hex(status);
        log("\n");
        return false;
    }

    unsafe { write32(slot_va + REG_QUEUE_SEL, GPU_QUEUE_CONTROL) };
    if !configure_queue(slot_va, dma_pa, CTRL_DESC_OFF, CTRL_AVAIL_OFF, CTRL_USED_OFF) {
        unsafe { write32(slot_va + REG_STATUS, STATUS_FAILED) };
        return false;
    }
    unsafe { write32(slot_va + REG_QUEUE_SEL, GPU_QUEUE_CURSOR) };
    if !configure_queue(slot_va, dma_pa, CURSOR_DESC_OFF, CURSOR_AVAIL_OFF, CURSOR_USED_OFF) {
        unsafe { write32(slot_va + REG_STATUS, STATUS_FAILED) };
        return false;
    }

    unsafe {
        write32(slot_va + REG_STATUS,
                STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK | STATUS_DRIVER_OK);
    };

    true
}

// =============================================================================
// Per-command submit + wait + verify (controlq).
// =============================================================================
//
// Each invocation:
//   - assumes the caller has populated REQ_OFF with the full request
//     (24-B ctrl_hdr at REQ_OFF + body at REQ_OFF + 24) over `req_len` bytes.
//   - rebuilds the 2-descriptor chain at desc head 0:
//       desc[0]: REQ_OFF, req_len, NEXT,  next=1
//       desc[1]: RESP_OFF, resp_len, WRITE, next=0
//   - zeroes resp.hdr.type so a no-response surfaces as 0 instead of stale.
//   - updates avail.ring[seq % QUEUE_SIZE] = 0 (desc head is always 0).
//   - DSB; avail.idx = seq + 1; DSB.
//   - kicks controlq (QueueNotify=0).
//   - waits IRQ; ACKs InterruptStatus.
//   - asserts used.idx == seq + 1.
//   - reads resp.hdr.type and returns it.
//
// `seq` is the avail.idx value BEFORE the bump (0 for the first
// command, 1 for the second, etc.). After the call returns, the
// caller's monotonic counter advances by 1.

struct Controlq {
    slot_va: u64,
    dma_va: u64,
    dma_pa: u64,
    irq_handle: i64,
    seq: u16, // next avail.idx to use (also the count of completed commands)
}

impl Controlq {
    fn submit_and_wait(&mut self, req_len: u32, resp_len: u32) -> Result<u32, ()> {
        let next_seq = self.seq.wrapping_add(1);

        // Zero resp ctrl_hdr (24 B) so missed response surfaces as
        // type=0 rather than uninitialized.
        for i in 0..(GPU_CTRL_HDR_LEN as u64) {
            unsafe { write_u8(self.dma_va + RESP_OFF + i, 0) };
        }

        // Build descriptor chain. Each descriptor is 16 bytes:
        //   le64 addr; le32 len; le16 flags; le16 next;
        let desc_va = self.dma_va + CTRL_DESC_OFF;

        unsafe {
            write64(desc_va + 0,  self.dma_pa + REQ_OFF);
            write32(desc_va + 8,  req_len);
            write16(desc_va + 12, VIRTQ_DESC_F_NEXT);
            write16(desc_va + 14, 1);

            write64(desc_va + 16, self.dma_pa + RESP_OFF);
            write32(desc_va + 24, resp_len);
            write16(desc_va + 28, VIRTQ_DESC_F_WRITE);
            write16(desc_va + 30, 0);
        };

        // avail.ring[seq % QUEUE_SIZE] = 0 (desc head 0 reused per command).
        let avail_va = self.dma_va + CTRL_AVAIL_OFF;
        let ring_slot = (self.seq % QUEUE_SIZE) as u64;
        unsafe {
            write16(avail_va + 0, 0); // flags
            write16(avail_va + 4 + ring_slot * 2, 0);
        };

        // VIRTIO 1.2 §2.7.13.1: descriptor + ring slot writes MUST be
        // visible before the idx bump.
        dsb_sy();
        unsafe { write16(avail_va + 2, next_seq) };
        dsb_sy();

        // Kick controlq.
        unsafe { write32(self.slot_va + REG_QUEUE_NOTIFY, GPU_QUEUE_CONTROL) };

        // Wait for completion. Tolerate INT_CONFIG_CHANGE-only wakes
        // (host display geometry shifts, etc.) by retrying until the
        // device's INT_USED_BUFFER bit fires or we exhaust the cap.
        let mut wakes = 0u32;
        loop {
            if wakes >= MAX_NON_USED_BUFFER_WAKES {
                log("virtio-gpu: FAIL — too many non-used-buffer wakes\n");
                return Err(());
            }
            let count = unsafe { t_irq_wait(self.irq_handle) };
            if count < 0 {
                log("virtio-gpu: FAIL — SYS_IRQ_WAIT returned error\n");
                return Err(());
            }
            let int_status = unsafe { read32(self.slot_va + REG_INTERRUPT_STATUS) };
            unsafe { write32(self.slot_va + REG_INTERRUPT_ACK, int_status) };
            if int_status & INT_USED_BUFFER != 0 { break; }
            wakes += 1;
        }
        let _ = INT_CONFIG_CHANGE; // referenced for symmetry; ACKed via int_status

        let used_va = self.dma_va + CTRL_USED_OFF;
        let used_idx = unsafe { read16(used_va + 2) };
        // VIRTIO 1.2 §2.7.13.2: barrier between observing used.idx
        // advance and reading the response buffer the descriptor
        // pointed at. Without it, an out-of-order ARM core may
        // speculate the resp.hdr.type read before the used.idx load,
        // returning the pre-advance zero (which would mis-classify the
        // OK response as a hardware fault).
        virtio_rmb();
        if used_idx != next_seq {
            log("virtio-gpu: FAIL — used.idx != expected (got ");
            log_dec(used_idx as u32);
            log(", expected ");
            log_dec(next_seq as u32);
            log(")\n");
            return Err(());
        }

        let resp_va = self.dma_va + RESP_OFF;
        let resp_type = unsafe { read32(resp_va + 0) };

        self.seq = next_seq;
        Ok(resp_type)
    }
}

// =============================================================================
// Request-body builders.
// =============================================================================
//
// All builders write the 24-byte ctrl_hdr at REQ_OFF followed by the
// command-specific body. The Controlq submit picks up `req_len` from
// here.

unsafe fn write_ctrl_hdr(req_va: u64, cmd_type: u32) {
    // type, flags=0, fence_id=0, ctx_id=0, ring_idx=0, padding[3]=0
    write32(req_va + 0,  cmd_type);
    write32(req_va + 4,  0);
    write64(req_va + 8,  0);
    write32(req_va + 16, 0);
    write_u8(req_va + 20, 0);
    write_u8(req_va + 21, 0);
    write_u8(req_va + 22, 0);
    write_u8(req_va + 23, 0);
}

unsafe fn write_rect(va: u64, x: u32, y: u32, w: u32, h: u32) {
    write32(va + 0,  x);
    write32(va + 4,  y);
    write32(va + 8,  w);
    write32(va + 12, h);
}

// Body layouts per VIRTIO 1.2 §5.7.6.
//
// Body length (excluding the 24-B ctrl_hdr):
//   GET_DISPLAY_INFO         : 0 B
//   RESOURCE_CREATE_2D       : 16 B (resource_id, format, width, height)
//   RESOURCE_ATTACH_BACKING  : 8 + 16*nr_entries B
//   SET_SCANOUT              : 24 B (rect, scanout_id, resource_id)
//   TRANSFER_TO_HOST_2D      : 32 B (rect, offset, resource_id, padding)
//   RESOURCE_FLUSH           : 24 B (rect, resource_id, padding)

fn req_get_display_info(dma_va: u64) -> u32 {
    let req_va = dma_va + REQ_OFF;
    unsafe { write_ctrl_hdr(req_va, VIRTIO_GPU_CMD_GET_DISPLAY_INFO) };
    GPU_CTRL_HDR_LEN
}

fn req_resource_create_2d(dma_va: u64, resource_id: u32, format: u32,
                          width: u32, height: u32) -> u32 {
    let req_va = dma_va + REQ_OFF;
    unsafe {
        write_ctrl_hdr(req_va, VIRTIO_GPU_CMD_RESOURCE_CREATE_2D);
        write32(req_va + 24, resource_id);
        write32(req_va + 28, format);
        write32(req_va + 32, width);
        write32(req_va + 36, height);
    };
    GPU_CTRL_HDR_LEN + 16
}

fn req_resource_attach_backing(dma_va: u64, resource_id: u32,
                               entry_addr: u64, entry_len: u32) -> u32 {
    let req_va = dma_va + REQ_OFF;
    unsafe {
        write_ctrl_hdr(req_va, VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING);
        write32(req_va + 24, resource_id);
        write32(req_va + 28, 1); // nr_entries
        // virtio_gpu_mem_entry: { le64 addr; le32 length; le32 padding }
        write64(req_va + 32, entry_addr);
        write32(req_va + 40, entry_len);
        write32(req_va + 44, 0);
    };
    GPU_CTRL_HDR_LEN + 8 + 16
}

fn req_set_scanout(dma_va: u64, scanout_id: u32, resource_id: u32,
                   x: u32, y: u32, w: u32, h: u32) -> u32 {
    let req_va = dma_va + REQ_OFF;
    unsafe {
        write_ctrl_hdr(req_va, VIRTIO_GPU_CMD_SET_SCANOUT);
        write_rect(req_va + 24, x, y, w, h);
        write32(req_va + 40, scanout_id);
        write32(req_va + 44, resource_id);
    };
    GPU_CTRL_HDR_LEN + 24
}

fn req_transfer_to_host_2d(dma_va: u64, resource_id: u32, offset: u64,
                           x: u32, y: u32, w: u32, h: u32) -> u32 {
    let req_va = dma_va + REQ_OFF;
    unsafe {
        write_ctrl_hdr(req_va, VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D);
        write_rect(req_va + 24, x, y, w, h);
        write64(req_va + 40, offset);
        write32(req_va + 48, resource_id);
        write32(req_va + 52, 0); // padding
    };
    GPU_CTRL_HDR_LEN + 32
}

fn req_resource_flush(dma_va: u64, resource_id: u32,
                      x: u32, y: u32, w: u32, h: u32) -> u32 {
    let req_va = dma_va + REQ_OFF;
    unsafe {
        write_ctrl_hdr(req_va, VIRTIO_GPU_CMD_RESOURCE_FLUSH);
        write_rect(req_va + 24, x, y, w, h);
        write32(req_va + 40, resource_id);
        write32(req_va + 44, 0); // padding
    };
    GPU_CTRL_HDR_LEN + 24
}

// =============================================================================
// Framebuffer test pattern.
// =============================================================================
//
// 4 quadrants × 64×64 pixels each. Solid colors:
//   TL: red   (B=00, G=00, R=FF, A=FF) → u32 LE 0xFFFF0000
//   TR: green (B=00, G=FF, R=00, A=FF) → u32 LE 0xFF00FF00
//   BL: blue  (B=FF, G=00, R=00, A=FF) → u32 LE 0xFF0000FF
//   BR: white (B=FF, G=FF, R=FF, A=FF) → u32 LE 0xFFFFFFFF
//
// Each row is FB_WIDTH (= 128) u32s. The split is at column 64 and row 64.

const COLOR_RED: u32   = 0xFFFF_0000;
const COLOR_GREEN: u32 = 0xFF00_FF00;
const COLOR_BLUE: u32  = 0xFF00_00FF;
const COLOR_WHITE: u32 = 0xFFFF_FFFF;

fn fill_framebuffer(fb_va: u64) {
    let half_w = FB_WIDTH / 2;
    let half_h = FB_HEIGHT / 2;
    for y in 0..FB_HEIGHT {
        for x in 0..FB_WIDTH {
            let c = match (x < half_w, y < half_h) {
                (true,  true)  => COLOR_RED,
                (false, true)  => COLOR_GREEN,
                (true,  false) => COLOR_BLUE,
                (false, false) => COLOR_WHITE,
            };
            let off = (y * FB_WIDTH + x) as u64 * (FB_BPP as u64);
            unsafe { write32(fb_va + off, c) };
        }
    }
}

// =============================================================================
// Step driver — wraps Controlq with per-step logging and resp.hdr.type
// verification. On a wrong resp, prints what was expected.
// =============================================================================

fn step(ctrl: &mut Controlq, label: &str,
        req_len: u32, resp_len: u32, expected: u32) -> Result<(), ()> {
    match ctrl.submit_and_wait(req_len, resp_len) {
        Ok(resp_type) => {
            if resp_type != expected {
                log("virtio-gpu: FAIL — ");
                log(label);
                log(" resp_type=");
                log_hex(resp_type);
                log(" (expected ");
                log_hex(expected);
                log(")\n");
                Err(())
            } else {
                Ok(())
            }
        }
        Err(()) => {
            log("virtio-gpu: FAIL — ");
            log(label);
            log(" submit_and_wait error\n");
            Err(())
        }
    }
}

// =============================================================================
// Entry point.
// =============================================================================

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    let mmio_base = match claim_virtio_mmio_bank() {
        Some(va) => va,
        None => {
            log("virtio-gpu: FAIL — virtio-mmio bank claim failed\n");
            unsafe { t_exits(1) };
        }
    };

    let (slot, slot_va) = match find_gpu_slot(mmio_base) {
        Some(found) => found,
        None => {
            log("virtio-gpu: SKIP — no virtio-mmio slot has DeviceID=16 (no -device virtio-gpu-device wired in QEMU?)\n");
            return 0;
        }
    };

    let intid = VIRTIO_MMIO_GIC_INTID_BASE + slot;

    let version = unsafe { read32(slot_va + REG_VERSION) };
    if version != VIRTIO_MMIO_VERSION_MODERN {
        log("virtio-gpu: FAIL — slot reports legacy version (");
        log_dec(version);
        log("); modern (2) required\n");
        unsafe { t_exits(1) };
    }

    let irq_handle = unsafe { t_irq_create(intid, T_RIGHT_SIGNAL) };
    if irq_handle < 0 {
        log("virtio-gpu: FAIL — SYS_IRQ_CREATE failed\n");
        unsafe { t_exits(1) };
    }

    // Ring DMA: 4 KiB for desc + avail + used + req + resp.
    let ring_handle = unsafe {
        t_dma_create(DMA_BUFSIZE, T_RIGHT_READ | T_RIGHT_WRITE | T_RIGHT_MAP)
    };
    if ring_handle < 0 {
        log("virtio-gpu: FAIL — SYS_DMA_CREATE failed for ring\n");
        unsafe { t_exits(1) };
    }
    let ring_pa = unsafe {
        t_dma_map(ring_handle, DMA_USER_VA, T_PROT_READ | T_PROT_WRITE)
    };
    if ring_pa < 0 {
        log("virtio-gpu: FAIL — SYS_DMA_MAP failed for ring\n");
        unsafe { t_exits(1) };
    }
    // Pre-warm so userland_demand_page installs the Normal-WB PTE
    // before any device-side write races a CPU-side store.
    for off in (0..DMA_BUFSIZE).step_by(PAGE_SIZE as usize) {
        unsafe { write_u8(DMA_USER_VA + off, 0) };
    }

    // Framebuffer DMA: 64 KiB (FB_SIZE) for backing pages.
    let fb_handle = unsafe {
        t_dma_create(FB_SIZE, T_RIGHT_READ | T_RIGHT_WRITE | T_RIGHT_MAP)
    };
    if fb_handle < 0 {
        log("virtio-gpu: FAIL — SYS_DMA_CREATE failed for framebuffer\n");
        unsafe { t_exits(1) };
    }
    let fb_pa = unsafe {
        t_dma_map(fb_handle, FB_USER_VA, T_PROT_READ | T_PROT_WRITE)
    };
    if fb_pa < 0 {
        log("virtio-gpu: FAIL — SYS_DMA_MAP failed for framebuffer\n");
        unsafe { t_exits(1) };
    }
    // Pre-warm every framebuffer page (16 pages at 64 KiB).
    for off in (0..FB_SIZE).step_by(PAGE_SIZE as usize) {
        unsafe { write_u8(FB_USER_VA + off, 0) };
    }

    if !init_device(slot_va, ring_pa as u64) {
        unsafe { t_exits(1) };
    }

    let mut ctrl = Controlq {
        slot_va,
        dma_va: DMA_USER_VA,
        dma_pa: ring_pa as u64,
        irq_handle,
        seq: 0,
    };

    // 1. GET_DISPLAY_INFO — proves the controlq still works under the
    //    new submit_and_wait machinery; verifies the substrate.
    let req_len = req_get_display_info(DMA_USER_VA);
    if step(&mut ctrl, "GET_DISPLAY_INFO",
            req_len, GPU_RESP_DISPLAY_INFO_LEN,
            VIRTIO_GPU_RESP_OK_DISPLAY_INFO).is_err() {
        unsafe { t_exits(1) };
    }

    // Log informational config-space + pmodes[0] (carried over from
    // the P4-L probe — pmodes[0].enabled is QEMU-backend-dependent
    // and not load-bearing).
    let num_scanouts = unsafe {
        read32(slot_va + REG_CONFIG_BASE + GPU_CFG_NUM_SCANOUTS)
    };
    let num_capsets = unsafe {
        read32(slot_va + REG_CONFIG_BASE + GPU_CFG_NUM_CAPSETS)
    };
    let pm0_va = DMA_USER_VA + RESP_OFF + (GPU_CTRL_HDR_LEN as u64);
    let pm0_width   = unsafe { read32(pm0_va + 8) };
    let pm0_height  = unsafe { read32(pm0_va + 12) };
    let pm0_enabled = unsafe { read32(pm0_va + 16) };
    log("virtio-gpu: display_info slot=");
    log_dec(slot);
    log(" intid=");
    log_dec(intid);
    log(" num_scanouts=");
    log_dec(num_scanouts);
    log(" num_capsets=");
    log_dec(num_capsets);
    log(" pmodes[0]=");
    log_dec(pm0_width);
    log("x");
    log_dec(pm0_height);
    log(" enabled=");
    log_dec(pm0_enabled);
    log("\n");

    // 2. RESOURCE_CREATE_2D — host-side resource (no backing yet).
    let req_len = req_resource_create_2d(DMA_USER_VA, FB_RESOURCE_ID,
                                         VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM,
                                         FB_WIDTH, FB_HEIGHT);
    if step(&mut ctrl, "RESOURCE_CREATE_2D",
            req_len, GPU_CTRL_HDR_LEN,
            VIRTIO_GPU_RESP_OK_NODATA).is_err() {
        unsafe { t_exits(1) };
    }

    // 3. Fill the guest framebuffer with the 4-quadrant test pattern
    //    BEFORE ATTACH_BACKING. The device only reads from the
    //    backing during TRANSFER_TO_HOST_2D, but doing the fill here
    //    means the host's first observation reflects the intended
    //    pattern (no torn state).
    fill_framebuffer(FB_USER_VA);
    // DMB to ensure all framebuffer stores are visible before the
    // device-side read kicked off by TRANSFER_TO_HOST_2D.
    dsb_sy();

    // 4. RESOURCE_ATTACH_BACKING — single mem_entry pointing at the
    //    framebuffer DMA PA. The kernel-side buddy allocator backs
    //    kobj_dma_create with one physically contiguous chunk, so a
    //    single (addr, length) pair describes the whole 64 KiB.
    let req_len = req_resource_attach_backing(DMA_USER_VA, FB_RESOURCE_ID,
                                              fb_pa as u64, FB_SIZE as u32);
    if step(&mut ctrl, "RESOURCE_ATTACH_BACKING",
            req_len, GPU_CTRL_HDR_LEN,
            VIRTIO_GPU_RESP_OK_NODATA).is_err() {
        unsafe { t_exits(1) };
    }

    // 5. SET_SCANOUT — bind scanout 0 to FB_RESOURCE_ID over the
    //    full framebuffer rect.
    let req_len = req_set_scanout(DMA_USER_VA, FB_SCANOUT_ID, FB_RESOURCE_ID,
                                  0, 0, FB_WIDTH, FB_HEIGHT);
    if step(&mut ctrl, "SET_SCANOUT",
            req_len, GPU_CTRL_HDR_LEN,
            VIRTIO_GPU_RESP_OK_NODATA).is_err() {
        unsafe { t_exits(1) };
    }

    // 6. TRANSFER_TO_HOST_2D — copy guest backing → host resource
    //    over the full rect, offset 0.
    let req_len = req_transfer_to_host_2d(DMA_USER_VA, FB_RESOURCE_ID, 0,
                                          0, 0, FB_WIDTH, FB_HEIGHT);
    if step(&mut ctrl, "TRANSFER_TO_HOST_2D",
            req_len, GPU_CTRL_HDR_LEN,
            VIRTIO_GPU_RESP_OK_NODATA).is_err() {
        unsafe { t_exits(1) };
    }

    // 7. RESOURCE_FLUSH — present the host resource on the bound
    //    scanout. The device's display backend (QEMU's gl-on-egl or
    //    sdl/gtk/vnc when one is configured) picks up the rect.
    let req_len = req_resource_flush(DMA_USER_VA, FB_RESOURCE_ID,
                                     0, 0, FB_WIDTH, FB_HEIGHT);
    if step(&mut ctrl, "RESOURCE_FLUSH",
            req_len, GPU_CTRL_HDR_LEN,
            VIRTIO_GPU_RESP_OK_NODATA).is_err() {
        unsafe { t_exits(1) };
    }

    log("virtio-gpu: PASS — scanout pipeline (CREATE_2D + ATTACH_BACKING + SET_SCANOUT + TRANSFER + FLUSH) on ");
    log_dec(FB_WIDTH);
    log("x");
    log_dec(FB_HEIGHT);
    log(" B8G8R8A8 framebuffer; slot=");
    log_dec(slot);
    log(" intid=");
    log_dec(intid);
    log(" cmds=");
    log_dec(ctrl.seq as u32);
    log("\n");
    0
}
