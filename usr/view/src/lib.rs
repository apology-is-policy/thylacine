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

/// The compressed-input cap, held alongside the decode peak it is counted with
/// (VIEW_MAX_PIXELS below).
pub const READ_CAP: usize = 16 * 1024 * 1024;

/// view's own decode pixel budget, bounding the decode peak + the held compressed
/// input, checked from the headers BEFORE decode so an image past it is a clean
/// report, never a death at a page fault when the system runs out of memory
/// mid-decode. The peak per pixel depends on the format. A PROGRESSIVE JPEG
/// holds a full-image coefficient buffer per input component (~2 B * components
/// * npx, up to 4 for CMYK, zune mcu_prog.rs) beside its output, ~12 B/px; a
/// baseline one ~8. A PNG (zune-png 0.4.10) holds its whole inflated stream
/// beside its output buffer, both at the sample width: ~8 B/px for 8-bit RGBA,
/// ~16 for 16-bit, and an interlaced image a third buffer (~12, ~24); a stream
/// longer than its dimensions doubles zune-inflate's buffer before it is refused
/// (~32 B/px for 16-bit, transiently), and a PNG counts its input twice, held
/// and its IDAT data copied. So at 3M pixels: 12*3M + 16 MiB = 52 MiB for a
/// JPEG, 80 MiB for a 16-bit PNG, 104 MiB interlaced, 128 MiB for a malformed
/// one. The heap grows from the user pool (B-1c), so these are what a decode may
/// draw from it, not a wall; the 3M was set when the heap was a fixed 64 MiB,
/// where the former 6M (88 MiB) OOM-exited a progressive JPEG (holotype F1).
/// halcyond caps the CHANNEL downstream (display-adaptive, at most 1 Mpx), and
/// view reduces a larger raster to that cap before it uploads; this bounds
/// view's local decode.
pub const VIEW_MAX_PIXELS: u64 = 3 * 1024 * 1024;

/// Read a PNG's pixel dimensions from its headers WITHOUT decoding the image.
/// Callers use this to reject an over-budget image (a clean error) BEFORE the
/// full decode, whose peak working set (samples + argb + the compressed input)
/// is several times the pixel count, and the heap grows to hold it: the
/// caller's budget is the only bound. Cheap: parses only the IHDR.
pub fn png_dimensions(bytes: &[u8]) -> Result<(u32, u32), &'static str> {
    use zune_png::PngDecoder;
    let mut dec = PngDecoder::new(bytes);
    dec.decode_headers().map_err(|_| "png: malformed headers")?;
    let (w, h) = dec.get_dimensions().ok_or("png: no dimensions")?;
    Ok((w as u32, h as u32))
}

/// Does an image of `w x h` fit a decode budget of `max` pixels? The budget is
/// the CALLER's, not [`MAX_PIXELS`]: the decode peak is the compressed input +
/// the samples buffer + the ARGB buffer, all live at once, and the heap grows
/// to hold it, so the budget is what bounds the decode. Both viewers (`view`
/// inline, `gallery` fullscreen) call this on the [`png_dimensions`] result
/// before decoding. Checked in u64 -- no overflow.
pub fn within_pixel_budget(w: u32, h: u32, max: u64) -> bool {
    (w as u64) * (h as u64) <= max
}

/// The largest `w' x h'` with the aspect of `w x h` holding at most `max_px`
/// pixels and at most `max_side` on either side, or `None` when `w x h` already
/// fits both. Never larger than the source and never zero in either dimension:
/// an extreme aspect keeps one row or column.
pub fn fitted_size(w: u32, h: u32, max_px: u64, max_side: u32) -> Option<(u32, u32)> {
    let (w64, h64, side) = (w as u64, h as u64, max_side.max(1) as u64);
    if w == 0 || h == 0 || (w64 * h64 <= max_px && w64 <= side && h64 <= side) {
        return None;
    }
    let max_px = max_px.max(1);
    // The width each bound allows; the narrowest wins. `by_h` is the width at
    // which the height reaches the side bound.
    let by_px = isqrt(max_px.saturating_mul(w64) / h64);
    let by_h = side.saturating_mul(w64) / h64;
    let mut tw = by_px.min(by_h).min(side).clamp(1, w64);
    let mut th = (tw * h64 / w64).clamp(1, h64.min(side));
    th = th.min((max_px / tw).max(1));
    tw = tw.min((max_px / th).max(1));
    Some((tw as u32, th as u32))
}

