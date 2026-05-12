// /virtio-gpu — fourth composed userspace driver (P4-L).
//
// Probe scope for the VirtIO GPU device class (DeviceID = 16). Proves
// the composed-hw-handle SVC substrate (MMIO + DMA + IRQ) generalizes
// to a device class with:
//   - two virtqueues (controlq idx 0 + cursorq idx 1; spec §5.7.2),
//   - a controlq command/response pattern (driver writes request to a
//     device-readable descriptor + device writes response to a
//     device-writable descriptor),
//   - a flat le32 config-space (events_read + events_clear +
//     num_scanouts + num_capsets per spec §5.7.4),
//   - VIRTIO_GPU_CMD_GET_DISPLAY_INFO (spec §5.7.6.1) — the canonical
//     "what scanouts does this device expose" command. Used by every
//     real virtio-gpu driver before any 2D surface setup.
//
// Scope (intentionally narrow):
//   - Init transport: RESET → ACK → DRIVER → DeviceFeatures (require
//     VIRTIO_F_VERSION_1; decline EDID + VIRGL + RESOURCE_UUID +
//     RESOURCE_BLOB + CONTEXT_INIT — none of those are needed for
//     GET_DISPLAY_INFO) → FEATURES_OK → configure BOTH queues
//     (controlq idx 0 + cursorq idx 1; cursorq stays empty but the
//     spec lists it as one of the two required virtqueues) →
//     DRIVER_OK.
//   - Send VIRTIO_GPU_CMD_GET_DISPLAY_INFO via controlq (2-descriptor
//     chain: 24-byte req hdr OUT + 408-byte resp payload IN).
//   - Wait on IRQ, ACK InterruptStatus, parse the response: verify
//     resp.hdr.type == VIRTIO_GPU_RESP_OK_DISPLAY_INFO (= 0x1101).
//   - Log num_scanouts from config space + pmodes[0] dimensions +
//     enabled flag (informational; QEMU virt with -nographic still
//     reports num_scanouts >= 1 — virtio-gpu's scanout count is a
//     device capability, not a runtime backend state). Exit clean.
//   - Do NOT call any RESOURCE_CREATE_2D / SET_SCANOUT / TRANSFER /
//     FLUSH — those belong to a Phase 8 Halcyon-prep sub-chunk that
//     actually puts pixels on a scanout.
//
// What this test specifically guards against (beyond blk/net/input):
//   - DeviceID=16 dispatch in virtio_mmio_find_by_device_id.
//   - Configuring TWO virtqueues before DRIVER_OK (every prior driver
//     configured exactly one; cursorq is the second-queue setup the
//     virtio-mmio register surface exposes via REG_QUEUE_SEL=1).
//   - controlq command/response chain pattern: descriptor 0 is OUT
//     (NEXT flag, no WRITE), descriptor 1 is IN (WRITE flag, no NEXT).
//     The descriptor.next field linkage is symmetric to virtio-blk's
//     3-descriptor chain but with a different shape (no separate
//     status byte; virtio-gpu response embeds status in resp.hdr.type).
//   - Selector-less flat config-space (offset 0x100..0x110 contains
//     four le32 fields with no selector mechanism — distinct from
//     virtio-input's select+subsel+size+u indirection).
//
// Future P4-L-scanout: requires a 2D resource lifecycle
// (RESOURCE_CREATE_2D → ATTACH_BACKING → TRANSFER_TO_HOST_2D →
// SET_SCANOUT → RESOURCE_FLUSH), guest-physical backing pages, and
// either a real display backend (so the pixels are observable) or a
// host-side capture path. Halcyon-prep work.

#![no_std]
#![no_main]

