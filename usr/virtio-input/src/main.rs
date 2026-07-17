// /virtio-input — third composed userspace driver.
//
// Two-stage scope:
//
//   P4-K (probe; previously landed): RESET → DRIVER_OK → read NAME +
//   EV_BITS for classification. Proves the composed-hw-handle SVC
//   substrate generalizes to DeviceID=18 with selector-based
//   config-space + RX-only eventq.
//
//   P4-K-events (this chunk): adds event consumption via t_irq_create
//   + bounded poll on `used.idx`. The kernel test pre-fires SPI 77
//   before spawning the child (P4-Ic-latency / P4-Ic5-IRQ-probe
//   pre-pend pattern); the child consumes that one wake via a single
//   t_irq_wait, then transitions to a busy-poll on `used.idx`. If a
//   host-side input event (delivered via QMP `send-key` from
//   tools/test.sh) lands, the driver drains the used ring, parses the
//   8-byte virtio_input_event records, and looks for the target
//   `{type=EV_KEY, code=KEY_A=30, value=1}` (key-press). On match:
//   PASS exit 0. On poll cap exhaustion: SKIP exit 0 (boot continues,
//   no hang on interactive tools/run-vm.sh that lacks QMP injection).
//
// What this chunk specifically guards against (beyond the probe):
//   - eventq RX drain: used.ring[k % QUEUE_SIZE] = desc_id; the
//     8-byte event lives at desc_pool[desc_id]. Per VIRTIO 1.2
//     §2.7.13, the desc_id is the head of the chain — for virtio-input
//     all descriptors are single (no chaining) so head == id.
//   - Descriptor recycling: after consuming each used entry, the
//     driver re-publishes the same desc_id back to avail.ring[k %
//     QUEUE_SIZE], bumps avail.idx, DSB. Symmetric to virtio-net-loop
//     RX recycling.
//   - Event record parsing: u16 type + u16 code + u32 value. Pinned
//     against drift by reading at fixed byte offsets.
//   - InterruptStatus ACK discipline: each wake from t_irq_wait reads
//     INTERRUPT_STATUS + writes INTERRUPT_ACK with the same bits, so
//     the device's interrupt line deasserts before the next event
//     batch.
//   - SKIP-on-poll-exhaustion: a wall-clock poll budget (#362;
//     POLL_BUDGET_MS via CNTVCT_EL0, with an iteration backstop per
//     the #188 discipline) ensures the driver terminates even without
//     QMP injection. Boot proceeds; the test framework's status==0
//     contract is preserved.
//
// CI verification: `tools/test.sh` spawns a QMP injector
// (tools/qmp-inject-key.py) at QEMU launch; it connects to QMP during
// boot, tail-follows the boot log for the sentinel "virtio-input:
// AWAITING_QMP_KEY", and fires `send-key` for `a` within ~25 ms of the
// sentinel. The driver then sees the EV_KEY/30/1 event in the eventq
// and prints "virtio-input: saw target key". `tools/test.sh` greps
// the log for that line after boot completes and fails the run if
// THYLACINE_INPUT_INJECT != 0 and the line is absent.

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

// Linux key codes (linux/input-event-codes.h). Used by QEMU's qcode
// mapping: send-key with qcode "a" delivers EV_KEY/KEY_A/value.
const KEY_A: u16 = 30;

// Virtqueue indices.
const INPUT_QUEUE_EVENT: u32 = 0;

// Event format: struct virtio_input_event {
//     __le16 type;
//     __le16 code;
//     __le32 value;
// } = 8 bytes per record.
const VIRTIO_INPUT_EVENT_LEN: u32 = 8;

// VIRTIO 1.2 §4.2.5 InterruptStatus bits.
const INT_USED_BUFFER:  u32 = 1 << 0;
const INT_CONFIG_CHANGE: u32 = 1 << 1;

