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

use core::sync::atomic::{AtomicU32, Ordering};

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
const _: () = assert!(core::mem::offset_of!(WeftReadyHdr, ready_mask) == 4);
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

/// Read the geometry mirror netd wrote into a freshly-mapped ring Burrow at `base`
/// (the inverse of `init_ring`'s header write -- the Weft-6c native client's first
/// step after `SYS_WEFT_MAP`). Validates the WEFT_MAGIC, recovers the byte offsets,
/// and computes `ring_size = payload_off + payload_size` (the whole-Burrow size,
/// which the kernel's weft-bound Loom detection requires the registered buffer
/// length to equal exactly). Returns None on a wrong magic (an unmapped or
/// non-weft page) or a degenerate header.
///
/// The header is written once at grant and immutable thereafter; the volatile
/// per-field reads keep the optimizer from assuming constancy across a re-map.
/// A wrong geometry can only mis-shape THIS guest's own ring I/O -- the kernel
/// validates every weft slice against its private `weft_ring_view` (I-30), so a
/// scribbled mirror never crosses a safety boundary.
///
/// # Safety
/// `base` must point at a readable mapping of at least one ring header (a
/// `SYS_WEFT_MAP` return is page-aligned, so the u32 field reads are aligned).
pub unsafe fn read_ring_geom(base: *const u8) -> Option<RingGeom> {
    let rd = |off: usize| core::ptr::read_volatile(base.add(off) as *const u32);
    if rd(0) != WEFT_MAGIC {
        return None; // unmapped / non-weft page
    }
    let ring_entries = rd(4); // WeftRingHdr: ring_entries @ 4
    let desc_off = rd(8); //                desc_off     @ 8
    let payload_off = rd(12); //            payload_off  @ 12
    let payload_size = rd(16); //           payload_size @ 16
    let ready_off = rd(32); //              ready_off    @ 32
    let ring_size = payload_off.checked_add(payload_size)?;
    if ring_entries == 0 || payload_size == 0 {
        return None; // degenerate -- a live ring always has a non-empty payload
    }
    Some(RingGeom {
        ring_size,
        ring_entries,
        desc_off,
        payload_off,
        payload_size,
        ready_off,
    })
}

// =============================================================================
// Readiness-ring drive primitives (Weft-6c) -- the userspace mirror of the kernel
// weft.c weft_ready_signal / weft_ready_observe. The producer (netd) bumps the
// edge after writing RX bytes into a flow's ring; the consumer (the native client)
// observes the edge syscall-free (the Shenango single-cache-line poll, the §6
// busy-poll fast path). Single-writer-per-word: the producer owns ready_seq/
// ready_mask, the consumer owns wait_seq/wait_active (the consumer-park leg --
// arm_park/unpark -- is the v1.x direct-park mode, validated-not-wired; the native
// client's park is the Loom CQ wait, so v1.0 needs only signal + observe). The
// no-lost-wake reasoning is the store-buffer register-then-observe of
// specs/weft_readiness.tla (I-9); these wrappers reproduce the kernel orderings
// exactly so the cross-Proc seq-cst story holds across the shared page.
// =============================================================================

// Byte offsets of the readiness words within a WeftReadyHdr (asserted above).
const READY_SEQ_OFF: usize = 0; // producer-owned
const READY_MASK_OFF: usize = 4; // producer-owned
const WAIT_SEQ_OFF: usize = 64; // consumer-owned
const WAIT_ACTIVE_OFF: usize = 68; // consumer-owned

/// Producer (netd) readiness signal: publish `mask`, then bump `ready_seq`
/// (seq-cst), returning whether a parked consumer must be woken. Mirrors the
/// kernel `weft_ready_signal`: the seq-cst bump is the StoreLoad barrier before
/// the `wait_active` load, so a consumer that armed concurrently is never missed.
/// The native client's park is the Loom CQ wait (no weft Rendez), so v1.0 netd
/// ignores the return -- the bump alone drives the consumer's `ready_observe`
/// busy-poll. Single-writer-per-word: only the producer writes ready_seq/ready_mask.
///
/// # Safety
/// `ready_base` must point at a live, writable `WeftReadyHdr` in a mapping this
/// Proc owns (the ring base + `ready_off`).
pub unsafe fn ready_signal(ready_base: *mut u8, mask: u32) -> bool {
    let base = ready_base as *const u8;
    let mask_w = &*(base.add(READY_MASK_OFF) as *const AtomicU32);
    let seq_w = &*(base.add(READY_SEQ_OFF) as *const AtomicU32);
    let active_w = &*(base.add(WAIT_ACTIVE_OFF) as *const AtomicU32);
    let wseq_w = &*(base.add(WAIT_SEQ_OFF) as *const AtomicU32);
    mask_w.store(mask, Ordering::Relaxed);
    // add_fetch semantics: the post-increment value is what the kernel compares.
    let seq = seq_w.fetch_add(1, Ordering::SeqCst).wrapping_add(1);
    let armed = active_w.load(Ordering::SeqCst) != 0;
    let wseq = wseq_w.load(Ordering::Relaxed);
    armed && wseq != seq
}

