// /virtio-net-arp — full ARP request/reply round-trip (P4-Jb).
//
// Successor to virtio-net-probe. Adds the RX side: pre-publish buffers
// on the RX virtqueue, then after sending the broadcast ARP request
// we wait for slirp's ARP reply to land in one of those buffers.
// Parse the received frame's ethertype + ARP opcode + sender IP.
//
// What this proves end-to-end (beyond what P4-Ja covered):
//   - Configuring TWO virtqueues simultaneously (RX = 0 + TX = 1).
//     Each has its own desc/avail/used trio + its own QueueReady.
//   - Pre-publishing RX buffers: the device fills buffers we DMA at
//     it (descriptors with VIRTQ_DESC_F_WRITE; device writes the
//     virtio_net_hdr + Ethernet frame into the buffer; used.ring[k]
//     reports the buffer the device consumed + the bytes written).
//   - IRQ aggregation: TX completion + RX delivery may fire one or
//     two IRQs (the device coalesces under spec §4.2.2.5).
//     We drain BOTH used rings on every wake.
//   - Slirp's host-side ARP gateway returns a 64-byte reply
//     (52:55:0a:00:02:02 → us; opcode=2; sender=10.0.2.2).
//
// QEMU slirp's ARP behavior: when the guest broadcasts an ARP request
// for a slirp-allocated IP (10.0.2.2 = gateway, 10.0.2.3 = DNS,
// 10.0.2.4 = host-loopback alias), slirp synthesizes a reply with the
// matching IP's slirp MAC. We target 10.0.2.2.

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

const VIRTIO_MMIO_GIC_INTID_BASE: u32 = 32 + 16; // = 48

// =============================================================================
// User-VA layout.
// =============================================================================

const MMIO_USER_VA: u64 = 0x0050_0000;
const RING_DMA_USER_VA: u64 = 0x0061_0000;       // 4 KiB rings + TX side
const RXPOOL_DMA_USER_VA: u64 = 0x0062_0000;     // 32 KiB RX buffer pool

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

// Queue indices: RX = 0, TX = 1 per virtio-net §5.1.2 (single virtqueue
// pair, no VIRTIO_NET_F_MQ).
const NET_QUEUE_RX: u32 = 0;
const NET_QUEUE_TX: u32 = 1;

const VIRTQ_DESC_F_NEXT: u16  = 1;
const VIRTQ_DESC_F_WRITE: u16 = 2;

const VIRTIO_NET_HDR_LEN: u32 = 12;

// =============================================================================
// DMA layout.
// =============================================================================
//
// Ring DMA (4 KiB, single page):
//   0x000  TX desc table (256 B)
//   0x100  TX avail ring
//   0x200  TX used ring
//   0x300  TX virtio_net_hdr (12 B)
//   0x320  TX Ethernet frame (42 B)
//   0x400  RX desc table (256 B)
//   0x500  RX avail ring
//   0x600  RX used ring
//
// RX buffer pool (32 KiB):
//   k * 2048 + [0..(2048-1)]   buffer for RX desc k

const QUEUE_SIZE: u16 = 16;
const RING_DMA_SIZE: u64 = PAGE_SIZE;          // 4096
const RXPOOL_DMA_SIZE: u64 = 32 * 1024;        // 16 × 2 KiB
const RX_BUF_LEN: u32 = 2048;                  // per buffer

const TX_DESC_OFF: u64    = 0x000;
const TX_AVAIL_OFF: u64   = 0x100;
const TX_USED_OFF: u64    = 0x200;
const TX_HDR_OFF: u64     = 0x300;
const TX_FRAME_OFF: u64   = 0x320;

const RX_DESC_OFF: u64    = 0x400;
const RX_AVAIL_OFF: u64   = 0x500;
const RX_USED_OFF: u64    = 0x600;

const ETH_HDR_LEN: u32 = 14;
const ARP_LEN: u32     = 28;
const TX_FRAME_LEN: u32 = ETH_HDR_LEN + ARP_LEN;

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
// This binary is larger than virtio-net-probe and its LOAD segment
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

