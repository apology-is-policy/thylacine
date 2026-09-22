// picker -- the theme picker's model and painter (HALCYON-INSTRUMENT 9.4,
// I-7): the pure half of the display-theme control. `menuset` carries a
// `Picker` as one model on the ONE Role::Menu surface (beside the verb menu
// and the dialog), so the compositor's grab, click-away and Esc are exactly
// H-3c's. This file thinks; it never syscalls.
//
// The registry is the gallery directory: the caller reads every
// /lib/halcyon/themes/*.toml that loads as an Instrument theme and builds
// the list here, grouped and ordered by the optional [meta] group / rank
// keys (4.2), the id breaking a tie. The rows are 7.3's; the four miniature
// colours are each theme's OWN (desktop / open / structure / amber).

use alloc::string::String;
use alloc::vec::Vec;

use cartoon::{Cartoon, Op};
use libhalcyon::instrument::Group;
use libhalcyon::theme::Argb;

use crate::indicator;
use crate::layout::Sheet;
use crate::raster::GlyphSource;

/// The picker's fixed width (the mockup's `.theme-menu`), logical px.
pub const WIDTH: i32 = 286;
/// The heading row (`DISPLAY THEME` + count).
const HEADING_H: i32 = 27;
/// A group label row (`DARK FIELD`, ...).
const GROUP_H: i32 = 21;
/// An option row (miniature | title/subtitle | check).
const OPTION_H: i32 = 52;
const PAD: i32 = 5;
const ROW_PAD_X: i32 = 8;
/// The option grid: 42 | 1fr | 16, gap 10.
const MINI_COL: i32 = 42;
const CHECK_COL: i32 = 16;
const GRID_GAP: i32 = 10;
const OPT_PAD_X: i32 = 8;
const OPT_PAD_Y: i32 = 6;
/// The miniature (`.theme-preview`): 38 x 28, pad 4, three bars gap 2.
const MINI_W: i32 = 38;
const MINI_H: i32 = 28;
const MINI_PAD: i32 = 4;
const MINI_GAP: i32 = 2;
/// The display margin the picker's height leaves (9.4: `display - 72`).
pub const DISPLAY_MARGIN: i32 = 72;
/// Type sizes (logical): heading/group mono, the title Sans, the subtitle
/// mono, the check mono.
const HEADING_PX: f32 = 9.0;
const GROUP_PX: f32 = 8.0;
const TITLE_PX: f32 = 12.0;
const SUB_PX: f32 = 9.0;
const CHECK_PX: f32 = 12.0;
const HEADING_TRACK: f32 = 0.11;
const GROUP_TRACK: f32 = 0.13;

/// One gallery theme, resolved for the picker.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PickerTheme {
    pub id: String,
    /// `[meta] name` -- the title.
    pub name: String,
    /// `[meta] tagline` -- the subtitle; the id when the file gave none.
    pub tagline: String,
    pub group: Option<Group>,
    pub rank: u8,
    /// The four miniature colours: the theme's own `desktop` / `open` /
    /// `structure` / `amber` (7.3: "each miniature in ITS OWN theme's four
    /// colours").
    pub pv_bg: Argb,
    pub pv_pane: Argb,
    pub pv_rule: Argb,
    pub pv_signal: Argb,
}

/// The group's sort rank and picker label; `None` is the trailing OTHER.
fn group_order(g: Option<Group>) -> u8 {
    match g {
        Some(Group::Dark) => 0,
        Some(Group::Terminal) => 1,
        Some(Group::Light) => 2,
        None => 3,
    }
}

fn group_label(g: Option<Group>) -> &'static str {
    match g {
        Some(x) => x.label(),
        None => libhalcyon::instrument::OTHER_GROUP_LABEL,
    }
}

/// A key on the picker surface.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum PickerKey {
    Up,
    Down,
    Home,
    End,
    /// Enter or Space (9.4: both commit).
    Commit,
    None,
}

