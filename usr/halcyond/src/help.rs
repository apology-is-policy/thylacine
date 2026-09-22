// help -- the keyboard reference's model and painter (HALCYON-INSTRUMENT 9.5
// + the kit's section 9 "Help dialog", I-7b): the FOURTH model `menuset`
// carries on the ONE Role::Menu surface, so the compositor's grab, its Esc
// and its click-away are exactly H-3c's. This file thinks; it never
// syscalls.
//
// Its frame is NOT the 14.5 dialog family's, which is why it is its own
// slice and its own file: 540 wide against 480, a header carrying a close x
// against an eyebrow/title pair alone, a two-column key grid against a
// wrapped paragraph, and a footer paragraph against a button row.
//
// THE ROWS NAME OUR CHORDS. They are derived from the compositor's `chords`
// file (`super+[shift+]<key> <action>`, one binding per line -- the same
// text `rail::hints_from_chords` reads for the footer hints), never from a
// literal: a rebind is a rebind of the reference, and an action with no
// binding has no row. The four-arrow focus and move sets collapse to one
// ARROWS row each exactly when all four are bound to the four arrow keys,
// the footer hint's rule.

use alloc::format;
use alloc::string::String;
use alloc::vec::Vec;

use cartoon::{Cartoon, Op};
use libhalcyon::theme::Argb;

use crate::layout::Sheet;
use crate::raster::GlyphSource;

/// The card's width (the kit's `.help-card`), logical px.
pub const MAX_W: i32 = 540;
/// The viewport margin the card never crosses (`100vw - 32`).
pub const VIEW_MARGIN: i32 = 32;
/// The header's minimum height and its 17 / 18 / 15 / 22 padding.
const HEAD_MIN_H: i32 = 78;
const HEAD_PAD_TOP: i32 = 17;
const HEAD_PAD_RIGHT: i32 = 18;
const HEAD_PAD_BOT: i32 = 15;
const HEAD_PAD_LEFT: i32 = 22;
/// The title's margin above (the `h1`'s margin-top).
const TITLE_GAP: i32 = 7;
/// The close control: 29 x 29, a 1 px `structure` border, the x at 20.
const CLOSE_BOX: i32 = 29;
/// The key list's padding (12 / 22) and each row's 10 px vertical padding.
const LIST_PAD_Y: i32 = 12;
const LIST_PAD_X: i32 = 22;
const ROW_PAD_Y: i32 = 10;
/// The row grid: 190 | rest, gap 18.
const KEYS_COL: i32 = 190;
const GRID_GAP: i32 = 18;
/// A key cap: min-width 26, padding 4 / 6.
const KEY_MIN_W: i32 = 26;
const KEY_PAD_X: i32 = 6;
const KEY_PAD_Y: i32 = 4;
/// The ` + ` between two caps (the reference's literal text node).
const CAP_JOIN: &str = " + ";
/// The footer paragraph: padding 15 / 22 / 20, line-height 1.55.
const FOOT_PAD_TOP: i32 = 15;
const FOOT_PAD_X: i32 = 22;
const FOOT_PAD_BOT: i32 = 20;
const FOOT_LH: f32 = 1.55;
/// One wheel notch / arrow press of scroll, logical px (a row is ~38).
const SCROLL_STEP: i32 = 40;

const EYEBROW_PX: f32 = 10.0;
const EYEBROW_TRACK: f32 = 0.12;
const TITLE_PX: f32 = 23.0;
const KEY_PX: f32 = 10.0;
const DESC_PX: f32 = 13.0;
const FOOT_PX: f32 = 13.0;
const CLOSE_PX: f32 = 20.0;

/// The card's fixed words (the reference's own).
pub const EYEBROW: &str = "CONTROL REFERENCE";
pub const TITLE: &str = "Workspace keys";
/// The explanatory paragraph -- OURS, and true of this system: the divider
/// drag and its double-click are I-6's, Escape's three jobs are the
/// compositor's, and the one-open-tile rule is 6.2's.
pub const FOOTER: &str = "Drag a divider to resize a pair; double-click it to restore the even split. \
Escape ends a drag, dismisses a menu, and closes this reference. Every stack keeps exactly one tile open.";

