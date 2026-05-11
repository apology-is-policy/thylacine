// /virtio-blk-probe — first composed userspace driver (P4-Ic5b2).
//
// First userspace binary to drive a real device end-to-end. Composes
// every hardware-handle syscall the v1.0 Thylacine surface exposes:
//   - SYS_MMIO_CREATE + SYS_MMIO_MAP   (P4-Ib + P4-Ic2) for the
//     virtio-mmio transport registers.
//   - SYS_DMA_CREATE + SYS_DMA_MAP     (P4-Ic5b1b) for the contiguous
//     pinned virtqueue rings + req / data / status descriptors.
//   - SYS_IRQ_CREATE + SYS_IRQ_WAIT    (P4-Ib + P4-G) for the device's
//     completion IRQ.
//
// What this proves end-to-end:
//   - VirtIO 1.2 modern transport via the MMIO surface: ACKNOWLEDGE →
//     DRIVER → DeviceFeatures readback → DriverFeatures write (bit 32
//     VIRTIO_F_VERSION_1) → FEATURES_OK → virtqueue setup → DRIVER_OK.
//   - Split virtqueue (§2.7): chained descriptors via the NEXT flag,
//     device-writable (WRITE flag) data + status descriptors.
//   - Cache-coherent DMA on QEMU virt: Normal cacheable PTE (the
//     BURROW_TYPE_DMA arm of userland_demand_page) with `dsb sy`
//     barriers around the MMIO notify suffices; no explicit cache
//     maintenance ops needed.
//   - IRQ-driven completion: enable IRQ via t_irq_create BEFORE
//     issuing the request, kick the device via QueueNotify, sleep on
//     t_irq_wait until kobj_irq_dispatch increments pending_count
//     from the GIC handler.
//   - Block-0 round-trip: device writes the on-disk signature
//     ("THYLACINE-DISK-1") into the data descriptor's buffer; CPU
//     observes the same bytes after t_irq_wait returns; status byte
//     == VIRTIO_BLK_S_OK.
//
// Target: QEMU virt's virtio-mmio bank at PA 0x0a000000..0x0a004000
// (32 slots × 0x200 each = 4 pages). Slot assignment depends on
// `-device virtio-blk-device,drive=disk0` position in
// tools/run-vm.sh's flag list — QEMU virt assigns slots in REVERSE
// creation order, so the first virtio device gets slot 31. The probe
// scans all 32 slots for DeviceID = 2 (block), so the precise slot
// number is informational rather than load-bearing.
//
// INTID derivation: QEMU virt's hw/arm/virt.c maps virtio-mmio slot k
// to SPI (16 + k) → GIC INTID (32 + 16 + k) = 48 + k. The kernel's
// existing virtio_init probes the same slots via mmu_map_mmio but
// never calls gic_enable_irq for any of them (kernel has no in-kernel
// virtio driver post-P4-F's rng probe), so the INTID is free for the
// userspace driver to claim via t_irq_create.

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
//
// Bank base + slot stride are platform constants per QEMU virt machine
// hw/arm/virt.c. A Phase 5+ DTB-driven discovery API would lift these
// hardcodings; at v1.0 the values are pinned in-source.

const VIRTIO_MMIO_BASE_PA: u64 = 0x0a00_0000;
const VIRTIO_MMIO_SLOT_STRIDE: u64 = 0x200;            // 512 bytes per slot
const VIRTIO_MMIO_NUM_SLOTS: u64 = 32;
const PAGE_SIZE: u64 = 0x1000;

// 4 pages cover all 32 slots. Each page hosts 8 slots (PAGE_SIZE /
// SLOT_STRIDE = 4096 / 512 = 8). We claim every page so every slot is
// readable for the probe sweep.
const VIRTIO_MMIO_NUM_PAGES: u64 = (VIRTIO_MMIO_NUM_SLOTS * VIRTIO_MMIO_SLOT_STRIDE) / PAGE_SIZE;

// SPI base for virtio-mmio on QEMU virt. INTID = 32 (GIC SPI offset) +
// (16 + slot_index). Pinned per hw/arm/virt.c::vms->irqmap[VIRT_MMIO].
const VIRTIO_MMIO_GIC_SPI_BASE: u32 = 16;
const VIRTIO_MMIO_GIC_INTID_BASE: u32 = 32 + VIRTIO_MMIO_GIC_SPI_BASE; // = 48

// =============================================================================
// User-VA layout (probe-private; the kernel demand-pages on first access).
// =============================================================================

