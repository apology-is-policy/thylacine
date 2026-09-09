// inlinewire -- the inline-media place-request wire (I-47, HALCYON.md 14.7).
//
// `view` (the unprivileged, sacrificial decoder) hands a decoded raster to
// halcyond over a per-pane control endpoint as a BOUNDED WRITE: a 16-byte
// header, then w*h ARGB pixels (0xAARRGGBB, one LE u32 each) immediately
// after it. halcyond validates the header BEFORE allocating, accumulates the
// payload, and injects an Item::Image. This crate is the sole home of that
// contract so the writer and reader can never drift; it carries NO decoder and
// NO syscalls, so depending on it drags neither zune (into halcyond) nor
// libthyla-rs (into a host test).

#![no_std]

/// Magic "HPL1" (Halcyon inline PLace v1), LE. A write that does not open with
/// it is not a place-request and is refused -- the first line of the
/// format-fuzz defense (HALCYON.md 14.7.7).
pub const MAGIC: u32 = 0x314c_5048; // b"HPL1" little-endian (pack()[0..4] == "HPL1")

/// The only pixel format v0 carries: 0xAARRGGBB, one LE u32 per pixel, w-tight
/// rows -- exactly what `cartoon::Op::Image` blits.
pub const FORMAT_ARGB8888: u32 = 1;

/// The fixed header size: magic + format + w + h, four LE u32s.
pub const HEADER_LEN: usize = 16;

/// Per-dimension and total bounds -- refused before any allocation, so a
/// hostile header can never drive a large reserve. MAX_PIXELS caps the payload
/// at 64 MiB (16 Mpx * 4); the per-pane quota (the channel's finer bound) lands
/// with the full feature.
pub const MAX_W: u32 = 8192;
pub const MAX_H: u32 = 8192;
pub const MAX_PIXELS: u64 = 16 * 1024 * 1024;

/// A validated place-request header. `format` is always `FORMAT_ARGB8888` for a
/// value returned by [`parse`]; `w`/`h` are within bounds and `w*h <=
/// MAX_PIXELS`.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct PlaceHeader {
    pub format: u32,
    pub w: u32,
    pub h: u32,
}

impl PlaceHeader {
    /// An ARGB8888 header for `w` x `h`. Callers pass dims a decoder produced;
    /// [`parse`] is what enforces the bounds on the far side.
    pub fn argb(w: u32, h: u32) -> PlaceHeader {
        PlaceHeader {
            format: FORMAT_ARGB8888,
            w,
            h,
        }
    }

    /// The 16-byte wire header (magic, format, w, h; all LE).
    pub fn pack(&self) -> [u8; HEADER_LEN] {
        let mut b = [0u8; HEADER_LEN];
        b[0..4].copy_from_slice(&MAGIC.to_le_bytes());
        b[4..8].copy_from_slice(&self.format.to_le_bytes());
        b[8..12].copy_from_slice(&self.w.to_le_bytes());
        b[12..16].copy_from_slice(&self.h.to_le_bytes());
        b
    }

    /// Parse + FULLY VALIDATE a header from the leading bytes. `None` on: short
    /// buffer, wrong magic, unknown format, a zero or over-bound dimension, or
    /// a pixel count over `MAX_PIXELS`. A `Some` result is safe to size an
    /// allocation from ([`payload_len`](Self::payload_len) cannot overflow).
    pub fn parse(bytes: &[u8]) -> Option<PlaceHeader> {
        if bytes.len() < HEADER_LEN {
            return None;
        }
        let g = |o: usize| u32::from_le_bytes([bytes[o], bytes[o + 1], bytes[o + 2], bytes[o + 3]]);
        if g(0) != MAGIC {
            return None;
        }
        let format = g(4);
        let w = g(8);
        let h = g(12);
        if format != FORMAT_ARGB8888 {
            return None;
        }
        if w == 0 || h == 0 || w > MAX_W || h > MAX_H {
            return None;
        }
        if (w as u64) * (h as u64) > MAX_PIXELS {
            return None;
        }
        Some(PlaceHeader { format, w, h })
    }

    /// The payload byte count (w*h*4). Bounded by `MAX_PIXELS` for any parsed
    /// header, so it fits usize on a 32-bit target too.
    pub fn payload_len(&self) -> usize {
        (self.w as usize) * (self.h as usize) * 4
    }

    /// The whole wire message length: header + payload.
    pub fn total_len(&self) -> usize {
        HEADER_LEN + self.payload_len()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pack_parse_round_trip() {
        let h = PlaceHeader::argb(640, 400);
        let bytes = h.pack();
        assert_eq!(bytes.len(), HEADER_LEN);
        assert_eq!(&bytes[0..4], b"HPL1", "magic reads as HPL1 in a hexdump");
        assert_eq!(PlaceHeader::parse(&bytes), Some(h));
        assert_eq!(h.payload_len(), 640 * 400 * 4);
        assert_eq!(h.total_len(), HEADER_LEN + 640 * 400 * 4);
    }

    #[test]
    fn parse_rejects_garbage_and_bounds() {
        assert_eq!(PlaceHeader::parse(&[]), None, "empty");
        assert_eq!(PlaceHeader::parse(&[0u8; 8]), None, "short");
        assert_eq!(PlaceHeader::parse(&[0u8; 16]), None, "zero magic");
        // good magic, unknown format
        let mut b = PlaceHeader::argb(2, 2).pack();
        b[4] = 9;
        assert_eq!(PlaceHeader::parse(&b), None, "unknown format");
        // zero dim
        assert_eq!(PlaceHeader::parse(&PlaceHeader::argb(0, 4).pack()), None, "w=0");
        assert_eq!(PlaceHeader::parse(&PlaceHeader::argb(4, 0).pack()), None, "h=0");
        // over per-dim bound
        assert_eq!(PlaceHeader::parse(&PlaceHeader::argb(MAX_W + 1, 1).pack()), None, "w>MAX");
        // over pixel cap (both dims in-range, product over cap)
        assert_eq!(PlaceHeader::parse(&PlaceHeader::argb(8192, 8192).pack()), None, "w*h over cap");
    }
}
