// /virtio-net-loop — steady-state RX recycling + TX descriptor reuse (P4-Jc).
//
// Successor to virtio-net-arp (P4-Jb). Closes the steady-state gap left
// open by the single-shot probe: each consumed RX buffer is re-published
// to the avail ring after the frame is processed, and the TX side uses
// a static descriptor[k] → buffer[k] mapping that the device round-
// robins through as we advance avail.idx. This is the descriptor
// discipline a real continuous-RX driver (and 9P-over-TCP in Phase 5)
// will need.
//
// What this proves beyond virtio-net-arp:
//   - RX descriptor recycling: 24 ARP replies flow through 16 RX
//     buffers; descriptor 0 is consumed, re-published with a wrapping
//     bump of rx_avail_idx, then re-used by the device on the next
//     reply. Demonstrates the wait/recycle cycle that any production
//     driver needs.
//   - TX descriptor reuse: 24 outbound ARP requests through 16 TX
//     descriptors; descriptor k = tx_avail_idx % 16, so wraparound
//     past 16 hits descriptor 0 again. The driver gates new sends on
//     `tx_avail_idx.wrapping_sub(tx_seen_used) < QUEUE_SIZE` so we
//     never re-use a descriptor still in flight.
//   - u16 wraparound on both avail/used idx counters via wrapping_add
//     + `!=` comparison (per VIRTIO 1.2 §2.6 the idx fields are u16
//     monotonic with implicit wraparound at 65536).
//   - IRQ batching: a single IRQ wake can carry many TX completions +
//     many RX deliveries; the drain loop processes all of them, then
//     reloads the TX side if room, then waits for the next IRQ.
//
// Same composed hw-handle SVC surface as virtio-net-arp:
//   - t_mmio_create + t_mmio_map (one bank)
//   - t_dma_create  + t_dma_map  (two allocations: 4 KiB ring +
//                                  32 KiB RX pool)
//   - t_irq_create  + t_irq_wait
//
// The whole point: virtio-net-arp proves single-shot mechanics;
// virtio-net-loop proves the discipline scales to continuous RX.
// 9P-over-TCP in Phase 5 = many requests/responses per second; this
// is the descriptor-recycling foundation that path will sit on.

#![no_std]
#![no_main]

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

const VIRTIO_MMIO_GIC_INTID_BASE: u32 = 32 + 16; // = 48

// =============================================================================
// User-VA layout.
// =============================================================================

const MMIO_USER_VA: u64 = 0x0050_0000;
const RING_DMA_USER_VA: u64 = 0x0061_0000;
const RXPOOL_DMA_USER_VA: u64 = 0x0062_0000;

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
const REG_CONFIG_BASE: u64 = 0x100;

const VIRTIO_MMIO_MAGIC: u32 = 0x7472_6976;
const VIRTIO_MMIO_VERSION_MODERN: u32 = 2;
const VIRTIO_DEVICE_ID_NET: u32 = 1;

const STATUS_ACKNOWLEDGE: u32 = 1;
const STATUS_DRIVER: u32      = 2;
const STATUS_DRIVER_OK: u32   = 4;
const STATUS_FEATURES_OK: u32 = 8;
const STATUS_FAILED: u32      = 128;

const VIRTIO_F_VERSION_1_BIT_BANK1: u32 = 1 << 0;

// =============================================================================
// virtio-net specifics.
// =============================================================================

const VIRTIO_NET_F_MAC_BIT: u32    = 1 << 5;
const VIRTIO_NET_F_STATUS_BIT: u32 = 1 << 16;

const NET_CFG_MAC: u64    = 0;
const NET_CFG_STATUS: u64 = 6;

const VIRTIO_NET_S_LINK_UP: u16 = 1 << 0;

const NET_QUEUE_RX: u32 = 0;
const NET_QUEUE_TX: u32 = 1;

const VIRTQ_DESC_F_WRITE: u16 = 2;

const VIRTIO_NET_HDR_LEN: u32 = 12;

// =============================================================================
// Steady-state knobs.
// =============================================================================
//
// QUEUE_SIZE = 16 on both queues. N_TARGET = 24 round-trips guarantees:
//   - TX side: descriptors 0..15 get used once, then wraparound recycles
//     descriptors 0..7 a second time.
//   - RX side: 24 inbound frames through 16 buffers; descriptors 0..7
//     get re-published once after their first frame is consumed.
//
// The 1.5× ratio is the smallest value that meaningfully exercises
// wrap on both sides without making the test slow.

const QUEUE_SIZE: u16 = 16;
const N_TARGET: u32 = 24;

// Defensive upper bound on the IRQ-wait loop. Slirp delivers each
// reply within tens of microseconds; the whole 24-round-trip sequence
// completes well under 100 ms on QEMU. 256 iterations is the
// belt-and-suspenders ceiling.
const MAX_IRQ_ITERATIONS: u32 = 256;