/// Map a KEY press (rune-first, then evdev code) to a picker key. Enter and
/// Space both commit (9.4); j/k move like the menu's.
pub fn picker_key(code: u16, rune: u32) -> PickerKey {
    match rune {
        0x6b => PickerKey::Up,              // k
        0x6a => PickerKey::Down,            // j
        0x0d | 0x0a | 0x20 => PickerKey::Commit, // Enter / Space
        _ => match code {
            103 => PickerKey::Up,
            108 => PickerKey::Down,
            102 => PickerKey::Home,
            107 => PickerKey::End,
            28 | 96 => PickerKey::Commit, // Enter / KP-Enter
            57 => PickerKey::Commit,      // Space
            _ => PickerKey::None,
        },
    }
}

/// The open picker.
#[derive(Clone, Debug)]
pub struct Picker {
    /// The themes in DISPLAY order (grouped, ranked, id-broken).
    pub options: Vec<PickerTheme>,
    /// The selected row (keyboard focus); wraps.
    pub sel: usize,
    /// The current theme's row -- the check; None when the theme in force is
    /// not in the gallery (a user file, or the built-in).
    pub current: Option<usize>,
}

impl Picker {
    /// Build from the resolved gallery list and the id of the theme in
    /// force. Sorts by (group order, rank, id); focuses the current theme
    /// (else the first row) and applies nothing.
    pub fn build(mut themes: Vec<PickerTheme>, current_id: &str) -> Picker {
        themes.sort_by(|a, b| {
            group_order(a.group)
                .cmp(&group_order(b.group))
                .then(a.rank.cmp(&b.rank))
                .then_with(|| a.id.cmp(&b.id))
        });
        let current = themes.iter().position(|t| t.id == current_id);
        let sel = current.unwrap_or(0);
        Picker {
            options: themes,
            sel,
            current,
        }
    }

    pub fn is_empty(&self) -> bool {
        self.options.is_empty()
    }

    /// The selected theme's id, if any.
    pub fn selected_id(&self) -> Option<&str> {
        self.options.get(self.sel).map(|t| t.id.as_str())
    }

    /// Apply a key: Up/Down WRAP over the options (9.4), Home/End jump,
    /// Commit yields the selected id.
    pub fn key(&mut self, k: PickerKey) -> Option<String> {
        let n = self.options.len();
        if n == 0 {
            return None;
        }
        match k {
            PickerKey::Up => {
                self.sel = if self.sel == 0 { n - 1 } else { self.sel - 1 };
                None
            }
            PickerKey::Down => {
                self.sel = if self.sel + 1 >= n { 0 } else { self.sel + 1 };
                None
            }
            PickerKey::Home => {
                self.sel = 0;
                None
            }
            PickerKey::End => {
                self.sel = n - 1;
                None
            }
            PickerKey::Commit => self.selected_id().map(String::from),
            PickerKey::None => None,
        }
    }

    /// A wheel delta (REL_WHEEL: +1 = up): moves the selection, clamped (no
    /// wrap on the wheel), so the view follows -- the menu's precedent.
    pub fn wheel(&mut self, delta: i32) {
        if self.options.is_empty() {
            return;
        }
        let last = self.options.len() - 1;
        self.sel = if delta > 0 {
            self.sel.saturating_sub(delta as usize)
        } else {
            self.sel.saturating_add(delta.unsigned_abs() as usize).min(last)
        };
    }
}

/// The option index at surface point (x, y), accounting for the scroll the
/// painter uses (derived from the selection), or None. For a click and for
/// hover.
pub fn option_at(p: &Picker, x: i32, y: i32, w: u32, h: u32, sheet: &Sheet) -> Option<usize> {
    let (rl, content) = rows(p, sheet);
    let top = scroll_top(&rl, p.sel, h as i32, content);
    let pad = sheet.ipx(PAD);
    if x < pad || x >= w as i32 - pad {
        return None;
    }
    for (row, y0, rh) in &rl {
        if let PRow::Option(i) = row {
            let ry = *y0 - top;
            if y >= ry && y < ry + *rh {
                return Some(*i);
            }
        }
    }
    None
}

