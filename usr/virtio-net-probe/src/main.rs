// /virtio-net-probe — second composed userspace driver (P4-Ja).
//
// Mirror of virtio-blk-probe (P4-Ic5b2) for the NET device class. Proves
// that the userspace-driver discipline established for block generalizes
// to a different VirtIO device: same composed-hw-handle SVC surface,
// same virtqueue mechanics, but a different DeviceID (1 = net) + a
// different device-specific config-space layout + a different on-the-
// wire payload (Ethernet frame instead of block request).
//
// Scope:
//   - Init: ACK → DRIVER → DeviceFeatures (require VIRTIO_F_VERSION_1;
//     opportunistically accept VIRTIO_NET_F_MAC + VIRTIO_NET_F_STATUS;
//     reject MRG_RXBUF + CTRL_VQ to keep the protocol surface minimum)
//     → DriverFeatures → FEATURES_OK → configure TX queue (index 1) →
//     DRIVER_OK.
//   - Config space: read 6-byte MAC; read 2-byte status; verify LINK_UP
//     bit (per §5.1.6.2; slirp asserts the link up by default).
//   - TX: build virtio_net_hdr (12 B, all zero — VIRTIO_NET_HDR_GSO_NONE)
//     + a broadcast ARP request for who-has 10.0.2.2 from 10.0.2.15.
//     Submit a 2-descriptor chain (hdr OUT + frame OUT), kick the
//     QueueNotify, wait for TX-IRQ, verify used.idx advanced to 1.
//
// Why TX-only:
//   - RX requires pre-publishing empty buffers; the device fills them
//     on packet arrival. Doable, but the chunk scope here is to prove
//     TX virtqueue mechanics symmetric to virtio-blk-probe's READ.
//   - Receiving the ARP response is a P4-Jb concern.
//
// Why a broadcast ARP request:
//   - Recognizable ethertype (0x0806) — easier to spot in
//     -object filter-dump traces than random bytes.
//   - Minimum payload (42 bytes); fits trivially in a single descriptor.
//   - Slirp's user-mode network will respond to this on host side; the
//     response just gets dropped by us in P4-Ja (no RX). A future
//     P4-Jb probe will set up RX + reap the response.
//
// QEMU virtio-net slot: P4-Ja places -device virtio-net-device AFTER
// disk_flags in run-vm.sh, so it lands in slot 30 (one below blk's
// slot 31). The probe scans all 32 slots for DeviceID=1.
//
// INTID = 48 + slot, same as blk. Edge-triggered SPI per the kernel-side
// gic_set_spi_edge_triggered called inside kobj_irq_create.

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
// User-VA layout (probe-private).
// =============================================================================
//
// Distinct from virtio-blk-probe's user-VAs so a future composed test
// running both in parallel wouldn't collide. (Today each probe runs
// in its own process so collisions are impossible anyway.)

const MMIO_USER_VA: u64 = 0x0050_0000;
const DMA_USER_VA: u64  = 0x0061_0000;

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
const VIRTIO_DEVICE_ID_NET: u32  = 1;

// Status bits (§2.1).
const STATUS_ACKNOWLEDGE: u32 = 1;
const STATUS_DRIVER: u32      = 2;
const STATUS_DRIVER_OK: u32   = 4;
const STATUS_FEATURES_OK: u32 = 8;
const STATUS_FAILED: u32      = 128;

// VIRTIO_F_VERSION_1 = bit 32 → bank-1 bit 0.
const VIRTIO_F_VERSION_1_BIT_BANK1: u32 = 1 << 0;

// =============================================================================
// virtio-net specifics (§5.1).
// =============================================================================
//
// Feature bits (in bank 0 unless tagged BANK1):
//   bit  0 VIRTIO_NET_F_CSUM        — driver computes checksum (decline)
//   bit  1 VIRTIO_NET_F_GUEST_CSUM  — device computes RX checksum (decline)
//   bit  5 VIRTIO_NET_F_MAC         — config space has explicit MAC (accept)
//   bit 15 VIRTIO_NET_F_MRG_RXBUF   — RX descriptor merging (decline; complicates RX)
//   bit 16 VIRTIO_NET_F_STATUS      — config space has status field (accept)
//   bit 17 VIRTIO_NET_F_CTRL_VQ     — control virtqueue exists (decline)
//
// We accept exactly: VIRTIO_F_VERSION_1 (bank 1), VIRTIO_NET_F_MAC,
// VIRTIO_NET_F_STATUS. The two virtqueues (RX = 0, TX = 1) are always
// present; we configure only TX in P4-Ja.