/// Every action the reference can name, in DISPLAY order, with the words we
/// use for it. An action absent from the `chords` file contributes no row.
const ACTION_ROWS: &[(&str, &str)] = &[
    ("focus-left", "Focus the pane to the left"),
    ("focus-right", "Focus the pane to the right"),
    ("focus-up", "Focus the pane above"),
    ("focus-down", "Focus the pane below"),
    ("move-left", "Move the focused tile left"),
    ("move-right", "Move the focused tile right"),
    ("move-up", "Move the focused tile up"),
    ("move-down", "Move the focused tile down"),
    ("split-h", "Split the focused pane horizontally"),
    ("split-v", "Split the focused pane vertically"),
    ("split-toggle", "Flip the split's orientation"),
    ("new-tile", "Open a new tile in the focused pane"),
    ("zoom", "Zoom the focused tile to the workspace"),
    ("tab", "Show the stack as tabs"),
    ("stack", "Show the stack's headers"),
    ("cycle", "Open the next tile"),
    ("cycle-back", "Open the previous tile"),
    ("close", "Close the focused tile"),
    ("picker", "Choose a display theme"),
    ("help", "Show this reference"),
    ("scale-up", "Enlarge the display"),
    ("scale-down", "Reduce the display"),
    ("scale-reset", "Restore the measured display size"),
];

/// One row: the key caps in order, and what the binding does.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct HelpRow {
    pub caps: Vec<String>,
    pub desc: String,
}

/// The open reference.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Help {
    pub eyebrow: String,
    pub title: String,
    pub rows: Vec<HelpRow>,
    pub footer: String,
    /// The body's scroll offset in px, already clamped by whoever moved it.
    pub scroll: i32,
    /// The pointer is over the close control.
    pub close_hover: bool,
}

impl Help {
    /// Build the reference from the compositor's `chords` text.
    pub fn from_chords(text: &str) -> Help {
        Help {
            eyebrow: String::from(EYEBROW),
            title: String::from(TITLE),
            rows: rows_from_chords(text),
            footer: String::from(FOOTER),
            scroll: 0,
            close_hover: false,
        }
    }
}

/// The `chords` file's bindings: `super+[shift+]<key> <action>`, one per
/// line, at most `rail::CHORD_LINES_MAX`. A malformed line is skipped.
fn parse(text: &str) -> Vec<(&str, &str)> {
    text.lines()
        .take(crate::rail::CHORD_LINES_MAX)
        .filter_map(|l| {
            let mut it = l.split_ascii_whitespace();
            let combo = it.next()?;
            let action = it.next()?;
            if it.next().is_some() || !combo.starts_with("super+") {
                return None;
            }
            Some((combo, action))
        })
        .collect()
}

/// One combo token as a key cap: the grammar's punctuation names become the
/// glyph they stand for, everything else is uppercased. The arrows are NOT
/// spelled with arrow glyphs -- the mono subset carries none (its extras are
/// the lambda, the check, the guillemets, the minus, the command mark and
/// the box set) -- so the reference says LEFT / RIGHT / UP / DOWN, and the
/// four together say ARROWS, exactly as the footer hint does.
fn key_cap(tok: &str) -> String {
    match tok {
        "slash" => String::from("/"),
        "minus" => String::from("-"),
        "equal" => String::from("="),
        _ => tok.to_uppercase(),
    }
}

fn cap_list(combo: &str) -> Vec<String> {
    combo.split('+').map(key_cap).collect()
}

/// The rows for the bindings in force (8.2's rule, widened from the footer's
/// two hints to the whole vocabulary).
pub fn rows_from_chords(text: &str) -> Vec<HelpRow> {
    let binds = parse(text);
    let bound = |combo: &str, action: &str| binds.iter().any(|b| b.0 == combo && b.1 == action);
    let mut used: Vec<String> = Vec::new();
    let mut out: Vec<HelpRow> = Vec::new();
    // The two four-arrow groups, exactly when all four are on the arrows.
    for (prefix, shift, desc) in [
        ("focus", false, "Focus a neighbouring pane"),
        ("move", true, "Move the focused tile"),
    ] {
        let all_four = ["left", "right", "up", "down"].iter().all(|d| {
            let combo = if shift {
                format!("super+shift+{}", d)
            } else {
                format!("super+{}", d)
            };
            bound(&combo, &format!("{}-{}", prefix, d))
        });
        if !all_four {
            continue;
        }
        let mut caps = alloc::vec![String::from("SUPER")];
        if shift {
            caps.push(String::from("SHIFT"));
        }
        caps.push(String::from("ARROWS"));
        out.push(HelpRow {
            caps,
            desc: String::from(desc),
        });
        for d in ["left", "right", "up", "down"] {
            used.push(format!("{}-{}", prefix, d));
        }
    }
    for (action, desc) in ACTION_ROWS {
        if used.iter().any(|u| u == action) {
            continue;
        }
        let Some(combo) = binds.iter().find(|b| b.1 == *action).map(|b| b.0) else {
            continue;
        };
        out.push(HelpRow {
            caps: cap_list(combo),
            desc: String::from(*desc),
        });
    }
    out
}

/// A key on the reference surface.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum HelpKey {
    /// Dismiss (the close control's keyboard twin). Escape is the
    /// compositor's own dismiss and never routes here.
    Close,
    Up,
    Down,
    Home,
    End,
    None,
}

