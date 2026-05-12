// /virtio-input — third composed userspace driver (P4-K).
//
// Probe-only chunk for the VirtIO INPUT device class (DeviceID = 18).
// Proves the composed-hw-handle SVC substrate (MMIO + DMA, with IRQ
// retained for future event consumption) generalizes to a class with:
//   - a selector-based device-specific config-space (selectors per
//     VIRTIO 1.2 §5.8.4, distinct from blk's direct register layout +
//     net's MAC/status fields),
//   - RX-only mechanics (the device fills the eventq; the driver
//     pre-publishes empty buffers and drains them as events arrive),
//   - 8-byte event records (struct virtio_input_event = u16 type +
//     u16 code + u32 value).
//
// Scope (intentionally narrow):
//   - Init transport: RESET → ACK → DRIVER → DeviceFeatures (require
//     VIRTIO_F_VERSION_1; decline everything else — input devices
//     don't have feature negotiation as elaborate as net/blk) →
//     FEATURES_OK → configure eventq (index 0; QUEUE_SIZE=16; 16 RX
//     descriptors pre-published) → DRIVER_OK.
//   - Read device name via the selector mechanism: select=ID_NAME,
//     subsel=0; read size byte + the first N bytes of u.string;
//     surface in the boot log.
//   - Read EV_BITS for EV_KEY: select=EV_BITS, subsel=EV_KEY=1; if
//     size > 0 the device claims at least one key code. This is the
//     classification signal that distinguishes a keyboard from a
//     mouse without having to consume events.
//   - Exit clean. Do NOT t_irq_wait — QEMU virt has no host-side
//     input injection in v1.0 (would require a -monitor send-key or
//     QMP wiring on every CI run); waiting on an event would hang the
//     test indefinitely.
//
// What this test specifically guards against (beyond the existing
// virtio-blk + virtio-net probes):
//   - DeviceID=18 dispatch in virtio_mmio_find_by_device_id.
//   - Selector-based config-space read path (write select + subsel,
//     observe size byte updated by device, read N bytes of u.string).
//   - eventq (queue 0, RX-direction) configuration: 16 WRITE
//     descriptors with avail.idx=16 published BEFORE DRIVER_OK. The
//     virtio-net-probe configured only TX (queue 1) at P4-Ja; this
//     proves queue 0 RX setup is symmetric to virtio-net-arp's RX
//     setup but at a different device class.
//   - QUEUE_NOTIFY routing for queue index 0 (would be exercised if
//     the device fired an event; we don't drive that path here).
//
// Future P4-K-events:
//   - Add t_irq_create + t_irq_wait; consume drained events from the
//     used ring; validate event type/code matches an injected key.
//   - Requires a CI mechanism for input injection (QMP send-key or
//     -monitor stdin scripting); deferred to Phase 5+ or to the
//     Halcyon-prep work that puts a real keyboard surface online.

#![no_std]
#![no_main]