const VIRTIO_NET_F_MAC_BIT: u32    = 1 << 5;
const VIRTIO_NET_F_STATUS_BIT: u32 = 1 << 16;

// Config-space field offsets (relative to REG_CONFIG_BASE).
const NET_CFG_MAC: u64    = 0;     // 6 bytes
const NET_CFG_STATUS: u64 = 6;     // 2 bytes (only valid when VIRTIO_NET_F_STATUS was negotiated)

const VIRTIO_NET_S_LINK_UP: u16 = 1 << 0;

// Queue indices.
const NET_QUEUE_TX: u32 = 1;

// Descriptor flags (§2.7.5).
const VIRTQ_DESC_F_NEXT: u16 = 1;

// VIRTIO 1.0+ net header: 12 bytes (vs 10 for legacy). With
// VIRTIO_F_VERSION_1 the 12-byte header is mandatory regardless of
// MRG_RXBUF (per §5.1.6.1 + erratum: the num_buffers field is
// always present in modern format).
const VIRTIO_NET_HDR_LEN: u32 = 12;

// =============================================================================
// DMA buffer layout (single 4 KiB page; TX queue only).
// =============================================================================

const QUEUE_SIZE: u16 = 16;
const DMA_BUFSIZE: u64 = PAGE_SIZE;

const DESC_OFF: u64     = 0x000;
const AVAIL_OFF: u64    = 0x100;
const USED_OFF: u64     = 0x200;
const NET_HDR_OFF: u64  = 0x300;
const FRAME_OFF: u64    = 0x320;

