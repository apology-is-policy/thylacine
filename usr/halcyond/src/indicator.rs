// The position indicator (HALCYON-INSTRUMENT 7.7; ruling 7): a static
// report of the scroll position, shown only while the content overflows its
// viewport -- no fade, no drag, no click-to-jump, no focus of its own, no
// ink change on hover. Wheel and keys scroll the content as they do today;
// this only says where the view is. Instrument only: the legacy tile has no
// indicator, and `Sheet::indicator` is what the painters key on.
//
// Geometry, LOGICAL px at 100 through the sheet's `ipx` (the shared scale
// helper): an 8 px lane reserved INSIDE the content viewport on overflow
// (the text never sits under the thumb; line breaks may change by it -- an
// explicit replacement of the CSS's platform-dependent `scrollbar-width:
// thin`), no drawn track (the body keeps its own ground), a 3 px thumb
// inset 3 from the right and 4 from either end, at least 24 tall, `dim` at
// full opacity, rectangular. With content extent C and viewport V:
//
//   hidden when C <= V; L = max(0, V - 8); T = min(L, max(24, L * V / C));
//   travel = L - T; offset = clamp(scroll, 0, C - V);
//   leading edge = 4 + travel * offset / (C - V); T = L when L < 24.
//
// Follow-tail puts the thumb's END exactly at V - 4 (offset = C - V), and
// while the reader is in history (scroll_up > 0 in the tile's bottom-anchored
// model) an append keeps them there, so the indicator never reports "at
// end" until they return. A raw full-screen application owns its grid and
// gets none (14.7).

use crate::layout::Sheet;

/// The lane's width: what the layout width shrinks by on overflow.
pub const LANE: i32 = 8;
pub const THUMB_W: i32 = 3;
pub const INSET_RIGHT: i32 = 3;
pub const INSET_END: i32 = 4;
pub const MIN_THUMB: i32 = 24;

/// The lane the layout reserves on overflow, at the sheet's scale (0 when
/// the sheet paints no indicator).
pub fn lane(sheet: &Sheet) -> i32 {
    if sheet.indicator {
        sheet.ipx(LANE)
    } else {
        0
    }
}

/// The thumb's (leading edge, length) inside a viewport `view_h` tall over
/// content `content_h` tall scrolled `scroll` px from the top; None when
/// the content fits (the indicator is hidden) or the sheet paints none.
pub fn thumb(view_h: i32, content_h: i32, scroll: i32, sheet: &Sheet) -> Option<(i32, i32)> {
    if !sheet.indicator {
        return None;
    }
    thumb_raw(view_h, content_h, scroll, sheet.ipx(INSET_END), sheet.ipx(MIN_THUMB))
}

/// The thumb math, parameterised by the end inset and the minimum thumb, so
/// the picker (min thumb 18, always shown on overflow -- 7.7's "Picker list"
/// row) and the document/terminal (min 24, gated on `sheet.indicator`) share
/// ONE definition. None when the content fits or the viewport is empty.
pub fn thumb_raw(view_h: i32, content_h: i32, scroll: i32, end: i32, min: i32) -> Option<(i32, i32)> {
    if content_h <= view_h || view_h <= 0 {
        return None;
    }
    let l = (view_h - 2 * end).max(0);
    let t = if l < min {
        l
    } else {
        let prop = (l as i64 * view_h as i64) / content_h as i64;
        l.min((prop as i32).max(min))
    };
    let travel = (l - t).max(0);
    let range = content_h - view_h;
    let offset = scroll.clamp(0, range);
    let lead = end + ((2 * travel as i64 * offset as i64 + range as i64) / (2 * range as i64)) as i32;
    Some((lead, t))
}

/// The picker's min-thumb floor (7.7): 18 logical px.
pub const PICKER_MIN_THUMB: i32 = 18;

/// The thumb's rect (x, y, w, h) for a viewport `view_w` wide whose top is
/// at `view_y`, or None when hidden.
pub fn thumb_rect(
    view_w: i32,
    view_y: i32,
    view_h: i32,
    content_h: i32,
    scroll: i32,
    sheet: &Sheet,
) -> Option<(i32, i32, i32, i32)> {
    let (lead, t) = thumb(view_h, content_h, scroll, sheet)?;
    let w = sheet.ipx(THUMB_W);
    let x = view_w - sheet.ipx(INSET_RIGHT) - w;
    Some((x, view_y + lead, w, t))
}

#[cfg(test)]
mod tests {
    use super::*;
    use libhalcyon::instrument::{Bundle, Profile};

    fn inst() -> Sheet {
        crate::layout::sheet_for(&Bundle::builtin(Profile::Instrument), 100, crate::layout::TEST_DISPLAY_W)
    }

    #[test]
    fn hidden_until_the_content_overflows_and_never_under_legacy() {
        let s = inst();
        assert_eq!(thumb(500, 500, 0, &s), None, "fits exactly: hidden");
        assert_eq!(thumb(500, 400, 0, &s), None, "fits: hidden");
        assert!(thumb(500, 501, 0, &s).is_some(), "one px over: shown");
        assert_eq!(lane(&s), 8);
        let legacy = crate::layout::daylight_sheet(100);
        assert_eq!(thumb(500, 5000, 0, &legacy), None, "the legacy sheet paints no indicator");
        assert_eq!(lane(&legacy), 0);
    }

    #[test]
    fn the_thumb_is_proportional_with_a_floor_and_ends_at_the_tail_inset() {
        let s = inst();
        // V = 500, C = 1000: L = 492, T = 492 * 500 / 1000 = 246, travel 246.
        assert_eq!(thumb(500, 1000, 0, &s), Some((4, 246)), "at the top: leading edge 4");
        let (lead, t) = thumb(500, 1000, 500, &s).unwrap();
        assert_eq!(lead + t, 500 - 4, "at the tail: the end edge sits at V - 4");
        assert_eq!(thumb(500, 1000, 250, &s), Some((4 + 123, 246)), "halfway: the middle of the travel");
        // A long history: the thumb floors at 24.
        let (_, t) = thumb(500, 100_000, 0, &s).unwrap();
        assert_eq!(t, 24);
        let (lead, t) = thumb(500, 100_000, 99_500, &s).unwrap();
        assert_eq!(lead + t, 496);
        // A viewport shorter than the minimum thumb: T = L.
        assert_eq!(thumb(20, 200, 0, &s), Some((4, 12)));
        // Offsets past the range clamp.
        assert_eq!(thumb(500, 1000, 5000, &s), thumb(500, 1000, 500, &s));
        assert_eq!(thumb(500, 1000, -7, &s), thumb(500, 1000, 0, &s));
    }

    #[test]
    fn the_rect_sits_in_the_lane_at_the_right_inset() {
        let s = inst();
        let (x, y, w, h) = thumb_rect(640, 32, 500, 1000, 500, &s).unwrap();
        assert_eq!((x, w), (640 - 3 - 3, 3));
        assert_eq!(y + h, 32 + 500 - 4);
        assert!(thumb_rect(640, 0, 500, 500, 0, &s).is_none());
    }

    #[test]
    fn the_geometry_scales_through_the_sheet() {
        let s = crate::layout::sheet_for(&Bundle::builtin(Profile::Instrument), 200, crate::layout::TEST_DISPLAY_W);
        assert_eq!(lane(&s), 16);
        let (lead, t) = thumb(1000, 2000, 0, &s).unwrap();
        assert_eq!((lead, t), (8, 492));
        let (x, _, w, _) = thumb_rect(1280, 0, 1000, 2000, 0, &s).unwrap();
        assert_eq!((x, w), (1280 - 6 - 6, 6));
    }
}
