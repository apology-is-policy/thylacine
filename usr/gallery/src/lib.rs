// gallery -- the pure brain (I-47, HALCYON.md 14.7): the letterbox fit + the
// nearest-neighbor blit for the fullscreen `view --fullscreen` variant. No
// syscalls and no decode here (the bin owns tapestryd; `view` owns the decode,
// so gallery reuses that ALREADY-AUDITED format-fuzz surface rather than adding
// a second one); this module is the display geometry, host-tested.

#![no_std]

/// The letterbox placement of an `sw x sh` image inside a `dw x dh` display:
/// aspect-preserving, centred, scaled to FILL the display. A fullscreen viewer
/// upscales -- the deliberate divergence from the inline path's native-if-fits
/// ruling (0b7741f1): a fullscreen view shows the image as large as its aspect
/// allows, not a tiny native raster marooned on black. Degenerate inputs yield
/// a zero rect (fail-safe, like every other malformed reference in this stack).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Fit {
    pub ox: u32,
    pub oy: u32,
    pub fw: u32,
    pub fh: u32,
}

/// The opaque letterbox background (scanout is opaque, so the bars must be too).
pub const LETTERBOX: u32 = 0xFF00_0000;

// Linux evdev keycodes on the virtio-input wire (a tapestry Event's `code`).
const KEY_ESC: u16 = 1;
const KEY_Q: u16 = 16;

/// Does this key event ask gallery to exit? A press or auto-repeat (`value`
/// non-zero -- a release is 0) of Esc or q, matched by keycode OR by the decoded
/// rune (so a remapped keyboard that reports a different code but the 'q' rune
/// still works). Pure over the raw event fields so the bin's event loop stays a
/// thin dispatch and this decision is host-tested.
pub fn is_exit_key(code: u16, value: u32, rune: u32) -> bool {
    value != 0 && (code == KEY_ESC || code == KEY_Q || rune == 'q' as u32 || rune == 'Q' as u32)
}

/// Aspect-preserving fit of `sw x sh` into `dw x dh`, scaled to fill.
pub fn fit_rect(sw: u32, sh: u32, dw: u32, dh: u32) -> Fit {
    if sw == 0 || sh == 0 || dw == 0 || dh == 0 {
        return Fit { ox: 0, oy: 0, fw: 0, fh: 0 };
    }
    // Cross-multiply to compare source and display aspect without floats. The
    // source's WIDTH is the binding edge iff sw/sh >= dw/dh, i.e. sw*dh >= dw*sh:
    // then the width fills the display and the height is scaled by the same
    // ratio (bars top/bottom); otherwise the height binds (bars left/right).
    let (fw, fh) = if (sw as u64) * (dh as u64) >= (dw as u64) * (sh as u64) {
        (dw, (((sh as u64) * (dw as u64)) / (sw as u64)).max(1) as u32)
    } else {
        ((((sw as u64) * (dh as u64)) / (sh as u64)).max(1) as u32, dh)
    };
    // The division can only round DOWN, so fw<=dw and fh<=dh already; the mins
    // are a belt-and-suspenders clamp that also makes the centring subtraction
    // provably non-wrapping.
    let fw = fw.min(dw);
    let fh = fh.min(dh);
    Fit { ox: (dw - fw) / 2, oy: (dh - fh) / 2, fw, fh }
}

