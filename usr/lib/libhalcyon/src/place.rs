//! The compositor's placement math: a surface inside a pane's content rect.
//!
//! One truth for two readers. tapestryd composes, hit-tests and paints the
//! bars by `letterbox`; the acceptance battery derives its host-side sample
//! points from the SAME function, so the two can never mirror each other
//! wrongly. `scaled_clip` is the damage half: the destination region a
//! partial present of a letterboxed surface actually changes, so the CPU
//! compose redraws that band instead of the whole scaled frame -- a single-
//! slot presenter (the SDL class) blinking a cursor at 70 Hz would otherwise
//! rescale a display-sized rect per blink.

/// The letterbox placement of a (sw, sh) surface inside a (cw, ch) content
/// rect: `(ox, oy, dw, dh)` -- the aspect-preserving scaled extent, centered
/// (the offsets are content-relative). Same-size is the identity; a
/// degenerate scale floors at 1x1 so the rect is never empty.
pub fn letterbox(sw: u32, sh: u32, cw: u32, ch: u32) -> (u32, u32, u32, u32) {
    if sw == cw && sh == ch {
        return (0, 0, cw, ch);
    }
    // Width-bound iff cw/sw <= ch/sh  <=>  cw*sh <= ch*sw (u64: no overflow
    // for display-scale dims).
    let (dw, dh) = if (cw as u64) * (sh as u64) <= (ch as u64) * (sw as u64) {
        (
            cw,
            (((sh as u64) * (cw as u64)) / (sw as u64).max(1)) as u32,
        )
    } else {
        (
            (((sw as u64) * (ch as u64)) / (sh as u64).max(1)) as u32,
            ch,
        )
    };
    let (dw, dh) = (dw.max(1), dh.max(1));
    ((cw - dw) / 2, (ch - dh) / 2, dw, dh)
}

/// The nearest-neighbour source coordinate of destination pixel `d` when a
/// `s`-wide source scales to `dw` -- THE mapping the scaled compose samples
/// by (`compose_cpu`); `scaled_clip` is derived from it, so the two cannot
/// drift.
pub fn nearest_src(d: u32, s: u32, dw: u32) -> u32 {
    (((d as u64) * (s as u64)) / (dw as u64).max(1)) as u32
}

/// The destination-local region a damage rect `(x, y, w, h)` of a (sw, sh)
/// surface projects onto when the surface scales to (dw, dh): every
/// destination pixel whose `nearest_src` lies inside the damage is inside
/// the returned `(cx, cy, cw, ch)` (a SUPERSET: the bounds round outward,
/// and a pixel just outside the damage may be redrawn from its unchanged
/// source, which is harmless). Never empty for a non-empty damage; clamped
/// inside (dw, dh).
pub fn scaled_clip(
    (x, y, w, h): (u32, u32, u32, u32),
    sw: u32,
    sh: u32,
    dw: u32,
    dh: u32,
) -> (u32, u32, u32, u32) {
    fn axis(lo: u32, hi: u32, s: u32, d: u32) -> (u32, u32) {
        let s = s.max(1) as u64;
        let d64 = d as u64;
        // floor for the start (superset), ceil for the end.
        let a = (((lo as u64) * d64 / s) as u32).min(d.saturating_sub(1));
        let b = (((hi as u64) * d64).div_ceil(s) as u32).min(d);
        // A damage rect that projects to nothing (a scale-down drops its
        // columns) still yields one pixel so the caller has a region.
        (a, b.max(a + 1))
    }
    if w == 0 || h == 0 || dw == 0 || dh == 0 {
        return (0, 0, 0, 0);
    }
    let (cx0, cx1) = axis(x, x.saturating_add(w).min(sw), sw, dw);
    let (cy0, cy1) = axis(y, y.saturating_add(h).min(sh), sh, dh);
    (cx0, cy0, cx1 - cx0, cy1 - cy0)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn letterbox_same_size_is_identity() {
        assert_eq!(letterbox(632, 772, 632, 772), (0, 0, 632, 772));
    }

    #[test]
    fn letterbox_fits_dosbox_into_the_display_pillarboxed() {
        // 640x417 (DOSBox-X with its menu bar) zoomed on 1280x800: height-
        // bound, 26 px pillars, no distortion.
        assert_eq!(letterbox(640, 417, 1280, 800), (26, 0, 1227, 800));
        // 640x400 is exactly 16:10 -- fills the display.
        assert_eq!(letterbox(640, 400, 1280, 800), (0, 0, 1280, 800));
        // In a 632x772 tile: width-bound, centered vertically.
        assert_eq!(letterbox(640, 400, 632, 772), (0, 188, 632, 395));
    }

    #[test]
    fn letterbox_never_yields_an_empty_rect() {
        assert_eq!(letterbox(4000, 1, 10, 10), (0, 4, 10, 1));
        assert_eq!(letterbox(1, 4000, 10, 10), (4, 0, 1, 10));
    }

    #[test]
    fn scaled_clip_of_the_full_surface_is_the_full_destination() {
        assert_eq!(
            scaled_clip((0, 0, 640, 400), 640, 400, 1280, 800),
            (0, 0, 1280, 800)
        );
        assert_eq!(
            scaled_clip((0, 0, 640, 400), 640, 400, 632, 395),
            (0, 0, 632, 395)
        );
    }

    #[test]
    fn scaled_clip_is_the_identity_when_unscaled() {
        assert_eq!(
            scaled_clip((10, 20, 30, 40), 640, 400, 640, 400),
            (10, 20, 30, 40)
        );
    }

    #[test]
    fn scaled_clip_is_empty_only_for_an_empty_damage() {
        assert_eq!(scaled_clip((0, 0, 0, 5), 640, 400, 1280, 800), (0, 0, 0, 0));
        // One source column dropped by a 2:1 scale-down still yields a
        // 1-px region.
        let (_, _, w, h) = scaled_clip((1, 0, 1, 400), 640, 400, 320, 200);
        assert!(w >= 1 && h >= 1);
    }

    /// The property the compose relies on: every destination pixel whose
    /// nearest source lies in the damage is inside the clip -- brute-forced
    /// over a grid of scales (up, down, mixed) and damage rects.
    #[test]
    fn scaled_clip_covers_every_pixel_the_damage_reaches() {
        let sizes = [(7u32, 5u32), (16, 10), (13, 29)];
        let dsts = [(7u32, 5u32), (32, 20), (3, 2), (40, 25), (16, 10)];
        for &(sw, sh) in &sizes {
            for &(dw, dh) in &dsts {
                for x in 0..sw {
                    for y in (0..sh).step_by(2) {
                        for w in 1..=(sw - x).min(5) {
                            for h in 1..=(sh - y).min(4) {
                                let (cx, cy, cw, ch) = scaled_clip((x, y, w, h), sw, sh, dw, dh);
                                assert!(cx + cw <= dw && cy + ch <= dh, "inside dst");
                                assert!(cw >= 1 && ch >= 1, "non-empty");
                                for col in 0..dw {
                                    for row in 0..dh {
                                        let sx = nearest_src(col, sw, dw);
                                        let sy = nearest_src(row, sh, dh);
                                        let hits = sx >= x && sx < x + w && sy >= y && sy < y + h;
                                        let inside = col >= cx
                                            && col < cx + cw
                                            && row >= cy
                                            && row < cy + ch;
                                        assert!(
                                            !hits || inside,
                                            "({sw}x{sh}->{dw}x{dh}) damage ({x},{y},{w},{h}) reaches ({col},{row}) outside clip ({cx},{cy},{cw},{ch})"
                                        );
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