/// Map a key press (rune first, then evdev code) to a reference key: Enter
/// and Space dismiss like the close control, the arrows and j/k scroll,
/// Home/End jump. Escape is the compositor's.
pub fn help_key(code: u16, rune: u32) -> HelpKey {
    match rune {
        0x0d | 0x0a | 0x20 => return HelpKey::Close,
        0x6b => return HelpKey::Up,   // k
        0x6a => return HelpKey::Down, // j
        _ => {}
    }
    match code {
        28 | 96 | 57 => HelpKey::Close, // Enter / KP-Enter / Space
        103 => HelpKey::Up,
        108 => HelpKey::Down,
        102 => HelpKey::Home,
        107 => HelpKey::End,
        _ => HelpKey::None,
    }
}

impl Help {
    /// Apply a key. `Close` yields Some(()) -- the caller dismisses; the
    /// movers scroll the body within its bounds and yield None (a repaint).
    pub fn key(&mut self, k: HelpKey, w: u32, h: u32, sheet: &Sheet, gs: &mut GlyphSource) -> Option<()> {
        match k {
            HelpKey::Close => Some(()),
            HelpKey::Up => {
                self.scroll_by(-sheet.ipx(SCROLL_STEP), w, h, sheet, gs);
                None
            }
            HelpKey::Down => {
                self.scroll_by(sheet.ipx(SCROLL_STEP), w, h, sheet, gs);
                None
            }
            HelpKey::Home => {
                self.scroll = 0;
                None
            }
            HelpKey::End => {
                self.scroll = scroll_max(self, w, h, sheet, gs);
                None
            }
            HelpKey::None => None,
        }
    }

    /// A wheel delta (REL_WHEEL: +1 = up) scrolls the body, clamped.
    pub fn wheel(&mut self, delta: i32, w: u32, h: u32, sheet: &Sheet, gs: &mut GlyphSource) {
        self.scroll_by(-delta * sheet.ipx(SCROLL_STEP), w, h, sheet, gs);
    }

    fn scroll_by(&mut self, dy: i32, w: u32, h: u32, sheet: &Sheet, gs: &mut GlyphSource) {
        let max = scroll_max(self, w, h, sheet, gs);
        self.scroll = (self.scroll + dy).clamp(0, max);
    }

    /// Is the pointer over the close control (surface coords)?
    pub fn close_at(&self, x: i32, y: i32, w: u32, sheet: &Sheet) -> bool {
        let (cx, cy, cw, ch) = close_rect(w, sheet);
        x >= cx && x < cx + cw && y >= cy && y < cy + ch
    }
}

/// The close control's rect: the header's top-right, inside its padding.
fn close_rect(w: u32, sheet: &Sheet) -> (i32, i32, i32, i32) {
    let box_w = sheet.ipx(CLOSE_BOX);
    let x = w as i32 - sheet.ipx(HEAD_PAD_RIGHT) - box_w;
    (x.max(0), sheet.ipx(HEAD_PAD_TOP), box_w, box_w)
}

fn line_h(gs: &mut GlyphSource, face: u8, px: f32) -> i32 {
    gs.line_metrics(face, px)
        .map(|m| m.ascent + m.descent)
        .unwrap_or(px as i32)
}

/// The fixed header's height. The 78 is a BORDER-BOX minimum (the reference
/// sets `* { box-sizing: border-box }`), so it covers the two paddings AND
/// the 1 px `structure` rule below the header -- which is why the hairline
/// is inside the max, not added after it.
fn head_h(sheet: &Sheet, gs: &mut GlyphSource) -> i32 {
    let inner = sheet.ipx(HEAD_PAD_TOP)
        + line_h(gs, sheet.face_mono_text, sheet.px(EYEBROW_PX))
        + sheet.ipx(TITLE_GAP)
        + line_h(gs, sheet.face_medium, sheet.px(TITLE_PX))
        + sheet.ipx(HEAD_PAD_BOT)
        + sheet.hairline;
    inner.max(sheet.ipx(HEAD_MIN_H))
}

/// Every row is one line tall, so they share a height: the taller of a key
/// cap and the description line, plus the 10 px pads and the separator.
fn row_h(sheet: &Sheet, gs: &mut GlyphSource) -> i32 {
    let cap = 2 * sheet.ipx(KEY_PAD_Y) + line_h(gs, sheet.face_mono_text, sheet.px(KEY_PX));
    let desc = line_h(gs, sheet.face_body, sheet.px(DESC_PX));
    2 * sheet.ipx(ROW_PAD_Y) + cap.max(desc) + sheet.hairline
}

