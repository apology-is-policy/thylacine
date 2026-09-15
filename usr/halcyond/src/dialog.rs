// dialog -- the modal dialog family's model and painter (HALCYON-INSTRUMENT
// 14.5, I-7): the third model `menuset` carries on the ONE Role::Menu
// surface, so the compositor's grab, click-away (= Cancel) and Esc (=
// Cancel) are exactly H-3c's. This file thinks; it never syscalls.
//
// A dialog is square: a header (an `amber` eyebrow in Cornucopia -- the MONO
// face, per 14.5 and the kit's `.eyebrow { font: 500 10px/1 "IBM Plex Mono" }`;
// it painted in Sans from I-7 until the I-7b self-audit caught the code
// disagreeing with its own comment, this doc and the kit at once -- a Sans
// title), a `secondary` body wrapped at the width, and a right-aligned
// footer of buttons. The default button carries an `amber` border and
// `text` ink (never a filled amber rectangle); a destructive one `error`
// border and ink, never pre-focused; the focused button an `amber` outline
// inset 2. Consumers as built: the RESET confirmation and the running-close
// confirmation (the help modal is I-7b, its frame different).

use alloc::string::String;
use alloc::vec::Vec;

use cartoon::{Cartoon, Op};
use libhalcyon::theme::Argb;

use crate::layout::Sheet;
use crate::raster::GlyphSource;

pub const MAX_W: i32 = 480;
pub const VIEW_MARGIN: i32 = 32;
const HEAD_PAD_TOP: i32 = 18;
const HEAD_PAD_X: i32 = 20;
const HEAD_PAD_BOT: i32 = 12;
const TITLE_GAP: i32 = 7;
const BODY_PAD_X: i32 = 20;
const BODY_PAD_BOT: i32 = 18;
const FOOT_PAD_Y: i32 = 12;
const FOOT_PAD_X: i32 = 20;
const FOOT_GAP: i32 = 8;
const BTN_H: i32 = 30;
const BTN_PAD_X: i32 = 12;
const EYEBROW_PX: f32 = 10.0;
const EYEBROW_TRACK: f32 = 0.12;
const TITLE_PX: f32 = 23.0;
const BODY_PX: f32 = 14.0;
const BODY_LH: f32 = 1.5;
const BTN_PX: f32 = 12.0;

/// One footer button.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Button {
    pub label: String,
    /// The owner's tag for this button (`cancel` / `reset` / `close`),
    /// returned on activation.
    pub tag: String,
    /// The default button: an `amber` border and `text` ink (14.5).
    pub default: bool,
    /// A destructive button: `error` border and ink, never pre-focused.
    pub destructive: bool,
}

impl Button {
    fn new(label: &str, tag: &str) -> Button {
        Button {
            label: String::from(label),
            tag: String::from(tag),
            default: false,
            destructive: false,
        }
    }
}

/// A modal dialog.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Dialog {
    /// The kind, for the diagnostic say (`reset` / `close`).
    pub kind: &'static str,
    pub eyebrow: String,
    pub title: String,
    pub body: String,
    pub buttons: Vec<Button>,
    /// The focused button (keyboard); the caller pre-focuses the default,
    /// never a destructive one.
    pub focus: usize,
}

impl Dialog {
    fn finish(kind: &'static str, eyebrow: &str, title: String, body: String, buttons: Vec<Button>) -> Dialog {
        // Pre-focus the default button; never a destructive one (14.5).
        let focus = buttons
            .iter()
            .position(|b| b.default && !b.destructive)
            .or_else(|| buttons.iter().position(|b| !b.destructive))
            .unwrap_or(0);
        Dialog {
            kind,
            eyebrow: String::from(eyebrow),
            title,
            body,
            buttons,
            focus,
        }
    }

    /// 14.5 / 9.5: the RESET confirmation. `Reset layout` is the default and
    /// pre-focused -- the dialog makes the geometry change deliberate, not
    /// biased against (the mockup's reset asks nothing).
    pub fn reset() -> Dialog {
        let cancel = Button::new("Cancel", "cancel");
        let mut go = Button::new("Reset layout", "reset");
        go.default = true;
        Dialog::finish(
            "reset",
            "WORKSPACE",
            String::from("Reset workspace layout?"),
            String::from("Rearrange this workspace. Running tiles will remain open."),
            alloc::vec![cancel, go],
        )
    }