use core::arch::asm;
use libthyla_rs::{
    T_PROT_READ, T_PROT_WRITE, T_RIGHT_MAP, T_RIGHT_READ, T_RIGHT_WRITE,
    t_dma_create, t_dma_map, t_exits, t_mmio_create, t_mmio_map, t_puts, t_putstr,
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
// User-VA layout (probe-private; distinct from blk/net so a future
// composed test could run multiple probes in parallel without VA
// collision).
// =============================================================================

const MMIO_USER_VA: u64 = 0x0070_0000;
const DMA_USER_VA: u64  = 0x0081_0000;

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
const VIRTIO_DEVICE_ID_INPUT: u32 = 18;

// Status bits (§2.1).
const STATUS_ACKNOWLEDGE: u32 = 1;
const STATUS_DRIVER: u32      = 2;
const STATUS_DRIVER_OK: u32   = 4;
const STATUS_FEATURES_OK: u32 = 8;
const STATUS_FAILED: u32      = 128;

// VIRTIO_F_VERSION_1 = bit 32 → bank-1 bit 0.
const VIRTIO_F_VERSION_1_BIT_BANK1: u32 = 1 << 0;

// =============================================================================
// virtio-input specifics (VIRTIO 1.2 §5.8).
// =============================================================================
//
// Device-specific config space layout (§5.8.4):
//   offset  type    field
//   0       u8      select       (driver-writable selector)
//   1       u8      subsel       (driver-writable sub-selector)
//   2       u8      size         (device-set: size of returned data)
//   3..8    u8[5]   reserved
//   8..136  union   u:
//                      char string[128]      (for NAME / SERIAL)
//                      u8   bitmap[128]      (for PROP_BITS / EV_BITS)
//                      virtio_input_absinfo  (for ABS_INFO)
//                      virtio_input_devids   (for ID_DEVIDS)
//
// Selectors (§5.8.4 "Device Configuration Layout"):
const INPUT_CFG_SELECT: u64 = 0;
const INPUT_CFG_SUBSEL: u64 = 1;
const INPUT_CFG_SIZE: u64   = 2;
const INPUT_CFG_U_BASE: u64 = 8;

const INPUT_CFG_ID_NAME: u8   = 0x01;
const INPUT_CFG_EV_BITS: u8   = 0x11;

// Event types (linux/input-event-codes.h, mirrored by VIRTIO §5.8.6.2):
const EV_SYN: u8 = 0;
const EV_KEY: u8 = 1;
const EV_REL: u8 = 2;

// Virtqueue indices.
const INPUT_QUEUE_EVENT: u32 = 0;

// Event format: struct virtio_input_event {
//     __le16 type;
//     __le16 code;
//     __le32 value;
// } = 8 bytes per record. Each pre-published descriptor points to one.
const VIRTIO_INPUT_EVENT_LEN: u32 = 8;

// =============================================================================
// DMA layout (single 4 KiB page; eventq only).
// =============================================================================
//
//   0x000 .. 0x100   desc[0..16]    (16 × 16 B)
//   0x100 .. 0x200   avail header + ring + used_event (4 + 32 + 2 = 38 B; round up)
//   0x200 .. 0x300   used  header + ring + avail_event (4 + 128 + 2 = 134 B; round up)
//   0x300 .. 0x380   event pool (16 × 8 B = 128 B)
//
// Each eventq descriptor is RX-direction:
//   desc[k].addr  = dma_pa + EVENT_POOL_OFF + k * VIRTIO_INPUT_EVENT_LEN
//   desc[k].len   = VIRTIO_INPUT_EVENT_LEN
//   desc[k].flags = VIRTQ_DESC_F_WRITE
//   desc[k].next  = 0 (unused; no chaining)
//
// avail.ring[k] = k for k in 0..16; avail.idx = 16 BEFORE DRIVER_OK so
// the device has all 16 buffers ready to fill the moment it's allowed
// to issue events.

const QUEUE_SIZE: u16 = 16;
const DMA_BUFSIZE: u64 = PAGE_SIZE;

const DESC_OFF: u64       = 0x000;
const AVAIL_OFF: u64      = 0x100;
const USED_OFF: u64       = 0x200;
const EVENT_POOL_OFF: u64 = 0x300;

// Descriptor flags (§2.7.5).
const VIRTQ_DESC_F_WRITE: u16 = 2;

// Compile-time sanity: event pool fits in the DMA buffer.
const _: () = {
    assert!(EVENT_POOL_OFF + (QUEUE_SIZE as u64) * (VIRTIO_INPUT_EVENT_LEN as u64) <= DMA_BUFSIZE);
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
    core::ptr::write_volatile(addr as *mut u32, val)
}

#[inline(always)]
unsafe fn write16(addr: u64, val: u16) {
    core::ptr::write_volatile(addr as *mut u16, val)
}

#[inline(always)]
unsafe fn write64(addr: u64, val: u64) {
    core::ptr::write_volatile(addr as *mut u64, val)
}

#[inline(always)]
unsafe fn read_u8(addr: u64) -> u8 {
    core::ptr::read_volatile(addr as *const u8)
}

#[inline(always)]
unsafe fn write_u8(addr: u64, val: u8) {
    core::ptr::write_volatile(addr as *mut u8, val)
}

#[inline(always)]
fn dsb_sy() {
    unsafe { asm!("dsb sy", options(nostack, preserves_flags)) }
}

// =============================================================================
// Diagnostics — integer formatters mirror virtio-blk-probe / -net-probe.
// =============================================================================
//
// R12-uaccess: SYS_PUTS demand-pages user VAs from kernel mode via the
// uaccess-fault dispatcher; binaries no longer need pretouch_rodata_pages.

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
            log("virtio-input: SYS_MMIO_CREATE failed for page ");
            log_dec(i as u32);
            log("\n");
            return None;
        }
        let map_rc = unsafe { t_mmio_map(handle, va, prot) };
        if map_rc < 0 {
            log("virtio-input: SYS_MMIO_MAP failed for page ");
            log_dec(i as u32);
            log("\n");
            return None;
        }
    }
    Some(MMIO_USER_VA)
}