use core::arch::asm;
use libthyla_rs::{
    T_PROT_READ, T_PROT_WRITE, T_RIGHT_MAP, T_RIGHT_READ, T_RIGHT_SIGNAL,
    T_RIGHT_WRITE, t_dma_create, t_dma_map, t_exits, t_irq_create, t_irq_wait,
    t_mmio_create, t_mmio_map, t_puts, t_putstr,
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
//
// The config space is flat le32s — there's no selector indirection
// (contrast virtio-input's select+subsel+size+u union at §5.8.4).
const GPU_CFG_NUM_SCANOUTS: u64 = 8;
const GPU_CFG_NUM_CAPSETS: u64  = 12;

// Virtqueue indices (§5.7.2).
const GPU_QUEUE_CONTROL: u32 = 0;
const GPU_QUEUE_CURSOR: u32  = 1;

// controlq command types (§5.7.6.7).
const VIRTIO_GPU_CMD_GET_DISPLAY_INFO: u32 = 0x0100;

// controlq response types (§5.7.6.7).
const VIRTIO_GPU_RESP_OK_DISPLAY_INFO: u32 = 0x1101;

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
// DMA layout (single 4 KiB page).
// =============================================================================
//
//   0x000..0x100   controlq desc[0..16]   (16 × 16 B)
//   0x100..0x200   controlq avail         (header + ring + used_event)
//   0x200..0x300   controlq used          (header + ring + avail_event)
//   0x300..0x400   cursorq desc[0..16]    (16 × 16 B — unused but configured)
//   0x400..0x500   cursorq avail          (idx stays 0)
//   0x500..0x600   cursorq used           (unused)
//   0x600..0x620   request header         (virtio_gpu_ctrl_hdr; 24 B)
//   0x620..0x7c0   response payload       (resp_display_info; 408 B)
//
// Total: 0x7c0 = 1984 B; fits in PAGE_SIZE = 4096.

const QUEUE_SIZE: u16 = 16;
const DMA_BUFSIZE: u64 = PAGE_SIZE;

const CTRL_DESC_OFF: u64   = 0x000;
const CTRL_AVAIL_OFF: u64  = 0x100;
const CTRL_USED_OFF: u64   = 0x200;
const CURSOR_DESC_OFF: u64 = 0x300;
const CURSOR_AVAIL_OFF: u64 = 0x400;
const CURSOR_USED_OFF: u64 = 0x500;
const REQ_OFF: u64         = 0x600;
const RESP_OFF: u64        = 0x620;

// Compile-time sanity: layout fits within the 4 KiB DMA buffer.
const _: () = {
    assert!(RESP_OFF + (GPU_RESP_DISPLAY_INFO_LEN as u64) <= DMA_BUFSIZE);
};

// =============================================================================
// Volatile MMIO + DMA access helpers.
// =============================================================================

#[inline(always)]
unsafe fn read32(addr: u64) -> u32 {
    core::ptr::read_volatile(addr as *const u32)
}

#[inline(always)]
unsafe fn write32(addr: u64, val: u32) {
    core::ptr::write_volatile(addr as *mut u32, val);
}

#[inline(always)]
unsafe fn write16(addr: u64, val: u16) {
    core::ptr::write_volatile(addr as *mut u16, val);
}

#[inline(always)]
unsafe fn read16(addr: u64) -> u16 {
    core::ptr::read_volatile(addr as *const u16)
}

#[inline(always)]
unsafe fn write64(addr: u64, val: u64) {
    core::ptr::write_volatile(addr as *mut u64, val);
}

#[inline(always)]
unsafe fn write_u8(addr: u64, val: u8) {
    core::ptr::write_volatile(addr as *mut u8, val);
}

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
//
// Writes the PA tuple (desc + driver + device) to the slot's registers
// after Queue_SEL has been written. Used twice: once for controlq with
// pre-populated descriptor 0 (the request) + descriptor 1 (the
// response), once for cursorq with avail.idx left at 0 (the queue
// exists but the driver never submits anything to it).
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
    // Step 1: RESET.
    unsafe { write32(slot_va + REG_STATUS, 0) };

    // Step 2: ACKNOWLEDGE.
    unsafe { write32(slot_va + REG_STATUS, STATUS_ACKNOWLEDGE) };

    // Step 3: DRIVER.
    unsafe { write32(slot_va + REG_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER) };

    // Step 4: read DeviceFeatures; require VIRTIO_F_VERSION_1.
    // bank-0 may advertise EDID / VIRGL / RESOURCE_UUID / RESOURCE_BLOB
    // / CONTEXT_INIT; we decline all of them — none are needed for
    // GET_DISPLAY_INFO, and accepting them would change the device's
    // expected request/response framing (RESOURCE_BLOB in particular
    // forces a different config-space layout).
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

    // Step 5: FEATURES_OK + readback.
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

    // Step 6: configure BOTH virtqueues. Per VIRTIO 1.2 §5.7.2 the GPU
    // device exposes controlq (idx 0) + cursorq (idx 1). The spec
    // doesn't strictly mandate both be active before DRIVER_OK, but
    // QEMU's virtio-gpu refuses to accept commands if cursorq is
    // left at QueueReady=0 — the device walks its queue list on
    // DRIVER_OK transitions and treats an unset cursorq as a config
    // error. Mirror Linux's drm/virtio_gpu init order (controlq then
    // cursorq).
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

    // Step 7: DRIVER_OK.
    unsafe {
        write32(slot_va + REG_STATUS,
                STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK | STATUS_DRIVER_OK);
    };

    true
}