    /// 14.5: the running-close confirmation -- `Close <tile>?` /
    /// `A process is still running.` + the (sanitised) command; `Cancel` is
    /// the default and pre-focused, `Close tile` destructive.
    pub fn close_running(tile: &str, cmd: &str) -> Dialog {
        let mut title = String::from("Close ");
        title.push_str(tile);
        title.push('?');
        let mut body = String::from("A process is still running.");
        if !cmd.is_empty() {
            body.push('\n');
            body.push_str(cmd);
        }
        let mut cancel = Button::new("Cancel", "cancel");
        cancel.default = true;
        let mut close = Button::new("Close tile", "close");
        close.destructive = true;
        Dialog::finish("close", "TILE", title, body, alloc::vec![cancel, close])
    }
}

/// A key on the dialog surface.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum DialogKey {
    Prev,
    Next,
    Activate,
    None,
}

/// Map a KEY (rune-first, then code + shift) to a dialog key: Left / Shift+Tab
/// go to the previous button, Right / Tab to the next, Enter / Space
/// activate. Esc is the compositor's dismiss (= Cancel), never routed here.
pub fn dialog_key(code: u16, rune: u32, shift: bool) -> DialogKey {
    match rune {
        0x0d | 0x0a | 0x20 => return DialogKey::Activate,
        _ => {}
    }
    match code {
        28 | 96 | 57 => DialogKey::Activate, // Enter / KP-Enter / Space
        105 => DialogKey::Prev,              // Left
        106 => DialogKey::Next,              // Right
        15 => {
            if shift {
                DialogKey::Prev
            } else {
                DialogKey::Next
            }
        }
        _ => DialogKey::None,
    }
}

impl Dialog {
    /// Apply a key: Prev/Next move the focus over the buttons (wrapping),
    /// Activate yields the focused button's tag.
    pub fn key(&mut self, k: DialogKey) -> Option<String> {
        let n = self.buttons.len();
        if n == 0 {
            return None;
        }
        match k {
            DialogKey::Prev => {
                self.focus = if self.focus == 0 { n - 1 } else { self.focus - 1 };
                None
            }
            DialogKey::Next => {
                self.focus = if self.focus + 1 >= n { 0 } else { self.focus + 1 };
                None
            }
            DialogKey::Activate => self.buttons.get(self.focus).map(|b| b.tag.clone()),
            DialogKey::None => None,
        }
    }

    /// The button whose rect a pointer at (x, y) is over (surface-local),
    /// for a click. The rects come from the same layout the painter uses.
    pub fn button_at(&self, x: i32, y: i32, w: u32, h: u32, sheet: &Sheet, gs: &mut GlyphSource) -> Option<usize> {
        let l = layout(self, w, h, sheet, gs);
        l.buttons
            .iter()
            .find(|(_, r)| x >= r.0 && x < r.0 + r.2 && y >= r.1 && y < r.1 + r.3)
            .map(|(i, _)| *i)
    }
}