const MMIO_USER_VA: u64 = 0x0050_0000;                 // 4 pages → 0x500000..0x504000
const DMA_USER_VA: u64  = 0x0060_0000;                 // 1 page

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

const VIRTIO_MMIO_MAGIC: u32     = 0x7472_6976;        // "virt" little-endian
const VIRTIO_MMIO_VERSION_MODERN: u32 = 2;
const VIRTIO_DEVICE_ID_BLK: u32  = 2;

// Status bits (§2.1).
const STATUS_ACKNOWLEDGE: u32    = 1;
const STATUS_DRIVER: u32         = 2;
const STATUS_DRIVER_OK: u32      = 4;
const STATUS_FEATURES_OK: u32    = 8;
const STATUS_FAILED: u32         = 128;

// VIRTIO_F_VERSION_1 = bit 32. In the DriverFeatures bank-1 register
// this is bit 0 of the high half.
const VIRTIO_F_VERSION_1_BIT_BANK1: u32 = 1 << 0;

// Descriptor flags (§2.7.5).
const VIRTQ_DESC_F_NEXT: u16  = 1;
const VIRTQ_DESC_F_WRITE: u16 = 2;

// virtio-blk request type (§5.2.6).
const VIRTIO_BLK_T_IN: u32  = 0;

// virtio-blk status byte (§5.2.7).
const VIRTIO_BLK_S_OK: u8 = 0;

// =============================================================================
// DMA buffer layout (single 4 KiB page).
// =============================================================================
//
// All offsets are 16-byte aligned per VIRTIO 1.2 ring alignment
// requirements; using fixed 256/256/256-byte windows is wasteful (we
// only need 256 + 38 + 134 + 16 + 512 + 1 = 957 bytes for queue_size
// = 16) but keeps the layout self-evident and lets each section grow
// without re-doing arithmetic.

const QUEUE_SIZE: u16 = 16;
const DMA_BUFSIZE: u64 = PAGE_SIZE;     // 4096 bytes; ample headroom

const DESC_OFF: u64   = 0x000;
const AVAIL_OFF: u64  = 0x100;
const USED_OFF: u64   = 0x200;
const REQ_OFF: u64    = 0x300;
const DATA_OFF: u64   = 0x400;
const STATUS_OFF: u64 = 0x600;

const DATA_LEN: u32 = 512;              // one block

// =============================================================================
// Volatile MMIO + memory access helpers.
// =============================================================================
//
// MMIO: kernel installs Device-nGnRnE PTEs (MAIR_IDX_DEVICE) for
// BURROW_TYPE_MMIO regions, so the hardware guarantees non-gathering
// + non-reordering access. We still need volatile in Rust to defeat
// compiler-level coalescing.
//
// DMA: kernel installs Normal cacheable PTEs (MAIR_IDX_NORMAL_WB) for
// BURROW_TYPE_DMA regions. Coherency between CPU and device is
// guaranteed on QEMU virt's modeled VirtIO transports (the device
// participates in the cache-coherency domain). We use volatile to
// defeat compiler reorder + explicit dsb-sy barriers around the
// notify-and-wait window.

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
unsafe fn read_u8(addr: u64) -> u8 {
    core::ptr::read_volatile(addr as *const u8)
}

#[inline(always)]
fn dsb_sy() {
    unsafe { asm!("dsb sy", options(nostack, preserves_flags)) }
}

// =============================================================================
// Diagnostic helpers.
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
    // Write the decimal representation right-aligned in buf; return
    // start offset. Caller passes the slice [start..11] to t_puts.
    let mut n = val;
    let mut i = 11;
    if n == 0 {
        i -= 1;
        buf[i] = b'0';
        return i;
    }
    while n > 0 {
        i -= 1;
        buf[i] = b'0' + (n % 10) as u8;
        n /= 10;
    }
    i
}

fn log(s: &str) {
    t_putstr(s);
}

fn log_dec(val: u32) {
    let mut buf = [0u8; 11];
    let start = write_dec_u32(&mut buf, val);
    unsafe {
        t_puts(buf.as_ptr().add(start), 11 - start);
    }
}

fn log_hex(val: u32) {
    let mut buf = [0u8; 10];
    write_hex_u32(&mut buf, val);
    unsafe { t_puts(buf.as_ptr(), 10) };
}

