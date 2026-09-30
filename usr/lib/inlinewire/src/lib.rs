// inlinewire -- the inline-media place-request wire (I-47, HALCYON.md 14.7).
//
// `view` (the unprivileged, sacrificial decoder) hands a decoded raster to
// halcyond over a per-pane control endpoint as a BOUNDED WRITE: a 32-byte
// header, then w*h ARGB pixels (0xAARRGGBB, one LE u32 each) immediately
// after it. halcyond validates the header BEFORE allocating, accumulates the
// payload, and associates its ID with an ordered text caption in a session.
// ID zero retains direct Item::Image insertion for the console renderer. This crate is the sole home of that
// contract so the writer and reader can never drift; it carries NO decoder and
// NO syscalls, so depending on it drags neither zune (into halcyond) nor
// libthyla-rs (into a host test).

#![no_std]

/// Magic "HPL2" (Halcyon inline PLace v2), LE. A write that does not open with
/// it is not a place-request and is refused -- the first line of the
/// format-fuzz defense (HALCYON.md 14.7.7).
pub const MAGIC: u32 = 0x324c_5048; // b"HPL2" little-endian (pack()[0..4] == "HPL2")

/// The only pixel format v0 carries: 0xAARRGGBB, one LE u32 per pixel, w-tight
/// rows -- exactly what `cartoon::Op::Image` blits.
pub const FORMAT_ARGB8888: u32 = 1;

/// The fixed header size: magic + format + w + h (four LE u32s), then a LE u128 ID.
pub const HEADER_LEN: usize = 32;

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
    pub id: u128,
    pub format: u32,
    pub w: u32,
    pub h: u32,
}

impl PlaceHeader {
    /// An ARGB8888 header for `w` x `h`. Callers pass dims a decoder produced;
    /// [`parse`] is what enforces the bounds on the far side.
    pub fn argb(w: u32, h: u32) -> PlaceHeader {
        PlaceHeader {
            id: 0,
            format: FORMAT_ARGB8888,
            w,
            h,
        }
    }

    /// The 32-byte wire header (magic, format, w, h, then the u128 id; all LE).
    pub fn pack(&self) -> [u8; HEADER_LEN] {
        let mut b = [0u8; HEADER_LEN];
        b[0..4].copy_from_slice(&MAGIC.to_le_bytes());
        b[4..8].copy_from_slice(&self.format.to_le_bytes());
        b[8..12].copy_from_slice(&self.w.to_le_bytes());
        b[12..16].copy_from_slice(&self.h.to_le_bytes());
        b[16..32].copy_from_slice(&self.id.to_le_bytes());
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
        Some(PlaceHeader { id: u128::from_le_bytes(bytes[16..32].try_into().ok()?), format, w, h })
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

/// The longest limit text: `u64::MAX` is 20 decimal digits, then the newline.
pub const LIMIT_TEXT_MAX: usize = 21;

/// What a read of `place` at offset 0 answers (HALCYON.md 14.7, the 2026-09-29
/// refinement): the channel's current per-image limit in pixels, as ASCII
/// decimal digits and a newline, so a client fits its raster BEFORE it uploads
/// instead of learning the cap from a refusal (`E_INVAL`, the same answer a
/// malformed header gets). Returns the filled length.
pub fn limit_text(pixels: u64, out: &mut [u8; LIMIT_TEXT_MAX]) -> usize {
    let mut digits = [0u8; LIMIT_TEXT_MAX - 1];
    let mut n = pixels;
    let mut i = digits.len();
    loop {
        i -= 1;
        digits[i] = b'0' + (n % 10) as u8;
        n /= 10;
        if n == 0 {
            break;
        }
    }
    let len = digits.len() - i;
    out[..len].copy_from_slice(&digits[i..]);
    out[len] = b'\n';
    len + 1
}

/// The bytes a read of `place` at `offset` for `count` returns: the part of the
/// limit text the read covers, and nothing past its end (end of file).
pub fn limit_read(pixels: u64, offset: u64, count: u32, buf: &mut [u8; LIMIT_TEXT_MAX]) -> &[u8] {
    let n = limit_text(pixels, buf);
    let start = offset.min(n as u64) as usize;
    let end = start.saturating_add(count as usize).min(n);
    &buf[start..end]
}

/// Parse a limit text exactly as [`limit_text`] writes it: one or more digits
/// with no leading zero, then one newline, and nothing else. `None` for any
/// other bytes, a zero limit, or a value past `u64`; the client then uploads
/// unfitted and lets the server decide.
pub fn parse_limit(bytes: &[u8]) -> Option<u64> {
    let (&last, digits) = bytes.split_last()?;
    if last != b'\n' || digits.is_empty() || digits[0] == b'0' {
        return None;
    }
    let mut v: u64 = 0;
    for &d in digits {
        if !d.is_ascii_digit() {
            return None;
        }
        v = v.checked_mul(10)?.checked_add(u64::from(d - b'0'))?;
    }
    Some(v)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn text(px: u64) -> ([u8; LIMIT_TEXT_MAX], usize) {
        let mut b = [0u8; LIMIT_TEXT_MAX];
        let n = limit_text(px, &mut b);
        (b, n)
    }

    #[test]
    fn the_limit_text_is_decimal_and_a_newline() {
        let (b, n) = text(1048576);
        assert_eq!(&b[..n], b"1048576\n");
        let (b, n) = text(0);
        assert_eq!(&b[..n], b"0\n");
        let (b, n) = text(u64::MAX);
        assert_eq!(&b[..n], b"18446744073709551615\n", "the widest value fits the buffer");
        assert_eq!(n, LIMIT_TEXT_MAX);
    }

    #[test]
    fn a_limit_text_parses_back_and_nothing_else_does() {
        for px in [1u64, 9, 10, 65536, 1048576, u64::MAX] {
            let (b, n) = text(px);
            assert_eq!(parse_limit(&b[..n]), Some(px));
        }
        for bad in [
            &b""[..], b"\n", b"1048576", b"1048576\n\n", b"1048576 \n", b" 1048576\n",
            b"+5\n", b"0\n", b"01\n", b"12a\n", b"18446744073709551616\n",
        ] {
            assert_eq!(parse_limit(bad), None, "{:?}", bad);
        }
    }

    #[test]
    fn a_read_returns_its_window_of_the_text_then_end_of_file() {
        let mut b = [0u8; LIMIT_TEXT_MAX];
        assert_eq!(limit_read(1048576, 0, 64, &mut b), b"1048576\n");
        assert_eq!(limit_read(1048576, 0, 3, &mut b), b"104");
        assert_eq!(limit_read(1048576, 3, 64, &mut b), b"8576\n");
        assert_eq!(limit_read(1048576, 8, 64, &mut b), b"", "at the end");
        assert_eq!(limit_read(1048576, u64::MAX, u32::MAX, &mut b), b"", "far past it");
        assert_eq!(limit_read(1048576, 0, 0, &mut b), b"");
    }

    #[test]
    fn pack_parse_round_trip() {
        let h = PlaceHeader::argb(640, 400);
        let bytes = h.pack();
        assert_eq!(bytes.len(), HEADER_LEN);
        assert_eq!(&bytes[0..4], b"HPL2", "magic reads as HPL2 in a hexdump");
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