/// A greedy word-wrap of `text` at `width` in `face`/`px`; explicit newlines
/// break too. Returns the lines.
fn wrap(gs: &mut GlyphSource, face: u8, px: f32, text: &str, width: i32) -> Vec<String> {
    let mut lines: Vec<String> = Vec::new();
    for para in text.split('\n') {
        let mut cur = String::new();
        for word in para.split(' ') {
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

struct Layout {
    body_lines: Vec<String>,
    body_top: i32,
    footer_top: i32,
    buttons: Vec<(usize, (i32, i32, i32, i32))>,
}

fn layout(d: &Dialog, w: u32, h: u32, sheet: &Sheet, gs: &mut GlyphSource) -> Layout {
    gs.set_kerning(sheet.kerning);
    let wi = w as i32;
    let content_w = wi - 2 * sheet.ipx(BODY_PAD_X);
    let body_lines = wrap(gs, sheet.face_body, sheet.px(BODY_PX), &d.body, content_w);
    let head_h = sheet.ipx(HEAD_PAD_TOP)
        + line_h(gs, sheet.face_mono_text, sheet.px(EYEBROW_PX))
        + sheet.ipx(TITLE_GAP)
        + line_h(gs, sheet.face_medium, sheet.px(TITLE_PX))
        + sheet.ipx(HEAD_PAD_BOT);
    let body_top = head_h;
    let body_lh = (sheet.px(BODY_PX) * BODY_LH) as i32;
    let body_h = body_lines.len() as i32 * body_lh + sheet.ipx(BODY_PAD_BOT);
    let footer_top = (body_top + body_h).min(h as i32 - footer_h(sheet));
    // The buttons, right-aligned.
    let mut buttons = Vec::new();
    let mut x = wi - sheet.ipx(FOOT_PAD_X);
    let by = footer_top + sheet.ipx(FOOT_PAD_Y);
    let bh = sheet.ipx(BTN_H);
    for (i, b) in d.buttons.iter().enumerate().rev() {
        let (_, lw) = gs.shape_run(sheet.face_medium, sheet.px(BTN_PX), b.label.chars());
        let bw = lw + 2 * sheet.ipx(BTN_PAD_X);
        x -= bw;
        buttons.push((i, (x, by, bw, bh)));
        x -= sheet.ipx(FOOT_GAP);
    }
    Layout { body_lines, body_top, footer_top, buttons }
}

fn line_h(gs: &mut GlyphSource, face: u8, px: f32) -> i32 {
    gs.line_metrics(face, px).map(|m| m.ascent + m.descent).unwrap_or((px) as i32)
}

fn footer_h(sheet: &Sheet) -> i32 {
    sheet.hairline + 2 * sheet.ipx(FOOT_PAD_Y) + sheet.ipx(BTN_H)
}

/// The dialog's surface size (14.5): min(480, display - 32) wide; the header
/// + wrapped body + footer tall, clamped to display - 32.
pub fn dialog_size(d: &Dialog, sheet: &Sheet, display_w: u32, display_h: u32, gs: &mut GlyphSource) -> (u32, u32) {
    gs.set_kerning(sheet.kerning);
    let w = sheet
        .ipx(MAX_W)
        .min(display_w as i32 - sheet.ipx(VIEW_MARGIN))
        .max(1) as u32;
    let content_w = w as i32 - 2 * sheet.ipx(BODY_PAD_X);
    let body_lines = wrap(gs, sheet.face_body, sheet.px(BODY_PX), &d.body, content_w);
    let head_h = sheet.ipx(HEAD_PAD_TOP)
        + line_h(gs, sheet.face_mono_text, sheet.px(EYEBROW_PX))
        + sheet.ipx(TITLE_GAP)
        + line_h(gs, sheet.face_medium, sheet.px(TITLE_PX))
        + sheet.ipx(HEAD_PAD_BOT);
    let body_lh = (sheet.px(BODY_PX) * BODY_LH) as i32;
    let body_h = body_lines.len() as i32 * body_lh + sheet.ipx(BODY_PAD_BOT);
    let h = (head_h + body_h + footer_h(sheet))
        .min(display_h as i32 - sheet.ipx(VIEW_MARGIN))
        .max(1) as u32;
    (w, h)
}

/// The display list (14.5 colours): `dialog_bg` ground, a 1 px
/// `focus_neutral` frame; the `amber` eyebrow + `text` title; the
/// `secondary` body; a `separator` above the footer; the buttons.
pub fn dialog_list(d: &Dialog, w: u32, h: u32, sheet: &Sheet, gs: &mut GlyphSource) -> Cartoon {
    gs.set_kerning(sheet.kerning);
    let i = &sheet.inst;
    let mut cart = Cartoon::new();
    if w == 0 || h == 0 {
        return cart;
    }
    let (wi, hi) = (w as i32, h as i32);
    let hair = sheet.hairline;
    let hu = hair as u32;
    let gen = gs.gen();
    cart.ops.push(Op::Clear { color: i.dialog_bg });
    for r in [
        (0, 0, w, hu),
        (0, (hi - hair).max(0), w, hu),
        (0, 0, hu, h),
        ((wi - hair).max(0), 0, hu, h),
    ] {
        cart.ops.push(Op::Rect { x: r.0, y: r.1, w: r.2, h: r.3, color: i.focus_neutral });
    }
    let l = layout(d, w, h, sheet, gs);
    // Eyebrow.
    let ex = sheet.ipx(HEAD_PAD_X);
    let ey = sheet.ipx(HEAD_PAD_TOP) + gs.line_metrics(sheet.face_mono_text, sheet.px(EYEBROW_PX)).map(|m| m.ascent).unwrap_or(8);
    let (erefs, _) = gs.shape_run_spaced(sheet.face_mono_text, sheet.px(EYEBROW_PX), EYEBROW_TRACK * sheet.px(EYEBROW_PX), d.eyebrow.chars());
    if !erefs.is_empty() {
        cart.push_glyphs(gen, ex, ey, i.amber, &erefs);
    }
    // Title.
    let ty = sheet.ipx(HEAD_PAD_TOP) + line_h(gs, sheet.face_mono_text, sheet.px(EYEBROW_PX)) + sheet.ipx(TITLE_GAP)
        + gs.line_metrics(sheet.face_medium, sheet.px(TITLE_PX)).map(|m| m.ascent).unwrap_or(18);
    let (trefs, _) = gs.shape_run(sheet.face_medium, sheet.px(TITLE_PX), d.title.chars());
    if !trefs.is_empty() {
        cart.push_glyphs(gen, ex, ty, i.text, &trefs);
    }
    // Body.
    let body_lh = (sheet.px(BODY_PX) * BODY_LH) as i32;
    let asc = gs.line_metrics(sheet.face_body, sheet.px(BODY_PX)).map(|m| m.ascent).unwrap_or(11);
    for (n, line) in l.body_lines.iter().enumerate() {
        let by = l.body_top + n as i32 * body_lh + asc;
        let (refs, _) = gs.shape_run(sheet.face_body, sheet.px(BODY_PX), line.chars());
        if !refs.is_empty() {
            cart.push_glyphs(gen, sheet.ipx(BODY_PAD_X), by, i.secondary, &refs);
        }
    }
    // Footer separator.
    cart.ops.push(Op::Rect { x: 0, y: l.footer_top, w, h: hu, color: i.separator });
    // Buttons.
    for (idx, r) in &l.buttons {
        let b = &d.buttons[*idx];
        let border = if b.destructive {
            i.error
        } else if b.default {
            i.amber
        } else {
            i.structure
        };
        let ink = if b.destructive {
            i.error
        } else if b.default {
            i.text
        } else {
            i.secondary
        };
        // The button's 1 px border (transparent ground).
        ring(&mut cart, r.0, r.1, r.2, r.3, hair, border);
        // The focused button: an `amber` outline inset 2.
        if *idx == d.focus {
            let o = sheet.ipx(2);
            ring(&mut cart, r.0 + o, r.1 + o, (r.2 - 2 * o).max(0), (r.3 - 2 * o).max(0), hair, i.amber);
        }
        let base = r.1 + centre_base(gs, sheet.face_medium, sheet.px(BTN_PX), r.3);
        let (lrefs, lw) = gs.shape_run(sheet.face_medium, sheet.px(BTN_PX), b.label.chars());
        if !lrefs.is_empty() {
            cart.push_glyphs(gen, r.0 + (r.2 - lw) / 2, base, ink, &lrefs);
        }
    }
    cart
}

fn centre_base(gs: &mut GlyphSource, face: u8, px: f32, box_h: i32) -> i32 {
    let (asc, desc) = gs.line_metrics(face, px).map(|m| (m.ascent, m.descent)).unwrap_or((8, 2));
    (box_h - (asc + desc)) / 2 + asc
}

fn ring(cart: &mut Cartoon, x: i32, y: i32, w: i32, h: i32, t: i32, color: Argb) {
    let put = |cart: &mut Cartoon, x: i32, y: i32, w: i32, h: i32| {
        if w > 0 && h > 0 {
            cart.ops.push(Op::Rect { x, y, w: w as u32, h: h as u32, color });
        }
    };
    put(cart, x, y, w, t);
    put(cart, x, y + h - t, w, t);
    put(cart, x, y, t, h);
    put(cart, x + w - t, y, t, h);
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::layout::sheet_for;
    use libhalcyon::instrument::{Bundle, Profile};

    fn sheet() -> Sheet {
        sheet_for(&Bundle::builtin(Profile::Instrument), 100, crate::layout::TEST_DISPLAY_W)
    }

    #[test]
    fn reset_prefocuses_the_default_and_activates_it() {
        let mut d = Dialog::reset();
        assert_eq!(d.title, "Reset workspace layout?");
        assert_eq!(d.eyebrow, "WORKSPACE");
        // Reset layout is the default and pre-focused.
        let f = d.focus;
        assert!(d.buttons[f].default && d.buttons[f].tag == "reset");
        assert_eq!(d.key(DialogKey::Activate).as_deref(), Some("reset"));
        // Prev/Next wrap; Cancel is reachable.
        d.key(DialogKey::Prev);
        assert_eq!(d.key(DialogKey::Activate).as_deref(), Some("cancel"));
    }

    #[test]
    fn a_destructive_close_never_prefocuses_the_destructive_button() {
        let d = Dialog::close_running("transcript", "cargo build");
        assert_eq!(d.title, "Close transcript?");
        let f = d.focus;
        assert!(!d.buttons[f].destructive, "focus is never on the destructive button");
        assert_eq!(d.buttons[f].tag, "cancel", "Cancel is the default here");
        // The destructive button is present.
        assert!(d.buttons.iter().any(|b| b.destructive && b.tag == "close"));
        // The body carries the sanitised command on its own line.
        assert!(d.body.contains("A process is still running."));
        assert!(d.body.contains("cargo build"));
    }

    #[test]
    fn dialog_key_maps_the_moves_and_the_activation() {
        assert_eq!(dialog_key(0, 0x0d, false), DialogKey::Activate);
        assert_eq!(dialog_key(57, 0, false), DialogKey::Activate);
        assert_eq!(dialog_key(105, 0, false), DialogKey::Prev);
        assert_eq!(dialog_key(106, 0, false), DialogKey::Next);
        assert_eq!(dialog_key(15, 0, false), DialogKey::Next, "Tab -> next");
        assert_eq!(dialog_key(15, 0, true), DialogKey::Prev, "Shift+Tab -> prev");
    }

    #[test]
    fn the_painter_shows_the_frame_the_eyebrow_and_the_button_borders() {
        let s = sheet();
        let mut gs = crate::raster::GlyphSource::new_vendored(512);
        let d = Dialog::close_running("build", "make");
        let (w, h) = dialog_size(&d, &s, 1440, 900, &mut gs);
        assert!(w <= s.ipx(MAX_W) as u32);
        let cart = dialog_list(&d, w, h, &s, &mut gs);
        let has_rect = |c: u32| cart.ops.iter().any(|op| matches!(op, Op::Rect { color, .. } if *color == c));
        assert!(cart.ops.iter().any(|op| matches!(op, Op::Clear { color } if *color == s.inst.dialog_bg)), "dialog_bg ground");
        assert!(has_rect(s.inst.focus_neutral), "the focus_neutral frame");
        assert!(has_rect(s.inst.separator), "the footer separator");
        assert!(has_rect(s.inst.error), "the destructive button's error border");
        assert!(has_rect(s.inst.amber), "the focused (default) button's amber outline");
        // The eyebrow is an amber glyph run.
        assert!(cart.ops.iter().any(|op| matches!(op, Op::Glyphs { color, .. } if *color == s.inst.amber)), "amber eyebrow");
    }

    #[test]
    fn a_click_hits_the_button_rects() {
        let s = sheet();
        let mut gs = crate::raster::GlyphSource::new_vendored(512);
        let d = Dialog::reset();
        let (w, h) = dialog_size(&d, &s, 1440, 900, &mut gs);
        // A point inside the last (rightmost) button's rect resolves to a button.
        let l = layout(&d, w, h, &s, &mut gs);
        let (idx, r) = l.buttons[0];
        let hit = d.button_at(r.0 + r.2 / 2, r.1 + r.3 / 2, w, h, &s, &mut gs);
        assert_eq!(hit, Some(idx));
        // A point outside every button is None.
        assert_eq!(d.button_at(2, 2, w, h, &s, &mut gs), None);
    }
}