// =============================================================================
// MMIO bank setup.
// =============================================================================
//
// Returns the user-VA base for the virtio-mmio bank (slot k is at
// MMIO_USER_VA + k * 0x200). Claims each of the 4 pages individually
// because kobj_mmio_create requires a page-aligned PA + size. All four
// claims succeed only if (a) no other userspace process holds a claim
// on the range and (b) the kernel hasn't reserved the range (P4-Ic5b1a
// dropped virtio-mmio from g_mmio_claims_kernel_ranges).

fn claim_virtio_mmio_bank() -> Option<u64> {
    for i in 0..VIRTIO_MMIO_NUM_PAGES {
        let pa = VIRTIO_MMIO_BASE_PA + i * PAGE_SIZE;
        let va = MMIO_USER_VA + i * PAGE_SIZE;
        let rights = T_RIGHT_READ | T_RIGHT_WRITE | T_RIGHT_MAP;
        let prot = T_PROT_READ | T_PROT_WRITE;

        let handle = unsafe { t_mmio_create(pa, PAGE_SIZE, rights) };
        if handle < 0 {
            log("virtio-blk-probe: SYS_MMIO_CREATE failed for page ");
            log_dec(i as u32);
            log("\n");
            return None;
        }
        let map_rc = unsafe { t_mmio_map(handle, va, prot) };
        if map_rc < 0 {
            log("virtio-blk-probe: SYS_MMIO_MAP failed for page ");
            log_dec(i as u32);
            log("\n");
            return None;
        }
    }
    Some(MMIO_USER_VA)
}

// Find the first slot in [0, 32) with MagicValue = "virt" and DeviceID
// == 2 (block). Returns (slot_index, slot_user_va) or None.
fn find_blk_slot(mmio_base_va: u64) -> Option<(u32, u64)> {
    for slot in 0..VIRTIO_MMIO_NUM_SLOTS {
        let slot_va = mmio_base_va + slot * VIRTIO_MMIO_SLOT_STRIDE;
        let magic = unsafe { read32(slot_va + REG_MAGIC_VALUE) };
        if magic != VIRTIO_MMIO_MAGIC {
            continue;
        }
        let dev_id = unsafe { read32(slot_va + REG_DEVICE_ID) };
        if dev_id == VIRTIO_DEVICE_ID_BLK {
            return Some((slot as u32, slot_va));
        }
    }
    None
}

// =============================================================================
// VirtIO 1.2 device initialization (per §3.1.1).
// =============================================================================
//
// Order is binding: a deviation causes the device to set the FAILED
// status bit and refuse further commands. We pass the slot's user-VA
// + the DMA buffer's PA-base (for the QueueDesc / QueueDriver /
// QueueDevice register writes).