/// The layout rows, in paint order.
enum PRow {
    Heading,
    Group(&'static str),
    Option(usize),
}

/// The full row list (heading, then each group's label + its options) and
/// the total content height at `sheet`'s scale.
fn rows(p: &Picker, sheet: &Sheet) -> (Vec<(PRow, i32, i32)>, i32) {
    let mut out: Vec<(PRow, i32, i32)> = Vec::new();
    let mut y = sheet.ipx(PAD);
    let h_head = sheet.ipx(HEADING_H);
    out.push((PRow::Heading, y, h_head));
    y += h_head;
    let mut last_group: Option<Option<Group>> = None;
    let h_group = sheet.ipx(GROUP_H);
    let h_opt = sheet.ipx(OPTION_H);
    for (i, t) in p.options.iter().enumerate() {
        if last_group != Some(t.group) {
            out.push((PRow::Group(group_label(t.group)), y, h_group));
            y += h_group;
            last_group = Some(t.group);
        }
        out.push((PRow::Option(i), y, h_opt));
        y += h_opt;
    }
    y += sheet.ipx(PAD);
    (out, y)
}

/// The surface size: WIDTH fixed, height min(content, `max_h`) (9.4). The
/// caller (`menuset`) passes `max_h` = display - 72.
pub fn picker_size(p: &Picker, sheet: &Sheet, max_h: u32) -> (u32, u32) {
    let (_, content) = rows(p, sheet);
    let w = sheet.ipx(WIDTH).max(1) as u32;
    let h = (content.max(1) as u32).min(max_h.max(1));
    (w, h)
}

fn centre_base(gs: &mut GlyphSource, face: u8, px: f32, box_h: i32) -> i32 {
    let (asc, desc) = gs
        .line_metrics(face, px)
        .map(|m| (m.ascent, m.descent))
        .unwrap_or((8, 2));
    (box_h - (asc + desc)) / 2 + asc
}

/// The scroll offset (px) that keeps the selected option's row visible in a
/// viewport `h` tall over `content` tall, given the row layout.
fn scroll_top(rows: &[(PRow, i32, i32)], sel: usize, h: i32, content: i32) -> i32 {
    if content <= h {
        return 0;
    }
    let sel_row = rows.iter().find_map(|(r, y, rh)| match r {
        PRow::Option(i) if *i == sel => Some((*y, *rh)),
        _ => None,
    });
    let (y0, rh) = match sel_row {
        Some(v) => v,
        None => return 0,
    };
    let max_top = content - h;
    // Keep [y0, y0+rh] within [top, top+h].
    let mut top = 0i32.max(y0 + rh - h);
    if y0 < top {
        top = y0;
    }
    top.clamp(0, max_top)
}

/// The display list for the picker on a w x h surface (7.3 colours): `pane`
/// ground, a 1 px `structure` frame; the heading; group rows on `header`;
/// options with the theme miniature, the title/subtitle, and the `amber`
/// check on the current row; the overflow thumb (7.7, min 18).
pub fn picker_list(p: &Picker, w: u32, h: u32, sheet: &Sheet, gs: &mut GlyphSource) -> Cartoon {
    gs.set_kerning(sheet.kerning);
    let i = &sheet.inst;
    let mut cart = Cartoon::new();
    if w == 0 || h == 0 {
        return cart;
    }
    let (wi, hi) = (w as i32, h as i32);
    let hair = sheet.hairline;
    let hu = hair as u32;
    cart.ops.push(Op::Clear { color: i.pane });
    // The 1 px `structure` frame.
    for r in [
        (0, 0, w, hu),
        (0, (hi - hair).max(0), w, hu),
        (0, 0, hu, h),
        ((wi - hair).max(0), 0, hu, h),
    ] {
        cart.ops.push(Op::Rect { x: r.0, y: r.1, w: r.2, h: r.3, color: i.structure });
    }
    let gen = gs.gen();
    let (row_list, content) = rows(p, sheet);
    let top = scroll_top(&row_list, p.sel, hi, content);
    let sep = i.separator;
    let pad = sheet.ipx(PAD);
    let pad_x = sheet.ipx(ROW_PAD_X);
    // The overflow thumb (7.7): reserved INSIDE the rows -- the option grid's
    // last column already leaves room, so the lane sits at the right edge.
    let lane = sheet.ipx(indicator::LANE);
    let thumb = indicator::thumb_raw(hi, content, top, sheet.ipx(indicator::INSET_END), sheet.ipx(indicator::PICKER_MIN_THUMB));

    for (row, y0, rh) in &row_list {
        let y = *y0 - top;
        if y + *rh <= 0 || y >= hi {
            continue; // off-screen
        }
        match row {
            PRow::Heading => {
                // `DISPLAY THEME` left, the count right, mono in `dim`, a
                // `separator` below.
                let base = y + centre_base(gs, sheet.face_mono_text, sheet.px(HEADING_PX), *rh);
                let (refs, _) = gs.shape_run_spaced(
                    sheet.face_mono_text,
                    sheet.px(HEADING_PX),
                    HEADING_TRACK * sheet.px(HEADING_PX),
                    "DISPLAY THEME".chars(),
                );
                if !refs.is_empty() {
                    cart.push_glyphs(gen, pad + pad_x, base, i.dim, &refs);
                }
                let mut count = String::new();
                let _ = core::fmt::write(&mut count, format_args!("{}", p.options.len()));
                let (crefs, cw) = gs.shape_run(sheet.face_mono_text, sheet.px(HEADING_PX), count.chars());
                if !crefs.is_empty() {
                    cart.push_glyphs(gen, wi - pad - pad_x - cw, base, i.dim, &crefs);
                }
                rule(&mut cart, pad, y + *rh - hair, wi - 2 * pad, hu, sep);
            }
            PRow::Group(label) => {
                rect(&mut cart, pad, y, (wi - 2 * pad).max(0), *rh, i.header);
                let base = y + centre_base(gs, sheet.face_mono_text, sheet.px(GROUP_PX), *rh);
                let (refs, _) = gs.shape_run_spaced(
                    sheet.face_mono_text,
                    sheet.px(GROUP_PX),
                    GROUP_TRACK * sheet.px(GROUP_PX),
                    label.chars(),
                );
                if !refs.is_empty() {
                    cart.push_glyphs(gen, pad + pad_x, base, i.dim, &refs);
                }
                rule(&mut cart, pad, y + *rh - hair, wi - 2 * pad, hu, sep);
            }
            PRow::Option(idx) => {
                let t = &p.options[*idx];
                if *idx == p.sel {
                    rect(&mut cart, pad, y, (wi - 2 * pad).max(0), *rh, i.hover);
                }
                let opx = pad + sheet.ipx(OPT_PAD_X);
                let opy = y + sheet.ipx(OPT_PAD_Y);
                let inner_h = *rh - 2 * sheet.ipx(OPT_PAD_Y);
                // The miniature, vertically centred in the row's content box.
                let mini_y = y + (*rh - sheet.ipx(MINI_H)) / 2;
                paint_miniature(&mut cart, t, opx, mini_y, sheet);
                // Title + subtitle, in the 1fr column.
                let text_x = opx + sheet.ipx(MINI_COL) + sheet.ipx(GRID_GAP);
                let text_r = wi - pad - sheet.ipx(OPT_PAD_X) - sheet.ipx(CHECK_COL) - sheet.ipx(GRID_GAP);
                let text_w = (text_r - text_x).max(0);
                let title_base = opy + centre_base(gs, sheet.face_medium, sheet.px(TITLE_PX), inner_h * 2 / 3);
                let title = crate::chrome::fit_end_pub(gs, sheet.face_medium, sheet.px(TITLE_PX), &t.name, text_w);
                let (trefs, _) = gs.shape_run(sheet.face_medium, sheet.px(TITLE_PX), title.chars());
                if !trefs.is_empty() {
                    cart.push_glyphs(gen, text_x, title_base, i.text, &trefs);
                }
                let sub = if t.tagline.is_empty() { t.id.as_str() } else { t.tagline.as_str() };
                let sub_base = opy + inner_h * 2 / 3 + centre_base(gs, sheet.face_mono_text, sheet.px(SUB_PX), inner_h / 3);
                let subf = crate::chrome::fit_end_pub(gs, sheet.face_mono_text, sheet.px(SUB_PX), sub, text_w);
                let (srefs, _) = gs.shape_run(sheet.face_mono_text, sheet.px(SUB_PX), subf.chars());
                if !srefs.is_empty() {
                    cart.push_glyphs(gen, text_x, sub_base, i.dim, &srefs);
                }
                // The check (U+2713) in `amber`, on the current theme's row.
                if p.current == Some(*idx) {
                    let cx = wi - pad - sheet.ipx(OPT_PAD_X) - sheet.ipx(CHECK_COL);
                    let cbase = y + centre_base(gs, sheet.face_mono_text, sheet.px(CHECK_PX), *rh);
                    let (crefs, _) = gs.shape_run(sheet.face_mono_text, sheet.px(CHECK_PX), "\u{2713}".chars());
                    if !crefs.is_empty() {
                        cart.push_glyphs(gen, cx, cbase, i.amber, &crefs);
                    }
                }
                rule(&mut cart, pad, y + *rh - hair, wi - 2 * pad, hu, sep);
            }
        }
    }
    // The thumb over everything, in `dim` (7.7).
    if let Some((lead, len)) = thumb {
        let x = wi - sheet.ipx(indicator::INSET_RIGHT) - sheet.ipx(indicator::THUMB_W);
        rect(&mut cart, x, lead, sheet.ipx(indicator::THUMB_W), len, i.dim);
        let _ = lane;
    }
    cart
}

/// The 38 x 28 miniature (7.3): ground `pv_bg`, a 1 px `pv_rule` frame,
/// three `pv_pane` bars (gap 2) each with a 1 px `pv_rule` left rule; the
/// first bar 1.45x with a 2 px `pv_signal` left rule; the last bar carries a
/// 1 px `pv_signal` line at 45 % of its height at .7 over `pv_pane`.
fn paint_miniature(cart: &mut Cartoon, t: &PickerTheme, x: i32, y: i32, sheet: &Sheet) {
    let hair = sheet.hairline;
    let mw = sheet.ipx(MINI_W);
    let mh = sheet.ipx(MINI_H);
    rect(cart, x, y, mw, mh, t.pv_bg);
    // Frame.
    ring(cart, x, y, mw, mh, hair, t.pv_rule);
    let pad = sheet.ipx(MINI_PAD);
    let gap = sheet.ipx(MINI_GAP);
    let ix = x + pad;
    let iy = y + pad;
    let iw = (mw - 2 * pad).max(0);
    let ih = (mh - 2 * pad).max(0);
    let bars_w = (iw - 2 * gap).max(0);
    // Flex 1.45 / 1 / 1; the last bar takes the remainder so they fill.
    let w1 = (bars_w as i64 * 145 / 345) as i32;
    let w2 = (bars_w as i64 * 100 / 345) as i32;
    let w3 = (bars_w - w1 - w2).max(0);
    let mut bx = ix;
    for (n, bw) in [(0usize, w1), (1, w2), (2, w3)] {
        if bw <= 0 {
            bx += bw + gap;
            continue;
        }
        rect(cart, bx, iy, bw, ih, t.pv_pane);
        // Each bar's 1 px `pv_rule` left rule; the first bar's is a 2 px
        // `pv_signal` rule instead.
        if n == 0 {
            rect(cart, bx, iy, (2 * hair).min(bw), ih, t.pv_signal);
        } else {
            rect(cart, bx, iy, hair.min(bw), ih, t.pv_rule);
        }
        if n == 2 {
            // The signal line at 45 % of the bar's height, .7 over pv_pane.
            let ly = iy + ih * 45 / 100;
            let line = libhalcyon::instrument::over(t.pv_pane, t.pv_signal, 179);
            rect(cart, bx + hair, ly, (bw - 2 * hair).max(0), hair, line);
        }
        bx += bw + gap;
    }
}

fn rect(cart: &mut Cartoon, x: i32, y: i32, w: i32, h: i32, color: Argb) {
    if w > 0 && h > 0 {
        cart.ops.push(Op::Rect { x, y, w: w as u32, h: h as u32, color });
    }
}

fn rule(cart: &mut Cartoon, x: i32, y: i32, w: i32, h: u32, color: Argb) {
    if w > 0 && h > 0 {
        cart.ops.push(Op::Rect { x, y, w: w as u32, h, color });
    }
}

fn ring(cart: &mut Cartoon, x: i32, y: i32, w: i32, h: i32, t: i32, color: Argb) {
    rect(cart, x, y, w, t, color);
    rect(cart, x, y + h - t, w, t, color);
    rect(cart, x, y, t, h, color);
    rect(cart, x + w - t, y, t, h, color);
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::layout::sheet_for;
    use libhalcyon::instrument::{Bundle, Profile};

    fn theme(id: &str, name: &str, g: Option<Group>, rank: u8) -> PickerTheme {
        PickerTheme {
            id: String::from(id),
            name: String::from(name),
            tagline: String::new(),
            group: g,
            rank,
            pv_bg: 0xFF010101,
            pv_pane: 0xFF020202,
            pv_rule: 0xFF030303,
            pv_signal: 0xFF040404,
        }
    }

    fn sheet() -> Sheet {
        sheet_for(&Bundle::builtin(Profile::Instrument), 100, crate::layout::TEST_DISPLAY_W)
    }

    #[test]
    fn build_groups_and_ranks_then_id() {
        let p = Picker::build(
            alloc::vec![
                theme("logic", "Warm Logic", Some(Group::Light), 2),
                theme("signal", "Signal Amber", Some(Group::Dark), 1),
                theme("carbon", "Carbon Optics", Some(Group::Dark), 2),
                theme("zzz", "Ungrouped", None, 0),
                theme("genera", "Genera Ivory", Some(Group::Light), 1),
            ],
            "carbon",
        );
        let ids: Vec<&str> = p.options.iter().map(|t| t.id.as_str()).collect();
        // Dark (rank 1,2), Light (rank 1,2), then OTHER.
        assert_eq!(ids, alloc::vec!["signal", "carbon", "genera", "logic", "zzz"]);
        assert_eq!(p.current, Some(1), "carbon is the checked row");
        assert_eq!(p.sel, 1, "opening focuses the current theme");
    }

    #[test]
    fn a_tie_breaks_on_id() {
        let p = Picker::build(
            alloc::vec![
                theme("b", "B", Some(Group::Dark), 5),
                theme("a", "A", Some(Group::Dark), 5),
            ],
            "missing",
        );
        assert_eq!(p.options[0].id, "a");
        assert_eq!(p.current, None, "a theme not in the gallery has no check");
        assert_eq!(p.sel, 0, "no current -> the first row");
    }

    #[test]
    fn keys_wrap_and_commit() {
        let mut p = Picker::build(
            alloc::vec![
                theme("a", "A", Some(Group::Dark), 0),
                theme("b", "B", Some(Group::Dark), 1),
                theme("c", "C", Some(Group::Dark), 2),
            ],
            "a",
        );
        assert_eq!(p.sel, 0);
        assert_eq!(p.key(PickerKey::Up), None);
        assert_eq!(p.sel, 2, "Up from the first wraps to the last");
        assert_eq!(p.key(PickerKey::Down), None);
        assert_eq!(p.sel, 0, "Down from the last wraps to the first");
        p.key(PickerKey::End);
        assert_eq!(p.sel, 2);
        p.key(PickerKey::Home);
        assert_eq!(p.sel, 0);
        p.key(PickerKey::Down);
        assert_eq!(p.key(PickerKey::Commit).as_deref(), Some("b"), "Commit yields the selected id");
    }

    #[test]
    fn space_and_enter_both_commit() {
        assert_eq!(picker_key(0, 0x20), PickerKey::Commit, "space rune commits");
        assert_eq!(picker_key(57, 0), PickerKey::Commit, "space code commits");
        assert_eq!(picker_key(28, 0), PickerKey::Commit, "enter commits");
        assert_eq!(picker_key(103, 0), PickerKey::Up);
        assert_eq!(picker_key(0, 0x6a), PickerKey::Down);
    }

    #[test]
    fn the_size_is_286_wide_and_clamped_tall() {
        let s = sheet();
        let p = Picker::build(alloc::vec![theme("a", "A", Some(Group::Dark), 0)], "a");
        let (w, h) = picker_size(&p, &s, 10_000);
        assert_eq!(w, s.ipx(WIDTH) as u32, "286 logical, scaled");
        // heading + one group + one option + two pads.
        let want = s.ipx(HEADING_H) + s.ipx(GROUP_H) + s.ipx(OPTION_H) + 2 * s.ipx(PAD);
        assert_eq!(h, want as u32);
        // Clamped by max_h.
        let (_, h2) = picker_size(&p, &s, 40);
        assert_eq!(h2, 40, "clamped to the display margin");
    }

    #[test]
    fn the_painter_shows_the_grounds_the_check_and_each_theme_s_own_colours() {
        let s = sheet();
        let mut gs = crate::raster::GlyphSource::new_vendored(512);
        let mut a = theme("a", "A", Some(Group::Dark), 0);
        a.pv_signal = 0xFF00FF00;
        let mut b = theme("b", "B", Some(Group::Light), 0);
        b.pv_signal = 0xFF0000FF;
        let p = Picker::build(alloc::vec![a, b], "b");
        let (w, h) = picker_size(&p, &s, 10_000);
        let cart = picker_list(&p, w, h, &s, &mut gs);
        let has_rect = |c: u32| cart.ops.iter().any(|op| matches!(op, Op::Rect { color, .. } if *color == c));
        let cleared = cart.ops.iter().any(|op| matches!(op, Op::Clear { color } if *color == s.inst.pane));
        assert!(cleared, "the ground is `pane`");
        assert!(has_rect(s.inst.structure), "the frame is `structure`");
        assert!(has_rect(s.inst.header), "a group row rests on `header`");
        assert!(has_rect(s.inst.hover), "the selected row is on `hover`");
        assert!(has_rect(0xFF00FF00), "theme a's own signal colour");
        assert!(has_rect(0xFF0000FF), "theme b's own signal colour");
        // The check is a glyph run in `amber`.
        let amber_glyphs = cart.ops.iter().any(|op| matches!(op, Op::Glyphs { color, .. } if *color == s.inst.amber));
        assert!(amber_glyphs, "the current row's check is amber");
    }

    #[test]
    fn scroll_keeps_the_selection_visible() {
        let s = sheet();
        let mut opts = Vec::new();
        for k in 0..40u8 {
            let mut id = String::from("t");
            let _ = core::fmt::write(&mut id, format_args!("{:02}", k));
            opts.push(theme(&id, "T", Some(Group::Dark), k));
        }
        let mut p = Picker::build(opts, "t00");
        let (rl, content) = rows(&p, &s);
        let view = 300;
        // At the top, no scroll.
        assert_eq!(scroll_top(&rl, 0, view, content), 0);
        // At the end, the selected row is within the viewport.
        p.sel = 39;
        let top = scroll_top(&rl, 39, view, content);
        let (y0, rh) = rl.iter().find_map(|(r, y, rh)| match r {
            PRow::Option(i) if *i == 39 => Some((*y, *rh)),
            _ => None,
        }).unwrap();
        assert!(y0 >= top && y0 + rh <= top + view, "the selected row is fully visible");
        assert!(top <= content - view, "never scrolled past the end");
    }
}