/// The integer square root, rounded down.
fn isqrt(n: u64) -> u64 {
    if n < 2 {
        return n;
    }
    let mut x = n;
    let mut y = n / 2 + (n & 1);
    while y < x {
        x = y;
        y = (x + n / x) / 2;
    }
    x
}

/// Reduce `r` to at most `max_px` pixels and `max_side` on a side, keeping its
/// aspect. Each new pixel is
/// the average of the source pixels it covers, colour weighted by alpha (the
/// raster is straight alpha), so a thin line fades rather than vanishing, as it
/// can under nearest-pixel sampling, and a transparent pixel lends a neighbour no
/// colour. A raster that fits comes back as it was.
pub fn fit(r: Raster, max_px: u64, max_side: u32) -> Raster {
    let Some((tw, th)) = fitted_size(r.w, r.h, max_px, max_side) else {
        return r;
    };
    let (w, h, tw64, th64) = (r.w as u64, r.h as u64, tw as u64, th as u64);
    if r.argb.len() as u64 != w * h {
        return r;
    }
    let mut out = Vec::with_capacity(tw as usize * th as usize);
    for dy in 0..th64 {
        let y0 = dy * h / th64;
        let y1 = ((dy + 1) * h / th64).max(y0 + 1);
        for dx in 0..tw64 {
            let x0 = dx * w / tw64;
            let x1 = ((dx + 1) * w / tw64).max(x0 + 1);
            let (mut sa, mut sr, mut sg, mut sb) = (0u64, 0u64, 0u64, 0u64);
            for y in y0..y1 {
                let row = (y * w) as usize;
                for &p in &r.argb[row + x0 as usize..row + x1 as usize] {
                    let a = u64::from(p >> 24);
                    sa += a;
                    sr += a * u64::from((p >> 16) & 0xFF);
                    sg += a * u64::from((p >> 8) & 0xFF);
                    sb += a * u64::from(p & 0xFF);
                }
            }
            let n = (y1 - y0) * (x1 - x0);
            let px = if sa == 0 {
                0
            } else {
                let c = |s: u64| (s + sa / 2) / sa;
                (((sa + n / 2) / n) << 24) | (c(sr) << 16) | (c(sg) << 8) | c(sb)
            };
            out.push(px as u32);
        }
    }
    Raster { w: tw, h: th, argb: out }
}

/// Decode a PNG to opaque-or-alpha ARGB. zune expands sub-8-bit and palette
/// images and reports the resulting colorspace; we normalize every case
/// (Luma / LumaA / RGB / RGBA, 8- or 16-bit) to 0xAARRGGBB. 16-bit samples are
/// taken high-byte, by the decoder in its own buffer; a Luma channel replicates across R/G/B; a missing
/// alpha is opaque. cartoon's Op::Image composites the alpha over the pane
/// ground, so a transparent PNG shows the pane through -- correct for inline.
pub fn decode_png(bytes: &[u8]) -> Result<Raster, &'static str> {
    use zune_core::options::DecoderOptions;
    use zune_core::result::DecodingResult;
    use zune_png::PngDecoder;

    // zune narrows a 16-bit image to its high bytes in its own output buffer, so
    // no 16-bit copy reaches this function. The decode's peak is zune's; the
    // callers' pixel budgets bound it (view's main.rs states it per format).
    let opts = DecoderOptions::default().png_set_strip_to_8bit(true);
    let mut dec = PngDecoder::new_with_options(bytes, opts);
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

    // Decode to 8-bit samples in the native (post-expansion) colorspace; the
    // strip above makes every depth arrive as 8-bit.
    let samples: Vec<u8> = match dec.decode().map_err(|_| "png: decode failed")? {
        DecodingResult::U8(v) => v,
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

/// Read a JPEG's pixel dimensions from its headers WITHOUT decoding the image.
/// The JPEG twin of [`png_dimensions`] -- the headers-only budget gate a viewer
/// applies before the heap-hungry decode. JPEG dimensions are 16-bit, so `w*h`
/// cannot overflow a u32.
pub fn jpeg_dimensions(bytes: &[u8]) -> Result<(u32, u32), &'static str> {
    use zune_jpeg::JpegDecoder;
    let mut dec = JpegDecoder::new(bytes);
    dec.decode_headers().map_err(|_| "jpeg: malformed headers")?;
    let info = dec.info().ok_or("jpeg: no dimensions")?;
    Ok((info.width as u32, info.height as u32))
}