fn init_device(slot_va: u64, dma_pa: u64) -> bool {
    // Step 1: RESET. Writing 0 to Status returns the device to its
    // post-power-on state, dropping any prior driver state.
    unsafe { write32(slot_va + REG_STATUS, 0) };

    // Step 2: ACKNOWLEDGE. "Driver has noticed the device."
    unsafe { write32(slot_va + REG_STATUS, STATUS_ACKNOWLEDGE) };

    // Step 3: DRIVER. "Driver knows how to drive the device."
    unsafe {
        write32(slot_va + REG_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER);
    };

    // Step 4: read DeviceFeatures (both banks) + write DriverFeatures.
    // VirtIO 1.2 mandates VIRTIO_F_VERSION_1 (bit 32) on every modern
    // device; without it the device falls back to legacy semantics
    // (Version=1) which our transport-modern probe does not support.
    unsafe { write32(slot_va + REG_DEVICE_FEATURES_SEL, 0) };
    let _dev_feat_lo = unsafe { read32(slot_va + REG_DEVICE_FEATURES) };

    unsafe { write32(slot_va + REG_DEVICE_FEATURES_SEL, 1) };
    let dev_feat_hi = unsafe { read32(slot_va + REG_DEVICE_FEATURES) };

    if dev_feat_hi & VIRTIO_F_VERSION_1_BIT_BANK1 == 0 {
        log("virtio-blk-probe: device lacks VIRTIO_F_VERSION_1\n");
        unsafe { write32(slot_va + REG_STATUS, STATUS_FAILED) };
        return false;
    }

    // Bank 0: claim nothing (no VIRTIO_BLK_F_* features needed for a
    // bare block-0 read with the canonical 3-descriptor chain).
    unsafe { write32(slot_va + REG_DRIVER_FEATURES_SEL, 0) };
    unsafe { write32(slot_va + REG_DRIVER_FEATURES, 0) };

    // Bank 1: claim VIRTIO_F_VERSION_1 only.
    unsafe { write32(slot_va + REG_DRIVER_FEATURES_SEL, 1) };
    unsafe {
        write32(slot_va + REG_DRIVER_FEATURES, VIRTIO_F_VERSION_1_BIT_BANK1);
    };

    // Step 5: FEATURES_OK. Driver has negotiated. Re-read Status to
    // confirm the device accepted our subset (per §3.1.1).
    unsafe {
        write32(slot_va + REG_STATUS,
                STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK);
    };
    let status = unsafe { read32(slot_va + REG_STATUS) };
    if status & STATUS_FEATURES_OK == 0 {
        log("virtio-blk-probe: FEATURES_OK rejected by device; status=");
        log_hex(status);
        log("\n");
        return false;
    }

    // Step 6: configure virtqueue 0 (the only queue for virtio-blk).
    unsafe { write32(slot_va + REG_QUEUE_SEL, 0) };

    let num_max = unsafe { read32(slot_va + REG_QUEUE_NUM_MAX) };
    if num_max < QUEUE_SIZE as u32 {
        log("virtio-blk-probe: QueueNumMax (");
        log_dec(num_max);
        log(") below QUEUE_SIZE\n");
        unsafe { write32(slot_va + REG_STATUS, STATUS_FAILED) };
        return false;
    }
    unsafe { write32(slot_va + REG_QUEUE_NUM, QUEUE_SIZE as u32) };

    // Step 7: register ring PAs. The DMA buffer is one contiguous
    // page; the three rings live at fixed offsets.
    let desc_pa = dma_pa + DESC_OFF;
    let avail_pa = dma_pa + AVAIL_OFF;
    let used_pa = dma_pa + USED_OFF;

    unsafe {
        write32(slot_va + REG_QUEUE_DESC_LOW, (desc_pa & 0xFFFF_FFFF) as u32);
        write32(slot_va + REG_QUEUE_DESC_HIGH, (desc_pa >> 32) as u32);
        write32(slot_va + REG_QUEUE_DRIVER_LOW, (avail_pa & 0xFFFF_FFFF) as u32);
        write32(slot_va + REG_QUEUE_DRIVER_HIGH, (avail_pa >> 32) as u32);
        write32(slot_va + REG_QUEUE_DEVICE_LOW, (used_pa & 0xFFFF_FFFF) as u32);
        write32(slot_va + REG_QUEUE_DEVICE_HIGH, (used_pa >> 32) as u32);
    };

    // Step 8: arm the queue.
    unsafe { write32(slot_va + REG_QUEUE_READY, 1) };

    // Step 9: DRIVER_OK. Device may now accept requests.
    unsafe {
        write32(slot_va + REG_STATUS,
                STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK | STATUS_DRIVER_OK);
    };

    true
}

// =============================================================================
// Submit a single VIRTIO_BLK_T_IN request for sector 0.
// =============================================================================
//
// Layout of the descriptor chain (head = 0):
//   desc[0]: req header (16 B, OUT — device reads)        → next=1
//   desc[1]: data buffer (512 B, IN — device writes)      → next=2
//   desc[2]: status byte (1 B, IN — device writes)        → last
//
// After populating + ringing the avail ring, we kick the device via
// QueueNotify and return. The caller waits on the IRQ.

