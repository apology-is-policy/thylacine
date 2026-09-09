// view -- the pure brain (I-47, HALCYON.md 14.7): media sniffing + decode to
// the ARGB raster halcyond's cartoon Op::Image wants. No syscalls here (the
// bin owns those); host-tested. Decode lives in this short-lived program, not
// in halcyond -- hostile image bytes must parse in a sacrificial process.

#![no_std]

extern crate alloc;

use alloc::vec::Vec;

/// A recognized media kind, from the file's leading magic bytes (never the
/// extension -- the bytes are the truth, and the obj-verb menu offers `view`
/// on any file).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Kind {
    Png,
    Jpeg,
    Other,
}

const PNG_MAGIC: [u8; 8] = [0x89, b'P', b'N', b'G', 0x0D, 0x0A, 0x1A, 0x0A];
const JPEG_MAGIC: [u8; 3] = [0xFF, 0xD8, 0xFF];

/// Sniff the media kind from the leading bytes.
pub fn sniff(bytes: &[u8]) -> Kind {
    if bytes.len() >= PNG_MAGIC.len() && bytes[..PNG_MAGIC.len()] == PNG_MAGIC {
        Kind::Png
    } else if bytes.len() >= JPEG_MAGIC.len() && bytes[..JPEG_MAGIC.len()] == JPEG_MAGIC {
        Kind::Jpeg
    } else {
        Kind::Other
    }
}

/// A decoded raster: `w`-tight ARGB rows (0xAARRGGBB, opaque), the exact shape
/// `cartoon::Op::Image` blits and the channel (slice 3) carries.
pub struct Raster {
    pub w: u32,
    pub h: u32,
    pub argb: Vec<u32>,
}

/// The decode-time bound (I-32 in spirit; the per-pane quota is the channel's,
/// slice 3): refuse an image whose pixel count would overflow the working set.
/// 64 megapixels is far above any real inline image and well under a Vec<u32>
/// length overflow.
pub const MAX_PIXELS: u64 = 64 * 1024 * 1024;

/// Decode a PNG to opaque-or-alpha ARGB. zune expands sub-8-bit and palette
/// images and reports the resulting colorspace; we normalize every case
/// (Luma / LumaA / RGB / RGBA, 8- or 16-bit) to 0xAARRGGBB. 16-bit samples are
/// taken high-byte (>> 8); a Luma channel replicates across R/G/B; a missing
/// alpha is opaque. cartoon's Op::Image composites the alpha over the pane
/// ground, so a transparent PNG shows the pane through -- correct for inline.
pub fn decode_png(bytes: &[u8]) -> Result<Raster, &'static str> {
    use zune_core::result::DecodingResult;
    use zune_png::PngDecoder;

    let mut dec = PngDecoder::new(bytes);
    dec.decode_headers().map_err(|_| "png: malformed headers")?;
    let (w, h) = dec.get_dimensions().ok_or("png: no dimensions")?;
    let cs = dec.get_colorspace().ok_or("png: unknown colorspace")?;
    let nc = cs.num_components();
    if nc == 0 || nc > 4 {
        return Err("png: unsupported colorspace");
    }
    let npx = (w as u64) * (h as u64);
    if npx == 0 || npx > MAX_PIXELS {
        return Err("png: image empty or over the pixel bound");
    }

    // Decode to 8-bit samples in the native (post-expansion) colorspace.
    let samples: Vec<u8> = match dec.decode().map_err(|_| "png: decode failed")? {
        DecodingResult::U8(v) => v,
        DecodingResult::U16(v) => v.iter().map(|&s| (s >> 8) as u8).collect(),
        _ => return Err("png: unsupported sample type"),
    };

    let want = (npx as usize)
        .checked_mul(nc)
        .ok_or("png: pixel-count overflow")?;
    if samples.len() < want {
        return Err("png: short pixel buffer");
    }

    let mut argb: Vec<u32> = Vec::with_capacity(npx as usize);
    for px in samples.chunks_exact(nc) {
        let (r, g, b, a) = match nc {
            1 => (px[0], px[0], px[0], 0xFF),
            2 => (px[0], px[0], px[0], px[1]),
            3 => (px[0], px[1], px[2], 0xFF),
            _ => (px[0], px[1], px[2], px[3]),
        };
        argb.push(
            ((a as u32) << 24) | ((r as u32) << 16) | ((g as u32) << 8) | (b as u32),
        );
    }
    Ok(Raster {
        w: w as u32,
        h: h as u32,
        argb,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sniff_reads_magic_not_extension() {
        assert_eq!(sniff(&PNG_MAGIC), Kind::Png);
        assert_eq!(sniff(&[0xFF, 0xD8, 0xFF, 0xE0, 0x00]), Kind::Jpeg);
        assert_eq!(sniff(b"#!/bin/rc\n"), Kind::Other);
        assert_eq!(sniff(b""), Kind::Other);
        assert_eq!(sniff(&[0x89, b'P']), Kind::Other, "a short prefix is not PNG");
    }

    // A 2x2 RGBA PNG (red / green // blue / white), the zune decode + the ARGB
    // normalization end to end. zune is portable, so this runs host-side.
    #[test]
    fn decode_png_2x2_rgba_to_argb() {
        let png = include_bytes!("testdata/2x2.png");
        assert_eq!(sniff(png), Kind::Png);
        let r = decode_png(png).expect("decode 2x2");
        assert_eq!((r.w, r.h), (2, 2));
        assert_eq!(r.argb.len(), 4);
        assert_eq!(r.argb[0], 0xFFFF_0000, "top-left red");
        assert_eq!(r.argb[1], 0xFF00_FF00, "top-right green");
        assert_eq!(r.argb[2], 0xFF00_00FF, "bottom-left blue");
        assert_eq!(r.argb[3], 0xFFFF_FFFF, "bottom-right white");
    }

    // The E2E witness card (testdata/make-test-png.py): a 640x400 RGB PNG. This
    // decodes the EXACT bytes the boot bakes to /test.png, pinning the zune RGB
    // path + the ARGB normalization against the fixture -- so an E2E miss is a
    // channel/display fault, never a decode surprise. Pixels are chosen off the
    // bright diagonal (which the generator draws white).
    #[test]
    fn decode_png_witness_card() {
        let png = include_bytes!("../testdata/test.png");
        assert_eq!(sniff(png), Kind::Png);
        let r = decode_png(png).expect("decode witness card");
        assert_eq!((r.w, r.h), (640, 400));
        assert_eq!(r.argb.len(), 640 * 400);
        // (10,10): the leftmost (red) bar, off the diagonal -> 0xFFE02020.
        assert_eq!(r.argb[10 * 640 + 10], 0xFFE0_2020, "red bar");
        // (600,10): the sixth (magenta) bar -> 0xFFE020E0.
        assert_eq!(r.argb[10 * 640 + 600], 0xFFE0_20E0, "magenta bar");
        // (100,350): the bottom luminance ramp, v = 100*255/639 = 39 -> gray.
        assert_eq!(r.argb[350 * 640 + 100], 0xFF27_2727, "gradient gray");
    }

    #[test]
    fn decode_png_rejects_garbage() {
        assert!(decode_png(b"not a png at all, just bytes").is_err());
        assert!(decode_png(&PNG_MAGIC).is_err(), "magic alone is not a decodable image");
    }
}