/// Consumer (client) readiness observe: acquire-load `ready_seq` (paired with the
/// producer's seq-cst bump that release-published the mask) plus the latest mask.
/// Syscall-free. The caller compares the returned seq to its private last-seen to
/// detect a producer edge. Mirrors the kernel `weft_ready_observe`. Returns
/// `(ready_seq, ready_mask)`.
///
/// # Safety
/// `ready_base` must point at a live, readable `WeftReadyHdr` in a mapping this
/// Proc owns. Reads only the producer-owned words.
pub unsafe fn ready_observe(ready_base: *const u8) -> (u32, u32) {
    let seq_w = &*(ready_base.add(READY_SEQ_OFF) as *const AtomicU32);
    let mask_w = &*(ready_base.add(READY_MASK_OFF) as *const AtomicU32);
    let seq = seq_w.load(Ordering::Acquire);
    let mask = mask_w.load(Ordering::Relaxed);
    (seq, mask)
}

// =============================================================================
// Fixed-slot SPSC drive (docs/NOCTURNE.md N-2b) -- an alternative drain mode to
// netd's addr-based descriptor ring, used by nocturned's per-voice audio ring.
// The payload region is treated as K = ring_entries FIXED slots of `slot_bytes`
// each; slot i sits at payload_off + (i % K) * slot_bytes. The CONSUMER computes
// the slot offset from cons_head % K and NEVER trusts a client-written desc.addr
// -- the only client value it reads is desc[i].len, snapshotted ONCE and
// validated before use (the I-30 discipline). This closes the addr-injection
// hazard by construction: no client-supplied address is ever on the read path.
//
// Single-producer / single-consumer, cross-Proc over the one shared ring Burrow
// (the kernel's consume-once share claim makes the mapper the unique producer):
//   * the PRODUCER owns prod_tail and Release-publishes a filled slot LAST, so
//     the consumer sees the slot's payload+len before it sees the slot exists;
//   * the CONSUMER owns cons_head (Release-frees a drained slot) and `dropped`;
//   * prod_tail's Release/Acquire pair carries the payload+len across the Proc
//     boundary, so the consumer reads a slot's bytes only AFTER the producer
//     published them (no torn period), and the full-check (pt-ch >= K) stops the
//     producer overwriting a slot the consumer has not yet freed.
// Both indices are free-running u32; wrapping_sub is exact while the invariant
// 0 <= pt-ch <= K holds (K << 2^32). `% K` (not `& (K-1)`) is used so a header
// with a non-power-of-two K -- which read_ring_geom does not reject -- cannot
// mis-index; for the real ring nocturned mints K via ring_layout (a power of two).
// =============================================================================

const PROD_TAIL_OFF: usize = 20; // WeftRingHdr.prod_tail (asserted @20)
const CONS_HEAD_OFF: usize = 24; // WeftRingHdr.cons_head (asserted @24)
const DROPPED_OFF: usize = 28; //   WeftRingHdr.dropped (cons_head + 4)

/// Producer: reserve, fill, and publish ONE fixed slot. `data` (non-empty, at
/// most `slot_bytes`) is copied into slot `prod_tail % ring_entries`; the slot's
/// desc.len is set to data.len(); prod_tail is Release-bumped LAST. Returns false
/// WITHOUT touching the ring if it is FULL (ring_entries slots in flight) -- the
/// caller's back-pressure policy (yield + retry) decides what to do. Non-blocking.
///
/// # Safety
/// `base` points at a live, writable mapping of the whole ring Burrow that this
/// Proc owns, and this Proc is the sole producer of this ring.
pub unsafe fn slot_produce(base: *mut u8, geom: &RingGeom, slot_bytes: u32, data: &[u8]) -> bool {
    let k = geom.ring_entries;
    if k == 0 || data.is_empty() || data.len() > slot_bytes as usize {
        return false;
    }
    let cbase = base as *const u8;
    let pt_w = &*(cbase.add(PROD_TAIL_OFF) as *const AtomicU32);
    let ch_w = &*(cbase.add(CONS_HEAD_OFF) as *const AtomicU32);
    let pt = pt_w.load(Ordering::Relaxed); // producer is the sole writer of prod_tail
    let ch = ch_w.load(Ordering::Acquire); // pairs with the consumer's Release free
    if pt.wrapping_sub(ch) >= k {
        return false; // full: every slot in flight, none freed
    }
    let i = (pt % k) as usize;
    let slot = geom.payload_off as usize + i * slot_bytes as usize;
    core::ptr::copy_nonoverlapping(data.as_ptr(), base.add(slot), data.len());
    // desc.addr is set for tidiness/debuggability only; the consumer IGNORES it
    // and computes the offset itself. desc.len IS the protocol (snapshotted +
    // validated by the consumer). Both are written before the Release publish.
    let desc = base.add(geom.desc_off as usize + i * DESC_LEN);
    core::ptr::write_volatile(desc as *mut u32, (i * slot_bytes as usize) as u32); // desc.addr
    core::ptr::write_volatile(desc.add(4) as *mut u32, data.len() as u32); // desc.len
    pt_w.store(pt.wrapping_add(1), Ordering::Release); // publish the slot LAST
    true
}

