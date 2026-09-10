// inlineaccum -- the inline-media place-request accumulator (I-47, HALCYON.md
// 14.7). This is the FORMAT-FUZZ-CRITICAL half of the /srv/halcyon channel, and
// it is PURE (no syscalls): the bin's 9P `h_write` drives it one Twrite at a
// time, and every untrusted-byte decision -- validate-before-allocate, the
// heap-safe per-image cap, sequential-only writes, no over-accumulation -- lives
// here so it is host-tested (the audit's regression floor) rather than buried in
// the syscall body.
//
// The wire contract itself is `inlinewire` (shared with `view`, the writer):
// a 16-byte header (magic/format/w/h) then w*h ARGB u32s as LE bytes. That
// crate's `PlaceHeader::parse` fully validates the header before this module
// commits a byte of the payload.

use alloc::vec::Vec;
use inlinewire::{PlaceHeader, HEADER_LEN};

/// The result of feeding one write to the accumulator.
pub enum AccumStep {
    /// The write was accepted; more bytes are needed to complete the image.
    More,
    /// The full raster arrived. The accumulator has reset and may receive a
    /// subsequent image on the same fid (offsets continue cumulatively).
    Done { w: u32, h: u32, argb: Vec<u32> },
    /// A protocol or bounds violation (bad header, over-cap dimensions, a
    /// non-sequential offset, or bytes past the declared image). The caller
    /// replies Rlerror and tears the transfer down -- a partial is discarded.
    Reject,
}

/// Accumulates one place-request at a time from sequential writes. `max_pixels`
/// is the caller's heap-safe per-image cap; it MUST be <= `inlinewire::MAX_PIXELS`
/// (the wire's own ceiling) and is what keeps a decoded raster from exhausting
/// the renderer's fixed heap. Held per (connection, place-fid); dropped -- with
/// any partial -- when that fid clunks or the connection tears down.
pub struct PlaceAccum {
    header: Option<PlaceHeader>,
    buf: Vec<u8>,
    /// Bytes of completed images already delivered on this fid: the base the
    /// next write's offset is measured from (so a second image on one fid, whose
    /// first Twrite arrives at the cumulative fid offset, still reads sequential).
    base: u64,
    max_pixels: u64,
}

impl PlaceAccum {
    pub fn new(max_pixels: u64) -> PlaceAccum {
        PlaceAccum {
            header: None,
            buf: Vec::new(),
            base: 0,
            max_pixels,
        }
    }

    /// Refresh the per-image cap WITHOUT disturbing an in-flight buffer (I-47
    /// F2). The cap gates the NEXT header parse, so a multi-image transfer on a
    /// reused fid (or a transfer whose first header has not yet arrived) picks
    /// up the current display-scaled cap rather than the one captured at
    /// construction. A header already parsed keeps its reserve unchanged.
    pub fn set_max_pixels(&mut self, max_pixels: u64) {
        self.max_pixels = max_pixels;
    }

    /// Feed one write at `offset`. Writes MUST be contiguous (the place channel
    /// does not support seeks); the header is resolved -- and validated against
    /// the wire rules AND the heap-safe cap -- before any payload is buffered, so
    /// a hostile header can never drive a large reserve.
    pub fn write(&mut self, offset: u64, data: &[u8]) -> AccumStep {
        // Sequential-only, measured from the base of the current image.
        if offset != self.base + self.buf.len() as u64 {
            return AccumStep::Reject;
        }

        // Resolve (or confirm) the header, bind it, and buffer this write's
        // bytes -- with every bound applied BEFORE the allocation, not after.
        let h = match self.header {
            Some(h) => {
                // Header known: never let the buffer grow past the declared total.
                if self.buf.len() + data.len() > h.total_len() {
                    return AccumStep::Reject;
                }
                self.buf.extend_from_slice(data);
                h
            }
            None => {
                let have = self.buf.len();
                // Not yet enough bytes to form a 16-byte header: buffer the few we
                // have (< HEADER_LEN total -- bounded) and wait for more.
                if have + data.len() < HEADER_LEN {
                    self.buf.extend_from_slice(data);
                    return AccumStep::More;
                }
                // Assemble exactly the header bytes from what we hold plus the
                // front of this write -- WITHOUT first extending `buf` with the
                // (possibly huge, possibly hostile) full `data`.
                let mut hb = [0u8; HEADER_LEN];
                hb[..have].copy_from_slice(&self.buf);
                hb[have..].copy_from_slice(&data[..HEADER_LEN - have]);
                let h = match PlaceHeader::parse(&hb) {
                    Some(h) => h,
                    None => return AccumStep::Reject,
                };
                // The heap-safe per-image cap (tighter than the wire's
                // MAX_PIXELS): reject before allocating a raster the renderer's
                // heap cannot hold.
                if (h.w as u64) * (h.h as u64) > self.max_pixels {
                    return AccumStep::Reject;
                }
                // This first write must not carry more than the whole image.
                if have + data.len() > h.total_len() {
                    return AccumStep::Reject;
                }
                self.header = Some(h);
                // Reserve the EXACT payload capacity now, before the first
                // extend, so the buffer never Vec-doubles: an incrementally
                // grown Vec rounds a just-over-8-MiB length up to a 16-MiB
                // capacity (a 2x overshoot), which is what let MAX_CONNS
                // accumulators reach the whole 64 MiB heap (audit F1/F2). With
                // reserve_exact the footprint is exactly total_len, so the
                // heap budget (placesrv PLACE_MAX_PIXELS x MAX_CONNS) is real.
                self.buf.reserve_exact(h.total_len() - self.buf.len());
                self.buf.extend_from_slice(data);
                h
            }
        };

        // Complete when the buffer holds exactly header + payload.
        if self.buf.len() == h.total_len() {
            // The payload is w*h*4 bytes -- a whole number of ARGB u32s.
            let argb: Vec<u32> = self.buf[HEADER_LEN..]
                .chunks_exact(4)
                .map(|c| u32::from_le_bytes([c[0], c[1], c[2], c[3]]))
                .collect();
            self.base += h.total_len() as u64;
            self.buf = Vec::new();
            self.header = None;
            return AccumStep::Done {
                w: h.w,
                h: h.h,
                argb,
            };
        }
        AccumStep::More
    }