const RING_DMA_SIZE: u64 = PAGE_SIZE;          // 4 KiB
const RXPOOL_DMA_SIZE: u64 = 32 * 1024;        // 16 × 2 KiB
const RX_BUF_LEN: u32 = 2048;

// DMA layout (4 KiB ring):
//   0x000  TX desc table (16 × 16 B = 256 B)
//   0x100  TX avail ring (4 + 2*16 = 36 B)
//   0x200  TX used ring (4 + 8*16 = 132 B)
//   0x400  RX desc table (256 B)
//   0x500  RX avail ring
//   0x600  RX used ring
//   0x700  TX buffer pool (16 × 64 B = 1024 B); spans [0x700..0xB00)
//
// Each TX buffer at TX_BUF_POOL_OFF + k * TX_BUF_STRIDE contains:
//   [virtio_net_hdr (12 B)][Eth hdr (14 B)][ARP (28 B)] = 54 B, padded
//   to 64 B for alignment. Descriptor k always points to buffer k;
//   the device cycles through descriptors as avail.idx advances. The
//   buffer is read-only from the device's perspective (TX = T_OUT),
//   so the contents never need to be rewritten in flight.

const TX_DESC_OFF: u64     = 0x000;
const TX_AVAIL_OFF: u64    = 0x100;
const TX_USED_OFF: u64     = 0x200;
const RX_DESC_OFF: u64     = 0x400;
const RX_AVAIL_OFF: u64    = 0x500;
const RX_USED_OFF: u64     = 0x600;
const TX_BUF_POOL_OFF: u64 = 0x700;
const TX_BUF_STRIDE: u64   = 64;

const ETH_HDR_LEN: u32 = 14;
const ARP_LEN: u32     = 28;
const TX_FRAME_LEN: u32 = ETH_HDR_LEN + ARP_LEN;
const TX_BUF_USED_LEN: u32 = VIRTIO_NET_HDR_LEN + TX_FRAME_LEN; // 54

// Compile-time sanity: TX pool must fit in the ring DMA region, and
// every queue region must be non-overlapping in monotonic order
// (TX desc < TX avail < TX used < RX desc < RX avail < RX used < pool).
const _: () = {
    let pool_end = TX_BUF_POOL_OFF + (QUEUE_SIZE as u64) * TX_BUF_STRIDE;
    assert!(pool_end <= RING_DMA_SIZE,
            "TX buffer pool overflows ring DMA region");
    assert!(TX_BUF_USED_LEN as u64 <= TX_BUF_STRIDE,
            "TX frame + virtio_net_hdr does not fit in one TX_BUF_STRIDE");
    assert!(TX_DESC_OFF + (QUEUE_SIZE as u64) * 16 <= TX_AVAIL_OFF);
    assert!(TX_AVAIL_OFF + 4 + (QUEUE_SIZE as u64) * 2 <= TX_USED_OFF);
    assert!(TX_USED_OFF + 4 + (QUEUE_SIZE as u64) * 8 <= RX_DESC_OFF);
    assert!(RX_DESC_OFF + (QUEUE_SIZE as u64) * 16 <= RX_AVAIL_OFF);
    assert!(RX_AVAIL_OFF + 4 + (QUEUE_SIZE as u64) * 2 <= RX_USED_OFF);
    assert!(RX_USED_OFF + 4 + (QUEUE_SIZE as u64) * 8 <= TX_BUF_POOL_OFF);
};

// VirtIO 1.2 §4.2.5 InterruptStatus bits.
const INT_USED_BUFFER:  u32 = 1 << 0;
const INT_CONFIG_CHANGE: u32 = 1 << 1;

// Cap on consecutive used-ring drain iterations to keep a hypothetical
// out-of-sync `cur_used / *_seen_used` pair from running up to ~65535
// iterations before terminating. Back-pressure (in-flight ≤ QUEUE_SIZE)
// prevents real drift today; the cap is defense in depth + survives a
// future driver that relaxes back-pressure.
const MAX_DRAIN_PER_BATCH: u16 = QUEUE_SIZE;

// =============================================================================
// Volatile MMIO + DMA accessors.
// =============================================================================