// =============================================================================
// DMA layout (single 4 KiB page; eventq only).
// =============================================================================
//
//   0x000 .. 0x100   desc[0..16]    (16 × 16 B)
//   0x100 .. 0x200   avail header + ring + used_event (4 + 32 + 2 = 38 B)
//   0x200 .. 0x300   used  header + ring + avail_event (4 + 128 + 2 = 134 B)
//   0x300 .. 0x380   event pool (16 × 8 B = 128 B)

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
// Event-consumption loop tuning.
// =============================================================================
//
// Poll budget (the #188 wall-clock pattern; #362). The busy-poll over
// `used.idx` is WALL-CLOCK-bounded via CNTVCT_EL0, not iteration-
// bounded: an iteration count is substrate-speed-dependent (HVF-native
// runs ~10-50x more iterations per second than TCG), so a cap sized
// for TCG under-sizes the window on HVF to well under a second --
// exactly when the host-side injector is slowest (ci-smp-gate load:
// 4-8 vCPU threads starving host-side helpers; 23/40 boots missed the
// key, #362). POLL_BUDGET_MS is the SKIP-path cost on injector-less
// interactive boots; the PASS path exits as soon as the key lands.
//
// MAX_POLL_ITER is the UNCONDITIONAL iteration backstop (the #188
// discipline: a frozen/misconfigured counter must not hang the loop) --
// sized well above the budget on the fastest substrate, never the
// working bound. CNTFRQ_EL0 == 0 falls back to a pure iteration cap
// (FREQ0_FALLBACK_ITER, the pre-#362 value).
//
// MAX_EVENTS_TO_PROCESS: drains at most this many events even if more
// arrive. Each QMP `send-key` produces 4 events (KEY press + SYN +
// KEY release + SYN); 32 leaves headroom for a few extra injections
// (the #362 injector re-sends once if the success marker lags) without
// unbounded looping.

const POLL_BUDGET_MS: u64 = 3_000;
const MAX_POLL_ITER: u64 = 4_000_000_000;
const FREQ0_FALLBACK_ITER: u64 = 200_000_000;
const CLOCK_CHECK_MASK: u64 = 0xFFF; // read CNTVCT every 4096 iterations
const MAX_EVENTS_TO_PROCESS: u32 = 32;

// =============================================================================
// Volatile MMIO + DMA access helpers.
// =============================================================================

#[inline(always)]
unsafe fn read32(addr: u64) -> u32 { libthyla_rs::hardware::mmio_read32(addr) }

#[inline(always)]
unsafe fn write32(addr: u64, val: u32) { libthyla_rs::hardware::mmio_write32(addr, val) }

#[inline(always)]
unsafe fn read16(addr: u64) -> u16 { libthyla_rs::hardware::mmio_read16(addr) }

#[inline(always)]
unsafe fn write16(addr: u64, val: u16) { libthyla_rs::hardware::mmio_write16(addr, val) }

#[inline(always)]
unsafe fn write64(addr: u64, val: u64) { libthyla_rs::hardware::mmio_write64(addr, val) }

#[inline(always)]
unsafe fn read_u8(addr: u64) -> u8 { libthyla_rs::hardware::mmio_read8(addr) }

#[inline(always)]
unsafe fn write_u8(addr: u64, val: u8) { libthyla_rs::hardware::mmio_write8(addr, val) }

#[inline(always)]
fn dsb_sy() {
    unsafe { asm!("dsb sy", options(nostack, preserves_flags)) }
}

#[inline(always)]
fn yield_cpu() {
    unsafe { asm!("yield", options(nostack, preserves_flags)) }
}

// EL0 counter reads (EL0VCTEN is enabled kernel-side; the irq-bench
// idiom). The isb orders the read against prior speculation -- cheap at
// the CLOCK_CHECK_MASK cadence.
#[inline(always)]
fn read_cntvct() -> u64 {
    let v: u64;
    unsafe {
        asm!("isb", "mrs {0}, cntvct_el0", out(reg) v,
             options(nomem, nostack, preserves_flags));
    }
    v
}

#[inline(always)]
fn read_cntfrq() -> u64 {
    let v: u64;
    unsafe {
        asm!("mrs {0}, cntfrq_el0", out(reg) v,
             options(nomem, nostack, preserves_flags));
    }
    v
}