/// Consumer: peek, copy, and free ONE fixed slot into `scratch` (which must be at
/// least `slot_bytes`). Returns the valid byte count copied, or 0 for "no data
/// this period" -- either the ring is EMPTY, or the slot's client-written desc.len
/// failed validation and was DROPPED (both read as silence upstream). Snapshots
/// desc.len ONCE and validates it (0 < len <= slot_bytes, len % align == 0 when
/// align != 0), copies from the CONSUMER-COMPUTED offset (never desc.addr), then
/// Release-bumps cons_head. Non-blocking.
///
/// Ordering: the Acquire load of prod_tail synchronizes-with the producer's
/// Release publish, so for a slot with cons_head < prod_tail the producer's
/// payload+len writes happen-before these reads -- the desc.len read and the
/// payload copy are ordered after it, never concurrent, so the plain volatile
/// read + copy are not a data race. The Release store of cons_head is bumped
/// AFTER the copy, so the producer never overwrites a slot mid-copy.
///
/// # Safety
/// `base` points at a live, writable mapping of the whole ring Burrow that this
/// Proc owns, and this Proc is the sole consumer of this ring.
pub unsafe fn slot_consume(
    base: *mut u8,
    geom: &RingGeom,
    slot_bytes: u32,
    align: u32,
    scratch: &mut [u8],
) -> usize {
    let k = geom.ring_entries;
    if k == 0 || scratch.len() < slot_bytes as usize {
        return 0;
    }
    let cbase = base as *const u8;
    let pt_w = &*(cbase.add(PROD_TAIL_OFF) as *const AtomicU32);
    let ch_w = &*(cbase.add(CONS_HEAD_OFF) as *const AtomicU32);
    let ch = ch_w.load(Ordering::Relaxed); // consumer is the sole writer of cons_head
    let pt = pt_w.load(Ordering::Acquire); // pairs with the producer's Release publish
    if ch == pt {
        return 0; // empty
    }
    let i = (ch % k) as usize;
    let desc = cbase.add(geom.desc_off as usize + i * DESC_LEN);
    let len = core::ptr::read_volatile(desc.add(4) as *const u32); // desc.len, snapshot ONCE
    let valid = len != 0 && len <= slot_bytes && (align == 0 || len % align == 0);
    let n = if valid {
        let slot = geom.payload_off as usize + i * slot_bytes as usize;
        core::ptr::copy_nonoverlapping(cbase.add(slot), scratch.as_mut_ptr(), len as usize);
        len as usize
    } else {
        // Diagnostics only; the consumer is the sole writer of `dropped`.
        let dr_w = &*(cbase.add(DROPPED_OFF) as *const AtomicU32);
        dr_w.store(dr_w.load(Ordering::Relaxed).wrapping_add(1), Ordering::Relaxed);
        0
    };
    ch_w.store(ch.wrapping_add(1), Ordering::Release); // free the slot AFTER the copy
    n
}

/// The number of published-but-undrained slots (prod_tail - cons_head), read by
/// the consumer to decide whether a stopped stream has work. Acquire on prod_tail
/// pairs with the producer's publish; cons_head is the consumer's own value.
///
/// # Safety
/// `base` points at a live, readable mapping of this ring Burrow's header.
pub unsafe fn slot_pending(base: *const u8) -> u32 {
    let pt_w = &*(base.add(PROD_TAIL_OFF) as *const AtomicU32);
    let ch_w = &*(base.add(CONS_HEAD_OFF) as *const AtomicU32);
    let pt = pt_w.load(Ordering::Acquire);
    let ch = ch_w.load(Ordering::Relaxed);
    pt.wrapping_sub(ch)
}