/// Paint `dst` (a `dw x dh` ARGB 0xAARRGGBB frame) for a fullscreen view of the
/// `sw x sh` image `src`: fill with the letterbox colour, then nearest-neighbor
/// scale the image into the centred fit rect, forced opaque (the scanout is
/// opaque; a source alpha is dropped -- compositing over the letterbox is a v1
/// refinement). Slices shorter than their declared dimensions clamp rather than
/// panic. Returns the fit rect used.
pub fn paint(dst: &mut [u32], dw: u32, dh: u32, src: &[u32], sw: u32, sh: u32) -> Fit {
    let frame = (dw as usize).saturating_mul(dh as usize).min(dst.len());
    for p in dst[..frame].iter_mut() {
        *p = LETTERBOX;
    }
    let fit = fit_rect(sw, sh, dw, dh);
    if fit.fw == 0 || fit.fh == 0 {
        return fit;
    }
    let src_px = (sw as usize).saturating_mul(sh as usize);
    for y in 0..fit.fh {
        let sy = ((y as u64) * (sh as u64) / (fit.fh as u64)) as u32;
        let srow = (sy as usize) * (sw as usize);
        let drow = ((fit.oy + y) as usize) * (dw as usize) + (fit.ox as usize);
        for x in 0..fit.fw {
            let sx = ((x as u64) * (sw as u64) / (fit.fw as u64)) as u32;
            let si = srow + sx as usize;
            let di = drow + x as usize;
            // src bound (a short/hostile raster) and dst bound (the frame) are
            // both re-checked, so no index can escape either buffer.
            if si < src_px && si < src.len() && di < frame {
                dst[di] = 0xFF00_0000 | (src[si] & 0x00FF_FFFF);
            }
        }
    }
    fit
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn exact_aspect_fills_with_no_bars() {
        // 16:10 into 16:10 -> the whole display, centred at the origin.
        let f = fit_rect(1600, 1000, 1280, 800);
        assert_eq!(f, Fit { ox: 0, oy: 0, fw: 1280, fh: 800 });
    }

    #[test]
    fn wider_source_is_width_bound_bars_top_bottom() {
        // 400x100 (4:1) into 100x100: width fills (100), height 25, centred.
        let f = fit_rect(400, 100, 100, 100);
        assert_eq!(f, Fit { ox: 0, oy: 37, fw: 100, fh: 25 });
        assert!(f.fh <= 100 && f.fw <= 100);
    }

    #[test]
    fn taller_source_is_height_bound_bars_left_right() {
        // 100x400 (1:4) into 100x100: height fills (100), width 25, centred.
        let f = fit_rect(100, 400, 100, 100);
        assert_eq!(f, Fit { ox: 37, oy: 0, fw: 25, fh: 100 });
    }

    #[test]
    fn small_source_upscales_to_fill() {
        // The fullscreen divergence: a 50x50 image fills a 100x100 display.
        let f = fit_rect(50, 50, 100, 100);
        assert_eq!(f, Fit { ox: 0, oy: 0, fw: 100, fh: 100 });
    }

    #[test]
    fn degenerate_dims_yield_zero_rect() {
        assert_eq!(fit_rect(0, 10, 100, 100), Fit { ox: 0, oy: 0, fw: 0, fh: 0 });
        assert_eq!(fit_rect(10, 10, 0, 100), Fit { ox: 0, oy: 0, fw: 0, fh: 0 });
    }

    #[test]
    fn paint_fills_letterbox_then_scales_opaque() {
        // 1x1 red image into a 2x2 display -> width-bound (1:1 into 1:1) fills
        // the whole 2x2; every pixel is the source colour, forced opaque.
        let src = [0x00FF_0000u32]; // red, alpha 0 -> must be forced opaque
        let mut dst = [0u32; 4];
        let f = paint(&mut dst, 2, 2, &src, 1, 1);
        assert_eq!(f, Fit { ox: 0, oy: 0, fw: 2, fh: 2 });
        assert!(dst.iter().all(|&p| p == 0xFFFF_0000));
    }

    #[test]
    fn paint_leaves_bars_black_where_no_image() {
        // 2x1 image into 2x2 -> width-bound: fw=2, fh=1, oy=0 (rounds to the
        // top). Row 0 is the image, row 1 is the letterbox.
        let src = [0xFF11_2233u32, 0xFF44_5566u32];
        let mut dst = [0xDEADu32; 4];
        let f = paint(&mut dst, 2, 2, &src, 2, 1);
        assert_eq!(f, Fit { ox: 0, oy: 0, fw: 2, fh: 1 });
        assert_eq!(dst[0], 0xFF11_2233);
        assert_eq!(dst[1], 0xFF44_5566);
        assert_eq!(dst[2], LETTERBOX);
        assert_eq!(dst[3], LETTERBOX);
    }

    #[test]
    fn paint_clamps_a_short_source_without_panic() {
        // A raster shorter than sw*sh (hostile/truncated) must not index past
        // its slice; the missing pixels simply stay letterbox.
        let src = [0xFFAA_BBCCu32]; // claims 4x4 but holds 1 pixel
        let mut dst = [0u32; 16];
        let _ = paint(&mut dst, 4, 4, &src, 4, 4);
        assert_eq!(dst[0], 0xFFAA_BBCC); // the one real pixel landed
    }

    #[test]
    fn exit_key_matches_esc_and_q_on_press_only() {
        assert!(is_exit_key(1, 1, 0), "Esc keycode, press");
        assert!(is_exit_key(1, 2, 0), "Esc keycode, auto-repeat");
        assert!(is_exit_key(16, 1, 0), "q keycode, press");
        assert!(is_exit_key(999, 1, 'q' as u32), "q by rune on a remapped keyboard");
        assert!(is_exit_key(999, 1, 'Q' as u32), "Q by rune");
        assert!(!is_exit_key(1, 0, 0), "Esc RELEASE does not exit");
        assert!(!is_exit_key(16, 0, 0), "q release does not exit");
        assert!(!is_exit_key(30, 1, 'a' as u32), "another key does not exit");
    }

    #[test]
    fn paint_clamps_a_short_dst_without_panic() {
        // dst shorter than dw*dh clamps the fill + blit to the slice.
        let src = [0xFF01_0203u32; 4];
        let mut dst = [0u32; 2]; // claims 4x4
        let _ = paint(&mut dst, 4, 4, &src, 2, 2);
        // no panic; the two available cells were touched
        assert!(dst.iter().all(|&p| p == LETTERBOX || p == 0xFF01_0203));
    }
}