fn find_input_slot(mmio_base_va: u64) -> Option<(u32, u64)> {
    for slot in 0..VIRTIO_MMIO_NUM_SLOTS {
        let slot_va = mmio_base_va + slot * VIRTIO_MMIO_SLOT_STRIDE;
        let magic = unsafe { read32(slot_va + REG_MAGIC_VALUE) };
        if magic != VIRTIO_MMIO_MAGIC { continue; }
        let dev_id = unsafe { read32(slot_va + REG_DEVICE_ID) };
        if dev_id == VIRTIO_DEVICE_ID_INPUT {
            return Some((slot as u32, slot_va));
        }
    }
    None
}

// =============================================================================
// VirtIO 1.2 device initialization for the INPUT device.
// =============================================================================

fn init_device(slot_va: u64, dma_pa: u64, dma_va: u64) -> bool {
    // Step 1: RESET.
    unsafe { write32(slot_va + REG_STATUS, 0) };

    // Step 2: ACKNOWLEDGE.
    unsafe { write32(slot_va + REG_STATUS, STATUS_ACKNOWLEDGE) };

    // Step 3: DRIVER.
    unsafe { write32(slot_va + REG_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER) };

    // Step 4: read DeviceFeatures; require VIRTIO_F_VERSION_1 in bank 1.
    // input devices in VIRTIO 1.2 §5.8 don't define any device-specific
    // feature bits; bank 0 is all-zero in practice on QEMU. We decline
    // everything in bank 0 and accept only VERSION_1 in bank 1.
    unsafe { write32(slot_va + REG_DEVICE_FEATURES_SEL, 1) };
    let dev_feat_hi = unsafe { read32(slot_va + REG_DEVICE_FEATURES) };
    if dev_feat_hi & VIRTIO_F_VERSION_1_BIT_BANK1 == 0 {
        log("virtio-input: device lacks VIRTIO_F_VERSION_1\n");
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
        log("virtio-input: FEATURES_OK rejected; status=");
        log_hex(status);
        log("\n");
        return false;
    }

    // Step 6: configure eventq (index 0).
    unsafe { write32(slot_va + REG_QUEUE_SEL, INPUT_QUEUE_EVENT) };

    let num_max = unsafe { read32(slot_va + REG_QUEUE_NUM_MAX) };
    if num_max < QUEUE_SIZE as u32 {
        log("virtio-input: QueueNumMax(eventq)=");
        log_dec(num_max);
        log(" below QUEUE_SIZE\n");
        unsafe { write32(slot_va + REG_STATUS, STATUS_FAILED) };
        return false;
    }
    unsafe { write32(slot_va + REG_QUEUE_NUM, QUEUE_SIZE as u32) };

    let desc_pa = dma_pa + DESC_OFF;
    let avail_pa = dma_pa + AVAIL_OFF;
    let used_pa = dma_pa + USED_OFF;
    unsafe {
        write32(slot_va + REG_QUEUE_DESC_LOW,    (desc_pa & 0xFFFF_FFFF) as u32);
        write32(slot_va + REG_QUEUE_DESC_HIGH,   (desc_pa >> 32) as u32);
        write32(slot_va + REG_QUEUE_DRIVER_LOW,  (avail_pa & 0xFFFF_FFFF) as u32);
        write32(slot_va + REG_QUEUE_DRIVER_HIGH, (avail_pa >> 32) as u32);
        write32(slot_va + REG_QUEUE_DEVICE_LOW,  (used_pa & 0xFFFF_FFFF) as u32);
        write32(slot_va + REG_QUEUE_DEVICE_HIGH, (used_pa >> 32) as u32);
    };

    // Step 6b: populate the eventq with 16 RX descriptors pointing at
    // 16 distinct 8-byte slots in the event pool. avail.ring[k] = k;
    // avail.idx = 16 published BEFORE DRIVER_OK so the device has all
    // buffers from the moment events become legal.
    populate_eventq(dma_va, dma_pa);
    unsafe { write32(slot_va + REG_QUEUE_READY, 1) };

    // Step 7: DRIVER_OK.
    unsafe {
        write32(slot_va + REG_STATUS,
                STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK | STATUS_DRIVER_OK);
    };

    true
}

