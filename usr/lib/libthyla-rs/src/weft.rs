// Weft -- the userspace mirror of the per-flow capability dataplane ring ABI
// (kernel/include/thylacine/weft.h; NET-THROUGHPUT.md section 5/6). The shared
// per-flow ring Burrow carries a fixed on-page layout both sides agree on:
//
//   [ring_hdr 64][ready_hdr 128][desc[ring_entries] 16N][payload]
//
// netd (the ring's allocator, the 9P server) writes the geometry header + zeroes
// the readiness header at flow grant (init_ring); the native client (the guest's
// SYS_WEFT_MAP target) reads the geometry mirror + drives the descriptor / read-
// iness rings. This module is the loom.h <-> libthyla_rs::loom precedent for Weft:
// the `repr(C)` structs are byte-pinned to the kernel header by compile-time size
// + offset asserts, so a layout drift fails the build, never a silent corruption.
//
// Weft-6b-1 uses init_ring (netd) + the geometry mirror (the boot probe reads the
// magic). The descriptor / readiness DRIVE primitives (push / pop / poke) land
// with the native push/pop/wait API (Weft-6c) -- this file is the ABI substrate.

#![allow(dead_code)]

/// "WEFT" -- the ring header magic the guest reads to confirm a live ring.
pub const WEFT_MAGIC: u32 = 0x5746_4554;

/// Max descriptor slots per ring (a power-of-two sanity cap; the real ceiling is
/// the shared Burrow size, validated by ring_layout). Mirrors WEFT_MAX_ENTRIES.
pub const WEFT_MAX_ENTRIES: u32 = 1024;

/// Payloads below this stay on the byte-copy 9P path (the Plan 9 baseline;
/// virtio-9p's <1024 B copy threshold -- NET-THROUGHPUT.md section 4.8).
pub const WEFT_HYBRID_THRESHOLD: u32 = 1024;

/// Readiness edge masks (POLLIN/POLLOUT-aligned). Mirrors WEFT_READY_*.
pub const WEFT_READY_RX: u32 = 0x001;
pub const WEFT_READY_TX: u32 = 0x004;

/// Fixed control-region sizes (the layout constants ring_layout composes).
pub const RING_HDR_LEN: usize = 64;
pub const READY_HDR_LEN: usize = 128;
pub const DESC_LEN: usize = 16;

// =============================================================================
// weft_desc -- one payload descriptor the guest posts into the split-ring.
// `addr` is a byte offset WITHIN the payload region (never the Burrow base), so a
// descriptor can never point at the ring header or descriptor array. 16 bytes.
// =============================================================================
#[repr(C)]
#[derive(Copy, Clone)]
pub struct WeftDesc {
    pub addr: u32,  // payload-region-relative byte offset
    pub len: u32,   // payload byte count (0 is rejected as malformed)
    pub flags: u32, // reserved; must be 0 at v1.0
    pub _resv: u32,
}

const _: () = assert!(core::mem::size_of::<WeftDesc>() == 16);
const _: () = assert!(core::mem::offset_of!(WeftDesc, addr) == 0);
const _: () = assert!(core::mem::offset_of!(WeftDesc, len) == 4);
const _: () = assert!(core::mem::offset_of!(WeftDesc, flags) == 8);

// =============================================================================
// weft_ring_hdr -- the shared control words at offset 0 of the per-flow Burrow.
// 64 bytes. The geometry words are kernel/netd-written ONCE at grant and immutable
// thereafter (a user-readable MIRROR; neither side trusts it on the drain path).
// prod_tail is guest-owned (produces), cons_head is consumer-owned (drains).
// =============================================================================
#[repr(C)]
#[derive(Copy, Clone)]
pub struct WeftRingHdr {
    pub magic: u32,        // WEFT_MAGIC (mirror)
    pub ring_entries: u32, // descriptor slots, power of two (mirror)
    pub desc_off: u32,     // byte offset of the descriptor array (mirror)
    pub payload_off: u32,  // byte offset of the payload region (mirror)
    pub payload_size: u32, // payload-region byte length (mirror)
    pub prod_tail: u32,    // guest-owned: produced
    pub cons_head: u32,    // consumer-owned: drained (mirror)
    pub dropped: u32,      // descriptors rejected -- diagnostics
    pub ready_off: u32,    // byte offset of the readiness header (mirror)
    _pad: [u32; 7],
}

const _: () = assert!(core::mem::size_of::<WeftRingHdr>() == 64);
const _: () = assert!(core::mem::offset_of!(WeftRingHdr, prod_tail) == 20);
const _: () = assert!(core::mem::offset_of!(WeftRingHdr, cons_head) == 24);
const _: () = assert!(core::mem::offset_of!(WeftRingHdr, ready_off) == 32);