fn submit_read_sector_0(slot_va: u64, dma_va: u64, dma_pa: u64) {
    // Zero the status byte so a missing device write surfaces as a
    // non-OK byte (other than what the device might leave).
    unsafe { core::ptr::write_volatile((dma_va + STATUS_OFF) as *mut u8, 0xff) };

    // Populate the request header.
    let req_va = dma_va + REQ_OFF;
    unsafe {
        write32(req_va + 0x0, VIRTIO_BLK_T_IN);
        write32(req_va + 0x4, 0);         // reserved
        write64(req_va + 0x8, 0);         // sector = 0
    };

    // Populate the descriptor chain. Each descriptor is 16 bytes;
    // desc[i] starts at dma_va + DESC_OFF + i*16.
    //   le64 addr; le32 len; le16 flags; le16 next;
    let desc_va = dma_va + DESC_OFF;

    // desc[0]: req header (OUT to device).
    unsafe {
        write64(desc_va + 0,  dma_pa + REQ_OFF);
        write32(desc_va + 8,  16);
        write16(desc_va + 12, VIRTQ_DESC_F_NEXT);
        write16(desc_va + 14, 1);
    };

    // desc[1]: data buffer (IN; device writes 512 bytes here).
    unsafe {
        write64(desc_va + 16,  dma_pa + DATA_OFF);
        write32(desc_va + 24,  DATA_LEN);
        write16(desc_va + 28,  VIRTQ_DESC_F_NEXT | VIRTQ_DESC_F_WRITE);
        write16(desc_va + 30,  2);
    };

    // desc[2]: status byte (IN; device writes 1 byte here).
    unsafe {
        write64(desc_va + 32,  dma_pa + STATUS_OFF);
        write32(desc_va + 40,  1);
        write16(desc_va + 44,  VIRTQ_DESC_F_WRITE);
        write16(desc_va + 46,  0);
    };

    // Avail ring layout:
    //   le16 flags;     // offset 0
    //   le16 idx;       // offset 2 — points one past the last entry
    //   le16 ring[];    // offset 4 — ring[0] is the head we just built
    let avail_va = dma_va + AVAIL_OFF;
    unsafe {
        write16(avail_va + 0, 0);         // flags = 0 (no NO_INTERRUPT)
        write16(avail_va + 4, 0);         // ring[0] = desc index 0
    };

    // Memory barrier: the descriptor + req header + ring[0] writes
    // MUST be visible to the device before the idx bump (otherwise
    // the device could see idx=1 with garbage descriptors). VIRTIO
    // 1.2 §2.7.13.1 requires a memory barrier here.
    dsb_sy();

    // Bump avail.idx LAST.
    unsafe { write16(avail_va + 2, 1) };

    // Another barrier before the MMIO notify so the idx bump is
    // visible to the device's pre-fetch logic.
    dsb_sy();

    // Kick the device.
    unsafe { write32(slot_va + REG_QUEUE_NOTIFY, 0) };
}

// =============================================================================
// Wait for the device to complete + verify the result.
// =============================================================================
//
// Returns Ok(()) if used.idx advanced to 1, status byte == OK, and
// the first 16 bytes of the data buffer match the expected signature.
// Err otherwise. The IRQ wait + InterruptACK happen unconditionally;
// validation runs once we have a completion to inspect.

const DISK_SIGNATURE: &[u8; 16] = b"THYLACINE-DISK-1";

fn wait_and_verify(irq_handle: i64, slot_va: u64, dma_va: u64) -> Result<(), ()> {
    let count = unsafe { t_irq_wait(irq_handle) };
    if count < 0 {
        log("virtio-blk-probe: FAIL — SYS_IRQ_WAIT returned error\n");
        return Err(());
    }

    // Device-side IRQ status must be ACKed so the device deasserts
    // the interrupt line + clears its pending bit. The InterruptACK
    // register accepts the same bit pattern we just read from
    // InterruptStatus (per VIRTIO 1.2 §4.2.2).
    let int_status = unsafe { read32(slot_va + REG_INTERRUPT_STATUS) };
    unsafe { write32(slot_va + REG_INTERRUPT_ACK, int_status) };

    // Read the used ring — must reflect a single completion.
    //   le16 flags;     // offset 0
    //   le16 idx;       // offset 2
    //   used_elem ring[]; // offset 4 — { le32 id; le32 len }
    let used_va = dma_va + USED_OFF;
    let used_idx = unsafe { read16(used_va + 2) };
    if used_idx != 1 {
        log("virtio-blk-probe: FAIL — used.idx != 1 (got ");
        log_dec(used_idx as u32);
        log(")\n");
        return Err(());
    }

    let used_id = unsafe { read32(used_va + 4) };
    let used_len = unsafe { read32(used_va + 8) };
    if used_id != 0 {
        log("virtio-blk-probe: FAIL — used.ring[0].id != 0 (descriptor head mismatch; got id=");
        log_dec(used_id);
        log(")\n");
        return Err(());
    }
    if used_len != (DATA_LEN + 1) {
        // 512 (data) + 1 (status) = 513. Diagnostic only — not a hard
        // fail since the device may legitimately report a different
        // len under future feature negotiations.
        log("virtio-blk-probe: note — used.len=");
        log_dec(used_len);
        log(" (expected 513)\n");
    }

    // Status byte: 0 = OK, 1 = IOERR, 2 = UNSUPP. Anything but 0 is a
    // device-reported failure.
    let status = unsafe { read_u8(dma_va + STATUS_OFF) };
    if status != VIRTIO_BLK_S_OK {
        log("virtio-blk-probe: FAIL — device status=");
        log_dec(status as u32);
        log(" (expected 0 = VIRTIO_BLK_S_OK)\n");
        return Err(());
    }

    // Verify the signature. The disk image is generated by
    // tools/build.sh disk with the first 16 bytes set to
    // "THYLACINE-DISK-1"; any mismatch indicates either a wrong
    // device, a wrong sector, or DMA buffer not coherent with the
    // device's write (the canonical failure mode of the BURROW_TYPE_DMA
    // arm of userland_demand_page picking the wrong PTE attrs).
    for i in 0..16 {
        let got = unsafe { read_u8(dma_va + DATA_OFF + i) };
        if got != DISK_SIGNATURE[i as usize] {
            log("virtio-blk-probe: FAIL — signature mismatch at offset ");
            log_dec(i as u32);
            log(" (got 0x");
            log_hex(got as u32);
            log(", expected 0x");
            log_hex(DISK_SIGNATURE[i as usize] as u32);
            log(")\n");
            return Err(());
        }
    }

    Ok(())
}