#[inline(always)]
unsafe fn read32(addr: u64) -> u32 { core::ptr::read_volatile(addr as *const u32) }
#[inline(always)]
unsafe fn write32(addr: u64, val: u32) { core::ptr::write_volatile(addr as *mut u32, val) }
#[inline(always)]
unsafe fn write16(addr: u64, val: u16) { core::ptr::write_volatile(addr as *mut u16, val) }
#[inline(always)]
unsafe fn read16(addr: u64) -> u16 { core::ptr::read_volatile(addr as *const u16) }
#[inline(always)]
unsafe fn write64(addr: u64, val: u64) { core::ptr::write_volatile(addr as *mut u64, val) }
#[inline(always)]
unsafe fn read_u8(addr: u64) -> u8 { core::ptr::read_volatile(addr as *const u8) }
#[inline(always)]
unsafe fn write_u8(addr: u64, val: u8) { core::ptr::write_volatile(addr as *mut u8, val) }
#[inline(always)]
fn dsb_sy() { unsafe { asm!("dsb sy", options(nostack, preserves_flags)) } }

// =============================================================================
// Pretouch (P4-Ic7 surprise #2 workaround).
// =============================================================================
//
// R12-uaccess (kernel `7f78820`+): SYS_PUTS demand-pages user VAs from
// kernel mode via the uaccess-fault dispatcher (arch/arm64/uaccess.*).
// The `pretouch_rodata_pages()` discipline that this binary used to
// carry is retired; the kernel-side fixup is now the single mechanism.

// =============================================================================
// Diagnostics.
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

fn log_mac(mac: [u8; 6]) {
    let mut buf = [0u8; 17];
    let h2c = |x: u8| -> u8 { if x < 10 { b'0' + x } else { b'a' + (x - 10) } };
    for (i, b) in mac.iter().enumerate() {
        buf[i * 3]     = h2c(*b >> 4);
        buf[i * 3 + 1] = h2c(*b & 0xf);
        if i < 5 { buf[i * 3 + 2] = b':'; }
    }
    unsafe { t_puts(buf.as_ptr(), 17); }
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
            log("virtio-net-loop: SYS_MMIO_CREATE failed for page ");
            log_dec(i as u32); log("\n");
            return None;
        }
        let map_rc = unsafe { t_mmio_map(handle, va, prot) };
        if map_rc < 0 {
            log("virtio-net-loop: SYS_MMIO_MAP failed for page ");
            log_dec(i as u32); log("\n");
            return None;
        }
    }
    Some(MMIO_USER_VA)
}

fn find_net_slot(mmio_base_va: u64) -> Option<(u32, u64)> {
    for slot in 0..VIRTIO_MMIO_NUM_SLOTS {
        let slot_va = mmio_base_va + slot * VIRTIO_MMIO_SLOT_STRIDE;
        let magic = unsafe { read32(slot_va + REG_MAGIC_VALUE) };
        if magic != VIRTIO_MMIO_MAGIC { continue; }
        let dev_id = unsafe { read32(slot_va + REG_DEVICE_ID) };
        if dev_id == VIRTIO_DEVICE_ID_NET {
            return Some((slot as u32, slot_va));
        }
    }
    None
}

// =============================================================================
// Per-queue MMIO setup helper.
// =============================================================================

fn setup_queue(slot_va: u64, queue_idx: u32, desc_pa: u64, avail_pa: u64, used_pa: u64) -> bool {
    unsafe { write32(slot_va + REG_QUEUE_SEL, queue_idx) };
    let num_max = unsafe { read32(slot_va + REG_QUEUE_NUM_MAX) };
    if num_max < QUEUE_SIZE as u32 {
        log("virtio-net-loop: QueueNumMax(q=");
        log_dec(queue_idx);
        log(") = ");
        log_dec(num_max);
        log(" below QUEUE_SIZE\n");
        return false;
    }
    unsafe { write32(slot_va + REG_QUEUE_NUM, QUEUE_SIZE as u32) };
    unsafe {
        write32(slot_va + REG_QUEUE_DESC_LOW,    (desc_pa  & 0xFFFF_FFFF) as u32);
        write32(slot_va + REG_QUEUE_DESC_HIGH,   (desc_pa  >> 32) as u32);
        write32(slot_va + REG_QUEUE_DRIVER_LOW,  (avail_pa & 0xFFFF_FFFF) as u32);
        write32(slot_va + REG_QUEUE_DRIVER_HIGH, (avail_pa >> 32) as u32);
        write32(slot_va + REG_QUEUE_DEVICE_LOW,  (used_pa  & 0xFFFF_FFFF) as u32);
        write32(slot_va + REG_QUEUE_DEVICE_HIGH, (used_pa  >> 32) as u32);
    };
    unsafe { write32(slot_va + REG_QUEUE_READY, 1) };
    true
}

// =============================================================================
// RX pool pre-populate (identical to virtio-net-arp).
// =============================================================================
//
// Pre-publish all 16 RX descriptors with VIRTQ_DESC_F_WRITE pointing
// into the 32 KiB RX pool. avail.ring[k] = k for k in 0..16; avail.idx
// = 16 so the device sees all 16 as ready. Recycling after the steady
// state begins re-publishes consumed descriptors to slots
// `rx_avail_idx % QUEUE_SIZE` and bumps avail.idx (wrapping_add).

