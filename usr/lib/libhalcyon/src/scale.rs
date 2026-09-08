// scale -- the display scale (docs/HALCYON-SCALE.md 3): the percent the
// compositor derives from the display's physical size and every painter
// follows. Pure and host-tested; tapestryd derives, halcyond consumes.
//
// The rule is the operator's (HALCYON-COMPOSITION 1): logical pixels at a
// 96 DPI reference, `scale = DPI / 96` snapped to the nearest 0.25, round
// half up uniformly. The value here is that scale x 100 -- 100, 125, 150,
// 175, 200 -- so the ctl line and the verb carry an integer, never a float.

/// The v1 range (HALCYON-SCALE 8: above 200% needs three more bakes).
pub const SCALE_MIN: u16 = 100;
pub const SCALE_MAX: u16 = 200;
/// One snap step (0.25).
pub const SCALE_STEP: u16 = 25;
/// The reference DPI every logical pixel is defined at.
pub const REFERENCE_DPI: u32 = 96;

/// Round half up to a whole pixel, for a NON-NEGATIVE value (every logical
/// size is): `(v + 0.5) as i32` -- `f32::round` is not in `core`.
#[inline]
pub fn round_half_up(v: f32) -> i32 {
    (v + 0.5) as i32
}

/// A logical size in physical pixels at `pct` (round half up).
#[inline]
pub fn px(logical: f32, pct: u16) -> f32 {
    logical * (pct as f32) / 100.0
}

/// A logical INTEGER size in physical pixels at `pct`, round half up in
/// integers (`v x pct / 100` with the half added before the division).
#[inline]
pub const fn ipx(logical: i32, pct: u16) -> i32 {
    (logical * pct as i32 + 50) / 100
}

/// Is `pct` one of the five values the wire admits?
pub const fn is_valid_pct(pct: u16) -> bool {
    pct >= SCALE_MIN && pct <= SCALE_MAX && pct % SCALE_STEP == 0
}

/// One step up (+1) or down (-1), clamped to the range.
pub fn step(pct: u16, dir: i8) -> u16 {
    let cur = if is_valid_pct(pct) { pct } else { SCALE_MIN };
    if dir > 0 {
        (cur + SCALE_STEP).min(SCALE_MAX)
    } else if dir < 0 {
        cur.saturating_sub(SCALE_STEP).max(SCALE_MIN)
    } else {
        cur
    }
}

/// The scale for a `px_w x px_h` scanout of `mm_w x mm_h` millimetres:
/// each axis' DPI snapped to the nearest 0.25 of the reference, the
/// SMALLER of the two (a monitor lying about one axis must not blow the
/// other up), clamped to the v1 range. A zero dimension anywhere is 100.
pub fn scale_pct(px_w: u32, px_h: u32, mm_w: u32, mm_h: u32) -> u16 {
    if px_w == 0 || px_h == 0 || mm_w == 0 || mm_h == 0 {
        return SCALE_MIN;
    }
    let axis = |px: u32, mm: u32| -> u16 {
        let dpi = (px as f32) * 25.4 / (mm as f32);
        let quarters = (dpi / (REFERENCE_DPI as f32) * 4.0 + 0.5) as u32; // round half up
        // Clamp BEFORE the narrowing: a 1 mm axis under thousands of pixels
        // makes `quarters * 25` exceed u16, and a wrapped value need not be
        // a multiple of 25 -- an off-table percent from a hostile EDID.
        let pct = (quarters * 25).min(SCALE_MAX as u32) as u16;
        pct.clamp(SCALE_MIN, SCALE_MAX)
    };
    let w = axis(px_w, mm_w);
    let h = axis(px_h, mm_h);
    if w < h {
        w
    } else {
        h
    }
}

/// The largest physical dimension an EDID may claim before it is garbage
/// (2 m: no monitor; a zero is garbage too).
pub const EDID_MM_MAX: u32 = 2000;