/// A greedy word-wrap of the footer paragraph at `width`.
fn wrap(gs: &mut GlyphSource, face: u8, px: f32, text: &str, width: i32) -> Vec<String> {
    let mut lines: Vec<String> = Vec::new();
    for para in text.split('\n') {
        let mut cur = String::new();
        for word in para.split(' ') {
            if word.is_empty() {
                continue;
            }
            let cand = if cur.is_empty() {
                String::from(word)
            } else {
                let mut c = cur.clone();
                c.push(' ');
                c.push_str(word);
                c
            };
            let (_, w) = gs.shape_run(face, px, cand.chars());
            if w > width && !cur.is_empty() {
                lines.push(core::mem::take(&mut cur));
                cur = String::from(word);
            } else {
                cur = cand;
            }
        }
        lines.push(cur);
    }
    lines
}

fn foot_lines(h: &Help, w: u32, sheet: &Sheet, gs: &mut GlyphSource) -> Vec<String> {
    let avail = w as i32 - 2 * sheet.ipx(FOOT_PAD_X);
    wrap(gs, sheet.face_body, sheet.px(FOOT_PX), &h.footer, avail)
}

/// The scrolling body's full height (the key list + the footer paragraph).
fn body_h(h: &Help, w: u32, sheet: &Sheet, gs: &mut GlyphSource) -> i32 {
    let list = 2 * sheet.ipx(LIST_PAD_Y) + h.rows.len() as i32 * row_h(sheet, gs);
    let lh = (sheet.px(FOOT_PX) * FOOT_LH) as i32;
    let foot = sheet.ipx(FOOT_PAD_TOP) + foot_lines(h, w, sheet, gs).len() as i32 * lh + sheet.ipx(FOOT_PAD_BOT);
    list + foot
}

/// The largest legal scroll offset for this surface: 0 when the body fits.
/// The viewport floors at 0 -- a surface shorter than its own header would
/// otherwise make the bound LARGER than the body, letting the card scroll
/// its whole content out of sight.
fn scroll_max(h: &Help, w: u32, surf_h: u32, sheet: &Sheet, gs: &mut GlyphSource) -> i32 {
    let view = (surf_h as i32 - head_h(sheet, gs)).max(0);
    (body_h(h, w, sheet, gs) - view).max(0)
}

/// The card's surface size (9.5): min(540, display - 32) wide; the header +
/// the key rows + the footer tall, clamped to display - 32. A clamped card
/// scrolls its body under the fixed header.
pub fn help_size(h: &Help, sheet: &Sheet, display_w: u32, display_h: u32, gs: &mut GlyphSource) -> (u32, u32) {
    gs.set_kerning(sheet.kerning);
    let w = sheet
        .ipx(MAX_W)
        .min(display_w as i32 - sheet.ipx(VIEW_MARGIN))
        .max(1) as u32;
    let full = head_h(sheet, gs) + body_h(h, w, sheet, gs);
    let hgt = full
        .min(display_h as i32 - sheet.ipx(VIEW_MARGIN))
        .max(1) as u32;
    (w, hgt)
}