fn populate_rx(ring_dma_va: u64, rxpool_dma_pa: u64) {
    let desc_va = ring_dma_va + RX_DESC_OFF;
    for k in 0..QUEUE_SIZE as u64 {
        let buf_pa = rxpool_dma_pa + k * RX_BUF_LEN as u64;
        let d = desc_va + k * 16;
        unsafe {
            write64(d + 0,  buf_pa);
            write32(d + 8,  RX_BUF_LEN);
            write16(d + 12, VIRTQ_DESC_F_WRITE);
            write16(d + 14, 0);
        };
    }
    let avail_va = ring_dma_va + RX_AVAIL_OFF;
    unsafe { write16(avail_va + 0, 0) };          // flags
    for k in 0..QUEUE_SIZE {
        unsafe { write16(avail_va + 4 + (k as u64) * 2, k) };
    }
    dsb_sy();
    unsafe { write16(avail_va + 2, QUEUE_SIZE) }; // idx = 16
    dsb_sy();
}

// =============================================================================
// TX pool pre-populate.
// =============================================================================
//
// Build 16 TX buffers (each = [virtio_net_hdr (12)][Eth (14)][ARP (28)] = 54 B
// padded to TX_BUF_STRIDE = 64) and 16 TX descriptors each pointing to
// the matching buffer. Pre-init avail.ring[k] = k for k in 0..16; the
// device round-robins through descriptors as we advance avail.idx.
//
// The virtio_net_hdr is 12 zero bytes (no checksum offload, no GSO,
// num_buffers = 0 — but num_buffers is RX-side and unused for TX).
// The Ethernet header is broadcast destination + our MAC + ARP
// ethertype 0x0806. The ARP body is a request "who-has 10.0.2.2 tell
// 10.0.2.15." Slirp synthesizes a reply for each request we send.
//
// Buffer contents NEVER change after init — TX is T_OUT and the device
// is the reader.

const SLIRP_GUEST_IP: [u8; 4] = [10, 0, 2, 15];
const SLIRP_GATEWAY_IP: [u8; 4] = [10, 0, 2, 2];

fn build_tx_buffer(ring_dma_va: u64, k: u64, src_mac: [u8; 6]) {
    let buf = ring_dma_va + TX_BUF_POOL_OFF + k * TX_BUF_STRIDE;
    // virtio_net_hdr (12 B) — all zeros for plain frame transmission.
    for i in 0..VIRTIO_NET_HDR_LEN as u64 {
        unsafe { write_u8(buf + i, 0) };
    }
    let eth = buf + VIRTIO_NET_HDR_LEN as u64;
    let mut p = 0u64;
    for _ in 0..6 { unsafe { write_u8(eth + p, 0xff) }; p += 1; }
    for b in src_mac.iter() { unsafe { write_u8(eth + p, *b) }; p += 1; }
    unsafe { write_u8(eth + p, 0x08) }; p += 1;
    unsafe { write_u8(eth + p, 0x06) }; p += 1;
    let arp = eth + p;
    p = 0;
    unsafe { write_u8(arp + p, 0x00) }; p += 1; unsafe { write_u8(arp + p, 0x01) }; p += 1; // hw type Eth
    unsafe { write_u8(arp + p, 0x08) }; p += 1; unsafe { write_u8(arp + p, 0x00) }; p += 1; // proto IPv4
    unsafe { write_u8(arp + p, 6) }; p += 1;
    unsafe { write_u8(arp + p, 4) }; p += 1;
    unsafe { write_u8(arp + p, 0x00) }; p += 1; unsafe { write_u8(arp + p, 0x01) }; p += 1; // op = request
    for b in src_mac.iter() { unsafe { write_u8(arp + p, *b) }; p += 1; }
    for b in SLIRP_GUEST_IP.iter() { unsafe { write_u8(arp + p, *b) }; p += 1; }
    for _ in 0..6 { unsafe { write_u8(arp + p, 0) }; p += 1; }
    for b in SLIRP_GATEWAY_IP.iter() { unsafe { write_u8(arp + p, *b) }; p += 1; }
}