// =============================================================================
// Submit VIRTIO_GPU_CMD_GET_DISPLAY_INFO on controlq.
// =============================================================================
//
// 2-descriptor chain (head = 0):
//   desc[0]: req header (24 B; OUT → device reads)        next=1
//   desc[1]: resp payload (408 B; IN → device writes)     last
//
// avail.ring[0] = 0; avail.idx = 1 after barriers; kick QueueNotify=0.

fn submit_get_display_info(slot_va: u64, dma_va: u64, dma_pa: u64) {
    // Zero the response header so a missing device write surfaces as
    // type=0 rather than uninitialized.
    for i in 0..(GPU_CTRL_HDR_LEN as u64) {
        unsafe { write_u8(dma_va + RESP_OFF + i, 0) };
    }

    // Populate the request header at REQ_OFF (virtio_gpu_ctrl_hdr).
    let req_va = dma_va + REQ_OFF;
    unsafe {
        write32(req_va + 0,  VIRTIO_GPU_CMD_GET_DISPLAY_INFO); // type
        write32(req_va + 4,  0);                               // flags
        write64(req_va + 8,  0);                               // fence_id
        write32(req_va + 16, 0);                               // ctx_id
        write_u8(req_va + 20, 0);                              // ring_idx
        write_u8(req_va + 21, 0);                              // padding[0]
        write_u8(req_va + 22, 0);                              // padding[1]
        write_u8(req_va + 23, 0);                              // padding[2]
    };

    // Populate the descriptor chain. Each descriptor is 16 bytes:
    //   le64 addr; le32 len; le16 flags; le16 next;
    let desc_va = dma_va + CTRL_DESC_OFF;

    // desc[0]: request header (OUT to device).
    unsafe {
        write64(desc_va + 0,  dma_pa + REQ_OFF);
        write32(desc_va + 8,  GPU_CTRL_HDR_LEN);
        write16(desc_va + 12, VIRTQ_DESC_F_NEXT);
        write16(desc_va + 14, 1);
    };

    // desc[1]: response payload (IN; device writes the full
    // virtio_gpu_resp_display_info struct here).
    unsafe {
        write64(desc_va + 16, dma_pa + RESP_OFF);
        write32(desc_va + 24, GPU_RESP_DISPLAY_INFO_LEN);
        write16(desc_va + 28, VIRTQ_DESC_F_WRITE);
        write16(desc_va + 30, 0);
    };

    // controlq avail ring: flags=0, ring[0]=0, idx=1.
    let avail_va = dma_va + CTRL_AVAIL_OFF;
    unsafe {
        write16(avail_va + 0, 0);   // flags
        write16(avail_va + 4, 0);   // ring[0] = desc head 0
    };

    // VIRTIO 1.2 §2.7.13.1: descriptor + ring[0] writes MUST be
    // visible before the idx bump.
    dsb_sy();
    unsafe { write16(avail_va + 2, 1) }; // idx = 1
    dsb_sy();

    // Kick controlq (queue index 0).
    unsafe { write32(slot_va + REG_QUEUE_NOTIFY, GPU_QUEUE_CONTROL) };
}

// =============================================================================
// Wait + parse + verify.
// =============================================================================
//
// Success criteria:
//   - SYS_IRQ_WAIT returns >= 0.
//   - InterruptStatus has the buffer-used bit set (bit 0); ACK it.
//   - controlq used.idx advanced to 1.
//   - used.ring[0].id == 0 (the descriptor head we submitted).
//   - resp.hdr.type == VIRTIO_GPU_RESP_OK_DISPLAY_INFO (0x1101).
//
// Diagnostics: log num_scanouts (config-space le32 at offset 0x108)
// and pmodes[0] rectangle + enabled flag. These are informational —
// QEMU virt with -nographic still reports num_scanouts >= 1, but
// pmodes[0].enabled may be 0 (no scanout backend) or 1 (default
// 1024x768). The probe doesn't fail on either case; the OK response
// header is the load-bearing assertion.