fn populate_eventq(dma_va: u64, dma_pa: u64) {
    let desc_va = dma_va + DESC_OFF;
    let avail_va = dma_va + AVAIL_OFF;
    let event_pool_pa = dma_pa + EVENT_POOL_OFF;

    // Zero the event pool so a stale-read from desc[0] would be
    // obviously zero rather than uninitialized.
    for i in 0..(QUEUE_SIZE as u64) * (VIRTIO_INPUT_EVENT_LEN as u64) {
        unsafe { write_u8(dma_va + EVENT_POOL_OFF + i, 0) };
    }

    for k in 0..QUEUE_SIZE {
        let d_off = DESC_OFF + (k as u64) * 16;
        let buf_pa = event_pool_pa + (k as u64) * (VIRTIO_INPUT_EVENT_LEN as u64);
        unsafe {
            write64(dma_va + d_off + 0, buf_pa);
            write32(dma_va + d_off + 8, VIRTIO_INPUT_EVENT_LEN);
            write16(dma_va + d_off + 12, VIRTQ_DESC_F_WRITE);
            write16(dma_va + d_off + 14, 0); // no chaining
        };
        unsafe { write16(avail_va + 4 + (k as u64) * 2, k) };
        let _ = desc_va; // silence unused-binding when EVENT_POOL_OFF moves
    }

    unsafe { write16(avail_va + 0, 0) }; // flags
    dsb_sy();
    unsafe { write16(avail_va + 2, QUEUE_SIZE) }; // idx = 16
    dsb_sy();
}

// =============================================================================
// Selector-based config-space read helpers.
// =============================================================================
//
// Per VIRTIO 1.2 §5.8.5: after the driver writes select + subsel, the
// device updates `size` and the union to the requested data on the
// next read. We follow the convention of writing select/subsel +
// reading size, then reading min(size, max_buf) bytes from u.
//
// `read_str_field` returns the actual length (after the device's `size`
// byte clamps to <= 128) and fills `out` with up to N bytes.

fn write_selectors(slot_va: u64, select: u8, subsel: u8) {
    unsafe {
        write_u8(slot_va + REG_CONFIG_BASE + INPUT_CFG_SELECT, select);
        write_u8(slot_va + REG_CONFIG_BASE + INPUT_CFG_SUBSEL, subsel);
    };
    // QEMU's virtio-input device updates `size` synchronously on the
    // subsel write; no DSB needed here (device-side MMIO is strictly
    // ordered by the bus model).
}

fn read_cfg_size(slot_va: u64) -> u8 {
    unsafe { read_u8(slot_va + REG_CONFIG_BASE + INPUT_CFG_SIZE) }
}

fn read_cfg_byte(slot_va: u64, idx: u64) -> u8 {
    unsafe { read_u8(slot_va + REG_CONFIG_BASE + INPUT_CFG_U_BASE + idx) }
}

fn read_name(slot_va: u64, out: &mut [u8; 64]) -> usize {
    write_selectors(slot_va, INPUT_CFG_ID_NAME, 0);
    let size = read_cfg_size(slot_va) as usize;
    let copy_len = if size > out.len() { out.len() } else { size };
    for i in 0..copy_len {
        out[i] = read_cfg_byte(slot_va, i as u64);
    }
    copy_len
}