fn populate_tx(ring_dma_va: u64, ring_dma_pa: u64, src_mac: [u8; 6]) {
    // Pre-build all 16 buffers and descriptors.
    let desc_va = ring_dma_va + TX_DESC_OFF;
    for k in 0..QUEUE_SIZE as u64 {
        build_tx_buffer(ring_dma_va, k, src_mac);
        let buf_pa = ring_dma_pa + TX_BUF_POOL_OFF + k * TX_BUF_STRIDE;
        let d = desc_va + k * 16;
        unsafe {
            write64(d + 0,  buf_pa);
            write32(d + 8,  TX_BUF_USED_LEN);
            write16(d + 12, 0);                  // single descriptor, no NEXT
            write16(d + 14, 0);
        };
    }
    let avail_va = ring_dma_va + TX_AVAIL_OFF;
    unsafe { write16(avail_va + 0, 0) };         // flags
    for k in 0..QUEUE_SIZE {
        // avail.ring[k] = k. The device cycles through descriptors as
        // avail.idx advances; descriptor k always points to buffer k.
        unsafe { write16(avail_va + 4 + (k as u64) * 2, k) };
    }
    dsb_sy();
    unsafe { write16(avail_va + 2, 0) };         // idx = 0 (nothing in flight yet)
    dsb_sy();
}

// =============================================================================
// Device init.
// =============================================================================

fn init_device(slot_va: u64, ring_dma_va: u64, ring_pa: u64, rxpool_pa: u64,
               src_mac: &mut [u8; 6]) -> Option<u32> {
    unsafe { write32(slot_va + REG_STATUS, 0) };
    unsafe { write32(slot_va + REG_STATUS, STATUS_ACKNOWLEDGE) };
    unsafe { write32(slot_va + REG_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER) };

    unsafe { write32(slot_va + REG_DEVICE_FEATURES_SEL, 0) };
    let dev_feat_lo = unsafe { read32(slot_va + REG_DEVICE_FEATURES) };
    unsafe { write32(slot_va + REG_DEVICE_FEATURES_SEL, 1) };
    let dev_feat_hi = unsafe { read32(slot_va + REG_DEVICE_FEATURES) };

    if dev_feat_hi & VIRTIO_F_VERSION_1_BIT_BANK1 == 0 {
        log("virtio-net-loop: device lacks VIRTIO_F_VERSION_1\n");
        unsafe { write32(slot_va + REG_STATUS, STATUS_FAILED) };
        return None;
    }

    let want_lo = dev_feat_lo & (VIRTIO_NET_F_MAC_BIT | VIRTIO_NET_F_STATUS_BIT);
    unsafe { write32(slot_va + REG_DRIVER_FEATURES_SEL, 0) };
    unsafe { write32(slot_va + REG_DRIVER_FEATURES, want_lo) };
    unsafe { write32(slot_va + REG_DRIVER_FEATURES_SEL, 1) };
    unsafe { write32(slot_va + REG_DRIVER_FEATURES, VIRTIO_F_VERSION_1_BIT_BANK1) };

    unsafe {
        write32(slot_va + REG_STATUS,
                STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK);
    };
    let status = unsafe { read32(slot_va + REG_STATUS) };
    if status & STATUS_FEATURES_OK == 0 {
        log("virtio-net-loop: FEATURES_OK rejected; status=");
        log_hex(status); log("\n");
        return None;
    }

    // Configure both queues BEFORE DRIVER_OK so DRIVER_OK arms a fully
    // wired-up device.
    if !setup_queue(slot_va, NET_QUEUE_RX,
                    ring_pa + RX_DESC_OFF,
                    ring_pa + RX_AVAIL_OFF,
                    ring_pa + RX_USED_OFF) {
        unsafe { write32(slot_va + REG_STATUS, STATUS_FAILED) };
        return None;
    }
    if !setup_queue(slot_va, NET_QUEUE_TX,
                    ring_pa + TX_DESC_OFF,
                    ring_pa + TX_AVAIL_OFF,
                    ring_pa + TX_USED_OFF) {
        unsafe { write32(slot_va + REG_STATUS, STATUS_FAILED) };
        return None;
    }

    // Read MAC before populating TX pool (each TX buffer carries our MAC).
    if want_lo & VIRTIO_NET_F_MAC_BIT != 0 {
        for i in 0..6 {
            src_mac[i] = unsafe { read_u8(slot_va + REG_CONFIG_BASE + NET_CFG_MAC + i as u64) };
        }
    } else {
        // Reasonable fallback if the device doesn't advertise the MAC
        // feature bit; we override with the QEMU-provided MAC below.
        *src_mac = [0x52, 0x54, 0x00, 0x12, 0x34, 0x56];
    }

    populate_rx(ring_dma_va, rxpool_pa);
    populate_tx(ring_dma_va, ring_pa, *src_mac);

    unsafe {
        write32(slot_va + REG_STATUS,
                STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK | STATUS_DRIVER_OK);
    };
    Some(want_lo)
}

fn read_status(slot_va: u64) -> u16 {
    unsafe { read16(slot_va + REG_CONFIG_BASE + NET_CFG_STATUS) }
}