// =============================================================================
// Entry point.
// =============================================================================

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    // Phase 1: claim + map the virtio-mmio bank.
    let mmio_base = match claim_virtio_mmio_bank() {
        Some(va) => va,
        None => {
            log("virtio-blk-probe: FAIL — virtio-mmio bank claim failed\n");
            unsafe { t_exits(1) };
        }
    };

    // Phase 2: scan for the block device.
    let (slot, slot_va) = match find_blk_slot(mmio_base) {
        Some(found) => found,
        None => {
            log("virtio-blk-probe: SKIP — no virtio-mmio slot has DeviceID=2 (no virtio-blk-device wired in QEMU?)\n");
            // Exit 0 = skip; kernel-side guard also short-circuits in
            // this case.
            return 0;
        }
    };

    let intid = VIRTIO_MMIO_GIC_INTID_BASE + slot;

    let version = unsafe { read32(slot_va + REG_VERSION) };
    if version != VIRTIO_MMIO_VERSION_MODERN {
        log("virtio-blk-probe: FAIL — slot reports legacy version (");
        log_dec(version);
        log("); modern (2) required\n");
        unsafe { t_exits(1) };
    }

    // Phase 3: subscribe to the device's IRQ BEFORE enabling the
    // device. This ensures gic_attach + gic_enable_irq are in place
    // by the time the device fires its first completion interrupt.
    let irq_handle = unsafe { t_irq_create(intid, T_RIGHT_SIGNAL) };
    if irq_handle < 0 {
        log("virtio-blk-probe: FAIL — SYS_IRQ_CREATE failed\n");
        unsafe { t_exits(1) };
    }

    // Phase 4: allocate the DMA buffer for virtqueue rings + req/data/status.
    let dma_handle = unsafe {
        t_dma_create(DMA_BUFSIZE, T_RIGHT_READ | T_RIGHT_WRITE | T_RIGHT_MAP)
    };
    if dma_handle < 0 {
        log("virtio-blk-probe: FAIL — SYS_DMA_CREATE failed\n");
        unsafe { t_exits(1) };
    }
    let dma_pa = unsafe {
        t_dma_map(dma_handle, DMA_USER_VA, T_PROT_READ | T_PROT_WRITE)
    };
    if dma_pa < 0 {
        log("virtio-blk-probe: FAIL — SYS_DMA_MAP failed\n");
        unsafe { t_exits(1) };
    }

    // Touch the DMA buffer's pages so userland_demand_page installs
    // the Normal-WB PTEs (BURROW_TYPE_DMA arm). Reading from
    // unaccessed user-VA in a no_std + no_unwind context would
    // page-fault during the device init's first descriptor write
    // otherwise; reading + zeroing here pre-warms the mapping.
    for off in (0..DMA_BUFSIZE).step_by(PAGE_SIZE as usize) {
        unsafe {
            core::ptr::write_volatile((DMA_USER_VA + off) as *mut u8, 0);
        };
    }

    // Phase 5: VirtIO 1.2 initialization sequence.
    if !init_device(slot_va, dma_pa as u64) {
        unsafe { t_exits(1) };
    }

    // Phase 6: submit a read for sector 0 and wait for completion.
    submit_read_sector_0(slot_va, DMA_USER_VA, dma_pa as u64);

    match wait_and_verify(irq_handle, slot_va, DMA_USER_VA) {
        Ok(()) => {
            log("virtio-blk-probe: PASS — slot=");
            log_dec(slot);
            log(" intid=");
            log_dec(intid);
            log(" sig=THYLACINE-DISK-1\n");
            0
        }
        Err(()) => unsafe { t_exits(1) },
    }
}