// =============================================================================
// Diagnostics — integer formatters mirror virtio-blk-probe / -net-probe.
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
    unsafe { write32(slot_va + REG_STATUS, 0) };
    unsafe { write32(slot_va + REG_STATUS, STATUS_ACKNOWLEDGE) };
    unsafe { write32(slot_va + REG_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER) };

    // VIRTIO 1.2 3.1.1 step 4: read bank-0 device features before bank-1.
    // Some backends treat a missing bank-0 read as a protocol error.
    unsafe { write32(slot_va + REG_DEVICE_FEATURES_SEL, 0) };
    let _dev_feat_lo = unsafe { read32(slot_va + REG_DEVICE_FEATURES) };
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

    populate_eventq(dma_va, dma_pa);
    unsafe { write32(slot_va + REG_QUEUE_READY, 1) };

    unsafe {
        write32(slot_va + REG_STATUS,
                STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK | STATUS_DRIVER_OK);
    };

    true
}

fn populate_eventq(dma_va: u64, dma_pa: u64) {
    let avail_va = dma_va + AVAIL_OFF;
    let event_pool_pa = dma_pa + EVENT_POOL_OFF;

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
    }

    unsafe { write16(avail_va + 0, 0) }; // flags
    dsb_sy();
    unsafe { write16(avail_va + 2, QUEUE_SIZE) }; // idx = 16
    dsb_sy();
}

// =============================================================================
// Selector-based config-space read helpers.
// =============================================================================