// =============================================================================
// TX scheduling.
// =============================================================================
//
// Each send_pending_tx call enqueues as many new TX requests as room
// allows (tx_avail_idx.wrapping_sub(tx_seen_used) < QUEUE_SIZE), then
// bumps avail.idx + kicks once. The descriptor + buffer for each
// "new" TX are already pre-built — we just expose more avail.idx to
// the device.

fn send_pending_tx(slot_va: u64, ring_dma_va: u64,
                   tx_avail_idx: &mut u16, tx_seen_used: u16,
                   tx_sent: &mut u32) {
    let prev = *tx_avail_idx;
    while *tx_sent < N_TARGET
        && tx_avail_idx.wrapping_sub(tx_seen_used) < QUEUE_SIZE
    {
        *tx_avail_idx = tx_avail_idx.wrapping_add(1);
        *tx_sent += 1;
    }
    if *tx_avail_idx != prev {
        dsb_sy();
        unsafe { write16(ring_dma_va + TX_AVAIL_OFF + 2, *tx_avail_idx) };
        dsb_sy();
        unsafe { write32(slot_va + REG_QUEUE_NOTIFY, NET_QUEUE_TX) };
    }
}

// =============================================================================
// RX consumption + descriptor recycling.
// =============================================================================
//
// Drain the RX used ring: for each new entry, parse the frame, count
// validated ARP replies, and re-publish the descriptor to the avail
// ring at slot `rx_avail_idx % QUEUE_SIZE` with a wrapping bump of
// rx_avail_idx. This is the discipline the steady-state RX side
// needs — without it, the RX queue runs dry after QUEUE_SIZE frames
// and packet delivery stalls.
//
// The descriptor index reported in used.ring[k].id is the same `k`
// we originally published in avail.ring[k] = k, but a real driver
// shouldn't assume that — the device may consume descriptors in any
// order (and a future iteration with VIRTIO_NET_F_MRG_RXBUF would
// chain multiple descriptors). So we read desc_id from the used
// entry, then re-publish that exact desc_id back to avail.

fn drain_rx_used_with_recycle(slot_va: u64, ring_dma_va: u64,
                              rxpool_dma_va: u64,
                              rx_seen_used: &mut u16,
                              rx_avail_idx: &mut u16,
                              rx_validated: &mut u32) {
    let rx_used_va = ring_dma_va + RX_USED_OFF;
    let rx_avail_va = ring_dma_va + RX_AVAIL_OFF;
    let cur_used = unsafe { read16(rx_used_va + 2) };
    // VIRTIO 1.2 §2.7.13.2: barrier between observing used.idx advance
    // and reading used.ring[k] / the data buffer the descriptor pointed
    // at. Without it, an out-of-order ARM core may speculate the data
    // reads before the used.idx load, returning pre-advance bytes.
    virtio_rmb();

    let mut any_recycled = false;
    let mut iters: u16 = 0;
    while *rx_seen_used != cur_used && iters < MAX_DRAIN_PER_BATCH {
        let used_slot = (*rx_seen_used as u64) % QUEUE_SIZE as u64;
        let entry_va = rx_used_va + 4 + used_slot * 8;
        let desc_id = unsafe { read32(entry_va) } as u16;
        let len = unsafe { read32(entry_va + 4) };
        *rx_seen_used = rx_seen_used.wrapping_add(1);
        iters += 1;

        if parse_arp_reply(rxpool_dma_va, desc_id as u32, len) {
            *rx_validated += 1;
        }

        // Recycle: re-publish desc_id back to the avail ring.
        let avail_slot = (*rx_avail_idx as u64) % QUEUE_SIZE as u64;
        unsafe { write16(rx_avail_va + 4 + avail_slot * 2, desc_id) };
        *rx_avail_idx = rx_avail_idx.wrapping_add(1);
        any_recycled = true;
    }
    if any_recycled {
        dsb_sy();
        unsafe { write16(rx_avail_va + 2, *rx_avail_idx) };
        dsb_sy();
        // Notify the device that fresh buffers are available. Many
        // virtio implementations don't strictly require this on RX
        // (they poll the avail ring as needed), but the spec allows
        // either way and this matches the TX side's notification
        // discipline.
        unsafe { write32(slot_va + REG_QUEUE_NOTIFY, NET_QUEUE_RX) };
    }
}