/// The display list (7.3's help row): `dialog_bg` ground with a 1 px
/// `focus_neutral` frame; the `amber` eyebrow and `text` title; the key caps
/// on `kbd_bg` inside a `focus_neutral` frame; `secondary` descriptions; a
/// `separator` under each row and a `structure` rule under the header.
///
/// The body is painted FIRST and the header over it, so a scrolled row can
/// never show through the fixed header (an ordered display list is the clip
/// we do not otherwise have); the outer frame goes on last.
pub fn help_list(h: &Help, w: u32, hgt: u32, sheet: &Sheet, gs: &mut GlyphSource) -> Cartoon {
    gs.set_kerning(sheet.kerning);
    let i = &sheet.inst;
    let mut cart = Cartoon::new();
    if w == 0 || hgt == 0 {
        return cart;
    }
    let (wi, hi) = (w as i32, hgt as i32);
    let hair = sheet.hairline;
    let hu = hair as u32;
    let gen = gs.gen();
    cart.ops.push(Op::Clear { color: i.dialog_bg });

    let hh = head_h(sheet, gs);
    let top = h.scroll.clamp(0, scroll_max(h, w, hgt, sheet, gs));
    let rh = row_h(sheet, gs);
    let cap_h = 2 * sheet.ipx(KEY_PAD_Y) + line_h(gs, sheet.face_mono_text, sheet.px(KEY_PX));

    // --- the scrolling body: the key rows, then the footer paragraph.
    let list_x = sheet.ipx(LIST_PAD_X);
    let desc_x = list_x + sheet.ipx(KEYS_COL) + sheet.ipx(GRID_GAP);
    let desc_w = (wi - sheet.ipx(LIST_PAD_X) - desc_x).max(0);
    let mut y = hh + sheet.ipx(LIST_PAD_Y) - top;
    for row in &h.rows {
        if y + rh > 0 && y < hi {
            let inner = rh - 2 * sheet.ipx(ROW_PAD_Y) - hair;
            let cy = y + sheet.ipx(ROW_PAD_Y) + (inner - cap_h) / 2;
            // The caps, left to right, joined by ` + `, and never crossing
            // into the description's column: no default binding comes close
            // (the worst is ~166 of 190), but a rebind to a longer key name
            // must not collide with the text beside it. The FIRST cap always
            // draws -- an empty key column would be a worse lie than a wide
            // one -- so only a continuation is dropped.
            let caps_right = list_x + sheet.ipx(KEYS_COL);
            let mut x = list_x;
            for (n, cap) in row.caps.iter().enumerate() {
                let (krefs, kw) = gs.shape_run(sheet.face_mono_text, sheet.px(KEY_PX), cap.chars());
                let box_w = (kw + 2 * sheet.ipx(KEY_PAD_X)).max(sheet.ipx(KEY_MIN_W));
                let jw = if n > 0 {
                    gs.shape_run(sheet.face_body, sheet.px(DESC_PX), CAP_JOIN.chars()).1
                } else {
                    0
                };
                if n > 0 && x + jw + box_w > caps_right {
                    break;
                }
                if n > 0 {
                    let (jrefs, _) = gs.shape_run(sheet.face_body, sheet.px(DESC_PX), CAP_JOIN.chars());
                    if !jrefs.is_empty() {
                        let base = y + sheet.ipx(ROW_PAD_Y) + centre_base(gs, sheet.face_body, sheet.px(DESC_PX), inner);
                        cart.push_glyphs(gen, x, base, i.text, &jrefs);
                    }
                    x += jw;
                }
                rect(&mut cart, x, cy, box_w, cap_h, i.kbd_bg);
                ring(&mut cart, x, cy, box_w, cap_h, hair, i.focus_neutral);
                if !krefs.is_empty() {
                    let base = cy + centre_base(gs, sheet.face_mono_text, sheet.px(KEY_PX), cap_h);
                    cart.push_glyphs(gen, x + (box_w - kw) / 2, base, i.text, &krefs);
                }
                x += box_w;
            }
            // The description, elided to its column.
            let text = crate::chrome::fit_end_pub(gs, sheet.face_body, sheet.px(DESC_PX), &row.desc, desc_w);
            let (drefs, _) = gs.shape_run(sheet.face_body, sheet.px(DESC_PX), text.chars());
            if !drefs.is_empty() {
                let base = y + sheet.ipx(ROW_PAD_Y) + centre_base(gs, sheet.face_body, sheet.px(DESC_PX), inner);
                cart.push_glyphs(gen, desc_x, base, i.secondary, &drefs);
            }
            rect(&mut cart, list_x, y + rh - hair, (wi - 2 * list_x).max(0), hair, i.separator);
        }
        y += rh;
    }
    // The footer paragraph, below the list's bottom padding.
    let lh = (sheet.px(FOOT_PX) * FOOT_LH) as i32;
    let asc = gs
        .line_metrics(sheet.face_body, sheet.px(FOOT_PX))
        .map(|m| m.ascent)
        .unwrap_or(10);
    let mut fy = y + sheet.ipx(LIST_PAD_Y) + sheet.ipx(FOOT_PAD_TOP);
    for line in foot_lines(h, w, sheet, gs) {
        if fy + lh > 0 && fy < hi {
            let (refs, _) = gs.shape_run(sheet.face_body, sheet.px(FOOT_PX), line.chars());
            if !refs.is_empty() {
                cart.push_glyphs(gen, sheet.ipx(FOOT_PAD_X), fy + asc, i.secondary, &refs);
            }
        }
        fy += lh;
    }

    // --- the fixed header, OVER the body.
    rect(&mut cart, 0, 0, wi, hh, i.dialog_bg);
    let ex = sheet.ipx(HEAD_PAD_LEFT);
    let ey = sheet.ipx(HEAD_PAD_TOP)
        + gs.line_metrics(sheet.face_mono_text, sheet.px(EYEBROW_PX))
            .map(|m| m.ascent)
            .unwrap_or(8);
    let (erefs, _) = gs.shape_run_spaced(
        sheet.face_mono_text,
        sheet.px(EYEBROW_PX),
        EYEBROW_TRACK * sheet.px(EYEBROW_PX),
        h.eyebrow.chars(),
    );
    if !erefs.is_empty() {
        cart.push_glyphs(gen, ex, ey, i.amber, &erefs);
    }
    let ty = sheet.ipx(HEAD_PAD_TOP)
        + line_h(gs, sheet.face_mono_text, sheet.px(EYEBROW_PX))
        + sheet.ipx(TITLE_GAP)
        + gs.line_metrics(sheet.face_medium, sheet.px(TITLE_PX))
            .map(|m| m.ascent)
            .unwrap_or(18);
    let (trefs, _) = gs.shape_run(sheet.face_medium, sheet.px(TITLE_PX), h.title.chars());
    if !trefs.is_empty() {
        cart.push_glyphs(gen, ex, ty, i.text, &trefs);
    }
    // The close control: a 29 x 29 `structure` box (hovered `amber_muted`)
    // with the same multiplication sign the header's close mark uses.
    let (cx, cy, cw, ch) = close_rect(w, sheet);
    let border = if h.close_hover { i.amber_muted } else { i.structure };
    ring(&mut cart, cx, cy, cw, ch, hair, border);
    let (xrefs, xw) = gs.shape_run(sheet.face_body, sheet.px(CLOSE_PX), "\u{d7}".chars());
    if !xrefs.is_empty() {
        let ink = if h.close_hover { i.text } else { i.secondary };
        let base = cy + centre_base(gs, sheet.face_body, sheet.px(CLOSE_PX), ch);
        cart.push_glyphs(gen, cx + (cw - xw) / 2, base, ink, &xrefs);
    }
    rect(&mut cart, 0, hh - hair, wi, hair, i.structure);

    // --- the 1 px `focus_neutral` frame, over everything.
    for r in [
        (0, 0, w, hu),
        (0, (hi - hair).max(0), w, hu),
        (0, 0, hu, hgt),
        ((wi - hair).max(0), 0, hu, hgt),
    ] {
        cart.ops.push(Op::Rect {
            x: r.0,
            y: r.1,
            w: r.2,
            h: r.3,
            color: i.focus_neutral,
        });
    }
    cart
}