fn wait_and_verify(irq_handle: i64, slot_va: u64, dma_va: u64,
                   slot: u32, intid: u32) -> Result<(), ()> {
    let count = unsafe { t_irq_wait(irq_handle) };
    if count < 0 {
        log("virtio-gpu: FAIL — SYS_IRQ_WAIT returned error\n");
        return Err(());
    }

    let int_status = unsafe { read32(slot_va + REG_INTERRUPT_STATUS) };
    unsafe { write32(slot_va + REG_INTERRUPT_ACK, int_status) };

    let used_va = dma_va + CTRL_USED_OFF;
    let used_idx = unsafe { read16(used_va + 2) };
    if used_idx != 1 {
        log("virtio-gpu: FAIL — controlq used.idx != 1 (got ");
        log_dec(used_idx as u32);
        log(")\n");
        return Err(());
    }

    let used_id = unsafe { read32(used_va + 4) };
    if used_id != 0 {
        log("virtio-gpu: FAIL — used.ring[0].id != 0 (got id=");
        log_dec(used_id);
        log(")\n");
        return Err(());
    }

    let resp_va = dma_va + RESP_OFF;
    let resp_type = unsafe { read32(resp_va + 0) };
    if resp_type != VIRTIO_GPU_RESP_OK_DISPLAY_INFO {
        log("virtio-gpu: FAIL — response type=");
        log_hex(resp_type);
        log(" (expected 0x");
        log_hex(VIRTIO_GPU_RESP_OK_DISPLAY_INFO);
        log(" = OK_DISPLAY_INFO)\n");
        return Err(());
    }

    // Read config space for num_scanouts (informational).
    let num_scanouts = unsafe {
        read32(slot_va + REG_CONFIG_BASE + GPU_CFG_NUM_SCANOUTS)
    };
    let num_capsets = unsafe {
        read32(slot_va + REG_CONFIG_BASE + GPU_CFG_NUM_CAPSETS)
    };

    // pmodes[0] is at resp_va + GPU_CTRL_HDR_LEN (= 24).
    let pm0_va = resp_va + (GPU_CTRL_HDR_LEN as u64);
    let pm0_width   = unsafe { read32(pm0_va + 8) };
    let pm0_height  = unsafe { read32(pm0_va + 12) };
    let pm0_enabled = unsafe { read32(pm0_va + 16) };

    log("virtio-gpu: slot=");
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

    Ok(())
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

    // Subscribe to the IRQ BEFORE issuing the request (so gic_attach +
    // gic_enable_irq are in place by the time the device fires).
    let irq_handle = unsafe { t_irq_create(intid, T_RIGHT_SIGNAL) };
    if irq_handle < 0 {
        log("virtio-gpu: FAIL — SYS_IRQ_CREATE failed\n");
        unsafe { t_exits(1) };
    }

    let dma_handle = unsafe {
        t_dma_create(DMA_BUFSIZE, T_RIGHT_READ | T_RIGHT_WRITE | T_RIGHT_MAP)
    };
    if dma_handle < 0 {
        log("virtio-gpu: FAIL — SYS_DMA_CREATE failed\n");
        unsafe { t_exits(1) };
    }
    let dma_pa = unsafe {
        t_dma_map(dma_handle, DMA_USER_VA, T_PROT_READ | T_PROT_WRITE)
    };
    if dma_pa < 0 {
        log("virtio-gpu: FAIL — SYS_DMA_MAP failed\n");
        unsafe { t_exits(1) };
    }

    // Pre-warm the DMA page so userland_demand_page installs the
    // Normal-WB PTE before any device-side write would otherwise
    // race with the first CPU-side store.
    for off in (0..DMA_BUFSIZE).step_by(PAGE_SIZE as usize) {
        unsafe { write_u8(DMA_USER_VA + off, 0) };
    }

    if !init_device(slot_va, dma_pa as u64) {
        unsafe { t_exits(1) };
    }

    submit_get_display_info(slot_va, DMA_USER_VA, dma_pa as u64);

    match wait_and_verify(irq_handle, slot_va, DMA_USER_VA, slot, intid) {
        Ok(()) => {
            log("virtio-gpu: PASS — controlq GET_DISPLAY_INFO round-trip ok (slot=");
            log_dec(slot);
            log(" intid=");
            log_dec(intid);
            log(")\n");
            0
        }
        Err(()) => unsafe { t_exits(1) },
    }
}