fn parse_arp_reply(rxpool_dma_va: u64, desc_idx: u32, frame_len: u32) -> bool {
    if frame_len < VIRTIO_NET_HDR_LEN + ETH_HDR_LEN + ARP_LEN { return false; }
    let base = rxpool_dma_va + desc_idx as u64 * RX_BUF_LEN as u64;
    let eth = base + VIRTIO_NET_HDR_LEN as u64;
    let arp = eth + ETH_HDR_LEN as u64;

    let etype_hi = unsafe { read_u8(eth + 12) };
    let etype_lo = unsafe { read_u8(eth + 13) };
    if etype_hi != 0x08 || etype_lo != 0x06 { return false; }

    let op_hi = unsafe { read_u8(arp + 6) };
    let op_lo = unsafe { read_u8(arp + 7) };
    if op_hi != 0x00 || op_lo != 0x02 { return false; }

    // sender_ip at arp+14 must be slirp gateway (10.0.2.2).
    for i in 0..4 {
        if unsafe { read_u8(arp + 14 + i as u64) } != SLIRP_GATEWAY_IP[i] { return false; }
    }
    // target_ip at arp+24 must be our guest IP (10.0.2.15).
    for i in 0..4 {
        if unsafe { read_u8(arp + 24 + i as u64) } != SLIRP_GUEST_IP[i] { return false; }
    }
    true
}

// =============================================================================
// TX completion drain.
// =============================================================================

fn drain_tx_used(ring_dma_va: u64, tx_seen_used: &mut u16, tx_completed: &mut u32) {
    let tx_used_va = ring_dma_va + TX_USED_OFF;
    let cur_used = unsafe { read16(tx_used_va + 2) };
    // VIRTIO 1.2 §2.7.13.2: barrier after observing used.idx advance.
    // The TX-side drain doesn't read used.ring[k] data, but emitting
    // the barrier preserves the discipline for any future TX path that
    // does (e.g. reading used.ring[k].len to count bytes-flushed).
    virtio_rmb();
    let mut iters: u16 = 0;
    while *tx_seen_used != cur_used && iters < MAX_DRAIN_PER_BATCH {
        // We don't read used.ring[k].id — we don't need the specific
        // descriptor index. The send_pending_tx in-flight check uses
        // (tx_avail_idx - tx_seen_used) which gives us free-slot
        // count regardless of which specific descriptor finished.
        *tx_seen_used = tx_seen_used.wrapping_add(1);
        *tx_completed += 1;
        iters += 1;
    }
}

// =============================================================================
// Main steady-state loop.
// =============================================================================

fn run_round_trips(slot_va: u64, irq_handle: i64, ring_dma_va: u64,
                   rxpool_dma_va: u64) -> Result<(u32, u32), ()> {
    let mut tx_avail_idx: u16 = 0;
    let mut tx_seen_used: u16 = 0;
    let mut tx_sent: u32 = 0;
    let mut tx_completed: u32 = 0;
    let mut rx_seen_used: u16 = 0;
    // populate_rx leaves avail.idx = 16; mirror that here so we
    // recycle correctly (re-publishing extends past the initial 16).
    let mut rx_avail_idx: u16 = QUEUE_SIZE;
    let mut rx_validated: u32 = 0;

    // Prime the pump: kick the first TX batch (up to QUEUE_SIZE).
    send_pending_tx(slot_va, ring_dma_va,
                    &mut tx_avail_idx, tx_seen_used, &mut tx_sent);

    for _iter in 0..MAX_IRQ_ITERATIONS {
        if rx_validated >= N_TARGET { break; }

        let count = unsafe { t_irq_wait(irq_handle) };
        if count < 0 {
            log("virtio-net-loop: FAIL — SYS_IRQ_WAIT returned error\n");
            return Err(());
        }
        let int_status = unsafe { read32(slot_va + REG_INTERRUPT_STATUS) };
        unsafe { write32(slot_va + REG_INTERRUPT_ACK, int_status) };
        // VIRTIO 1.2 §4.2.5: a config-change-only wake (INT_CONFIG_CHANGE
        // without INT_USED_BUFFER) signals device-config state shift,
        // not used-ring progress. We ACK + continue without draining
        // (the next wake delivers the real ring update); the drain
        // calls below would no-op anyway since cur_used wouldn't have
        // advanced, but the explicit skip documents the intent.
        if int_status & INT_USED_BUFFER == 0 {
            continue;
        }
        let _ = INT_CONFIG_CHANGE; // referenced for symmetry; ACKed via int_status

        drain_tx_used(ring_dma_va, &mut tx_seen_used, &mut tx_completed);
        drain_rx_used_with_recycle(slot_va, ring_dma_va, rxpool_dma_va,
                                   &mut rx_seen_used, &mut rx_avail_idx,
                                   &mut rx_validated);
        send_pending_tx(slot_va, ring_dma_va,
                        &mut tx_avail_idx, tx_seen_used, &mut tx_sent);
    }

    if rx_validated < N_TARGET {
        log("virtio-net-loop: FAIL — exhausted IRQ iterations before N_TARGET; tx_sent=");
        log_dec(tx_sent);
        log(" tx_completed=");
        log_dec(tx_completed);
        log(" rx_validated=");
        log_dec(rx_validated);
        log("\n");
        return Err(());
    }
    Ok((tx_completed, rx_validated))
}