fn centre_base(gs: &mut GlyphSource, face: u8, px: f32, box_h: i32) -> i32 {
    let (asc, desc) = gs
        .line_metrics(face, px)
        .map(|m| (m.ascent, m.descent))
        .unwrap_or((8, 2));
    (box_h - (asc + desc)) / 2 + asc
}

fn rect(cart: &mut Cartoon, x: i32, y: i32, w: i32, h: i32, color: Argb) {
    if w > 0 && h > 0 {
        cart.ops.push(Op::Rect {
            x,
            y,
            w: w as u32,
            h: h as u32,
            color,
        });
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

    /// The compositor's default table as `Chords::render` writes it (the
    /// SHAPE of the file, not a claim about its content -- the production
    /// rows come from whatever the compositor published).
    const DEFAULTS: &str = "\
super+left focus-left
super+right focus-right
super+up focus-up
super+down focus-down
super+shift+left move-left
super+shift+right move-right
super+shift+up move-up
super+shift+down move-down
super+h split-h
super+v split-v
super+f zoom
super+t picker
super+shift+t tab
super+s stack
super+slash help
super+e split-toggle
super+tab cycle
super+shift+tab cycle-back
super+shift+q close
super+equal scale-up
super+minus scale-down
super+0 scale-reset
";

    fn sheet() -> Sheet {
        sheet_for(&Bundle::builtin(Profile::Instrument), 100, crate::layout::TEST_DISPLAY_W)
    }

    fn row(h: &Help, desc: &str) -> Option<HelpRow> {
        h.rows.iter().find(|r| r.desc == desc).cloned()
    }

    #[test]
    fn the_rows_name_the_bindings_in_force_not_a_vocabulary() {
        let h = Help::from_chords(DEFAULTS);
        // The four-arrow sets collapse to one ARROWS row each.
        let f = row(&h, "Focus a neighbouring pane").expect("the focus group");
        assert_eq!(f.caps, alloc::vec!["SUPER", "ARROWS"]);
        let m = row(&h, "Move the focused tile").expect("the move group");
        assert_eq!(m.caps, alloc::vec!["SUPER", "SHIFT", "ARROWS"]);
        // No per-direction row survives the group.
        assert!(row(&h, "Focus the pane to the left").is_none());
        // The punctuation keys read as their glyph, not their grammar name.
        assert_eq!(row(&h, "Show this reference").unwrap().caps, alloc::vec!["SUPER", "/"]);
        assert_eq!(row(&h, "Enlarge the display").unwrap().caps, alloc::vec!["SUPER", "="]);
        assert_eq!(row(&h, "Reduce the display").unwrap().caps, alloc::vec!["SUPER", "-"]);
        // Shift is its own cap, in order.
        assert_eq!(row(&h, "Close the focused tile").unwrap().caps, alloc::vec!["SUPER", "SHIFT", "Q"]);
        assert_eq!(row(&h, "Choose a display theme").unwrap().caps, alloc::vec!["SUPER", "T"]);
        // Two groups + every other default binding, one row each.
        assert_eq!(h.rows.len(), 2 + 14);
    }

    #[test]
    fn a_rebind_is_a_rebind_of_the_reference_and_an_unbound_action_has_no_row() {
        // `zoom` moved off F onto G; `picker` unbound entirely.
        let text = "super+g zoom\nsuper+slash help\n";
        let h = Help::from_chords(text);
        assert_eq!(row(&h, "Zoom the focused tile to the workspace").unwrap().caps, alloc::vec!["SUPER", "G"]);
        assert!(row(&h, "Choose a display theme").is_none(), "an unbound action has no row");
        // A partial arrow set does NOT collapse: the bound ones read alone.
        let part = "super+left focus-left\nsuper+up focus-up\n";
        let p = Help::from_chords(part);
        assert!(row(&p, "Focus a neighbouring pane").is_none());
        assert_eq!(row(&p, "Focus the pane to the left").unwrap().caps, alloc::vec!["SUPER", "LEFT"]);
        assert_eq!(row(&p, "Focus the pane above").unwrap().caps, alloc::vec!["SUPER", "UP"]);
        assert_eq!(p.rows.len(), 2);
    }

    #[test]
    fn a_malformed_file_yields_rows_for_only_its_well_formed_lines() {
        let text = "\ngarbage\nctrl+f zoom\nsuper+f\nsuper+f zoom extra\nsuper+f zoom\n";
        let h = Help::from_chords(text);
        assert_eq!(h.rows.len(), 1, "only the last line is a binding");
        assert_eq!(h.rows[0].caps, alloc::vec!["SUPER", "F"]);
        // An empty file is an empty reference, never a panic.
        assert!(Help::from_chords("").rows.is_empty());
        // The line cap holds.
        let mut many = String::new();
        for _ in 0..(crate::rail::CHORD_LINES_MAX + 50) {
            many.push_str("super+z none\n");
        }
        assert!(Help::from_chords(&many).rows.is_empty(), "`none` names no action row");
    }

    #[test]
    fn the_card_is_540_wide_and_clamped_to_the_viewport() {
        let s = sheet();
        let mut gs = crate::raster::GlyphSource::new_vendored(512);
        let h = Help::from_chords(DEFAULTS);
        let (w, hgt) = help_size(&h, &s, 1440, 900, &mut gs);
        assert_eq!(w, s.ipx(MAX_W) as u32, "the mockup's 540, scaled");
        assert!(hgt <= 900 - s.ipx(VIEW_MARGIN) as u32);
        // A narrow display takes the viewport - 32 branch.
        let (nw, _) = help_size(&h, &s, 400, 900, &mut gs);
        assert_eq!(nw, (400 - s.ipx(VIEW_MARGIN)) as u32);
        // A short display clamps the height rather than growing the surface.
        let (_, sh) = help_size(&h, &s, 1440, 300, &mut gs);
        assert_eq!(sh, (300 - s.ipx(VIEW_MARGIN)) as u32);
    }

    #[test]
    fn the_painter_shows_the_ground_the_frame_the_caps_and_the_close() {
        let s = sheet();
        let mut gs = crate::raster::GlyphSource::new_vendored(512);
        let h = Help::from_chords(DEFAULTS);
        let (w, hgt) = help_size(&h, &s, 1440, 900, &mut gs);
        let cart = help_list(&h, w, hgt, &s, &mut gs);
        let has_rect = |c: u32| cart.ops.iter().any(|op| matches!(op, Op::Rect { color, .. } if *color == c));
        assert!(
            cart.ops.iter().any(|op| matches!(op, Op::Clear { color } if *color == s.inst.dialog_bg)),
            "the ground is dialog_bg"
        );
        assert!(has_rect(s.inst.focus_neutral), "the 1 px frame and the cap frames");
        assert!(has_rect(s.inst.kbd_bg), "the key caps rest on kbd_bg");
        assert!(has_rect(s.inst.separator), "a separator under each row");
        assert!(has_rect(s.inst.structure), "the header rule and the close box");
        assert!(
            cart.ops.iter().any(|op| matches!(op, Op::Glyphs { color, .. } if *color == s.inst.amber)),
            "the eyebrow is amber"
        );
        assert!(
            cart.ops.iter().any(|op| matches!(op, Op::Glyphs { color, .. } if *color == s.inst.secondary)),
            "the descriptions and the footer are secondary"
        );
        // The hovered close control swaps its border and its ink.
        let mut hov = h.clone();
        hov.close_hover = true;
        let c2 = help_list(&hov, w, hgt, &s, &mut gs);
        assert!(
            c2.ops.iter().any(|op| matches!(op, Op::Rect { color, .. } if *color == s.inst.amber_muted)),
            "the hovered close box borders amber_muted"
        );
    }

    #[test]
    fn the_close_control_sits_inside_the_header_s_right_padding() {
        let s = sheet();
        let mut gs = crate::raster::GlyphSource::new_vendored(512);
        let h = Help::from_chords(DEFAULTS);
        let (w, _) = help_size(&h, &s, 1440, 900, &mut gs);
        let (cx, cy, cw, ch) = close_rect(w, &s);
        assert_eq!(cw, s.ipx(CLOSE_BOX));
        assert_eq!(ch, s.ipx(CLOSE_BOX));
        assert_eq!(cx + cw, w as i32 - s.ipx(HEAD_PAD_RIGHT));
        assert_eq!(cy, s.ipx(HEAD_PAD_TOP));
        assert!(h.close_at(cx + 2, cy + 2, w, &s));
        assert!(!h.close_at(cx - 4, cy + 2, w, &s), "left of the box is not the box");
        assert!(!h.close_at(cx + 2, cy + ch + 4, w, &s), "below the box is not the box");
    }

    #[test]
    fn the_body_scrolls_only_while_it_overflows_and_clamps_at_both_ends() {
        let s = sheet();
        let mut gs = crate::raster::GlyphSource::new_vendored(512);
        let mut h = Help::from_chords(DEFAULTS);
        // Tall enough for everything: no scroll is possible.
        let (w, tall) = help_size(&h, &s, 1440, 2000, &mut gs);
        h.wheel(-3, w, tall, &s, &mut gs);
        assert_eq!(h.scroll, 0, "a body that fits never scrolls");
        // Clamped to a short display: the body scrolls, bounded both ways.
        let (w2, short) = help_size(&h, &s, 1440, 300, &mut gs);
        let max = scroll_max(&h, w2, short, &s, &mut gs);
        assert!(max > 0, "the clamped card overflows");
        h.wheel(-100, w2, short, &s, &mut gs);
        assert_eq!(h.scroll, max, "the wheel stops at the end");
        h.wheel(100, w2, short, &s, &mut gs);
        assert_eq!(h.scroll, 0, "and at the start");
        assert_eq!(h.key(HelpKey::End, w2, short, &s, &mut gs), None);
        assert_eq!(h.scroll, max);
        assert_eq!(h.key(HelpKey::Home, w2, short, &s, &mut gs), None);
        assert_eq!(h.scroll, 0);
        // Close is the only key that yields.
        assert_eq!(h.key(HelpKey::Close, w2, short, &s, &mut gs), Some(()));
    }

    #[test]
    fn a_surface_that_changes_height_re_clamps_rather_than_stranding_the_body() {
        let s = sheet();
        let mut gs = crate::raster::GlyphSource::new_vendored(512);
        let mut h = Help::from_chords(DEFAULTS);
        let (w, short) = help_size(&h, &s, 1440, 300, &mut gs);
        h.key(HelpKey::End, w, short, &s, &mut gs);
        let at_end = h.scroll;
        assert!(at_end > 0, "the short card scrolls");
        // A CONFIGURE that makes the card TALLER leaves the stored offset past
        // the new bound. The painter clamps with the same helper the movers
        // use, so the card can never show a gap below its footer.
        let taller = short * 3;
        let max_tall = scroll_max(&h, w, taller, &s, &mut gs);
        assert!(max_tall < at_end, "a taller card bounds the offset lower");
        assert!(!help_list(&h, w, taller, &s, &mut gs).ops.is_empty(), "it still paints");
        // A surface shorter than its own header must not be able to scroll the
        // whole body out of sight: the viewport floors at 0, so the bound is
        // exactly the body -- never more.
        let hh = head_h(&s, &mut gs);
        assert_eq!(
            scroll_max(&h, w, (hh / 2) as u32, &s, &mut gs),
            body_h(&h, w, &s, &mut gs),
            "the bound is the body, never more"
        );
    }

    #[test]
    fn a_duplicate_binding_resolves_to_the_first_deterministically() {
        // A config push can bind two keys to one action. The row names the
        // FIRST, and the same file always yields the same reference.
        let text = "super+f zoom\nsuper+g zoom\n";
        let a = Help::from_chords(text);
        let b = Help::from_chords(text);
        assert_eq!(a.rows.len(), 1, "one action, one row");
        assert_eq!(a.rows[0].caps, alloc::vec!["SUPER", "F"]);
        assert_eq!(a.rows, b.rows, "the same file yields the same reference");
    }

    #[test]
    fn enter_space_and_the_movers_map_but_escape_does_not() {
        assert_eq!(help_key(28, 0), HelpKey::Close);
        assert_eq!(help_key(57, 0), HelpKey::Close);
        assert_eq!(help_key(0, 0x20), HelpKey::Close);
        assert_eq!(help_key(0, 0x0d), HelpKey::Close);
        assert_eq!(help_key(103, 0), HelpKey::Up);
        assert_eq!(help_key(108, 0), HelpKey::Down);
        assert_eq!(help_key(0, 0x6a), HelpKey::Down);
        assert_eq!(help_key(0, 0x6b), HelpKey::Up);
        // Escape (code 1) is the compositor's dismiss and never routes here.
        assert_eq!(help_key(1, 0x1b), HelpKey::None);
    }
}