    /// The heap this accumulator currently holds -- its buffer's CAPACITY, not
    /// its length (the capacity is what the allocator committed). After the
    /// header parses this equals `total_len` (reserve_exact, no Vec doubling --
    /// audit F2). A TEST/OBSERVABILITY accessor: the runtime does NOT sum it into
    /// a budget (audit F5). The aggregate footprint is bounded structurally --
    /// `MAX_CONNS`=1 means one accumulator at a time, and its per-image cap is set
    /// from the heap residual (placesrv `set_max_pixels`, F4). Were `MAX_CONNS`
    /// ever raised, THIS is the term a real cross-connection byte budget would sum.
    pub fn reserved_bytes(&self) -> usize {
        self.buf.capacity()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::vec;

    // A generous cap for the tests (2 Mpx -- the console spike's real cap).
    const CAP: u64 = 2 * 1024 * 1024;

    /// A complete place-request for a `w`x`h` image whose pixels ascend from 0.
    fn wire(w: u32, h: u32) -> (Vec<u8>, Vec<u32>) {
        let px: Vec<u32> = (0..(w as usize * h as usize)).map(|i| i as u32).collect();
        let mut msg = PlaceHeader::argb(w, h).pack().to_vec();
        for p in &px {
            msg.extend_from_slice(&p.to_le_bytes());
        }
        (msg, px)
    }

    fn expect_done(step: AccumStep) -> (u32, u32, Vec<u32>) {
        match step {
            AccumStep::Done { w, h, argb } => (w, h, argb),
            AccumStep::More => panic!("expected Done, got More"),
            AccumStep::Reject => panic!("expected Done, got Reject"),
        }
    }

    #[test]
    fn one_write_completes() {
        let (msg, px) = wire(2, 3);
        let mut a = PlaceAccum::new(CAP);
        let (w, h, argb) = expect_done(a.write(0, &msg));
        assert_eq!((w, h), (2, 3));
        assert_eq!(argb, px);
    }

    #[test]
    fn split_writes_complete() {
        // The real path: a Twrite carrying the header, then payload chunks --
        // and a chunk that straddles the header/payload boundary.
        let (msg, px) = wire(4, 4);
        let mut a = PlaceAccum::new(CAP);
        let mut off = 0u64;
        // First a sub-header dribble (< 16 bytes), then the rest in 10-byte bites.
        assert!(matches!(a.write(off, &msg[..10]), AccumStep::More));
        off += 10;
        let mut done = None;
        let mut i = 10usize;
        while i < msg.len() {
            let end = (i + 10).min(msg.len());
            match a.write(off, &msg[i..end]) {
                AccumStep::More => {}
                AccumStep::Done { w, h, argb } => done = Some((w, h, argb)),
                AccumStep::Reject => panic!("unexpected reject at {}", i),
            }
            off += (end - i) as u64;
            i = end;
        }
        let (w, h, argb) = done.expect("never completed");
        assert_eq!((w, h), (4, 4));
        assert_eq!(argb, px);
    }

    #[test]
    fn bad_magic_rejected_without_alloc() {
        let mut msg = PlaceHeader::argb(2, 2).pack().to_vec();
        msg[0] ^= 0xff; // corrupt the magic
        msg.extend_from_slice(&[0u8; 2 * 2 * 4]);
        let mut a = PlaceAccum::new(CAP);
        assert!(matches!(a.write(0, &msg), AccumStep::Reject));
    }

    #[test]
    fn over_cap_dimensions_rejected() {
        // A well-formed header (within the wire's own MAX_PIXELS) but past the
        // caller's tighter heap-safe cap: the header parses, the cap rejects.
        let small_cap = 4u64; // 4 px
        let (msg, _) = wire(3, 3); // 9 px > 4
        let mut a = PlaceAccum::new(small_cap);
        assert!(matches!(a.write(0, &msg), AccumStep::Reject));
    }

    #[test]
    fn header_claiming_giant_rejected_before_payload() {
        // The header alone, claiming a raster far over the cap: rejected on the
        // 16-byte header, so no payload is ever buffered.
        let hdr = PlaceHeader::argb(4000, 4000).pack(); // 16 Mpx > 2 Mpx cap
        let mut a = PlaceAccum::new(CAP);
        assert!(matches!(a.write(0, &hdr), AccumStep::Reject));
    }

    #[test]
    fn non_sequential_offset_rejected() {
        let (msg, _) = wire(2, 2);
        let mut a = PlaceAccum::new(CAP);
        assert!(matches!(a.write(0, &msg[..8]), AccumStep::More));
        // A gap (offset 12, not 8): a seek the channel does not support.
        assert!(matches!(a.write(12, &msg[8..]), AccumStep::Reject));
    }

    #[test]
    fn trailing_bytes_past_total_rejected() {
        let (mut msg, _) = wire(2, 2);
        msg.push(0xAA); // one byte too many
        let mut a = PlaceAccum::new(CAP);
        assert!(matches!(a.write(0, &msg), AccumStep::Reject));
    }

    #[test]
    fn capacity_is_exact_no_doubling() {
        // F2 regression: the buffer reserves EXACTLY total_len once the header
        // parses, so it never Vec-doubles as chunks arrive. A 512x512 image
        // (total_len = 16 + 512*512*4 = 1,048,592, just over the 1 MiB power of
        // two) would double to ~2 MiB capacity under incremental growth; with
        // reserve_exact it stays at total_len. Feed the header + a small first
        // chunk, then more chunks, and assert the footprint never exceeds
        // total_len (allowing only tiny allocator rounding, never a 2x jump).
        let (msg, _) = wire(512, 512);
        let total = msg.len();
        let mut a = PlaceAccum::new(CAP);
        // First write carries the header + 100 payload bytes -> reserve_exact fires.
        let _ = a.write(0, &msg[..HEADER_LEN + 100]);
        assert!(
            a.reserved_bytes() <= total + 4096,
            "footprint {} must be ~total_len {} (no 2x doubling)",
            a.reserved_bytes(),
            total
        );
        assert!(
            a.reserved_bytes() >= total,
            "footprint {} must cover the whole payload {}",
            a.reserved_bytes(),
            total
        );
        // Draining the rest in small chunks must not grow it past total.
        let mut off = HEADER_LEN + 100;
        while off < total {
            let end = (off + 37).min(total);
            let _ = a.write(off as u64, &msg[off..end]);
            off = end;
        }
    }

    #[test]
    fn two_images_on_one_fid() {
        // The second image's first write arrives at the cumulative fid offset.
        let (m1, p1) = wire(2, 2);
        let (m2, p2) = wire(3, 1);
        let mut a = PlaceAccum::new(CAP);
        let (w1, h1, a1) = expect_done(a.write(0, &m1));
        assert_eq!((w1, h1, &a1), (2, 2, &p1));
        let (w2, h2, a2) = expect_done(a.write(m1.len() as u64, &m2));
        assert_eq!((w2, h2, &a2), (3, 1, &p2));
    }

    #[test]
    fn partial_then_abandoned_stays_incomplete() {
        // A header promising more, then silence: the accumulator sits in More
        // (the caller discards it on clunk); it never spuriously completes.
        let (msg, _) = wire(8, 8);
        let mut a = PlaceAccum::new(CAP);
        assert!(matches!(a.write(0, &msg[..64]), AccumStep::More));
        // No further writes: nothing is delivered. (Drop = discard.)
        let _ = vec![0u8; 0];
    }

    #[test]
    fn set_max_pixels_gates_the_reused_accum() {
        // F2 regression: on a REUSED accum the refreshed cap gates the NEXT
        // image's header. Discriminating -- the SAME two-image sequence Rejects
        // the larger second image under the initial cap and completes it after
        // the refresh, so the cap (not a value captured at construction)
        // decides. This is the paneplace `_ => acc.set_max_pixels(..)` path that
        // lets a session compositor's per-image cap track a display resize.
        let (m1, p1) = wire(2, 2); // 4 px -- fits the initial cap
        let m2 = wire(3, 3).0; // 9 px -- over the initial 4-px cap
        // Without the refresh: the second image is rejected by the stale cap.
        let mut a = PlaceAccum::new(4);
        assert_eq!(expect_done(a.write(0, &m1)).2, p1);
        assert!(matches!(a.write(m1.len() as u64, &m2), AccumStep::Reject));
        // With the refresh: same sequence, cap raised after image 1 -> admitted.
        let mut b = PlaceAccum::new(4);
        assert_eq!(expect_done(b.write(0, &m1)).2, p1);
        b.set_max_pixels(16);
        let (w, h, _) = expect_done(b.write(m1.len() as u64, &m2));
        assert_eq!((w, h), (3, 3));
    }
}