fn log_ip(ip: [u8; 4]) {
    let mut buf = [0u8; 16];
    let mut p = 0;
    for (i, b) in ip.iter().enumerate() {
        let mut tmp = [0u8; 4];
        let mut n = *b as u32;
        let mut j = 4;
        if n == 0 { j -= 1; tmp[j] = b'0'; }
        while n > 0 { j -= 1; tmp[j] = b'0' + (n % 10) as u8; n /= 10; }
        for k in j..4 { buf[p] = tmp[k]; p += 1; }
        if i < 3 { buf[p] = b'.'; p += 1; }
    }
    unsafe { t_puts(buf.as_ptr(), p); }
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
            log("virtio-net-arp: SYS_MMIO_CREATE failed for page ");
            log_dec(i as u32); log("\n");
            return None;
        }
        let map_rc = unsafe { t_mmio_map(handle, va, prot) };
        if map_rc < 0 {
            log("virtio-net-arp: SYS_MMIO_MAP failed for page ");
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
// Per-queue setup helper.
// =============================================================================

fn setup_queue(slot_va: u64, queue_idx: u32, desc_pa: u64, avail_pa: u64, used_pa: u64) -> bool {
    unsafe { write32(slot_va + REG_QUEUE_SEL, queue_idx) };
    let num_max = unsafe { read32(slot_va + REG_QUEUE_NUM_MAX) };
    if num_max < QUEUE_SIZE as u32 {
        log("virtio-net-arp: QueueNumMax(q=");
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
// Pre-publish 16 RX descriptors with 2 KiB buffers from the RX pool.
// =============================================================================
//
// Each descriptor:
//   addr  = rxpool_pa + k * 2048
//   len   = 2048
//   flags = VIRTQ_DESC_F_WRITE
//   next  = 0
//
// Without VIRTIO_NET_F_MRG_RXBUF the device uses a single buffer per
// frame: it writes `virtio_net_hdr | Ethernet hdr | payload` into the
// buffer and reports `used.ring[k].len` = total bytes written.

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
// Device init.
// =============================================================================

fn init_device(slot_va: u64, ring_dma_va: u64, ring_pa: u64, rxpool_pa: u64) -> Option<u32> {
    unsafe { write32(slot_va + REG_STATUS, 0) };
    unsafe { write32(slot_va + REG_STATUS, STATUS_ACKNOWLEDGE) };
    unsafe { write32(slot_va + REG_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER) };

    unsafe { write32(slot_va + REG_DEVICE_FEATURES_SEL, 0) };
    let dev_feat_lo = unsafe { read32(slot_va + REG_DEVICE_FEATURES) };
    unsafe { write32(slot_va + REG_DEVICE_FEATURES_SEL, 1) };
    let dev_feat_hi = unsafe { read32(slot_va + REG_DEVICE_FEATURES) };

    if dev_feat_hi & VIRTIO_F_VERSION_1_BIT_BANK1 == 0 {
        log("virtio-net-arp: device lacks VIRTIO_F_VERSION_1\n");
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
        log("virtio-net-arp: FEATURES_OK rejected; status=");
        log_hex(status); log("\n");
        return None;
    }

    // Configure both queues (RX = 0, TX = 1) BEFORE DRIVER_OK.
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

    // Pre-publish RX buffers so the device has somewhere to deliver
    // packets the moment DRIVER_OK arms it.
    populate_rx(ring_dma_va, rxpool_pa);

    unsafe {
        write32(slot_va + REG_STATUS,
                STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK | STATUS_DRIVER_OK);
    };
    Some(want_lo)
}

fn read_mac(slot_va: u64) -> [u8; 6] {
    let mut mac = [0u8; 6];
    for i in 0..6 {
        mac[i] = unsafe { read_u8(slot_va + REG_CONFIG_BASE + NET_CFG_MAC + i as u64) };
    }
    mac
}

fn read_status(slot_va: u64) -> u16 {
    unsafe { read16(slot_va + REG_CONFIG_BASE + NET_CFG_STATUS) }
}

// =============================================================================
// Build + submit the broadcast ARP request.
// =============================================================================

const SLIRP_GUEST_IP: [u8; 4] = [10, 0, 2, 15];
const SLIRP_GATEWAY_IP: [u8; 4] = [10, 0, 2, 2];

fn build_arp_frame(ring_dma_va: u64, src_mac: [u8; 6]) {
    let f = ring_dma_va + TX_FRAME_OFF;
    let mut p = 0u64;
    for _ in 0..6 { unsafe { write_u8(f + p, 0xff) }; p += 1; }
    for b in src_mac.iter() { unsafe { write_u8(f + p, *b) }; p += 1; }
    unsafe { write_u8(f + p, 0x08) }; p += 1; unsafe { write_u8(f + p, 0x06) }; p += 1; // ethertype ARP
    // ARP body.
    unsafe { write_u8(f + p, 0x00) }; p += 1; unsafe { write_u8(f + p, 0x01) }; p += 1; // hw type Eth
    unsafe { write_u8(f + p, 0x08) }; p += 1; unsafe { write_u8(f + p, 0x00) }; p += 1; // proto IPv4
    unsafe { write_u8(f + p, 6) }; p += 1;
    unsafe { write_u8(f + p, 4) }; p += 1;
    unsafe { write_u8(f + p, 0x00) }; p += 1; unsafe { write_u8(f + p, 0x01) }; p += 1; // op = request
    for b in src_mac.iter() { unsafe { write_u8(f + p, *b) }; p += 1; }
    for b in SLIRP_GUEST_IP.iter() { unsafe { write_u8(f + p, *b) }; p += 1; }
    for _ in 0..6 { unsafe { write_u8(f + p, 0) }; p += 1; }
    for b in SLIRP_GATEWAY_IP.iter() { unsafe { write_u8(f + p, *b) }; p += 1; }
}

fn submit_tx_arp(slot_va: u64, ring_dma_va: u64, ring_dma_pa: u64) {
    for i in 0..VIRTIO_NET_HDR_LEN {
        unsafe { write_u8(ring_dma_va + TX_HDR_OFF + i as u64, 0) };
    }
    let desc_va = ring_dma_va + TX_DESC_OFF;
    unsafe {
        write64(desc_va + 0,  ring_dma_pa + TX_HDR_OFF);
        write32(desc_va + 8,  VIRTIO_NET_HDR_LEN);
        write16(desc_va + 12, VIRTQ_DESC_F_NEXT);
        write16(desc_va + 14, 1);
        write64(desc_va + 16, ring_dma_pa + TX_FRAME_OFF);
        write32(desc_va + 24, TX_FRAME_LEN);
        write16(desc_va + 28, 0);
        write16(desc_va + 30, 0);
    };
    let avail_va = ring_dma_va + TX_AVAIL_OFF;
    unsafe { write16(avail_va + 0, 0) };
    unsafe { write16(avail_va + 4, 0) };
    dsb_sy();
    unsafe { write16(avail_va + 2, 1) };
    dsb_sy();
    unsafe { write32(slot_va + REG_QUEUE_NOTIFY, NET_QUEUE_TX) };
}

// =============================================================================
// Wait + drain both queues + validate the ARP reply.
// =============================================================================
//
// On each IRQ wake:
//   1. Read + ACK InterruptStatus.
//   2. Check TX used.idx — if it advanced past last-seen, mark tx_done.
//   3. Check RX used.idx — for each new entry, parse it as an ARP reply.
//      The first match wins.
//
// Bounded at 8 iterations defensively; in practice slirp delivers the
// ARP reply within tens of microseconds, well under one timer tick.

#[derive(Clone, Copy)]
struct ArpReply {
    sender_mac: [u8; 6],
    sender_ip: [u8; 4],
    target_ip: [u8; 4],
}

fn parse_rx_frame(rxpool_dma_va: u64, desc_idx: u32, frame_len: u32) -> Option<ArpReply> {
    if frame_len < VIRTIO_NET_HDR_LEN + ETH_HDR_LEN + ARP_LEN {
        return None;
    }
    let base = rxpool_dma_va + desc_idx as u64 * RX_BUF_LEN as u64;
    let eth = base + VIRTIO_NET_HDR_LEN as u64;
    let arp = eth + ETH_HDR_LEN as u64;

    let etype_hi = unsafe { read_u8(eth + 12) };
    let etype_lo = unsafe { read_u8(eth + 13) };
    if etype_hi != 0x08 || etype_lo != 0x06 { return None; }

    let op_hi = unsafe { read_u8(arp + 6) };
    let op_lo = unsafe { read_u8(arp + 7) };
    if op_hi != 0x00 || op_lo != 0x02 { return None; }

    let mut sender_mac = [0u8; 6];
    for i in 0..6 { sender_mac[i] = unsafe { read_u8(arp + 8 + i as u64) }; }
    let mut sender_ip = [0u8; 4];
    for i in 0..4 { sender_ip[i] = unsafe { read_u8(arp + 14 + i as u64) }; }
    let mut target_ip = [0u8; 4];
    for i in 0..4 { target_ip[i] = unsafe { read_u8(arp + 24 + i as u64) }; }
    Some(ArpReply { sender_mac, sender_ip, target_ip })
}

fn wait_and_round_trip(irq_handle: i64, slot_va: u64, ring_dma_va: u64,
                       rxpool_dma_va: u64) -> Result<ArpReply, ()> {
    let mut rx_seen_idx: u16 = 0;
    let rx_used_va = ring_dma_va + RX_USED_OFF;

    // Receiving a valid ARP reply from slirp is itself evidence that
    // the device delivered our TX frame to slirp (slirp wouldn't
    // synthesize a reply otherwise). We don't need to check
    // TX used.idx independently — the RX completion is the
    // round-trip signal.
    //
    // Why 8 iterations: IRQ aggregation under VIRTIO §4.2.2.5 may
    // coalesce TX + RX into one notification or split into two.
    // Either way we expect the round-trip to finish in 1-2 wakes
    // (slirp delivers within tens of microseconds). 8 is the
    // defensive bound; in practice we exit after iteration 1 or 2.
    for _iter in 0..8u32 {
        let count = unsafe { t_irq_wait(irq_handle) };
        if count < 0 {
            log("virtio-net-arp: FAIL — SYS_IRQ_WAIT returned error\n");
            return Err(());
        }
        let int_status = unsafe { read32(slot_va + REG_INTERRUPT_STATUS) };
        unsafe { write32(slot_va + REG_INTERRUPT_ACK, int_status) };

        let rx_used_idx = unsafe { read16(rx_used_va + 2) };
        while rx_seen_idx != rx_used_idx {
            let slot_in_ring = rx_seen_idx as u64 % QUEUE_SIZE as u64;
            let entry_va = rx_used_va + 4 + slot_in_ring * 8;
            let id = unsafe { read32(entry_va + 0) };
            let len = unsafe { read32(entry_va + 4) };
            rx_seen_idx = rx_seen_idx.wrapping_add(1);

            if let Some(reply) = parse_rx_frame(rxpool_dma_va, id, len) {
                return Ok(reply);
            }
            // Not an ARP reply (some other broadcast or noise);
            // skip and keep looking through the rest of the ring.
            // The buffer's consumed at the device level — for a
            // single-shot probe we leak it (process exits after
            // wait_and_round_trip returns).
        }
    }
    log("virtio-net-arp: FAIL — no ARP reply after 8 IRQs\n");
    Err(())
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
            log("virtio-net-arp: FAIL — virtio-mmio bank claim failed\n");
            unsafe { t_exits(1) };
        }
    };
    let (slot, slot_va) = match find_net_slot(mmio_base) {
        Some(found) => found,
        None => {
            log("virtio-net-arp: SKIP — no virtio-mmio slot has DeviceID=1\n");
            return 0;
        }
    };
    let intid = VIRTIO_MMIO_GIC_INTID_BASE + slot;

    let version = unsafe { read32(slot_va + REG_VERSION) };
    if version != VIRTIO_MMIO_VERSION_MODERN {
        log("virtio-net-arp: FAIL — slot reports legacy version ");
        log_dec(version); log("\n");
        unsafe { t_exits(1) };
    }

    let irq_handle = unsafe { t_irq_create(intid, T_RIGHT_SIGNAL) };
    if irq_handle < 0 {
        log("virtio-net-arp: FAIL — SYS_IRQ_CREATE failed\n");
        unsafe { t_exits(1) };
    }

    let ring_dma_handle = unsafe {
        t_dma_create(RING_DMA_SIZE, T_RIGHT_READ | T_RIGHT_WRITE | T_RIGHT_MAP)
    };
    if ring_dma_handle < 0 {
        log("virtio-net-arp: FAIL — SYS_DMA_CREATE(rings) failed\n");
        unsafe { t_exits(1) };
    }
    let ring_dma_pa = unsafe {
        t_dma_map(ring_dma_handle, RING_DMA_USER_VA, T_PROT_READ | T_PROT_WRITE)
    };
    if ring_dma_pa < 0 {
        log("virtio-net-arp: FAIL — SYS_DMA_MAP(rings) failed\n");
        unsafe { t_exits(1) };
    }

    let rxpool_dma_handle = unsafe {
        t_dma_create(RXPOOL_DMA_SIZE, T_RIGHT_READ | T_RIGHT_WRITE | T_RIGHT_MAP)
    };
    if rxpool_dma_handle < 0 {
        log("virtio-net-arp: FAIL — SYS_DMA_CREATE(rxpool) failed\n");
        unsafe { t_exits(1) };
    }
    let rxpool_dma_pa = unsafe {
        t_dma_map(rxpool_dma_handle, RXPOOL_DMA_USER_VA, T_PROT_READ | T_PROT_WRITE)
    };
    if rxpool_dma_pa < 0 {
        log("virtio-net-arp: FAIL — SYS_DMA_MAP(rxpool) failed\n");
        unsafe { t_exits(1) };
    }

    // Pre-warm both DMA regions (page-by-page; touches all 4 KiB pages
    // in the 32 KiB rxpool to install BURROW_TYPE_DMA Normal-WB PTEs).
    for off in (0..RING_DMA_SIZE).step_by(PAGE_SIZE as usize) {
        unsafe { write_u8(RING_DMA_USER_VA + off, 0) };
    }
    for off in (0..RXPOOL_DMA_SIZE).step_by(PAGE_SIZE as usize) {
        unsafe { write_u8(RXPOOL_DMA_USER_VA + off, 0) };
    }

    let negotiated_lo = match init_device(slot_va, RING_DMA_USER_VA,
                                          ring_dma_pa as u64, rxpool_dma_pa as u64) {
        Some(want_lo) => want_lo,
        None => unsafe { t_exits(1) },
    };

    let mac = read_mac(slot_va);
    log("virtio-net-arp: slot=");
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
    log("\n");

    build_arp_frame(RING_DMA_USER_VA, mac);
    submit_tx_arp(slot_va, RING_DMA_USER_VA, ring_dma_pa as u64);

    let reply = match wait_and_round_trip(irq_handle, slot_va,
                                           RING_DMA_USER_VA, RXPOOL_DMA_USER_VA) {
        Ok(r) => r,
        Err(()) => unsafe { t_exits(1) },
    };

    let mut ok = true;
    if reply.sender_ip != SLIRP_GATEWAY_IP { ok = false; }
    if reply.target_ip != SLIRP_GUEST_IP { ok = false; }

    log("virtio-net-arp: rx_reply sender_mac=");
    log_mac(reply.sender_mac);
    log(" sender_ip=");
    log_ip(reply.sender_ip);
    log(" target_ip=");
    log_ip(reply.target_ip);
    log("\n");

    if !ok {
        log("virtio-net-arp: FAIL — IPs do not match expected slirp gateway/guest\n");
        unsafe { t_exits(1) };
    }

    log("virtio-net-arp: PASS — full ARP request/reply round-trip (slot=");
    log_dec(slot);
    log(" intid=");
    log_dec(intid);
    log(")\n");
    0
}