fn write_selectors(slot_va: u64, select: u8, subsel: u8) {
    unsafe {
        write_u8(slot_va + REG_CONFIG_BASE + INPUT_CFG_SELECT, select);
        write_u8(slot_va + REG_CONFIG_BASE + INPUT_CFG_SUBSEL, subsel);
    };
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
// Event drain.
// =============================================================================
//
// Reads `used.idx`; for each new entry since `last_used_idx`, parses
// the 8-byte event record at the descriptor's backing slot, logs the
// (type, code, value) tuple, and re-publishes the descriptor head back
// to the avail ring so the device can refill it.
//
// Returns the number of events consumed + whether the target key
// was observed in this batch.

struct DrainResult {
    consumed: u32,
    saw_target: bool,
    new_last_used_idx: u16,
    new_avail_idx: u16,
}

fn drain_used(dma_va: u64, last_used_idx: u16, mut avail_idx: u16,
              mut total_consumed: u32) -> DrainResult {
    let used_va = dma_va + USED_OFF;
    let avail_va = dma_va + AVAIL_OFF;
    let event_pool_va = dma_va + EVENT_POOL_OFF;

    let avail_idx_at_entry = avail_idx;
    let cur_used_idx = unsafe { read16(used_va + 2) };
    // VIRTIO 1.2 §2.7.13.2: barrier between observing used.idx advance
    // and reading used.ring[k] / the event pool the descriptor backed.
    // Without it, an out-of-order ARM core may speculate event-pool
    // reads before the used.idx load, returning pre-advance bytes (in
    // virtio-input that surfaces as a phantom EV_SYN classification —
    // zero type/code/value because populate_eventq pre-zeroed the
    // event slots — silently dropping the real key event).
    virtio_rmb();
    let mut idx = last_used_idx;
    let mut saw_target = false;

    while idx != cur_used_idx && total_consumed < MAX_EVENTS_TO_PROCESS {
        let slot = (idx % QUEUE_SIZE) as u64;
        // struct virtq_used_elem { le32 id; le32 len; } at used + 4 + slot*8
        let elem_va = used_va + 4 + slot * 8;
        let desc_id = unsafe { read32(elem_va + 0) };
        let used_len = unsafe { read32(elem_va + 4) };

        if desc_id >= QUEUE_SIZE as u32 {
            // Malformed used entry — desc_id out of range. Compare the FULL
            // u32 (a `desc_id as u16` truncation would let 0x1_0000 pass the
            // bound, then index event_pool at desc_id*8 = 512 KiB OOB).
            // Log + skip.
            log("virtio-input: WARN — used.elem.id out of range: ");
            log_dec(desc_id);
            log("\n");
            idx = idx.wrapping_add(1);
            total_consumed += 1;
            continue;
        }
        if used_len != VIRTIO_INPUT_EVENT_LEN {
            // Malformed used entry — len != 8 bytes (one full event).
            // QEMU's virtio-input-pci always writes 8; a host that
            // writes 0 or a non-multiple-of-8 here is mis-protocol.
            log("virtio-input: WARN — used.elem.len != 8: ");
            log_dec(used_len);
            log("\n");
            idx = idx.wrapping_add(1);
            total_consumed += 1;
            continue;
        }

        let evt_va = event_pool_va + (desc_id as u64) * (VIRTIO_INPUT_EVENT_LEN as u64);
        let evt_type = unsafe { read16(evt_va + 0) };
        let evt_code = unsafe { read16(evt_va + 2) };
        let evt_value = unsafe { read32(evt_va + 4) };

        log("virtio-input: event type=");
        log_dec(evt_type as u32);
        log(" code=");
        log_dec(evt_code as u32);
        log(" value=");
        log_dec(evt_value);
        log("\n");

        if evt_type == EV_KEY as u16 && evt_code == KEY_A && evt_value == 1 {
            saw_target = true;
        }

        // Recycle: re-publish desc_id back to avail.ring at slot
        // (avail_idx % QUEUE_SIZE). The device cycles through avail
        // entries as its internal index advances; bumping avail.idx
        // makes the descriptor available again.
        let avail_slot = (avail_idx % QUEUE_SIZE) as u64;
        unsafe { write16(avail_va + 4 + avail_slot * 2, desc_id as u16) };
        avail_idx = avail_idx.wrapping_add(1);

        idx = idx.wrapping_add(1);
        total_consumed += 1;
    }

    if avail_idx != avail_idx_at_entry {
        // Make recycled descriptor publications visible before the
        // idx bump (VIRTIO 1.2 §2.7.13.1). The captured entry value is
        // the source of truth — comparing against `read16(avail_va + 2)`
        // would work today (single-thread driver, no other writer) but
        // becomes racy under any future multi-threaded refactor.
        dsb_sy();
        unsafe { write16(avail_va + 2, avail_idx) };
        dsb_sy();
        // No notify needed — virtio-input's eventq has no notify-on-RX
        // semantic; the device polls avail.idx internally as it fills.
    }

    DrainResult {
        consumed: total_consumed,
        saw_target,
        new_last_used_idx: idx,
        new_avail_idx: avail_idx,
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

    // Subscribe to the IRQ BEFORE issuing the request. The kernel test
    // pre-pended SPI 77 before spawning us (P4-Ic-latency / P4-K-events
    // pattern); the moment t_irq_create enables the SPI, the GIC
    // delivers the pre-pended pending → kobj_irq_dispatch wakes any
    // subsequent t_irq_wait without sleeping.
    let irq_handle = unsafe { t_irq_create(intid, T_RIGHT_SIGNAL) };
    if irq_handle < 0 {
        log("virtio-input: FAIL — SYS_IRQ_CREATE failed\n");
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
    if key_bits == 0 && rel_bits == 0 {
        log("virtio-input: FAIL — device claims neither EV_KEY nor EV_REL (not a keyboard or mouse)\n");
        unsafe { t_exits(1) };
    }

    // Consume the kernel-prefired wake. This is iteration 0: the GIC
    // delivered a pending SPI at t_irq_create time; pending_count is
    // already > 0 so t_irq_wait returns without sleeping. We ACK the
    // device-side InterruptStatus (clearing any latched device bits)
    // and confirm used.idx == 0 (no events yet — kernel-side pre-fire
    // didn't go through the device).
    let prefire_count = unsafe { t_irq_wait(irq_handle) };
    if prefire_count < 0 {
        // Defensive log: handle is fresh from t_irq_create above, so
        // this shouldn't happen, but a corrupted handle table or
        // missing RIGHT_SIGNAL would surface here as a silent stall
        // otherwise. The busy-poll below proceeds either way since it
        // reads used.idx directly, but surfacing the anomaly aids
        // debugging.
        log("virtio-input: WARN — pre-fire t_irq_wait returned error\n");
    }
    let int_status = unsafe { read32(slot_va + REG_INTERRUPT_STATUS) };
    if int_status != 0 {
        unsafe { write32(slot_va + REG_INTERRUPT_ACK, int_status) };
    }
    let _ = INT_USED_BUFFER;     // referenced for symmetry with other drivers
    let _ = INT_CONFIG_CHANGE;   // ditto; busy-poll path doesn't branch on bit shape

    // Sentinel: tools/test.sh polls for this line and triggers QMP
    // `send-key` upon match. The interactive `tools/run-vm.sh` path
    // also prints it (but no injector is listening, so the busy-poll
    // below will time out cleanly into SKIP).
    log("virtio-input: AWAITING_QMP_KEY\n");

    // Busy-poll on `used.idx`, wall-clock-bounded (#362; see the
    // POLL_BUDGET_MS comment). The DMA region is Normal-WB so volatile
    // reads are cheap. ARM `yield` hints the CPU/scheduler to pace
    // itself. The clock starts AFTER the sentinel print -- the sentinel
    // is what triggers the host-side injector, so the budget counts
    // from the moment delivery becomes possible.
    let freq = read_cntfrq();
    let deadline = read_cntvct()
        .wrapping_add(freq.saturating_mul(POLL_BUDGET_MS) / 1000);
    let iter_cap: u64 = if freq == 0 { FREQ0_FALLBACK_ITER } else { MAX_POLL_ITER };

    let mut last_used_idx: u16 = 0;
    let mut avail_idx_local: u16 = QUEUE_SIZE; // matches populate_eventq's avail.idx = 16
    let mut total_consumed: u32 = 0;
    let mut saw_target = false;
    let mut iters: u64 = 0;

    while iters < iter_cap && !saw_target
          && total_consumed < MAX_EVENTS_TO_PROCESS {
        let cur_used = unsafe { read16(DMA_USER_VA + USED_OFF + 2) };
        if cur_used != last_used_idx {
            // ACK any device-asserted interrupt bits so the line
            // deasserts before the next batch (the device sets bit 0
            // on each used.idx advance).
            let int_status = unsafe { read32(slot_va + REG_INTERRUPT_STATUS) };
            if int_status != 0 {
                unsafe { write32(slot_va + REG_INTERRUPT_ACK, int_status) };
            }

            let r = drain_used(DMA_USER_VA, last_used_idx, avail_idx_local,
                               total_consumed);
            last_used_idx = r.new_last_used_idx;
            avail_idx_local = r.new_avail_idx;
            total_consumed = r.consumed;
            if r.saw_target { saw_target = true; }
            continue;
        }
        iters += 1;
        // Wrap-safe signed comparison; checked every 4096 idle
        // iterations so the hot poll stays two volatile reads + yield.
        if freq != 0 && (iters & CLOCK_CHECK_MASK) == 0
           && (read_cntvct().wrapping_sub(deadline) as i64) >= 0 {
            break;
        }
        yield_cpu();
    }

    if saw_target {
        log("virtio-input: saw target key (EV_KEY code=");
        log_dec(KEY_A as u32);
        log(" value=1)\n");
        log("virtio-input: PASS — event consumption end-to-end (slot=");
        log_dec(slot);
        log(" intid=");
        log_dec(intid);
        log(" events=");
        log_dec(total_consumed);
        log(")\n");
        0
    } else {
        // SKIP path: no QMP-injected key arrived within the poll
        // window. This is expected when running `tools/run-vm.sh`
        // interactively without a QMP injector; the kernel test
        // accepts exit-0 either way and a separate post-boot grep in
        // `tools/test.sh` enforces PASS when injection is expected.
        log("virtio-input: SKIP — no EV_KEY/");
        log_dec(KEY_A as u32);
        log("/1 observed within the ");
        log_dec(POLL_BUDGET_MS as u32);
        log(" ms poll budget (events_seen=");
        log_dec(total_consumed);
        log("); QMP injection likely absent (tools/run-vm.sh interactive run, or THYLACINE_INPUT_INJECT=0)\n");
        0
    }
}