fn read_ev_bits_count(slot_va: u64, ev_type: u8) -> u8 {
    write_selectors(slot_va, INPUT_CFG_EV_BITS, ev_type);
    read_cfg_size(slot_va)
}

fn log_name(buf: &[u8], n: usize) {
    let mut printable = n;
    while printable > 0 && buf[printable - 1] == 0 {
        printable -= 1;
    }
    if printable > 0 {
        unsafe { t_puts(buf.as_ptr(), printable) };
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
            log("virtio-input: FAIL — virtio-mmio bank claim failed\n");
            unsafe { t_exits(1) };
        }
    };

    let (slot, slot_va) = match find_input_slot(mmio_base) {
        Some(found) => found,
        None => {
            log("virtio-input: SKIP — no virtio-mmio slot has DeviceID=18 (no -device virtio-keyboard-device wired in QEMU?)\n");
            return 0;
        }
    };

    let intid = VIRTIO_MMIO_GIC_INTID_BASE + slot;

    let version = unsafe { read32(slot_va + REG_VERSION) };
    if version != VIRTIO_MMIO_VERSION_MODERN {
        log("virtio-input: FAIL — slot reports legacy version (");
        log_dec(version);
        log("); modern (2) required\n");
        unsafe { t_exits(1) };
    }

    let dma_handle = unsafe {
        t_dma_create(DMA_BUFSIZE, T_RIGHT_READ | T_RIGHT_WRITE | T_RIGHT_MAP)
    };
    if dma_handle < 0 {
        log("virtio-input: FAIL — SYS_DMA_CREATE failed\n");
        unsafe { t_exits(1) };
    }
    let dma_pa = unsafe {
        t_dma_map(dma_handle, DMA_USER_VA, T_PROT_READ | T_PROT_WRITE)
    };
    if dma_pa < 0 {
        log("virtio-input: FAIL — SYS_DMA_MAP failed\n");
        unsafe { t_exits(1) };
    }

    // Pre-warm DMA pages so the demand-page path installs PTEs before
    // the device starts writing events into the buffer pool.
    for off in (0..DMA_BUFSIZE).step_by(PAGE_SIZE as usize) {
        unsafe { write_u8(DMA_USER_VA + off, 0) };
    }

    if !init_device(slot_va, dma_pa as u64, DMA_USER_VA) {
        unsafe { t_exits(1) };
    }

    // Read device identity from config space + classify by EV_BITS.
    let mut name_buf = [0u8; 64];
    let name_len = read_name(slot_va, &mut name_buf);
    let key_bits = read_ev_bits_count(slot_va, EV_KEY);
    let rel_bits = read_ev_bits_count(slot_va, EV_REL);
    let _syn_bits = read_ev_bits_count(slot_va, EV_SYN);

    log("virtio-input: slot=");
    log_dec(slot);
    log(" intid=");
    log_dec(intid);
    log(" name=\"");
    log_name(&name_buf, name_len);
    log("\" name_len=");
    log_dec(name_len as u32);
    log(" key_bits=");
    log_dec(key_bits as u32);
    log(" rel_bits=");
    log_dec(rel_bits as u32);
    log("\n");

    if name_len == 0 {
        log("virtio-input: FAIL — device name is empty (config-space selector mechanism not responding)\n");
        unsafe { t_exits(1) };
    }

    // Classification: at least one EV_KEY bit means this device emits
    // key events (keyboard or button-bearing pointer device). QEMU's
    // virtio-keyboard-device claims a wide swath of EV_KEY codes.
    if key_bits == 0 && rel_bits == 0 {
        log("virtio-input: FAIL — device claims neither EV_KEY nor EV_REL (not a keyboard or mouse)\n");
        unsafe { t_exits(1) };
    }

    log("virtio-input: PASS — config-space + eventq init reached DRIVER_OK (slot=");
    log_dec(slot);
    log(" intid=");
    log_dec(intid);
    log(")\n");

    0
}