/// Decode a JPEG to opaque ARGB. zune-jpeg's output colorspace is only ever RGB
/// (colour) or Luma (grayscale) -- a CMYK/YCCK input is converted to RGB, never
/// emitted as 4 channels -- which we read from `get_output_colorspace` (never
/// assumed) and normalize to 0xAARRGGBB: a Luma channel replicates across R/G/B,
/// RGB maps straight through, alpha is always opaque (JPEG carries none). The
/// `nc != 1 && nc != 3` guard below is therefore DEFENSIVE: unreachable with
/// today's zune, it fail-closes should a future zune emit a 4th channel (which
/// would not be alpha, so must not be mis-mapped as RGBA).
pub fn decode_jpeg(bytes: &[u8]) -> Result<Raster, &'static str> {
    use zune_jpeg::JpegDecoder;

    let mut dec = JpegDecoder::new(bytes);
    dec.decode_headers().map_err(|_| "jpeg: malformed headers")?;
    let info = dec.info().ok_or("jpeg: no dimensions")?;
    let (w, h) = (info.width as u32, info.height as u32);
    let cs = dec.get_output_colorspace().ok_or("jpeg: unknown colorspace")?;
    let nc = cs.num_components();
    if nc != 1 && nc != 3 {
        return Err("jpeg: unsupported colorspace");
    }
    let npx = (w as u64) * (h as u64);
    if npx == 0 || npx > MAX_PIXELS {
        return Err("jpeg: image empty or over the pixel bound");
    }

    let samples: Vec<u8> = dec.decode().map_err(|_| "jpeg: decode failed")?;

    let want = (npx as usize)
        .checked_mul(nc)
        .ok_or("jpeg: pixel-count overflow")?;
    if samples.len() < want {
        return Err("jpeg: short pixel buffer");
    }

    let mut argb: Vec<u32> = Vec::with_capacity(npx as usize);
    for px in samples.chunks_exact(nc) {
        let (r, g, b) = match nc {
            1 => (px[0], px[0], px[0]),
            _ => (px[0], px[1], px[2]),
        };
        argb.push(0xFF00_0000 | ((r as u32) << 16) | ((g as u32) << 8) | (b as u32));
    }
    Ok(Raster { w, h, argb })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn isqrt_rounds_down_everywhere_it_is_asked() {
        for n in (0u64..20_000).chain([u64::MAX, u64::MAX - 1, 1 << 62, (1 << 32) - 1]) {
            let r = u128::from(isqrt(n));
            assert!(r * r <= u128::from(n) && (r + 1) * (r + 1) > u128::from(n), "n={}", n);
        }
    }

    #[test]
    fn a_fitted_size_keeps_the_aspect_under_the_limit() {
        const SIDE: u32 = 8192;
        assert_eq!(fitted_size(640, 400, 1 << 20, SIDE), None, "a raster that fits is left alone");
        assert_eq!(fitted_size(1024, 1024, 1 << 20, SIDE), None, "exactly at the limit fits");
        assert_eq!(fitted_size(2048, 1536, 1 << 20, SIDE), Some((1182, 886)));
        // An extreme aspect keeps one row or one column.
        assert_eq!(fitted_size(8192, 1, 100, SIDE), Some((100, 1)));
        assert_eq!(fitted_size(1, 8192, 100, SIDE), Some((1, 100)));
        // Within the pixel limit but past a side: held to the side, aspect kept.
        assert_eq!(fitted_size(10000, 100, 1 << 20, SIDE), Some((8192, 81)));
        assert_eq!(fitted_size(100, 10000, 1 << 20, SIDE), Some((81, 8100)));
        let sides = [1u32, 2, 3, 7, 640, 1000, 4001, 8192, 10000];
        for &w in &sides {
            for &h in &sides {
                for max in [1u64, 2, 3, 64 * 1024, 1 << 20, u64::MAX] {
                    for side in [1u32, 3, 4000, SIDE] {
                        let at = alloc::format!("{}x{} max {} side {}", w, h, max, side);
                        match fitted_size(w, h, max, side) {
                            None => {
                                assert!(u64::from(w) * u64::from(h) <= max && w <= side && h <= side, "{}", at)
                            }
                            Some((tw, th)) => {
                                assert!(tw >= 1 && th >= 1 && tw <= w && th <= h, "{}", at);
                                assert!(tw <= side && th <= side, "{}", at);
                                assert!(u64::from(tw) * u64::from(th) <= max, "{}", at);
                                // The aspect, to a pixel of rounding, unless a
                                // side is down to its one row or column.
                                if tw > 1 && th > 1 {
                                    let (a, b) = (u64::from(tw) * u64::from(h), u64::from(th) * u64::from(w));
                                    assert!(a.abs_diff(b) < u64::from(w), "aspect: {}", at);
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    #[test]
    fn fit_averages_what_each_new_pixel_covers() {
        // White beside black, reduced to one pixel: the average is grey, where
        // nearest-pixel sampling would return one of the two.
        let r = fit(Raster { w: 2, h: 1, argb: alloc::vec![0xFFFF_FFFF, 0xFF00_0000] }, 1, 8192);
        assert_eq!((r.w, r.h), (1, 1));
        assert_eq!(r.argb, [0xFF80_8080]);
        // Opaque red beside transparent green: alpha averages, and the colour is
        // red alone, because a transparent pixel carries no colour.
        let r = fit(Raster { w: 2, h: 1, argb: alloc::vec![0xFFFF_0000, 0x0000_FF00] }, 1, 8192);
        assert_eq!(r.argb, [0x80FF_0000]);
        let r = fit(Raster { w: 2, h: 2, argb: alloc::vec![0x00FF_FFFF; 4] }, 1, 8192);
        assert_eq!(r.argb, [0], "an all-transparent block is transparent black");
        // A raster that fits comes back untouched.
        let r = fit(Raster { w: 2, h: 1, argb: alloc::vec![1, 2] }, 2, 8192);
        assert_eq!((r.w, r.h, r.argb.as_slice()), (2, 1, &[1u32, 2][..]));
        // A raster within the pixel limit but past the side bound is reduced
        // to it: each pair of pixels becomes one.
        let r = fit(Raster { w: 4, h: 1, argb: alloc::vec![0xFF00_000A, 0xFF00_001E, 0xFF00_0032, 0xFF00_0046] }, u64::MAX, 2);
        assert_eq!((r.w, r.h, r.argb.as_slice()), (2, 1, &[0xFF00_0014u32, 0xFF00_003C][..]));
    }

    #[test]
    fn fit_tiles_the_source_exactly() {
        // A 6x4 raster of 2x2 blocks, each a distinct opaque grey, reduced to
        // 3x2: each new pixel must be exactly its own block, never a blend of two.
        let mut argb = Vec::new();
        for y in 0..4u32 {
            for x in 0..6u32 {
                let v = (y / 2) * 3 + x / 2;
                argb.push(0xFF00_0000 | (v * 20) << 16 | (v * 20) << 8 | v * 20);
            }
        }
        let r = fit(Raster { w: 6, h: 4, argb }, 6, 8192);
        assert_eq!((r.w, r.h), (3, 2));
        for (i, &p) in r.argb.iter().enumerate() {
            let v = i as u32 * 20;
            assert_eq!(p, 0xFF00_0000 | v << 16 | v << 8 | v, "block {}", i);
        }
    }

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
    fn decode_png_takes_the_top_byte_of_a_16_bit_sample() {
        // testdata/make-rgba16-png.py: a 2x2 RGBA PNG at 16 bits a sample, each
        // sample's low byte unlike its high one, so a narrowing that kept the
        // wrong byte reads as a different colour.
        let r = decode_png(include_bytes!("testdata/rgba16.png")).expect("decode 16-bit");
        assert_eq!((r.w, r.h), (2, 2));
        assert_eq!(r.argb, [0xFFE0_2020, 0xFF20_E020, 0x8012_569A, 0x0000_0000]);
    }

    #[test]
    fn decode_png_rejects_garbage() {
        assert!(decode_png(b"not a png at all, just bytes").is_err());
        assert!(decode_png(&PNG_MAGIC).is_err(), "magic alone is not a decodable image");
    }

    // The headers-only dimension read: the same dims decode_png reports, but
    // WITHOUT the full-image allocation (so a caller can reject an over-budget
    // image before the heap-hungry decode).
    #[test]
    fn png_dimensions_reads_ihdr_without_decoding() {
        assert_eq!(png_dimensions(include_bytes!("testdata/2x2.png")).unwrap(), (2, 2));
        assert_eq!(png_dimensions(include_bytes!("../testdata/test.png")).unwrap(), (640, 400));
        assert!(png_dimensions(b"not a png").is_err());
    }

    #[test]
    fn pixel_budget_is_inclusive_and_overflow_safe() {
        assert!(within_pixel_budget(640, 400, 6 * 1024 * 1024));
        assert!(within_pixel_budget(2048, 3072, 6 * 1024 * 1024), "6 Mpx fits a 6 Mpx budget");
        assert!(!within_pixel_budget(4000, 4000, 6 * 1024 * 1024), "16 Mpx over budget");
        // largest u32 dims must not overflow the product (u64 math)
        assert!(!within_pixel_budget(u32::MAX, u32::MAX, 6 * 1024 * 1024));
    }

    // A 32x32 four-quadrant JPEG (red TL / green TR // blue BL / white BR;
    // testdata/make-test-jpg.sh). JPEG is LOSSY (YCbCr + chroma subsampling +
    // quantization), so colors are asserted APPROXIMATELY, sampled at each
    // quadrant CENTER -- away from the 8x8-block boundaries at the quadrant
    // edges where chroma bleeds. This pins the zune-jpeg decode + the YCbCr->RGB
    // ->ARGB normalization (the JPEG twin of decode_png_2x2_rgba_to_argb).
    #[test]
    fn decode_jpeg_quadrants_to_argb() {
        let jpg = include_bytes!("testdata/quad.jpg");
        assert_eq!(sniff(jpg), Kind::Jpeg);
        let r = decode_jpeg(jpg).expect("decode quad.jpg");
        assert_eq!((r.w, r.h), (32, 32));
        assert_eq!(r.argb.len(), 32 * 32);
        let at = |x: u32, y: u32| r.argb[(y * 32 + x) as usize];
        let near = |px: u32, er: u8, eg: u8, eb: u8| {
            assert_eq!((px >> 24) & 0xFF, 0xFF, "opaque (JPEG has no alpha)");
            let rr = ((px >> 16) & 0xFF) as i32;
            let gg = ((px >> 8) & 0xFF) as i32;
            let bb = (px & 0xFF) as i32;
            let tol = 48; // generous: quality-90 flat-region error is well under this
            assert!(
                (rr - er as i32).abs() <= tol
                    && (gg - eg as i32).abs() <= tol
                    && (bb - eb as i32).abs() <= tol,
                "px {:08X} not near ({:02X},{:02X},{:02X})",
                px, er, eg, eb
            );
        };
        near(at(8, 8), 0xE0, 0x20, 0x20); // TL red
        near(at(24, 8), 0x20, 0xE0, 0x20); // TR green
        near(at(8, 24), 0x20, 0x20, 0xE0); // BL blue
        near(at(24, 24), 0xF0, 0xF0, 0xF0); // BR white
    }

    #[test]
    fn jpeg_dimensions_reads_headers_without_decoding() {
        assert_eq!(jpeg_dimensions(include_bytes!("testdata/quad.jpg")).unwrap(), (32, 32));
        assert!(jpeg_dimensions(b"not a jpeg").is_err());
    }

    #[test]
    fn decode_jpeg_rejects_garbage() {
        assert!(decode_jpeg(b"not a jpeg at all, just bytes").is_err());
        assert!(
            decode_jpeg(&JPEG_MAGIC).is_err(),
            "magic alone is not a decodable image"
        );
    }

    // A 16x16 GRAYSCALE JPEG (top half 0x40, bottom 0xC0; make-test-jpg.sh via
    // cjpeg -grayscale). A grayscale JPEG decodes as Luma (nc==1), exercising the
    // replicate-across-R/G/B arm the colour fixtures never reach: every pixel
    // must be gray (r==g==b) and opaque.
    #[test]
    fn decode_jpeg_grayscale_to_luma_argb() {
        let jpg = include_bytes!("testdata/gray.jpg");
        assert_eq!(sniff(jpg), Kind::Jpeg);
        let r = decode_jpeg(jpg).expect("decode gray.jpg");
        assert_eq!((r.w, r.h), (16, 16));
        assert_eq!(r.argb.len(), 16 * 16);
        for &px in &r.argb {
            assert_eq!((px >> 24) & 0xFF, 0xFF, "opaque");
            let (rr, gg, bb) = ((px >> 16) & 0xFF, (px >> 8) & 0xFF, px & 0xFF);
            assert!(rr == gg && gg == bb, "gray means r==g==b, got {:08X}", px);
        }
        let g = |x: u32, y: u32| ((r.argb[(y * 16 + x) as usize] >> 8) & 0xFF) as i32;
        assert!((g(8, 4) - 0x40).abs() <= 40, "top half ~0x40, got {}", g(8, 4));
        assert!((g(8, 12) - 0xC0).abs() <= 40, "bottom half ~0xC0, got {}", g(8, 12));
    }

    // A 32x32 PROGRESSIVE JPEG (jpegtran -progressive of quad.jpg; SOF2). The
    // progressive decode path holds full-image coefficient buffers the baseline
    // path does not (the peak the viewers' heap budgets now account for --
    // holotype F1); this pins that it still decodes to a w*h ARGB with the same
    // quadrant content.
    #[test]
    fn decode_jpeg_progressive_to_argb() {
        let jpg = include_bytes!("testdata/prog.jpg");
        assert_eq!(sniff(jpg), Kind::Jpeg);
        let r = decode_jpeg(jpg).expect("decode prog.jpg");
        assert_eq!((r.w, r.h), (32, 32));
        assert_eq!(r.argb.len(), 32 * 32);
        let at = |x: u32, y: u32| r.argb[(y * 32 + x) as usize];
        let chan = |px: u32, sh: u32| ((px >> sh) & 0xFF) as i32;
        let tl = at(8, 8); // red quadrant
        assert!(chan(tl, 16) > chan(tl, 8) + 40 && chan(tl, 16) > chan(tl, 0) + 40, "TL red-dominant, got {:08X}", tl);
        let br = at(24, 24); // white quadrant
        assert!(chan(br, 16) > 150 && chan(br, 8) > 150 && chan(br, 0) > 150, "BR bright, got {:08X}", br);
    }
}