// Frame: 14 (eth hdr) + 28 (ARP) = 42 bytes.
const ETH_HDR_LEN: u32 = 14;
const ARP_LEN: u32     = 28;
const FRAME_LEN: u32   = ETH_HDR_LEN + ARP_LEN;

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
unsafe fn read16(addr: u64) -> u16 {
    core::ptr::read_volatile(addr as *const u16)
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
// Diagnostics — integer formatters mirror virtio-blk-probe.
// =============================================================================
//
// NOTE: no pretouch_rodata_pages() here. The LOAD segment for this
// binary fits within a single page (objdump confirms .text + .rodata
// = ~3.2 KiB < 4 KiB), so every string literal lives in page 0 and
// the kernel-mode SYS_PUTS reads are backed by the same VMA that
// covers the entry point (page 0 is faulted in on first execution).
// If a future virtio-net binary grows past page 0, see P4-Ic7
// surprise #2 — the pretouch loop covers the gap until Phase 5+
// adds uaccess-emulation.

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
    let hex = |n: u8| -> [u8; 2] {
        let hi = n >> 4;
        let lo = n & 0xf;
        let h2c = |x: u8| -> u8 { if x < 10 { b'0' + x } else { b'a' + (x - 10) } };
        [h2c(hi), h2c(lo)]
    };
    for (i, b) in mac.iter().enumerate() {
        let h = hex(*b);
        buf[i * 3]     = h[0];
        buf[i * 3 + 1] = h[1];
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
            log("virtio-net-probe: SYS_MMIO_CREATE failed for page ");
            log_dec(i as u32);
            log("\n");
            return None;
        }
        let map_rc = unsafe { t_mmio_map(handle, va, prot) };
        if map_rc < 0 {
            log("virtio-net-probe: SYS_MMIO_MAP failed for page ");
            log_dec(i as u32);
            log("\n");
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
// VirtIO 1.2 device initialization for the NET device.
// =============================================================================
//
// Returns (negotiated_features_lo, negotiated_features_hi) on success.
// We only inspect feature bits during initialization to decide what to
// claim back to the device; post-init the negotiated bits drive what
// MUST be respected at runtime (e.g., NET_F_MAC means MAC reads from
// config space rather than being driver-chosen).

fn init_device(slot_va: u64, dma_pa: u64) -> Option<u32> {
    // Step 1: RESET.
    unsafe { write32(slot_va + REG_STATUS, 0) };

    // Step 2: ACKNOWLEDGE.
    unsafe { write32(slot_va + REG_STATUS, STATUS_ACKNOWLEDGE) };

    // Step 3: DRIVER.
    unsafe { write32(slot_va + REG_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER) };

    // Step 4a: read DeviceFeatures bank 0.
    unsafe { write32(slot_va + REG_DEVICE_FEATURES_SEL, 0) };
    let dev_feat_lo = unsafe { read32(slot_va + REG_DEVICE_FEATURES) };

    // Step 4b: read DeviceFeatures bank 1; verify VIRTIO_F_VERSION_1.
    unsafe { write32(slot_va + REG_DEVICE_FEATURES_SEL, 1) };
    let dev_feat_hi = unsafe { read32(slot_va + REG_DEVICE_FEATURES) };
    if dev_feat_hi & VIRTIO_F_VERSION_1_BIT_BANK1 == 0 {
        log("virtio-net-probe: device lacks VIRTIO_F_VERSION_1\n");
        unsafe { write32(slot_va + REG_STATUS, STATUS_FAILED) };
        return None;
    }

    // Step 4c: pick the bank-0 features we want to keep. We accept
    // NET_F_MAC + NET_F_STATUS if offered (best-effort), and drop
    // everything else. The probe doesn't need any of CSUM / GUEST_CSUM
    // / GSO / MRG_RXBUF / CTRL_VQ; declining keeps the device's TX
    // path in the canonical "just DMA what the driver wrote" mode.
    let want_lo = dev_feat_lo & (VIRTIO_NET_F_MAC_BIT | VIRTIO_NET_F_STATUS_BIT);

    unsafe { write32(slot_va + REG_DRIVER_FEATURES_SEL, 0) };
    unsafe { write32(slot_va + REG_DRIVER_FEATURES, want_lo) };

    unsafe { write32(slot_va + REG_DRIVER_FEATURES_SEL, 1) };
    unsafe { write32(slot_va + REG_DRIVER_FEATURES, VIRTIO_F_VERSION_1_BIT_BANK1) };

    // Step 5: FEATURES_OK + readback.
    unsafe {
        write32(slot_va + REG_STATUS,
                STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK);
    };
    let status = unsafe { read32(slot_va + REG_STATUS) };
    if status & STATUS_FEATURES_OK == 0 {
        log("virtio-net-probe: FEATURES_OK rejected by device; status=");
        log_hex(status);
        log("\n");
        return None;
    }

    // Step 6: configure TX virtqueue (index 1).
    unsafe { write32(slot_va + REG_QUEUE_SEL, NET_QUEUE_TX) };

    let num_max = unsafe { read32(slot_va + REG_QUEUE_NUM_MAX) };
    if num_max < QUEUE_SIZE as u32 {
        log("virtio-net-probe: QueueNumMax(TX)=");
        log_dec(num_max);
        log(" below QUEUE_SIZE\n");
        unsafe { write32(slot_va + REG_STATUS, STATUS_FAILED) };
        return None;
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
    unsafe { write32(slot_va + REG_QUEUE_READY, 1) };

    // Step 7: DRIVER_OK.
    unsafe {
        write32(slot_va + REG_STATUS,
                STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK | STATUS_DRIVER_OK);
    };

    Some(want_lo)
}

// =============================================================================
// Read MAC + status from device-specific config space.
// =============================================================================
//
// MAC bytes are stored in network order (byte 0 = first byte of OUI);
// the MMIO config space is byte-addressable for MAC and 16-bit-LE for
// status. We read MAC via byte-wise read_u8 to dodge any alignment
// pitfall, and status via aligned read16 (offset 6 is 2-byte-aligned).

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
// Build a broadcast ARP request and submit it on the TX queue.
// =============================================================================
//
// Frame layout (42 bytes total):
//   Ethernet header (14 B):
//     dst MAC      [0..6]   = ff:ff:ff:ff:ff:ff (broadcast)
//     src MAC      [6..12]  = our MAC (read from config)
//     ethertype    [12..14] = 0x0806 (ARP), network order
//   ARP body (28 B):
//     hw type      [0..2]   = 0x0001 (Ethernet), network order
//     proto type   [2..4]   = 0x0800 (IPv4), network order
//     hw len       [4]      = 6
//     proto len    [5]      = 4
//     opcode       [6..8]   = 0x0001 (request), network order
//     sender HW    [8..14]  = our MAC
//     sender IP    [14..18] = 10.0.2.15 (slirp guest IP)
//     target HW    [18..24] = 00:00:00:00:00:00
//     target IP    [24..28] = 10.0.2.2 (slirp gateway)

fn build_arp_frame(dma_va: u64, src_mac: [u8; 6]) {
    let f = dma_va + FRAME_OFF;
    let mut p = 0u64;
    // dst MAC: broadcast.
    for _ in 0..6 {
        unsafe { write_u8(f + p, 0xff) };
        p += 1;
    }
    // src MAC.
    for b in src_mac.iter() {
        unsafe { write_u8(f + p, *b) };
        p += 1;
    }
    // ethertype = 0x0806 (network order = big-endian).
    unsafe { write_u8(f + p, 0x08) }; p += 1;
    unsafe { write_u8(f + p, 0x06) }; p += 1;

    // ARP body.
    // hw type = 0x0001
    unsafe { write_u8(f + p, 0x00) }; p += 1;
    unsafe { write_u8(f + p, 0x01) }; p += 1;
    // proto type = 0x0800
    unsafe { write_u8(f + p, 0x08) }; p += 1;
    unsafe { write_u8(f + p, 0x00) }; p += 1;
    // hw len = 6, proto len = 4
    unsafe { write_u8(f + p, 6) }; p += 1;
    unsafe { write_u8(f + p, 4) }; p += 1;
    // opcode = 0x0001 (request)
    unsafe { write_u8(f + p, 0x00) }; p += 1;
    unsafe { write_u8(f + p, 0x01) }; p += 1;
    // sender HW = our MAC
    for b in src_mac.iter() { unsafe { write_u8(f + p, *b) }; p += 1; }
    // sender IP = 10.0.2.15
    unsafe { write_u8(f + p, 10) }; p += 1;
    unsafe { write_u8(f + p,  0) }; p += 1;
    unsafe { write_u8(f + p,  2) }; p += 1;
    unsafe { write_u8(f + p, 15) }; p += 1;
    // target HW = 00:00:00:00:00:00
    for _ in 0..6 { unsafe { write_u8(f + p, 0) }; p += 1; }
    // target IP = 10.0.2.2
    unsafe { write_u8(f + p, 10) }; p += 1;
    unsafe { write_u8(f + p,  0) }; p += 1;
    unsafe { write_u8(f + p,  2) }; p += 1;
    unsafe { write_u8(f + p,  2) }; p += 1;

    // Sanity (compile-time check would be cleaner but no const_fn here).
    if p != FRAME_LEN as u64 {
        log("virtio-net-probe: frame builder offset mismatch\n");
    }
}

// =============================================================================
// Submit + wait for completion.
// =============================================================================
//
// 2-descriptor chain on the TX queue:
//   desc[0]: virtio_net_hdr (12 B, OUT — device reads) → next=1
//   desc[1]: ethernet frame (42 B, OUT — device reads) → last
//
// Neither descriptor has VIRTQ_DESC_F_WRITE because TX is OUT-only.

fn submit_tx_arp(slot_va: u64, dma_va: u64, dma_pa: u64) {
    // Zero the virtio_net_hdr; all fields zero = no GSO, no checksum
    // offload, num_buffers irrelevant for TX (it's a RX-only field
    // per §5.1.6.1 erratum but VIRTIO 1.0+ requires the byte to be
    // present in TX too).
    for i in 0..VIRTIO_NET_HDR_LEN {
        unsafe { write_u8(dma_va + NET_HDR_OFF + i as u64, 0) };
    }

    // Descriptor chain.
    let desc_va = dma_va + DESC_OFF;
    unsafe {
        // desc[0]: net hdr.
        write64(desc_va + 0,  dma_pa + NET_HDR_OFF);
        write32(desc_va + 8,  VIRTIO_NET_HDR_LEN);
        write16(desc_va + 12, VIRTQ_DESC_F_NEXT);
        write16(desc_va + 14, 1);
        // desc[1]: frame.
        write64(desc_va + 16, dma_pa + FRAME_OFF);
        write32(desc_va + 24, FRAME_LEN);
        write16(desc_va + 28, 0); // last descriptor: no flags.
        write16(desc_va + 30, 0);
    };

    // Avail ring: ring[0] = head desc 0; bump idx LAST.
    let avail_va = dma_va + AVAIL_OFF;
    unsafe {
        write16(avail_va + 0, 0);
        write16(avail_va + 4, 0);
    };
    dsb_sy();
    unsafe { write16(avail_va + 2, 1) };
    dsb_sy();

    // Kick: write the TX queue's index to QueueNotify.
    unsafe { write32(slot_va + REG_QUEUE_NOTIFY, NET_QUEUE_TX) };
}

fn wait_and_verify_tx(irq_handle: i64, slot_va: u64, dma_va: u64) -> Result<(), ()> {
    let count = unsafe { t_irq_wait(irq_handle) };
    if count < 0 {
        log("virtio-net-probe: FAIL — SYS_IRQ_WAIT returned error\n");
        return Err(());
    }

    // Read + ACK InterruptStatus so the device deasserts the line.
    let int_status = unsafe { read32(slot_va + REG_INTERRUPT_STATUS) };
    unsafe { write32(slot_va + REG_INTERRUPT_ACK, int_status) };

    let used_va = dma_va + USED_OFF;
    let used_idx = unsafe { read16(used_va + 2) };
    if used_idx != 1 {
        log("virtio-net-probe: FAIL — TX used.idx != 1 (got ");
        log_dec(used_idx as u32);
        log(")\n");
        return Err(());
    }
    let used_id = unsafe { read32(used_va + 4) };
    if used_id != 0 {
        log("virtio-net-probe: FAIL — TX used.ring[0].id != 0 (got id=");
        log_dec(used_id);
        log(")\n");
        return Err(());
    }

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
            log("virtio-net-probe: FAIL — virtio-mmio bank claim failed\n");
            unsafe { t_exits(1) };
        }
    };

    let (slot, slot_va) = match find_net_slot(mmio_base) {
        Some(found) => found,
        None => {
            log("virtio-net-probe: SKIP — no virtio-mmio slot has DeviceID=1 (no -device virtio-net-device wired in QEMU?)\n");
            return 0;
        }
    };

    let intid = VIRTIO_MMIO_GIC_INTID_BASE + slot;

    let version = unsafe { read32(slot_va + REG_VERSION) };
    if version != VIRTIO_MMIO_VERSION_MODERN {
        log("virtio-net-probe: FAIL — slot reports legacy version (");
        log_dec(version);
        log("); modern (2) required\n");
        unsafe { t_exits(1) };
    }

    let irq_handle = unsafe { t_irq_create(intid, T_RIGHT_SIGNAL) };
    if irq_handle < 0 {
        log("virtio-net-probe: FAIL — SYS_IRQ_CREATE failed\n");
        unsafe { t_exits(1) };
    }

    let dma_handle = unsafe {
        t_dma_create(DMA_BUFSIZE, T_RIGHT_READ | T_RIGHT_WRITE | T_RIGHT_MAP)
    };
    if dma_handle < 0 {
        log("virtio-net-probe: FAIL — SYS_DMA_CREATE failed\n");
        unsafe { t_exits(1) };
    }
    let dma_pa = unsafe {
        t_dma_map(dma_handle, DMA_USER_VA, T_PROT_READ | T_PROT_WRITE)
    };
    if dma_pa < 0 {
        log("virtio-net-probe: FAIL — SYS_DMA_MAP failed\n");
        unsafe { t_exits(1) };
    }

    // Pre-warm DMA pages (Normal-WB; the demand-page path needs to
    // install PTEs before the device's DMA could race a CPU access).
    for off in (0..DMA_BUFSIZE).step_by(PAGE_SIZE as usize) {
        unsafe { write_u8(DMA_USER_VA + off, 0) };
    }

    let negotiated_lo = match init_device(slot_va, dma_pa as u64) {
        Some(want_lo) => want_lo,
        None => unsafe { t_exits(1) },
    };

    // Diagnostics: MAC + link status. Read MAC unconditionally (config
    // space has a MAC even if NET_F_MAC wasn't accepted, though it
    // wouldn't be authoritative in that case — for QEMU it always is).
    let mac = read_mac(slot_va);
    log("virtio-net-probe: slot=");
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

    // Build + send the ARP request.
    build_arp_frame(DMA_USER_VA, mac);
    submit_tx_arp(slot_va, DMA_USER_VA, dma_pa as u64);

    match wait_and_verify_tx(irq_handle, slot_va, DMA_USER_VA) {
        Ok(()) => {
            log("virtio-net-probe: PASS — broadcast ARP TX completed (slot=");
            log_dec(slot);
            log(" intid=");
            log_dec(intid);
            log(")\n");
            0
        }
        Err(()) => unsafe { t_exits(1) },
    }
}