// =============================================================================
// Entry point.
// =============================================================================

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    // R12-uaccess removed the pretouch_rodata_pages() workaround;
    // SYS_PUTS handles user-VA demand-paging itself.
    let mmio_base = match claim_virtio_mmio_bank() {
        Some(va) => va,
        None => {
            log("virtio-net-loop: FAIL — virtio-mmio bank claim failed\n");
            unsafe { t_exits(1) };
        }
    };
    let (slot, slot_va) = match find_net_slot(mmio_base) {
        Some(found) => found,
        None => {
            log("virtio-net-loop: SKIP — no virtio-mmio slot has DeviceID=1\n");
            return 0;
        }
    };
    let intid = VIRTIO_MMIO_GIC_INTID_BASE + slot;

    let version = unsafe { read32(slot_va + REG_VERSION) };
    if version != VIRTIO_MMIO_VERSION_MODERN {
        log("virtio-net-loop: FAIL — slot reports legacy version ");
        log_dec(version); log("\n");
        unsafe { t_exits(1) };
    }

    let irq_handle = unsafe { t_irq_create(intid, T_RIGHT_SIGNAL) };
    if irq_handle < 0 {
        log("virtio-net-loop: FAIL — SYS_IRQ_CREATE failed\n");
        unsafe { t_exits(1) };
    }

    let ring_dma_handle = unsafe {
        t_dma_create(RING_DMA_SIZE, T_RIGHT_READ | T_RIGHT_WRITE | T_RIGHT_MAP)
    };
    if ring_dma_handle < 0 {
        log("virtio-net-loop: FAIL — SYS_DMA_CREATE(rings) failed\n");
        unsafe { t_exits(1) };
    }
    let ring_dma_pa = unsafe {
        t_dma_map(ring_dma_handle, RING_DMA_USER_VA, T_PROT_READ | T_PROT_WRITE)
    };
    if ring_dma_pa < 0 {
        log("virtio-net-loop: FAIL — SYS_DMA_MAP(rings) failed\n");
        unsafe { t_exits(1) };
    }

    let rxpool_dma_handle = unsafe {
        t_dma_create(RXPOOL_DMA_SIZE, T_RIGHT_READ | T_RIGHT_WRITE | T_RIGHT_MAP)
    };
    if rxpool_dma_handle < 0 {
        log("virtio-net-loop: FAIL — SYS_DMA_CREATE(rxpool) failed\n");
        unsafe { t_exits(1) };
    }
    let rxpool_dma_pa = unsafe {
        t_dma_map(rxpool_dma_handle, RXPOOL_DMA_USER_VA, T_PROT_READ | T_PROT_WRITE)
    };
    if rxpool_dma_pa < 0 {
        log("virtio-net-loop: FAIL — SYS_DMA_MAP(rxpool) failed\n");
        unsafe { t_exits(1) };
    }

    // Pre-warm both DMA regions page-by-page.
    for off in (0..RING_DMA_SIZE).step_by(PAGE_SIZE as usize) {
        unsafe { write_u8(RING_DMA_USER_VA + off, 0) };
    }
    for off in (0..RXPOOL_DMA_SIZE).step_by(PAGE_SIZE as usize) {
        unsafe { write_u8(RXPOOL_DMA_USER_VA + off, 0) };
    }

    let mut mac = [0u8; 6];
    let negotiated_lo = match init_device(slot_va, RING_DMA_USER_VA,
                                          ring_dma_pa as u64, rxpool_dma_pa as u64,
                                          &mut mac) {
        Some(want_lo) => want_lo,
        None => unsafe { t_exits(1) },
    };

    log("virtio-net-loop: slot=");
    log_dec(slot);
    log(" intid=");
    log_dec(intid);
    log(" mac=");
    log_mac(mac);
    if negotiated_lo & VIRTIO_NET_F_STATUS_BIT != 0 {
        let st = read_status(slot_va);
        log(" link=");
        log_dec((st & VIRTIO_NET_S_LINK_UP) as u32);
    }
    log(" target=");
    log_dec(N_TARGET);
    log("\n");

    let (tx_completed, rx_validated) =
        match run_round_trips(slot_va, irq_handle, RING_DMA_USER_VA, RXPOOL_DMA_USER_VA) {
        Ok(counts) => counts,
        Err(()) => unsafe { t_exits(1) },
    };

    log("virtio-net-loop: PASS — ");
    log_dec(rx_validated);
    log("/");
    log_dec(N_TARGET);
    log(" ARP replies validated; tx_completed=");
    log_dec(tx_completed);
    log("; RX desc recycled past QUEUE_SIZE (slot=");
    log_dec(slot);
    log(" intid=");
    log_dec(intid);
    log(")\n");
    0
}