/// The display's image size in millimetres from a base EDID block
/// (untrusted device input; VESA E-EDID 1.4): None unless the block is 128
/// bytes with the fixed header, a valid checksum, and a size in
/// 1..=EDID_MM_MAX on both axes. The first detailed timing descriptor
/// (bytes 54..72, a timing iff its pixel clock is non-zero) carries the
/// size in mm; without one the basic parameters' centimetres (bytes 21,
/// 22) x 10 stand in.
pub fn parse_edid_mm(edid: &[u8]) -> Option<(u32, u32)> {
    if edid.len() < 128 {
        return None;
    }
    let b = &edid[..128];
    if b[..8] != [0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00] {
        return None;
    }
    let sum = b.iter().fold(0u8, |acc, &v| acc.wrapping_add(v));
    if sum != 0 {
        return None;
    }
    let dtd = &b[54..72];
    let (mm_w, mm_h) = if dtd[0] != 0 || dtd[1] != 0 {
        (
            dtd[12] as u32 | ((dtd[14] as u32 >> 4) << 8),
            dtd[13] as u32 | ((dtd[14] as u32 & 0xF) << 8),
        )
    } else {
        (b[21] as u32 * 10, b[22] as u32 * 10)
    };
    let ok = |v: u32| (1..=EDID_MM_MAX).contains(&v);
    if ok(mm_w) && ok(mm_h) {
        Some((mm_w, mm_h))
    } else {
        None
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A base EDID block with the header, the cm size, an optional DTD
    /// size in mm, and a correct checksum.
    fn edid(cm: (u8, u8), dtd_mm: Option<(u32, u32)>) -> [u8; 128] {
        let mut b = [0u8; 128];
        b[..8].copy_from_slice(&[0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00]);
        b[18] = 1; // version
        b[19] = 4;
        b[21] = cm.0;
        b[22] = cm.1;
        if let Some((w, h)) = dtd_mm {
            b[54] = 0x10; // a pixel clock: this descriptor is a timing
            b[55] = 0x27;
            b[66] = (w & 0xFF) as u8;
            b[67] = (h & 0xFF) as u8;
            b[68] = (((w >> 8) as u8) << 4) | ((h >> 8) as u8 & 0xF);
        }
        let sum = b[..127].iter().fold(0u8, |a, &v| a.wrapping_add(v));
        b[127] = 0u8.wrapping_sub(sum);
        b
    }

    #[test]
    fn the_snap_is_the_operators_rule() {
        // QEMU's generated EDID for 1280x800: about 100 DPI -> 1.04 -> 1.0.
        assert_eq!(scale_pct(1280, 800, 325, 203), 100);
        // A 96 DPI monitor is exactly 1.0; 27" 4K (597x336 mm) is 163 DPI
        // -> 1.70 -> 1.75; a 13" 2560x1600 (286x179) is 227 DPI -> 2.37 ->
        // 2.25 -> clamped 200; 144 DPI is 1.5 exactly.
        assert_eq!(scale_pct(1920, 1080, 508, 286), 100);
        assert_eq!(scale_pct(3840, 2160, 597, 336), 175);
        assert_eq!(scale_pct(2560, 1600, 286, 179), 200);
        assert_eq!(scale_pct(1920, 1200, 339, 212), 150);
        // Round half up at the step boundary: 1.125 -> 1.25 (quarters 4.5
        // -> 5); just under -> 1.0.
        assert_eq!(scale_pct(1080, 1080, 254, 254), 125); // 108 DPI = 1.125
        assert_eq!(scale_pct(1070, 1070, 254, 254), 100);
        // The smaller axis wins when they disagree.
        assert_eq!(scale_pct(1920, 1080, 254, 286), 100, "a lying width");
        // A zero anywhere is 1.0, never a division by zero.
        assert_eq!(scale_pct(0, 800, 300, 200), 100);
        assert_eq!(scale_pct(1280, 800, 0, 200), 100);
    }

    #[test]
    fn the_five_values_and_the_steps() {
        for p in [100u16, 125, 150, 175, 200] {
            assert!(is_valid_pct(p));
        }
        for p in [0u16, 50, 99, 101, 110, 225, 300] {
            assert!(!is_valid_pct(p));
        }
        assert_eq!(step(100, 1), 125);
        assert_eq!(step(175, 1), 200);
        assert_eq!(step(200, 1), 200, "clamped at the top");
        assert_eq!(step(125, -1), 100);
        assert_eq!(step(100, -1), 100, "clamped at the bottom");
        assert_eq!(step(150, 0), 150);
        assert_eq!(step(7, 1), 125, "an invalid current counts as 100");
    }

    #[test]
    fn the_pixel_helpers_round_half_up() {
        assert_eq!(round_half_up(17.5), 18);
        assert_eq!(round_half_up(17.49), 17);
        assert_eq!(ipx(20, 150), 30);
        assert_eq!(ipx(5, 150), 8, "7.5 rounds up");
        assert_eq!(ipx(1, 125), 1, "1.25 rounds down");
        assert_eq!(ipx(2, 125), 3, "2.5 rounds up");
        assert_eq!(ipx(6, 175), 11, "10.5 rounds up");
        assert!((px(11.5, 200) - 23.0).abs() < 1e-6);
        assert!((px(11.5, 100) - 11.5).abs() < 1e-6, "the identity at 100");
    }

    #[test]
    fn the_edid_parse_is_fail_safe() {
        assert_eq!(parse_edid_mm(&edid((60, 34), Some((597, 336)))), Some((597, 336)));
        assert_eq!(parse_edid_mm(&edid((60, 34), None)), Some((600, 340)), "the cm fallback");
        // Garbage: short, a bad header, a bad checksum, a zero size, an
        // absurd size -- all None, never a panic.
        assert_eq!(parse_edid_mm(&[0u8; 64]), None);
        let mut bad = edid((60, 34), Some((597, 336)));
        bad[0] = 1;
        assert_eq!(parse_edid_mm(&bad), None, "the header");
        let mut bad = edid((60, 34), Some((597, 336)));
        bad[100] ^= 1;
        assert_eq!(parse_edid_mm(&bad), None, "the checksum");
        assert_eq!(parse_edid_mm(&edid((0, 0), None)), None, "a zero size");
        assert_eq!(parse_edid_mm(&edid((60, 34), Some((0, 336)))), None);
        assert_eq!(parse_edid_mm(&edid((60, 34), Some((2500, 336)))), None, "absurd");
        // A block longer than 128 (extensions) parses its base block.
        let mut long = [0u8; 256];
        long[..128].copy_from_slice(&edid((60, 34), Some((597, 336))));
        assert_eq!(parse_edid_mm(&long), Some((597, 336)));
    }

    #[test]
    fn a_short_millimetre_axis_never_truncates_off_the_table() {
        // The scale round's F2: `quarters * 25` wrapped in u16 BEFORE the
        // clamp (2481 px over 1 mm: 2626 quarters -> 65650 -> 114) -- an
        // off-table percent from a hostile EDID, applied unguarded at boot.
        // The clamp happens in u32 now: every result over the admitted mm
        // range is one of the five.
        assert_eq!(scale_pct(2481, 1600, 1, 100), SCALE_MAX);
        for mm_w in 1..=4u32 {
            for px_w in (2000..=8192u32).step_by(7) {
                let p = scale_pct(px_w, 1600, mm_w, 100);
                assert!(is_valid_pct(p), "scale_pct({px_w}, 1600, {mm_w}, 100) = {p}");
            }
        }
    }
}