// =============================================================================
// weft_ready_hdr -- the Shenango single-cache-line readiness poke. Two cache
// lines, single-writer-per-word: the producer (netd) owns ready_seq/ready_mask;
// the consumer (guest) owns wait_seq/wait_active. 128 bytes.
// =============================================================================
#[repr(C)]
#[derive(Copy, Clone)]
pub struct WeftReadyHdr {
    pub ready_seq: u32,   // producer-owned: bumped per readiness edge
    pub ready_mask: u32,  // producer-owned: WEFT_READY_* of the latest edge
    _ppad: [u32; 14],     // pad the producer words to a full cache line
    pub wait_seq: u32,    // consumer-owned: the seq registered before parking
    pub wait_active: u32, // consumer-owned: 1 while parked, 0 while running
    _cpad: [u32; 14],     // pad the consumer words to a full cache line
}

const _: () = assert!(core::mem::size_of::<WeftReadyHdr>() == 128);
const _: () = assert!(core::mem::offset_of!(WeftReadyHdr, ready_seq) == 0);
const _: () = assert!(core::mem::offset_of!(WeftReadyHdr, wait_seq) == 64);
const _: () = assert!(core::mem::offset_of!(WeftReadyHdr, wait_active) == 68);

/// The computed [hdr][ready][desc[]][payload] geometry of a ring Burrow of `size`
/// bytes. The byte offsets the descriptor / readiness drive resolve against.
#[derive(Copy, Clone)]
pub struct RingGeom {
    pub ring_size: u32,
    pub ring_entries: u32,
    pub desc_off: u32,
    pub payload_off: u32,
    pub payload_size: u32,
    pub ready_off: u32,
}

/// Compute the standard layout for a ring Burrow of `size` bytes with
/// `ring_entries` descriptor slots. Mirrors kernel weft_ring_layout: `ring_entries`
/// is a non-zero power of two <= WEFT_MAX_ENTRIES, the header + readiness + desc
/// array + a non-empty payload region must fit, and the payload length must be
/// representable in the u32 descriptor domain. Returns None if it does not fit.
/// Does not touch the page.
pub fn ring_layout(size: u64, ring_entries: u32) -> Option<RingGeom> {
    if ring_entries == 0 || (ring_entries & (ring_entries - 1)) != 0 {
        return None; // must be a power of two
    }
    if ring_entries > WEFT_MAX_ENTRIES {
        return None;
    }
    if size > u32::MAX as u64 {
        return None; // ring_size is a u32 in the Rweft ABI
    }
    let ready_off = RING_HDR_LEN as u64; // 64
    let desc_off = ready_off + READY_HDR_LEN as u64; // 192
    let desc_bytes = ring_entries as u64 * DESC_LEN as u64;
    let mut payload_off = desc_off + desc_bytes;
    payload_off = (payload_off + 15) & !15; // 16-align the payload region
    if payload_off >= size {
        return None; // no room for a payload region
    }
    let payload_size = size - payload_off;
    if payload_size > u32::MAX as u64 {
        return None;
    }
    Some(RingGeom {
        ring_size: size as u32,
        ring_entries,
        desc_off: desc_off as u32,
        payload_off: payload_off as u32,
        payload_size: payload_size as u32,
        ready_off: ready_off as u32,
    })
}

/// Write the immutable geometry mirror (ring header) + zero the readiness header
/// into a freshly-attached ring Burrow at `base`. Mirrors weft_ring_init_hdr +
/// weft_ready_init_hdr. Called once by netd at flow grant, before SYS_WEFT_SHARE.
/// Returns the geometry (so the caller can embed ring_size/ring_entries in Rweft);
/// None if the layout does not fit `size`.
///
/// # Safety
/// `base` must point at a writable mapping of at least `size` bytes that nothing
/// else accesses concurrently (netd's own ANON ring, pre-share). It is page-aligned
/// (a SYS_BURROW_ATTACH return), so the repr(C) writes are aligned.
pub unsafe fn init_ring(base: *mut u8, size: u64, ring_entries: u32) -> Option<RingGeom> {
    let geom = ring_layout(size, ring_entries)?;
    let hdr = WeftRingHdr {
        magic: WEFT_MAGIC,
        ring_entries: geom.ring_entries,
        desc_off: geom.desc_off,
        payload_off: geom.payload_off,
        payload_size: geom.payload_size,
        prod_tail: 0,
        cons_head: 0,
        dropped: 0,
        ready_off: geom.ready_off,
        _pad: [0; 7],
    };
    core::ptr::write(base as *mut WeftRingHdr, hdr);
    let ready = WeftReadyHdr {
        ready_seq: 0,
        ready_mask: 0,
        _ppad: [0; 14],
        wait_seq: 0,
        wait_active: 0,
        _cpad: [0; 14],
    };
    core::ptr::write(base.add(geom.ready_off as usize) as *mut WeftReadyHdr, ready);
    Some(geom)
}
