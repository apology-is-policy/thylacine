// rail -- the two rails of the Instrument profile (HALCYON-INSTRUMENT 8,
// 8.1, 8.2, 8.3, 14.1, 14.3): the pure half. The top rail is a
// `Role::Rail` surface the compositor places at the strip its carve always
// reserves; the bottom rail is H-3d's status bar, restyled here whenever
// the sheet's profile is Instrument (`status::status_list` dispatches to
// `footer_list`). The bin (`railset` / `statusset`) owns the surfaces and
// the sources; every pixel decision is here, under host tests.
//
// The numbers are the golden's (`build/instrument-goldens/native-r2/
// matrix-carbon-1440x900-s100-baseDpr1`: the DOM boxes of
// `geometry-styles.json` and the PNG's rows), not the kit's CSS believed:
// the brand mark's border ring at (10, 10) 13 x 13 with its two 1 px
// strokes at (+4, +4) 5 tall and (+4, +8) 5 wide; the context at 222; the
// 1 x 12 separator at y 11 (10.5 snapped half up); the buttons 26 tall at
// y 4 (3.5 snapped half up), 2 apart, padded 9 inside 1 px side borders;
// the theme swatch 8 x 8 at y 13 with its ring (`Derived.swatch_ring`,
// #CDC199 on Carbon); the clock padded 10 / 5 against the 8 px right pad;
// the footer's 6 x 6 glyph box at (10, 10), its label at 24, the centre
// hints centred between the two end groups (the CSS space-between), the
// right group's 1 x 12 separator with 10 px margins.
//
// The type is the sheet's (7.2, since I-5): the brand in `face_brand`
// (600), the context's basename in `face_medium` (500), labels and buttons
// in `face_body` (400) at 11 / 10; the mono roles -- the chips, `?`, the
// clock (11), the whole footer (10; 9 narrow) -- in `face_mono_text`, the
// free-running Cornucopia at the type map's sizes (the island CELL's 6 px
// advance floor served them until I-5). `↺`, `‹`, `›` are Sans glyphs;
// `?`, `✓`, `!` and `·` mono ones. The `═` / `║` split icons and the `⌄`
// chevron stay DRAWN as marks of the golden's footprint: the golden's are a
// browser fallback font's glyphs (Plex Sans carries none of the three), 8
// wide where Cornucopia's box glyph at 10 px is 5, so the mark reproduces
// the oracle where the glyph would not. The letter-spacing the kit sets on
// every uppercase run (0.08 em) is honoured through
// `GlyphSource::shape_run_spaced`.

use alloc::string::String;
use alloc::vec::Vec;

use cartoon::{Cartoon, GlyphRef, Op};
use libhalcyon::theme::Argb;

use crate::layout::Sheet;
use crate::raster::GlyphSource;
use crate::status::{Condition, Slots, StatusModel};

/// Logical px at 100 % (8.1 / 8.2 / 14.1).
const PAD_L: i32 = 10;
const PAD_R: i32 = 8;
const BRAND_W: i32 = 212;
const BRAND_NARROW_W: i32 = 54;
const MARK: i32 = 13;
const MARK_STROKE_AT: i32 = 4;
const MARK_STROKE: i32 = 5;
const BRAND_GAP: i32 = 9;
const SEP_H: i32 = 12;
const SEP_MARGIN: i32 = 10;
const BTN_H: i32 = 26;
const BTN_PAD: i32 = 9;
const BTN_GAP: i32 = 2;
const BTN_INNER_GAP: i32 = 7;
const ICON_W: i32 = 8;
const HELP_W: i32 = 28;
const SWATCH: i32 = 8;
const CHEVRON_W: i32 = 8;
const CLOCK_PAD_L: i32 = 10;
const CLOCK_PAD_R: i32 = 5;
/// 8.3: at or below this logical width the rails take their narrow form.
pub const NARROW_W: i32 = 820;
const RAIL_GAP: i32 = 8;
const CHIP_W: i32 = 26;
const CHIP_H: i32 = 24;
const CHIP_Y: i32 = 5;
const CHIP_GAP: i32 = 4;
const CHIPS_VIEW_W: i32 = 189;
const CHIPS_SCROLLED_VIEW_W: i32 = 157;
const CHIP_ARROW_W: i32 = 16;
const CHIP_EDGE_H: i32 = 2;
const CHIP_EDGE_INSET: i32 = 4;
const FOOT_PAD: i32 = 10;
const GLYPH_BOX: i32 = 6;
const GLYPH_GAP: i32 = 8;

/// Section 10 (amended at I-8): the SUCCESS condition's glow -- `success`
/// at .25 under a box blur of 8, both scaled. The alpha is 64/256, the same
/// rounding `Derived` takes for its opaques, so a glow and a derived opaque
/// of the same stated percentage agree.
const GLOW_ALPHA: u8 = 64;
const GLOW_BLUR: i32 = 8;
const HINT_GAP: i32 = 8;
const RUN_SQUARE: i32 = 4;

/// The rails' type (the golden's CSS), LOGICAL: the sheet scales it.
pub const RAIL_PX: f32 = 11.0;
pub const BUTTON_PX: f32 = 10.0;
pub const FOOTER_PX: f32 = 10.0;
pub const FOOTER_NARROW_PX: f32 = 9.0;
/// The kit's `letter-spacing: .08em` on every uppercase run.
const TRACK_EM: f32 = 0.08;
/// 8.2: the command shown is at most this many characters.
pub const CMD_MAX: usize = 96;
/// The most `chords` lines the hints read: the compositor renders one line
/// per bound (key, shift) over a vocabulary of a few dozen keys, so this is
/// a margin, and the bound is OURS rather than the other process's (r1 B-F6).
pub const CHORD_LINES_MAX: usize = 128;
/// The most `layout` rows a reset plan considers: the layout file's own
/// node cap (`libhalcyon::layout::MAX_NODES`), so the plan's quadratic
/// walks are bounded here, not by the compositor's pane cap (r1 B-F5).
pub const RESET_ROWS_MAX: usize = libhalcyon::layout::MAX_NODES;
/// 8.2: the label of the idle condition.
pub const READY: &str = "READY";

/// What the top rail shows (8.1); the sources are the bin's.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct RailModel {
    /// The live workspace NUMBERS, ascending, at least one (14.1: chips past
    /// one). S4: the set is sparse, so this is the identities themselves --
    /// a count could not label `01 03 04`.
    pub workspaces: Vec<u8>,
    /// The active workspace's POSITION in `workspaces` (not its number): the
    /// painter compares `i == active` over the chips it lays out, and the
    /// label comes from the list.
    pub active: u8,
    /// The first chip shown when the strip scrolls (14.1: the ‹ › reveal);
    /// the painter clamps it so the active chip is always in view.
    pub chip_scroll: u8,
    /// The focused tile's working directory (home folded to `~`); empty
    /// when none.
    pub cwd: String,
    /// The focused tile's name; empty when nothing is focused.
    pub title: String,
    /// The theme in force, by name.
    pub theme: String,
    pub hour: u8,
    pub minute: u8,
}

impl RailModel {
    pub fn empty() -> RailModel {
        RailModel {
            workspaces: alloc::vec![1],
            active: 0,
            chip_scroll: 0,
            cwd: String::new(),
            title: String::new(),
            theme: String::new(),
            hour: 0,
            minute: 0,
        }
    }
}

/// A pointer target on the top rail (8.1 / 14.1).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum RailHit {
    /// The mark (and, with one workspace, its label): the workspace list.
    Brand,
    /// A workspace chip (0-based).
    Chip(u8),
    /// The ‹ / › reveal buttons.
    ChipsPrev,
    ChipsNext,
    SplitH,
    SplitV,
    Theme,
    Reset,
    Help,
}

/// The pointer's state over the rail: what it hovers and what is pressed.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub struct RailInk {
    pub hover: Option<RailHit>,
    pub pressed: Option<RailHit>,
}

/// (x, y, w, h) in rail pixels.
pub type Box4 = (i32, i32, i32, i32);

/// Where the rail's targets and spans landed, in rail pixels: the hit
/// test's input and the witness's say line.
#[derive(Clone, PartialEq, Eq, Debug, Default)]
pub struct RailZones {
    pub brand: Box4,
    pub chips: Vec<(u8, Box4)>,
    pub chips_prev: Option<Box4>,
    pub chips_next: Option<Box4>,
    /// The first chip shown after the clamp (what an arrow press starts
    /// from).
    pub chip_first: u8,
    /// The context span (x, w); (0, 0) when hidden.
    pub ctx: (i32, i32),
    /// The action buttons, right to left as laid.
    pub buttons: Vec<(RailHit, Box4)>,
    /// The clock's glyph run (x, w).
    pub clock: (i32, i32),
}

impl RailZones {
    /// The zones a say line is keyed on: the targets, never the context
    /// span (which moves with every `cd`).
    pub fn stable(&self) -> RailZones {
        RailZones {
            ctx: (0, 0),
            ..self.clone()
        }
    }
}

fn inside(b: Box4, x: i32, y: i32) -> bool {
    x >= b.0 && x < b.0 + b.2 && y >= b.1 && y < b.1 + b.3
}

/// What a point on the rail is over.
pub fn rail_hit(z: &RailZones, x: i32, y: i32) -> Option<RailHit> {
    for (hit, b) in &z.buttons {
        if inside(*b, x, y) {
            return Some(*hit);
        }
    }
    for (i, b) in &z.chips {
        if inside(*b, x, y) {
            return Some(RailHit::Chip(*i));
        }
    }
    if z.chips_prev.is_some_and(|b| inside(b, x, y)) {
        return Some(RailHit::ChipsPrev);
    }
    if z.chips_next.is_some_and(|b| inside(b, x, y)) {
        return Some(RailHit::ChipsNext);
    }
    if inside(z.brand, x, y) {
        return Some(RailHit::Brand);
    }
    None
}

/// A run's baseline that centres its line box in `h` rows.
fn centred_in(gs: &mut GlyphSource, face: u8, px: f32, h: i32) -> i32 {
    let (asc, desc) = gs
        .line_metrics(face, px)
        .map(|m| (m.ascent, m.descent))
        .unwrap_or((8, 2));
    (h - (asc + desc)) / 2 + asc
}

/// Half-up centring of a `size` box in `space` (the browser's snapped
/// 3.5 -> 4, 10.5 -> 11).
fn centre(space: i32, size: i32) -> i32 {
    (space - size + 1) / 2
}

struct Run {
    refs: Vec<GlyphRef>,
    width: i32,
}

/// An uppercase run in `face` at `px`, tracked at 0.08 em.
fn tracked(gs: &mut GlyphSource, face: u8, px: f32, text: &str) -> Run {
    let (refs, width) = gs.shape_run_spaced(face, px, TRACK_EM * px, text.chars());
    Run { refs, width }
}

/// `tracked` continuing one sub-pixel pen across runs: the fraction carried
/// in and out (`GlyphSource::shape_run_spaced_from`). The context's three
/// runs are three inks and two faces on ONE line of the golden -- laid as
/// one pen they land on its 287.2 (kerned, HarfBuzz to 0.02 px); truncated
/// one by one they came up 1.2 short (r2).
fn tracked_from(gs: &mut GlyphSource, face: u8, px: f32, text: &str, rem: i32) -> (Run, i32) {
    let (refs, width, rem) = gs.shape_run_spaced_from(face, px, TRACK_EM * px, text.chars(), rem);
    (Run { refs, width }, rem)
}

fn width_of(gs: &mut GlyphSource, face: u8, px: f32, text: &str) -> i32 {
    tracked(gs, face, px, text).width
}

/// `text` cut from its end to fit `avail`, with an ellipsis; whole when it
/// fits, empty when not even the ellipsis does.
fn fit_end(gs: &mut GlyphSource, face: u8, px: f32, text: &str, avail: i32) -> String {
    if width_of(gs, face, px, text) <= avail {
        return String::from(text);
    }
    let ell = '\u{2026}';
    let mut chars: Vec<char> = text.chars().collect();
    while !chars.is_empty() {
        chars.pop();
        let mut cand: String = chars.iter().collect();
        cand.push(ell);
        if width_of(gs, face, px, &cand) <= avail {
            return cand;
        }
    }
    String::new()
}

/// `text` with its middle replaced by an ellipsis until it fits `avail`
/// (14.3: the cwd's leading segments give way first); whole when it fits,
/// the ellipsis alone when nothing else does.
fn fit_middle(gs: &mut GlyphSource, face: u8, px: f32, text: &str, avail: i32) -> String {
    if width_of(gs, face, px, text) <= avail {
        return String::from(text);
    }
    let mut chars: Vec<char> = text.chars().collect();
    loop {
        if chars.is_empty() {
            return String::from("\u{2026}");
        }
        chars.remove(chars.len() / 2);
        let mut cand: String = chars[..chars.len() / 2].iter().collect();
        cand.push('\u{2026}');
        cand.extend(chars[chars.len() / 2..].iter());
        if width_of(gs, face, px, &cand) <= avail {
            return cand;
        }
    }
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

/// A 1 px ring inside (x, y, w, h).
fn ring(cart: &mut Cartoon, x: i32, y: i32, w: i32, h: i32, t: i32, color: Argb) {
    rect(cart, x, y, w, t, color);
    rect(cart, x, y + h - t, w, t, color);
    rect(cart, x, y, t, h, color);
    rect(cart, x + w - t, y, t, h, color);
}

/// Section 10's glow: `color` at `alpha` under a box blur of `radius`,
/// spreading `radius` past the rect on every side. Pushed BEFORE what it
/// sits under, since the executor paints in list order.
fn glow(cart: &mut Cartoon, x: i32, y: i32, w: i32, h: i32, color: Argb, alpha: u8, radius: i32) {
    if w > 0 && h > 0 && radius >= 0 {
        cart.ops.push(Op::Glow {
            x,
            y,
            w: w as u32,
            h: h as u32,
            color,
            alpha,
            radius: radius as u32,
        });
    }
}

fn push(cart: &mut Cartoon, gen: u32, x: i32, base: i32, color: Argb, run: &Run) {
    if !run.refs.is_empty() {
        cart.push_glyphs(gen, x, base, color, &run.refs);
    }
}

/// The top rail's display list for a `w` x `h` surface at the sheet's
/// scale (8.1, 8.3, 14.1), and where its targets landed. Left to right:
/// the brand (the mark, then `WORKSPACE 01` or the chips), the context
/// (`<cwd> │ <title>`, the cwd's leading segments `dim`, its basename
/// `text`, the title `secondary`; the cwd middle-ellipsised first, then
/// the title end-ellipsised); at the right, the clock and the five action
/// buttons laid right to left. At or below 820 logical wide: the mark and
/// the active number only, no context, icon-only buttons. A zero-sized
/// rail yields an empty list.
pub fn rail_list(
    m: &RailModel,
    ink: RailInk,
    w: u32,
    h: u32,
    sheet: &Sheet,
    gs: &mut GlyphSource,
) -> (Cartoon, RailZones) {
    // The source follows the sheet in force at every painter entry (r2 A-F2).
    gs.set_kerning(sheet.kerning);
    let mut cart = Cartoon::new();
    let mut z = RailZones::default();
    if w == 0 || h == 0 {
        return (cart, z);
    }
    let i = &sheet.inst;
    let (wi, hi) = (w as i32, h as i32);
    let hair = sheet.hairline.max(1);
    let ch = hi - hair;
    let narrow = wi <= sheet.ipx(NARROW_W);
    let gen = gs.gen();
    let px = sheet.px(RAIL_PX);
    let bpx = sheet.px(BUTTON_PX);
    let (body, medium, brand, mono) = (sheet.face_body, sheet.face_medium, sheet.face_brand, sheet.face_mono_text);
    let mono_px = sheet.chrome_mono_px;
    cart.ops.push(Op::Clear { color: i.rail });
    rect(&mut cart, 0, ch, wi, hair, i.structure);

    // --- The brand ---------------------------------------------------------
    let pad_l = sheet.ipx(PAD_L);
    let mark = sheet.ipx(MARK);
    let my = centre(ch, mark);
    ring(&mut cart, pad_l, my, mark, mark, hair, i.amber_muted);
    let at = sheet.ipx(MARK_STROKE_AT);
    let stroke = sheet.ipx(MARK_STROKE);
    rect(&mut cart, pad_l + at, my + at, hair, stroke, i.amber);
    rect(&mut cart, pad_l + at, my + at + stroke - hair, stroke, hair, i.amber);
    let label_x = pad_l + mark + sheet.ipx(BRAND_GAP);
    let n = (m.workspaces.len() as u8).max(1);
    let cluster_w = sheet.ipx(if narrow { BRAND_NARROW_W } else { BRAND_W });
    if narrow || n == 1 {
        let (run, face, fpx) = if narrow {
            let mut num = String::new();
            // S4: the ACTIVE NUMBER, read from the list -- `active` is a
            // position, and with a sparse set position + 1 is not the number.
            let active_num = m.workspaces.get(m.active as usize).copied().unwrap_or(1);
            let _ = core::fmt::write(&mut num, format_args!("{:02}", active_num as u32));
            (tracked(gs, mono, mono_px, &num), mono, mono_px)
        } else {
            (tracked(gs, brand, px, "WORKSPACE 01"), brand, px)
        };
        let base = centred_in(gs, face, fpx, ch);
        push(&mut cart, gen, label_x, base, i.text, &run);
        z.brand = (pad_l, 0, (label_x + run.width - pad_l).min(cluster_w), ch);
    } else {
        // 14.1: the chips in their viewport, ‹ › past 189.
        z.brand = (pad_l, 0, mark, ch);
        let chip_w = sheet.ipx(CHIP_W);
        let chip_h = sheet.ipx(CHIP_H);
        let chip_y = sheet.ipx(CHIP_Y);
        let gap = sheet.ipx(CHIP_GAP);
        let pitch = (chip_w + gap).max(1); // r1 B-F14: never a zero divisor
        let total = n as i32 * chip_w + (n as i32 - 1) * gap;
        let view_w = sheet.ipx(CHIPS_VIEW_W);
        let scrolls = total > view_w;
        let (view_x, view_w) = if scrolls {
            let aw = sheet.ipx(CHIP_ARROW_W);
            (label_x + aw, sheet.ipx(CHIPS_SCROLLED_VIEW_W))
        } else {
            (label_x, view_w)
        };
        let visible = ((view_w + gap) / pitch).max(1) as u8;
        let mut first = m.chip_scroll.min(n.saturating_sub(visible));
        if m.active < first {
            first = m.active;
        } else if m.active >= first + visible {
            first = m.active + 1 - visible;
        }
        z.chip_first = first;
        if scrolls {
            let aw = sheet.ipx(CHIP_ARROW_W);
            let prev = (label_x, 0, aw, ch);
            let next = (view_x + view_w, 0, aw, ch);
            let can_prev = first > 0;
            let can_next = first + visible < n;
            for (b, glyph, can, hit) in [
                (prev, "\u{2039}", can_prev, RailHit::ChipsPrev),
                (next, "\u{203A}", can_next, RailHit::ChipsNext),
            ] {
                let run = tracked(gs, body, px, glyph);
                let color = if !can {
                    i.dim
                } else if ink.pressed == Some(hit) {
                    i.amber
                } else {
                    i.secondary
                };
                let base = centred_in(gs, body, px, ch);
                push(&mut cart, gen, b.0 + (b.2 - run.width) / 2, base, color, &run);
            }
            z.chips_prev = Some(prev);
            z.chips_next = Some(next);
        }
        for c in first..n.min(first + visible) {
            let cx = view_x + (c - first) as i32 * pitch;
            let b = (cx, chip_y, chip_w, chip_h);
            let active = c == m.active;
            let hovered = ink.hover == Some(RailHit::Chip(c));
            if active || hovered {
                rect(&mut cart, cx, chip_y, chip_w, chip_h, i.hover);
            }
            if active {
                let inset = sheet.ipx(CHIP_EDGE_INSET);
                let eh = sheet.ipx(CHIP_EDGE_H);
                rect(&mut cart, cx + inset, chip_y + chip_h - eh, chip_w - 2 * inset, eh, i.amber);
            }
            let mut num = String::new();
            // S4: the chip's LABEL is the number it stands for, not its
            // position in the strip.
            let chip_num = m.workspaces.get(c as usize).copied().unwrap_or(c + 1);
            let _ = core::fmt::write(&mut num, format_args!("{:02}", chip_num as u32));
            let run = tracked(gs, mono, mono_px, &num);
            let base = chip_y + centred_in(gs, mono, mono_px, chip_h);
            let color = if active || hovered { i.text } else { i.secondary };
            push(&mut cart, gen, cx + (chip_w - run.width) / 2, base, color, &run);
            z.chips.push((c, b));
        }
    }

    // --- The actions, right to left -------------------------------------------
    let mut xr = wi - sheet.ipx(PAD_R);
    // The clock.
    {
        let mut text = String::new();
        let _ = core::fmt::write(&mut text, format_args!("{:02}:{:02}", m.hour, m.minute));
        let run = tracked(gs, mono, sheet.clock_px, &text);
        let tx = xr - sheet.ipx(CLOCK_PAD_R) - run.width;
        let base = centred_in(gs, mono, sheet.clock_px, ch);
        push(&mut cart, gen, tx, base, i.text, &run);
        z.clock = (tx, run.width);
        xr = tx - sheet.ipx(CLOCK_PAD_L);
    }
    let bh = sheet.ipx(BTN_H);
    let by = centre(ch, bh);
    let pad = sheet.ipx(BTN_PAD);
    let border = hair;
    let igap = sheet.ipx(BTN_INNER_GAP);
    let icon_w = sheet.ipx(ICON_W);
    let bgap = sheet.ipx(BTN_GAP);
    let label_base = by + centred_in(gs, body, bpx, bh);
    // A button's ink and ground by its pointer state.
    let button_ink = |hit: RailHit| -> Argb {
        if ink.pressed == Some(hit) {
            i.amber
        } else if ink.hover == Some(hit) {
            i.text
        } else {
            i.secondary
        }
    };
    // The frame under a hovered button: `hover` ground + `structure` sides.
    let frame = |cart: &mut Cartoon, hit: RailHit, bx: i32, bw: i32| {
        if ink.hover == Some(hit) || ink.pressed == Some(hit) {
            rect(cart, bx, by, bw, bh, i.hover);
            rect(cart, bx, by, border, bh, i.structure);
            rect(cart, bx + bw - border, by, border, bh, i.structure);
        }
    };
    // `?` (icon-only, 28 wide, mono).
    {
        let bw = sheet.ipx(HELP_W);
        let bx = xr - bw;
        frame(&mut cart, RailHit::Help, bx, bw);
        let run = tracked(gs, mono, bpx, "?");
        let base = by + centred_in(gs, mono, bpx, bh);
        push(&mut cart, gen, bx + (bw - run.width) / 2, base, button_ink(RailHit::Help), &run);
        z.buttons.push((RailHit::Help, (bx, by, bw, bh)));
        xr = bx - bgap;
    }
    // `↺ RESET`.
    {
        let hit = RailHit::Reset;
        let label = if narrow { None } else { Some(tracked(gs, body, bpx, "RESET")) };
        let lw = label.as_ref().map_or(0, |r| igap + r.width);
        let bw = 2 * border + 2 * pad + icon_w + lw;
        let bx = xr - bw;
        frame(&mut cart, hit, bx, bw);
        let color = button_ink(hit);
        let ix = bx + border + pad;
        let icon = tracked(gs, body, bpx, "\u{21BA}");
        push(&mut cart, gen, ix + (icon_w - icon.width) / 2, label_base, color, &icon);
        if let Some(r) = &label {
            push(&mut cart, gen, ix + icon_w + igap, label_base, color, r);
        }
        z.buttons.push((hit, (bx, by, bw, bh)));
        xr = bx - bgap;
    }
    // `■ <theme> ⌄`.
    {
        let hit = RailHit::Theme;
        let sw = sheet.ipx(SWATCH);
        let cw = sheet.ipx(CHEVRON_W);
        let name = m.theme.to_uppercase();
        let label = if narrow || name.is_empty() {
            None
        } else {
            Some(tracked(gs, body, bpx, &name))
        };
        let lw = label.as_ref().map_or(0, |r| igap + r.width);
        let bw = 2 * border + 2 * pad + sw + lw + igap + cw;
        let bx = xr - bw;
        frame(&mut cart, hit, bx, bw);
        let color = button_ink(hit);
        let sx = bx + border + pad;
        let sy = by + centre(bh, sw);
        rect(&mut cart, sx, sy, sw, sw, i.amber);
        ring(&mut cart, sx, sy, sw, sw, hair, sheet.derived.swatch_ring);
        let mut x = sx + sw;
        if let Some(r) = &label {
            push(&mut cart, gen, x + igap, label_base, color, r);
            x += igap + r.width;
        }
        // The chevron: a 6-wide `v` of three 1 px rows, centred in its 8
        // box, in `dim` (the kit's `.chevron`); neither face carries U+2304.
        let cx = x + igap + (cw - sheet.ipx(6)) / 2;
        let cy = by + sheet.ipx(13);
        let u = sheet.ipx(1).max(1);
        for (dx, dy, dw) in [
            (0, 0, 2),
            (4, 0, 2),
            (1, 1, 2),
            (3, 1, 2),
            (2, 2, 2),
        ] {
            rect(&mut cart, cx + dx * u, cy + dy * u, dw * u, u, i.dim);
        }
        z.buttons.push((hit, (bx, by, bw, bh)));
        xr = bx - bgap;
    }
    // `║ SPLIT V` then `═ SPLIT H` (icons drawn: U+2550/2551 are in no face
    // until I-5's mono re-subset).
    for (hit, text, vertical) in [(RailHit::SplitV, "SPLIT V", true), (RailHit::SplitH, "SPLIT H", false)] {
        let label = if narrow { None } else { Some(tracked(gs, body, bpx, text)) };
        let lw = label.as_ref().map_or(0, |r| igap + r.width);
        let bw = 2 * border + 2 * pad + icon_w + lw;
        let bx = xr - bw;
        frame(&mut cart, hit, bx, bw);
        let color = button_ink(hit);
        let ix = bx + border + pad;
        let u = sheet.ipx(1).max(1);
        if vertical {
            // Two 1 px columns 2 apart, 13 tall from the box's row 6.
            for dx in [2, 4] {
                rect(&mut cart, ix + dx * u, by + sheet.ipx(6), u, sheet.ipx(13), color);
            }
        } else {
            // Two 1 px rows 2 apart, the icon's 8 wide, at the box's rows 12 and 14.
            for dy in [12, 14] {
                rect(&mut cart, ix, by + dy * u, icon_w, u, color);
            }
        }
        if let Some(r) = &label {
            push(&mut cart, gen, ix + icon_w + igap, label_base, color, r);
        }
        z.buttons.push((hit, (bx, by, bw, bh)));
        xr = bx - bgap;
    }
    let actions_x = xr + bgap;

    // --- The context (14.3), in what is left ----------------------------------
    if !narrow && !(m.cwd.is_empty() && m.title.is_empty()) {
        let ctx_x = pad_l + cluster_w;
        // The three runs lay on ONE pen (`tracked_from`), whose whole width
        // can exceed the sum of their separately truncated measures by up
        // to two pixels (three carried fractions); the cuts below measure
        // the parts, so the composite reserves that slack against the gap.
        const CARRY_SLACK: i32 = 2;
        let avail = actions_x - sheet.ipx(RAIL_GAP) - ctx_x - CARRY_SLACK;
        if avail > 0 {
            let cwd = m.cwd.to_uppercase();
            let title = m.title.to_uppercase();
            let (lead, base_seg) = match cwd.rfind('/') {
                Some(p) if p + 1 < cwd.len() => (String::from(&cwd[..=p]), String::from(&cwd[p + 1..])),
                Some(_) => (cwd.clone(), String::new()),
                None => (String::new(), cwd.clone()),
            };
            let sep_w = if !cwd.is_empty() && !title.is_empty() {
                2 * sheet.ipx(SEP_MARGIN) + hair
            } else {
                0
            };
            let base_w = width_of(gs, medium, px, &base_seg);
            let title_w = width_of(gs, body, px, &title);
            // The cwd's leading segments give way first (middle-ellipsised),
            // then the title is cut from its end; the basename stays whole
            // while anything else can yield.
            let lead_avail = avail - base_w - sep_w - title_w;
            let lead = if lead.is_empty() { lead } else { fit_middle(gs, body, px, &lead, lead_avail) };
            let lead_w = width_of(gs, body, px, &lead);
            let mut sep_w = sep_w;
            let mut title = fit_end(gs, body, px, &title, avail - lead_w - base_w - sep_w);
            if title.is_empty() {
                sep_w = 0;
            }
            let base_seg = if lead_w + base_w + sep_w > avail {
                fit_end(gs, medium, px, &base_seg, avail - lead_w - sep_w)
            } else {
                base_seg
            };
            let base = centred_in(gs, body, px, ch);
            let mut x = ctx_x;
            let (r, rem) = tracked_from(gs, body, px, &lead, 0);
            push(&mut cart, gen, x, base, i.dim, &r);
            x += r.width;
            let (r, rem) = tracked_from(gs, medium, px, &base_seg, rem);
            push(&mut cart, gen, x, base, i.text, &r);
            x += r.width;
            if sep_w > 0 {
                let sm = sheet.ipx(SEP_MARGIN);
                let sh = sheet.ipx(SEP_H);
                rect(&mut cart, x + sm, centre(ch, sh), hair, sh, i.structure);
                x += sep_w;
            }
            let (r, _) = tracked_from(gs, body, px, &title, rem);
            push(&mut cart, gen, x, base, i.secondary, &r);
            x += r.width;
            if title.is_empty() {
                title.clear();
            }
            z.ctx = (ctx_x, x - ctx_x);
        }
    }
    (cart, z)
}

/// A chord hint (8.2): the chord's label (`SUPER + ARROWS`) and the
/// action's (`FOCUS`).
pub type Hint = (String, String);

/// `super+shift+tab` -> `SUPER + SHIFT + TAB`.
fn combo_label(combo: &str) -> String {
    let mut out = String::new();
    for (k, part) in combo.split('+').enumerate() {
        if k > 0 {
            out.push_str(" + ");
        }
        out.push_str(&part.to_uppercase());
    }
    out
}

/// 8.2: the footer's chord hints from the compositor's `chords` file
/// (`super+[shift+]<key> <action>`, one binding per line) -- OUR chords,
/// generated from the bindings in force, never literals: `SUPER + ARROWS
/// FOCUS` when the four focus actions sit on the four arrow keys, `SUPER +
/// <combo> TILES` for whatever `cycle` is bound to. An unbound action has
/// no hint; a malformed line is skipped.
pub fn hints_from_chords(text: &str) -> Vec<Hint> {
    let binds: Vec<(&str, &str)> = text
        .lines()
        .take(CHORD_LINES_MAX)
        .filter_map(|l| {
            let mut it = l.split_ascii_whitespace();
            let combo = it.next()?;
            let action = it.next()?;
            if it.next().is_some() || !combo.starts_with("super+") {
                return None;
            }
            Some((combo, action))
        })
        .collect();
    let bound = |combo: &str, action: &str| binds.iter().any(|b| *b == (combo, action));
    let combo_of = |action: &str| binds.iter().find(|b| b.1 == action).map(|b| b.0);
    let mut out = Vec::new();
    if bound("super+left", "focus-left")
        && bound("super+right", "focus-right")
        && bound("super+up", "focus-up")
        && bound("super+down", "focus-down")
    {
        out.push((String::from("SUPER + ARROWS"), String::from("FOCUS")));
    }
    if let Some(c) = combo_of("cycle") {
        out.push((combo_label(c), String::from("TILES")));
    }
    out
}

/// 8.2: the command a footer label may carry -- control characters
/// blanked, at most `CMD_MAX` characters, uppercase.
pub fn sanitise_cmd(cmd: &str) -> String {
    cmd.chars()
        .map(|c| if c.is_control() { ' ' } else { c })
        .take(CMD_MAX)
        .collect::<String>()
        .to_uppercase()
}

/// The footer's four conditions (8.2), from the model's facts.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum FooterState {
    Ready,
    Running,
    Success,
    Failure,
}

pub fn footer_state(m: &StatusModel) -> FooterState {
    if m.running {
        FooterState::Running
    } else {
        match m.condition {
            Condition::Err => FooterState::Failure,
            Condition::Ok if !m.cmd.trim().is_empty() => FooterState::Success,
            _ => FooterState::Ready,
        }
    }
}

/// The footer's label for its state: `READY`, `RUNNING · <cmd>`, `EXIT 0 ·
/// <cmd>`, `EXIT <n> · <cmd>` (the code alone when none is known, the
/// command alone when none is).
pub fn footer_label(m: &StatusModel) -> String {
    let cmd = sanitise_cmd(m.cmd.trim());
    let mut s = match footer_state(m) {
        FooterState::Ready => return String::from(READY),
        FooterState::Running => String::from("RUNNING"),
        FooterState::Success => String::from("EXIT 0"),
        FooterState::Failure => match m.exit_code {
            Some(n) if n != 0 => {
                let mut t = String::from("EXIT ");
                let _ = core::fmt::write(&mut t, format_args!("{}", n));
                t
            }
            _ => String::from("EXIT"),
        },
    };
    if !cmd.is_empty() {
        s.push_str(" \u{b7} ");
        s.push_str(&cmd);
    }
    s
}

/// The bottom rail's display list under Instrument (8.2 / 14.3) for a `w`
/// x `h` surface at the sheet's scale, and where the slots landed (`cond`
/// = the glyph box through the label, `ctx` = the centre hints, `clock` =
/// the right group; `ws` unused). Left: the 6 x 6 glyph box -- hollow
/// `secondary` when READY, a filled 4 x 4 `amber` square when running, `✓`
/// in `success`, `!` in `error` -- gap 8, the label in `secondary` (a
/// transient notice takes the slot instead: uppercase, `amber` for an
/// action, `error` for a refusal, no glyph). Centre: the hints, `dim` /
/// `secondary` by position with `·` in `structure`, centred between the
/// end groups, hidden at or below 820 wide. Right: `<n> PANES`, a 1 x 12
/// `structure` separator with 10 px margins, the host name or `LOCAL`.
pub fn footer_list(
    m: &StatusModel,
    w: u32,
    h: u32,
    sheet: &Sheet,
    gs: &mut GlyphSource,
) -> (Cartoon, Slots) {
    // The source follows the sheet in force at every painter entry (r2 A-F2).
    gs.set_kerning(sheet.kerning);
    let mut cart = Cartoon::new();
    let mut slots = Slots::default();
    if w == 0 || h == 0 {
        return (cart, slots);
    }
    let i = &sheet.inst;
    let (wi, hi) = (w as i32, h as i32);
    let hair = sheet.hairline.max(1);
    let oy = hair;
    let ch = hi - hair;
    let narrow = wi <= sheet.ipx(NARROW_W);
    let gen = gs.gen();
    // 8.2: mono 10 / 500 (9 narrow) -- `face_mono_text` at the footer's
    // size, the Regular serving the mockup's 500 (ruling 11).
    let mono = sheet.face_mono_text;
    let logical_px = if narrow { FOOTER_NARROW_PX } else { FOOTER_PX };
    let mono_px = sheet.px(logical_px);
    cart.ops.push(Op::Clear { color: i.rail });
    rect(&mut cart, 0, 0, wi, hair, i.structure);
    let base = oy + centred_in(gs, mono, mono_px, ch);
    let pad = sheet.ipx(FOOT_PAD);

    // --- Right: the pane count, the separator, the host ------------------------
    let host = m.host.as_deref().unwrap_or("LOCAL").to_uppercase();
    let host_run = tracked(gs, mono, mono_px, &host);
    let host_x = wi - pad - host_run.width;
    push(&mut cart, gen, host_x, base, i.secondary, &host_run);
    let sm = sheet.ipx(SEP_MARGIN);
    let sh = sheet.ipx(SEP_H);
    let sep_x = host_x - sm - hair;
    rect(&mut cart, sep_x, oy + centre(ch, sh), hair, sh, i.structure);
    let mut panes = String::new();
    let _ = core::fmt::write(
        &mut panes,
        format_args!("{} PANE{}", m.pane_count, if m.pane_count == 1 { "" } else { "S" }),
    );
    let panes_run = tracked(gs, mono, mono_px, &panes);
    let panes_x = sep_x - sm - panes_run.width;
    push(&mut cart, gen, panes_x, base, i.secondary, &panes_run);
    slots.clock = (panes_x, wi - pad - panes_x);
    let right_start = panes_x;

    // --- Left: the condition, or the notice ---------------------------------------
    let gb = sheet.ipx(GLYPH_BOX);
    let gx = pad;
    let gy = oy + centre(ch, gb);
    let label_x = gx + gb + sheet.ipx(GLYPH_GAP);
    let (label, label_ink) = match &m.notice {
        Some((text, refusal)) => (text.to_uppercase(), if *refusal { i.error } else { i.amber }),
        None => {
            match footer_state(m) {
                FooterState::Ready => ring(&mut cart, gx, gy, gb, gb, hair, i.secondary),
                FooterState::Running => {
                    let sq = sheet.ipx(RUN_SQUARE);
                    rect(&mut cart, gx + centre(gb, sq), gy + centre(gb, sq), sq, sq, i.amber);
                }
                FooterState::Success => {
                    // 10 (amended at I-8): the sage glow is the SUCCESS
                    // square's ALONE -- 8.2 keeps RUNNING explicitly
                    // pulse-free and replaces the kit's sage-filled READY
                    // square with a hollow `secondary` one, so `success` is
                    // the one state where 10's literal sage is the right ink.
                    glow(&mut cart, gx, gy, gb, gb, i.success, GLOW_ALPHA, sheet.ipx(GLOW_BLUR));
                    let run = tracked(gs, mono, mono_px, "\u{2713}");
                    push(&mut cart, gen, (gx + (gb - run.width) / 2).max(0), base, i.success, &run);
                }
                FooterState::Failure => {
                    let run = tracked(gs, mono, mono_px, "!");
                    push(&mut cart, gen, (gx + (gb - run.width) / 2).max(0), base, i.error, &run);
                }
            }
            (footer_label(m), i.secondary)
        }
    };

    // --- Centre: the hints -----------------------------------------------------
    struct Span {
        run: Run,
        color: Argb,
    }
    let mut spans: Vec<Span> = Vec::new();
    if !narrow {
        let mut k = 0usize;
        for (n, (chord, action)) in m.hints.iter().enumerate() {
            if n > 0 {
                spans.push(Span {
                    run: tracked(gs, mono, mono_px, "\u{b7}"),
                    color: i.structure,
                });
                k += 1;
            }
            for text in [chord, action] {
                let color = if k % 2 == 0 { i.dim } else { i.secondary };
                spans.push(Span {
                    run: tracked(gs, mono, mono_px, text),
                    color,
                });
                k += 1;
            }
        }
    }
    let hgap = sheet.ipx(HINT_GAP);
    let centre_w = spans.iter().map(|s| s.run.width).sum::<i32>()
        + hgap * spans.len().saturating_sub(1) as i32;

    // The label takes what the centre leaves it: cut from its end.
    let label_limit = if !spans.is_empty() {
        // The centre needs its width plus a gap each side inside the free
        // span between the end groups; the label yields first.
        let free = right_start - hgap - label_x;
        free - centre_w - 2 * hgap
    } else {
        right_start - hgap - label_x
    };
    let label = fit_end(gs, mono, mono_px, &label, label_limit.max(0));
    let label_run = tracked(gs, mono, mono_px, &label);
    push(&mut cart, gen, label_x, base, label_ink, &label_run);
    let left_end = label_x + label_run.width;
    slots.cond = (gx, left_end - gx);

    if !spans.is_empty() && right_start - left_end - 2 * hgap >= centre_w {
        let mut x = left_end + (right_start - left_end - centre_w) / 2;
        slots.ctx = (x, centre_w);
        slots.ctx_ink = slots.ctx;
        for s in &spans {
            push(&mut cart, gen, x, base, s.color, &s.run);
            x += s.run.width + hgap;
        }
    }
    (cart, slots)
}

/// 9.5 Reset: the pane verbs that re-equalise every split's weights and
/// re-expand each stack's first tile, from the compositor's `layout` text
/// -- `(pane id, verb with its arguments)`, in order: `weight 1` for every
/// child of a split carrying a non-default weight, `focus` on the first
/// leaf of every stack whose open tile is not its first, then `focus` back
/// on the tile that held it (or, when that tile's own stack re-expanded,
/// its stack's first). Never restores a layout file, never respawns.
pub fn reset_plan(layout: &str) -> Vec<(u32, String)> {
    struct Row {
        depth: usize,
        id: u32,
        focused: bool,
        container: Option<(bool, u32)>, // (stacked, active)
        weight: u32,
    }
    let mut rows: Vec<Row> = Vec::new();
    for line in layout.lines() {
        if rows.len() >= RESET_ROWS_MAX {
            return Vec::new();
        }
        let depth = line.len() - line.trim_start().len();
        let mut it = line.trim_start().split_ascii_whitespace();
        let Some(head) = it.next() else { continue };
        let focused = head.ends_with('*');
        let Ok(id) = head.trim_end_matches('*').parse::<u32>() else { continue };
        let Some(kind) = it.next() else { continue };
        let mut n_active = 0u32;
        let mut weight = 1u32;
        for t in it.clone() {
            if let Some(v) = t.strip_prefix("active=") {
                n_active = v.parse().unwrap_or(0);
            } else if let Some(v) = t.strip_prefix("w=") {
                weight = v.parse().unwrap_or(1);
            }
        }
        let container = match kind {
            "leaf" => None,
            "stacked" | "tabbed" => Some((true, n_active)),
            _ => Some((false, n_active)),
        };
        rows.push(Row {
            depth: depth / 2,
            id,
            focused,
            container,
            weight,
        });
    }
    // The parent of row r: the nearest earlier row at depth - 1.
    let parent_of = |r: usize| -> Option<usize> {
        let d = rows[r].depth;
        if d == 0 {
            return None;
        }
        (0..r).rev().find(|&p| rows[p].depth + 1 == d)
    };
    // The direct children of row p, in order.
    let children_of = |p: usize| -> Vec<usize> {
        let d = rows[p].depth;
        let mut out = Vec::new();
        for r in p + 1..rows.len() {
            if rows[r].depth <= d {
                break;
            }
            if rows[r].depth == d + 1 {
                out.push(r);
            }
        }
        out
    };
    // The first leaf under row r (r itself when a leaf).
    let first_leaf = |mut r: usize| -> u32 {
        loop {
            if rows[r].container.is_none() {
                return rows[r].id;
            }
            match children_of(r).first() {
                Some(&c) => r = c,
                None => return rows[r].id,
            }
        }
    };
    let mut plan: Vec<(u32, String)> = Vec::new();
    for r in 0..rows.len() {
        if let Some(p) = parent_of(r) {
            if rows[p].container == Some((false, rows[p].container.map_or(0, |c| c.1))) && rows[r].weight != 1 {
                plan.push((rows[r].id, String::from("weight 1")));
            }
        }
    }
    let focused_row = rows.iter().position(|r| r.focused);
    let mut last_focus: Option<u32> = None;
    for r in 0..rows.len() {
        if let Some((true, active)) = rows[r].container {
            if active != 0 {
                if let Some(&c) = children_of(r).first() {
                    let id = first_leaf(c);
                    plan.push((id, String::from("focus")));
                    last_focus = Some(id);
                }
            }
        }
    }
    if last_focus.is_some() {
        if let Some(f) = focused_row {
            let target = match parent_of(f) {
                Some(p) if matches!(rows[p].container, Some((true, a)) if a != 0) => {
                    children_of(p).first().map_or(rows[f].id, |&c| first_leaf(c))
                }
                _ => rows[f].id,
            };
            if last_focus != Some(target) {
                plan.push((target, String::from("focus")));
            }
        }
    }
    plan
}

/// 8.2 / 14.3: the panes of a layout -- every stack of one and every stack
/// counted once (a tile that is its stack's first), less the leaves
/// `foreign` names: a leaf hosting a surface the owner does not describe
/// is a backgrounded system leaf (the console renderer a session stepped
/// back, sharing the root -- the dump does not mark it, so only the owner
/// can tell it from its own).
pub fn pane_count(tiles: &[crate::chrome::TileInfo], foreign: impl Fn(&crate::chrome::TileInfo) -> bool) -> u32 {
    tiles.iter().filter(|t| t.index == 1 && !foreign(t)).count() as u32
}

#[cfg(test)]
mod tests {
    use super::*;
    use libhalcyon::instrument::{Bundle, Profile};

    fn carbon() -> Sheet {
        crate::layout::sheet_for(&Bundle::builtin(Profile::Instrument), 100, crate::layout::TEST_DISPLAY_W)
    }

    fn model() -> RailModel {
        RailModel {
            workspaces: alloc::vec![1],
            active: 0,
            chip_scroll: 0,
            cwd: String::from("~/systems/compositor"),
            title: String::from("src / renderer.rs"),
            theme: String::from("Carbon Optics"),
            hour: 9,
            minute: 41,
        }
    }

    fn rects(c: &Cartoon) -> Vec<(i32, i32, u32, u32, u32)> {
        c.ops
            .iter()
            .filter_map(|o| match *o {
                Op::Rect { x, y, w, h, color } => Some((x, y, w, h, color)),
                _ => None,
            })
            .collect()
    }

    /// (x, y, w, h, colour, alpha, radius) per glow.
    fn glows(c: &Cartoon) -> Vec<(i32, i32, u32, u32, u32, u8, u32)> {
        c.ops
            .iter()
            .filter_map(|o| match *o {
                Op::Glow { x, y, w, h, color, alpha, radius } => Some((x, y, w, h, color, alpha, radius)),
                _ => None,
            })
            .collect()
    }

    /// (x, baseline, ink, glyph count, width) per glyph run.
    fn runs(c: &Cartoon) -> Vec<(i32, i32, u32, usize, i32)> {
        c.ops
            .iter()
            .filter_map(|op| match *op {
                Op::Glyphs {
                    baseline_x,
                    baseline_y,
                    color,
                    start,
                    count,
                    ..
                } => {
                    let g = &c.runs[start as usize..(start + count) as usize];
                    Some((baseline_x, baseline_y, color, g.len(), g.iter().map(|r| r.advance).sum()))
                }
                _ => None,
            })
            .collect()
    }

    const CARBON_RAIL: u32 = 0xFF07_090A;
    const CARBON_STRUCTURE: u32 = 0xFF45_4B48;
    const CARBON_AMBER: u32 = 0xFFC7_B98B;
    const CARBON_AMBER_MUTED: u32 = 0xFF81_785D;
    const CARBON_TEXT: u32 = 0xFFF2_F3EF;
    const CARBON_SECONDARY: u32 = 0xFFAF_B4B0;
    const CARBON_DIM: u32 = 0xFF73_7A76;
    const CARBON_HOVER: u32 = 0xFF19_1C1D;
    const CARBON_ERROR: u32 = 0xFFBD_7770;

    /// 8.1 at 100 % on the golden's 1440 x 34: the `rail` ground and the
    /// `structure` last row; the mark's ring at (10, 10) 13 x 13 with its
    /// strokes at (14, 14) 1 x 5 and (14, 18) 5 x 1; `WORKSPACE 01` from
    /// 32 in `text`; the context from 222 -- `~/SYSTEMS/` dim, `COMPOSITOR`
    /// text, a 1 x 12 separator at y 11 ten past it, the title in
    /// secondary; the clock ending 13 from the right edge (8 + 5); `?` 28
    /// wide, every button 26 tall at y 4, 2 apart, laid right to left; the
    /// swatch 8 x 8 at y 13 with its ring.
    #[test]
    fn the_top_rail_lays_out_the_goldens_boxes_at_100() {
        let s = carbon();
        let mut gs = GlyphSource::new_vendored(64);
        let (c, z) = rail_list(&model(), RailInk::default(), 1440, 34, &s, &mut gs);
        assert!(matches!(c.ops[0], Op::Clear { color: CARBON_RAIL }));
        let r = rects(&c);
        assert!(r.contains(&(0, 33, 1440, 1, CARBON_STRUCTURE)), "the structure line is the last row");
        // The mark: the ring's four edges and the two strokes.
        assert!(r.contains(&(10, 10, 13, 1, CARBON_AMBER_MUTED)));
        assert!(r.contains(&(10, 22, 13, 1, CARBON_AMBER_MUTED)));
        assert!(r.contains(&(10, 10, 1, 13, CARBON_AMBER_MUTED)));
        assert!(r.contains(&(22, 10, 1, 13, CARBON_AMBER_MUTED)));
        assert!(r.contains(&(14, 14, 1, 5, CARBON_AMBER)), "the vertical stroke: {:?}", r);
        assert!(r.contains(&(14, 18, 5, 1, CARBON_AMBER)), "the horizontal stroke");
        let g = runs(&c);
        let label = g.iter().find(|x| x.0 == 32).expect("the brand label at 32");
        assert_eq!(label.2, CARBON_TEXT);
        assert_eq!(label.3, "WORKSPACE 01".chars().count());
        assert!(label.1 > 12 && label.1 < 24, "the 11 px baseline sits in the 33 rows: {}", label.1);
        assert_eq!(z.brand.0, 10);
        // The context: three runs from 222, then the separator 10 past the
        // basename, then the title 10 past the separator.
        assert_eq!(z.ctx.0, 222);
        let lead = g.iter().find(|x| x.0 == 222).expect("the cwd's lead at 222");
        assert_eq!(lead.2, CARBON_DIM);
        assert_eq!(lead.3, "~/SYSTEMS/".chars().count());
        let base = g.iter().find(|x| x.0 == 222 + lead.4).expect("the basename after the lead");
        assert_eq!(base.2, CARBON_TEXT);
        assert_eq!(base.3, "COMPOSITOR".chars().count());
        let sep_x = 222 + lead.4 + base.4 + 10;
        assert!(r.contains(&(sep_x, 11, 1, 12, CARBON_STRUCTURE)), "the 1 x 12 separator at y 11: {:?}", r);
        let title = g.iter().find(|x| x.0 == sep_x + 1 + 10).expect("the title after the separator");
        assert_eq!(title.2, CARBON_SECONDARY);
        assert_eq!(title.3, "SRC / RENDERER.RS".chars().count());
        assert_eq!(z.ctx.1, title.0 + title.4 - 222);
        // The clock: five mono glyphs in `text`, ending 13 from the edge.
        let clock = g.iter().find(|x| x.0 == z.clock.0).expect("the clock run");
        assert_eq!(clock.2, CARBON_TEXT);
        assert_eq!(clock.3, 5);
        assert_eq!(z.clock.0 + z.clock.1, 1440 - 8 - 5);
        // The buttons, right to left: help 28 wide, then reset, theme,
        // split v, split h -- each 26 tall at y 4, 2 apart, ending 10 before
        // the clock's run.
        let names: Vec<RailHit> = z.buttons.iter().map(|b| b.0).collect();
        assert_eq!(
            names,
            alloc::vec![RailHit::Help, RailHit::Reset, RailHit::Theme, RailHit::SplitV, RailHit::SplitH]
        );
        let help = z.buttons[0].1;
        assert_eq!((help.1, help.2, help.3), (4, 28, 26));
        assert_eq!(help.0 + help.2, z.clock.0 - 10, "the help box ends where the clock's padding starts");
        for w in z.buttons.windows(2) {
            let (a, b) = (w[0].1, w[1].1);
            assert_eq!(b.0 + b.2 + 2, a.0, "2 px between {:?} and {:?}", w[1].0, w[0].0);
            assert_eq!((b.1, b.3), (4, 26));
        }
        // The swatch: amber 8 x 8 at y 13 inside the theme button, ringed.
        let theme = z.buttons[2].1;
        let sx = theme.0 + 1 + 9;
        assert!(r.contains(&(sx, 13, 8, 8, CARBON_AMBER)), "the swatch at ({}, 13): {:?}", sx, r);
        assert!(r.contains(&(sx, 13, 8, 1, s.derived.swatch_ring)), "its ring's top edge");
        // The split icons are drawn marks in `secondary`: two 8-wide rows
        // for `═` at the box's rows 12 and 14, two 13-tall columns for `║`.
        let sh = z.buttons[4].1;
        let ix = sh.0 + 1 + 9;
        assert!(r.contains(&(ix, 4 + 12, 8, 1, CARBON_SECONDARY)) && r.contains(&(ix, 4 + 14, 8, 1, CARBON_SECONDARY)));
        let sv = z.buttons[3].1;
        let ix = sv.0 + 1 + 9;
        assert!(r.contains(&(ix + 2, 4 + 6, 1, 13, CARBON_SECONDARY)) && r.contains(&(ix + 4, 4 + 6, 1, 13, CARBON_SECONDARY)));
        // The type map's widths against the golden's DOM boxes (7.2, I-5;
        // `geometry-styles.json` at 1440 x 900 / 100%): the clock `09:41`
        // in mono 11 tracked .08 em is 31.9 (the `#clock` box's 46.906
        // less its 10 / 5 padding); the whole context 287.219 (Sans 11
        // tracked .88 px, the basename in 500); the button labels SPLIT H
        // 41.562, SPLIT V 40.594, CARBON OPTICS 87.281, RESET 33.594 in
        // Sans 400 at 10 tracked .8 px; the split-h BOX 76.453. Within a
        // pixel: the pen truncates its 1/256 remainder at the run's end
        // and the browser reports the fractional box. Before I-5 the clock
        // ran at the 12 px island cell (5 x 6 + tracking = 35) and the
        // labels in Text 450, so these are the type change's witnesses.
        let near = |got: i32, want: f32| (got as f32 - want).abs() <= 1.0;
        assert!(near(z.clock.1, 31.9), "the clock run: {} vs 31.9", z.clock.1);
        assert!(near(z.ctx.1, 287.219), "the context: {} vs 287.219", z.ctx.1);
        let label_in = |b: (i32, i32, i32, i32), n: usize| -> i32 {
            g.iter()
                .find(|x| x.3 == n && x.0 > b.0 && x.0 < b.0 + b.2)
                .map(|x| x.4)
                .unwrap_or_else(|| panic!("a {n}-glyph label inside {b:?}: {g:?}"))
        };
        assert!(near(label_in(sh, 7), 41.562), "SPLIT H: {}", label_in(sh, 7));
        assert!(near(label_in(sv, 7), 40.594), "SPLIT V: {}", label_in(sv, 7));
        assert!(near(label_in(theme, 13), 87.281), "CARBON OPTICS: {}", label_in(theme, 13));
        let reset = z.buttons[1].1;
        assert!(near(label_in(reset, 5), 33.594), "RESET: {}", label_in(reset, 5));
        assert!(near(sh.2, 76.453), "the split-h box: {}", sh.2);
        // The hit test names each target and nothing between them.
        assert_eq!(rail_hit(&z, 15, 15), Some(RailHit::Brand));
        assert_eq!(rail_hit(&z, 60, 15), Some(RailHit::Brand), "the label is the brand's too");
        assert_eq!(rail_hit(&z, help.0 + 5, 10), Some(RailHit::Help));
        assert_eq!(rail_hit(&z, help.0 + 5, 2), None, "above the 26 tall box");
        assert_eq!(rail_hit(&z, sh.0 + 5, 10), Some(RailHit::SplitH));
        assert_eq!(rail_hit(&z, 600, 15), None, "the context is no target");
        // No context, no separator: a rail with nothing focused.
        let (c2, z2) = rail_list(&RailModel { cwd: String::new(), title: String::new(), ..model() }, RailInk::default(), 1440, 34, &s, &mut gs);
        assert_eq!(z2.ctx, (0, 0));
        assert!(!rects(&c2).iter().any(|x| x.1 == 11 && x.3 == 12), "no separator without a context");
        assert!(rail_list(&model(), RailInk::default(), 0, 34, &s, &mut gs).0.ops.is_empty());
    }

    /// The pointer states: a hovered button gets the `hover` ground with
    /// `structure` sides and `text` ink; a pressed one `amber` ink.
    #[test]
    fn a_hovered_button_lights_and_a_pressed_one_goes_amber() {
        let s = carbon();
        let mut gs = GlyphSource::new_vendored(64);
        let hover = RailInk { hover: Some(RailHit::Reset), pressed: None };
        let (c, z) = rail_list(&model(), hover, 1440, 34, &s, &mut gs);
        let reset = z.buttons[1].1;
        let r = rects(&c);
        assert!(r.contains(&(reset.0, 4, reset.2 as u32, 26, CARBON_HOVER)), "the hover ground under the box");
        assert!(r.contains(&(reset.0, 4, 1, 26, CARBON_STRUCTURE)) && r.contains(&(reset.0 + reset.2 - 1, 4, 1, 26, CARBON_STRUCTURE)));
        let label = runs(&c).into_iter().find(|x| x.3 == 5 && x.0 > reset.0 && x.0 < reset.0 + reset.2).expect("RESET");
        assert_eq!(label.2, CARBON_TEXT);
        let pressed = RailInk { hover: Some(RailHit::Reset), pressed: Some(RailHit::Reset) };
        let (c, _) = rail_list(&model(), pressed, 1440, 34, &s, &mut gs);
        let label = runs(&c).into_iter().find(|x| x.3 == 5 && x.0 > reset.0 && x.0 < reset.0 + reset.2).expect("RESET");
        assert_eq!(label.2, CARBON_AMBER);
        // Unhovered: no ground, secondary ink.
        let (c, _) = rail_list(&model(), RailInk::default(), 1440, 34, &s, &mut gs);
        assert!(!rects(&c).iter().any(|x| x.4 == CARBON_HOVER));
    }

    /// 14.3: a long cwd loses its middle first (the basename stays), then
    /// the title is cut from its end; on a rail with no room for a title
    /// the separator goes too.
    #[test]
    fn the_context_middle_ellipsises_the_cwd_then_cuts_the_title() {
        let s = carbon();
        let mut gs = GlyphSource::new_vendored(64);
        let long = RailModel {
            cwd: String::from("~/projects/thylacine/usr/lib/libhalcyon/src/deeply/nested/instrument"),
            title: String::from("a rather long tile title that will not fit either"),
            ..model()
        };
        let (c, z) = rail_list(&long, RailInk::default(), 1000, 34, &s, &mut gs);
        let g = runs(&c);
        let lead = g.iter().find(|x| x.0 == 222).expect("the lead");
        assert!(lead.3 < "~/projects/thylacine/usr/lib/libhalcyon/src/deeply/nested/".len(), "the lead was cut");
        let base = g.iter().find(|x| x.0 == 222 + lead.4).expect("the basename");
        assert_eq!(base.3, "INSTRUMENT".len(), "the basename stays whole");
        let title = g.iter().filter(|x| x.2 == CARBON_SECONDARY && x.0 > base.0).last().expect("the title");
        assert!(title.3 < long.title.len(), "the title was cut");
        let actions_x = z.buttons.last().unwrap().1 .0;
        assert!(z.ctx.0 + z.ctx.1 <= actions_x - 8, "the context ends before the actions");
        // The same on the reference width fits whole (the golden's text).
        let (c, _) = rail_list(&model(), RailInk::default(), 1440, 34, &s, &mut gs);
        assert!(runs(&c).iter().any(|x| x.3 == "SRC / RENDERER.RS".len()));
    }

    /// 8.3 at 800 wide: the brand is the mark and the active number, no
    /// context, icon-only buttons (28 wide but the theme control), the same
    /// clock.
    #[test]
    fn the_narrow_rail_keeps_the_mark_the_number_and_the_icons() {
        let s = carbon();
        let mut gs = GlyphSource::new_vendored(64);
        let (c, z) = rail_list(&model(), RailInk::default(), 800, 34, &s, &mut gs);
        assert_eq!(z.ctx, (0, 0), "no context");
        let g = runs(&c);
        let num = g.iter().find(|x| x.0 == 32).expect("the active number at 32");
        assert_eq!((num.3, num.2), (2, CARBON_TEXT));
        assert!(z.brand.2 <= 54);
        for (hit, b) in &z.buttons {
            match hit {
                RailHit::Theme => assert_eq!(b.2, 2 + 18 + 8 + 7 + 8, "swatch + gap + chevron"),
                _ => assert_eq!(b.2, 28, "{:?} is icon-only", hit),
            }
        }
        assert!(!g.iter().any(|x| x.3 == 5 && x.2 == CARBON_SECONDARY), "no RESET / SPLIT labels");
        // At 821 the wide layout returns.
        let (_, z) = rail_list(&model(), RailInk::default(), 821, 34, &s, &mut gs);
        assert!(z.ctx.1 > 0);
        assert!(z.buttons.iter().any(|b| b.1 .2 > 60));
    }

    /// 14.1 with three workspaces: chips 26 x 24 at y 5, 4 apart, from 32;
    /// the active one on `hover` with the 2 px amber edge inset 4; with nine
    /// the strip scrolls -- ‹ › 16 wide at each end of a 157 viewport, the
    /// active chip always inside it.
    #[test]
    fn the_chips_lay_out_and_scroll_to_the_active_one() {
        let s = carbon();
        let mut gs = GlyphSource::new_vendored(64);
        let three = RailModel { workspaces: alloc::vec![1, 2, 3], active: 1, ..model() };
        let (c, z) = rail_list(&three, RailInk::default(), 1440, 34, &s, &mut gs);
        assert_eq!(z.chips.len(), 3);
        assert_eq!(z.chips[0].1, (32, 5, 26, 24));
        assert_eq!(z.chips[1].1, (62, 5, 26, 24));
        assert!(z.chips_prev.is_none());
        let r = rects(&c);
        assert!(r.contains(&(62, 5, 26, 24, CARBON_HOVER)), "the active chip's ground");
        assert!(r.contains(&(66, 5 + 24 - 2, 18, 2, CARBON_AMBER)), "its amber edge inset 4");
        assert_eq!(rail_hit(&z, 70, 10), Some(RailHit::Chip(1)));
        assert_eq!(rail_hit(&z, 15, 10), Some(RailHit::Brand));
        let nine = RailModel {
            workspaces: alloc::vec![1, 2, 3, 4, 5, 6, 7, 8, 9],
            active: 8,
            chip_scroll: 0,
            ..model()
        };
        let (_, z) = rail_list(&nine, RailInk::default(), 1440, 34, &s, &mut gs);
        assert_eq!(z.chips_prev, Some((32, 0, 16, 33)));
        assert_eq!(z.chips_next, Some((32 + 16 + 157, 0, 16, 33)));
        assert_eq!(z.chip_first, 4, "five chips fit; the ninth is shown");
        assert!(z.chips.iter().any(|c| c.0 == 8));
        assert!(z.chips.iter().all(|c| c.1 .0 >= 48 && c.1 .0 + 26 <= 48 + 157));
        assert_eq!(rail_hit(&z, 40, 10), Some(RailHit::ChipsPrev));
        assert_eq!(rail_hit(&z, 210, 10), Some(RailHit::ChipsNext));
        // The context still starts at 222 past the cluster.
        assert_eq!(z.ctx.0, 222);
    }

    fn footer_model() -> StatusModel {
        let mut m = StatusModel::empty();
        m.pane_count = 3;
        m.hints = alloc::vec![
            (String::from("SUPER + ARROWS"), String::from("FOCUS")),
            (String::from("SUPER + TAB"), String::from("TILES")),
        ];
        m
    }

    /// 10 (amended at I-8, operator-answered): the sage glow is the SUCCESS
    /// square's ALONE. The positive is one glow on the 6 x 6 glyph box in
    /// `success` at .25 / blur 8; the three NEGATIVES are what make this a
    /// witness of "success alone" rather than of "a glow exists at all" --
    /// 8.2 keeps RUNNING explicitly pulse-free and replaced the kit's
    /// sage-filled READY square with a hollow `secondary` one.
    #[test]
    fn the_sage_glow_belongs_to_the_success_square_alone() {
        let s = carbon();
        let mut gs = GlyphSource::new_vendored(64);
        let mut ok = footer_model();
        ok.condition = Condition::Ok;
        ok.cmd = String::from("make");
        let (c, _) = footer_list(&ok, 1440, 25, &s, &mut gs);
        assert_eq!(
            glows(&c),
            alloc::vec![(10, 10, 6, 6, s.inst.success, 64u8, 8u32)],
            "the success square's glow, on the golden's 6 x 6 at (10, 10)"
        );
        // UNDER the check, not over it: the executor paints in list order.
        let gi = c.ops.iter().position(|o| matches!(o, Op::Glow { .. })).unwrap();
        let ci = c
            .ops
            .iter()
            .position(|o| matches!(o, Op::Glyphs { color, .. } if *color == s.inst.success))
            .unwrap();
        assert!(gi < ci, "the glow is pushed before the check it sits under");

        // The three states that must carry NO glow.
        let ready = footer_model();
        let mut running = footer_model();
        running.condition = Condition::Ok;
        running.cmd = String::from("make");
        running.running = true;
        let mut failed = footer_model();
        failed.condition = Condition::Err;
        failed.cmd = String::from("make");
        for (name, m) in [("ready", ready), ("running", running), ("failure", failed)] {
            let (c, _) = footer_list(&m, 1440, 25, &s, &mut gs);
            assert!(glows(&c).is_empty(), "{} carries no glow", name);
        }
    }

    /// 8.2 at 100 % on the golden's 1440 x 25: the `structure` first row;
    /// the hollow `secondary` 6 x 6 at (10, 10) and `READY` at 24 while
    /// idle; the hints centred between the end groups, dim / secondary by
    /// position with the dot in `structure`; `3 PANES`, the 1 x 12
    /// separator with 10 px margins and `LOCAL` ending at 1430.
    #[test]
    fn the_footer_lays_out_the_goldens_boxes() {
        let s = carbon();
        let mut gs = GlyphSource::new_vendored(64);
        let (c, sl) = footer_list(&footer_model(), 1440, 25, &s, &mut gs);
        assert!(matches!(c.ops[0], Op::Clear { color: CARBON_RAIL }));
        let r = rects(&c);
        assert!(r.contains(&(0, 0, 1440, 1, CARBON_STRUCTURE)));
        assert!(r.contains(&(10, 10, 6, 1, CARBON_SECONDARY)) && r.contains(&(10, 15, 6, 1, CARBON_SECONDARY)));
        assert!(r.contains(&(10, 10, 1, 6, CARBON_SECONDARY)) && r.contains(&(15, 10, 1, 6, CARBON_SECONDARY)));
        assert!(!r.iter().any(|x| x.4 == CARBON_AMBER), "nothing amber while idle");
        let g = runs(&c);
        let ready = g.iter().find(|x| x.0 == 24).expect("READY at 24");
        assert_eq!((ready.2, ready.3), (CARBON_SECONDARY, 5));
        assert!(ready.1 > 1 && ready.1 < 25);
        assert_eq!(sl.cond.0, 10);
        // The right group.
        let local = g.iter().find(|x| x.3 == 5 && x.0 > 1300).expect("LOCAL");
        assert_eq!(local.0 + local.4, 1430);
        let sep = r.iter().find(|x| x.3 == 12 && x.0 > 1300).expect("the separator");
        assert_eq!((sep.0, sep.1, sep.2), (local.0 - 10 - 1, 1 + 6, 1));
        let panes = g.iter().find(|x| x.3 == 7 && x.0 > 1200).expect("3 PANES");
        assert_eq!(panes.0 + panes.4, sep.0 - 10);
        assert_eq!(sl.clock, (panes.0, 1430 - panes.0));
        // The footer's type against the golden's boxes (7.2 / 8.2, I-5):
        // mono 10 tracked .8 px -- `READY` 29.000 (`#status-text`), `3
        // PANES` 40.609 (`#pane-count`), `LOCAL` 29 (five glyphs), the
        // `·` 5.812; within a pixel of the fractional boxes. At the 12 px
        // island cell these were 34 / 47.6 / 34.
        let near = |got: i32, want: f32| (got as f32 - want).abs() <= 1.0;
        assert!(near(ready.4, 29.0), "READY: {}", ready.4);
        assert!(near(panes.4, 40.609), "3 PANES: {}", panes.4);
        assert!(near(local.4, 29.0), "LOCAL: {}", local.4);
        // The hints: five spans in position order, centred between the two
        // end groups.
        let centre: Vec<_> = g.iter().filter(|x| x.0 > 60 && x.0 < 1200).collect();
        assert_eq!(centre.len(), 5, "{:?}", centre);
        assert_eq!(centre[0].2, CARBON_DIM);
        assert_eq!(centre[1].2, CARBON_SECONDARY);
        assert_eq!(centre[2].2, CARBON_STRUCTURE);
        assert_eq!(centre[3].2, CARBON_SECONDARY);
        assert_eq!(centre[4].2, CARBON_DIM);
        assert!(near(centre[2].4, 5.812), "the dot: {}", centre[2].4);
        let left_end = ready.0 + ready.4;
        let (cx, cw) = sl.ctx;
        assert_eq!(cx, centre[0].0);
        assert!(((cx - left_end) - (panes.0 - (cx + cw))).abs() <= 1, "centred between the groups");
        // One pane reads singular.
        let (c, _) = footer_list(&StatusModel { pane_count: 1, ..footer_model() }, 1440, 25, &s, &mut gs);
        assert!(runs(&c).iter().any(|x| x.3 == "1 PANE".len() && x.0 > 1200));
        assert!(footer_list(&footer_model(), 0, 25, &s, &mut gs).0.ops.is_empty());
    }

    /// 8.2: the four conditions and the notice -- running fills a 4 x 4
    /// amber square and says `RUNNING · CMD`; success paints `✓` and `EXIT
    /// 0 · CMD`; a failure `!` in `error` and `EXIT n · CMD`; a notice takes
    /// the slot with no glyph, in its ink, uppercase.
    #[test]
    fn the_footers_conditions_and_the_notice() {
        let s = carbon();
        let mut gs = GlyphSource::new_vendored(64);
        let mut m = footer_model();
        m.cmd = String::from("make check");
        m.running = true;
        m.condition = Condition::Ok;
        assert_eq!(footer_state(&m), FooterState::Running);
        assert_eq!(footer_label(&m), "RUNNING \u{b7} MAKE CHECK");
        let (c, _) = footer_list(&m, 1440, 25, &s, &mut gs);
        assert!(rects(&c).contains(&(11, 11, 4, 4, CARBON_AMBER)), "the filled square: {:?}", rects(&c));
        m.running = false;
        m.exit_code = Some(0);
        assert_eq!(footer_state(&m), FooterState::Success);
        assert_eq!(footer_label(&m), "EXIT 0 \u{b7} MAKE CHECK");
        let (c, _) = footer_list(&m, 1440, 25, &s, &mut gs);
        assert!(runs(&c).iter().any(|x| x.2 == 0xFF81_9B85 && x.3 == 1 && x.0 < 24), "the check in `success`");
        m.condition = Condition::Err;
        m.exit_code = Some(2);
        assert_eq!(footer_label(&m), "EXIT 2 \u{b7} MAKE CHECK");
        let (c, _) = footer_list(&m, 1440, 25, &s, &mut gs);
        assert!(runs(&c).iter().any(|x| x.2 == CARBON_ERROR && x.3 == 1 && x.0 < 24), "the bang in `error`");
        m.exit_code = None;
        assert_eq!(footer_label(&m), "EXIT \u{b7} MAKE CHECK");
        // Nothing run yet is READY, whatever the pane's record says.
        let mut fresh = footer_model();
        fresh.condition = Condition::Ok;
        assert_eq!(footer_state(&fresh), FooterState::Ready);
        assert_eq!(footer_label(&fresh), "READY");
        // The notice.
        m.notice = Some((String::from("Final tile is protected"), true));
        let (c, _) = footer_list(&m, 1440, 25, &s, &mut gs);
        let g = runs(&c);
        let n = g.iter().find(|x| x.0 == 24).expect("the notice at the label's place");
        assert_eq!((n.2, n.3), (CARBON_ERROR, "FINAL TILE IS PROTECTED".len()));
        assert!(!g.iter().any(|x| x.0 < 24), "no glyph beside a notice");
        assert!(!rects(&c).iter().any(|x| x.4 == CARBON_SECONDARY && x.1 == 10), "no square either");
        m.notice = Some((String::from("Layout reset"), false));
        let (c, _) = footer_list(&m, 1440, 25, &s, &mut gs);
        assert!(runs(&c).iter().any(|x| x.0 == 24 && x.2 == CARBON_AMBER));
        // The command is sanitised and bounded.
        let mut long = footer_model();
        long.cmd = "x".repeat(200) + "\u{1b}y";
        let l = footer_label(&StatusModel { running: true, ..long });
        assert_eq!(l.chars().count(), "RUNNING \u{b7} ".chars().count() + CMD_MAX);
        assert!(!l.contains('\u{1b}'));
        assert_eq!(sanitise_cmd("a\tb"), "A B");
    }

    /// 8.3: at 800 wide the hints hide and the label still fits; a label
    /// too long for the room yields to the centre first.
    #[test]
    fn the_narrow_footer_hides_the_hints_and_the_label_yields() {
        let s = carbon();
        let mut gs = GlyphSource::new_vendored(64);
        let (c, sl) = footer_list(&footer_model(), 800, 25, &s, &mut gs);
        assert_eq!(sl.ctx, (0, 0));
        assert!(!runs(&c).iter().any(|x| x.2 == CARBON_STRUCTURE), "no dot");
        let mut m = footer_model();
        m.cmd = "a very long command line ".repeat(6);
        m.running = true;
        let (c, sl) = footer_list(&m, 1000, 25, &s, &mut gs);
        assert!(sl.ctx.1 > 0, "the hints stay");
        assert!(sl.cond.0 + sl.cond.1 <= sl.ctx.0 - 8, "the label ends before the centre");
        assert!(runs(&c).iter().any(|x| x.0 == 24 && x.3 < m.cmd.len()));
    }

    /// 8.2: the hints come from the bindings in force, never from a
    /// literal -- the arrows family, whatever `cycle` sits on, and nothing
    /// for an unbound action.
    #[test]
    fn the_hints_follow_the_chords_file() {
        let text = "super+left focus-left\nsuper+right focus-right\nsuper+up focus-up\nsuper+down focus-down\nsuper+tab cycle\nsuper+shift+q close\n";
        assert_eq!(
            hints_from_chords(text),
            alloc::vec![
                (String::from("SUPER + ARROWS"), String::from("FOCUS")),
                (String::from("SUPER + TAB"), String::from("TILES")),
            ]
        );
        let rebound = "super+left focus-left\nsuper+right focus-right\nsuper+up focus-up\nsuper+down focus-down\nsuper+shift+j cycle\n";
        assert_eq!(hints_from_chords(rebound)[1].0, "SUPER + SHIFT + J");
        let partial = "super+left focus-left\nsuper+tab cycle\n";
        assert_eq!(hints_from_chords(partial), alloc::vec![(String::from("SUPER + TAB"), String::from("TILES"))]);
        assert!(hints_from_chords("").is_empty());
        assert!(hints_from_chords("garbage line here\nsuper+\n").is_empty());
    }

    /// 9.5: the reset plan over a tree with a weighted split and two stacks
    /// -- `weight 1` for the weighted child only, `focus` on the first leaf
    /// of each stack whose open tile is not its first, then focus returned
    /// to the focused tile's stack first.
    #[test]
    fn the_reset_plan_equalises_weights_and_reexpands_first_tiles() {
        let tree = "epoch 9 focused 7\n1 splith n=2 active=1 [0,0,1280,800]\n  2 stacked n=2 active=1 [0,0,600,700] w=3\n    3 leaf surface=0 [0,0,0,0] hidden\n    4 leaf surface=1 [4,70,600,600]\n  5 stacked n=3 active=2 [0,0,0,0]\n    6 leaf surface=2 [0,0,0,0] hidden\n    7* leaf surface=3 [700,70,500,600]\n    8 leaf empty [0,0,0,0] hidden\n";
        assert_eq!(
            reset_plan(tree),
            alloc::vec![
                (2, String::from("weight 1")),
                (3, String::from("focus")),
                (6, String::from("focus")),
            ],
            "the focused tile's stack ends re-expanded on its first: no extra focus"
        );
        // Focus outside any stack: it comes back after the stacks re-expand.
        let tree = "epoch 9 focused 2\n1 splitv n=2 active=0 [0,0,1280,800]\n  2* leaf surface=0 [0,0,600,700] w=2\n  3 stacked n=2 active=1 [0,0,0,0] w=2\n    4 leaf surface=1 [0,0,0,0] hidden\n    5 leaf surface=2 [4,70,600,600]\n";
        assert_eq!(
            reset_plan(tree),
            alloc::vec![
                (2, String::from("weight 1")),
                (3, String::from("weight 1")),
                (4, String::from("focus")),
                (2, String::from("focus")),
            ]
        );
        // Already equal and every stack on its first: nothing to do.
        let tree = "epoch 1 focused 2\n1 splith n=2 active=0 [0,0,1,1]\n  2* leaf surface=0 [0,0,1,1]\n  3 stacked n=2 active=0 [0,0,1,1]\n    4 leaf surface=1 [0,0,1,1]\n    5 leaf surface=2 [0,0,0,0] hidden\n";
        assert!(reset_plan(tree).is_empty());
        assert!(reset_plan("").is_empty());
        // The pane count over the same trees: a stack counts once.
        let t = crate::chrome::parse_tree("epoch 1 focused 2\n1 splith n=2 active=0 [0,0,1,1]\n  2* leaf surface=0 [0,0,1,1]\n  3 stacked n=2 active=0 [0,0,1,1]\n    4 leaf surface=1 [0,0,1,1]\n    5 leaf surface=2 [0,0,0,0] hidden\n");
        assert_eq!(pane_count(&t, |_| false), 2);
        // A backgrounded system leaf beside the session's two: hosting a
        // surface the session does not describe -- not a pane.
        let t = crate::chrome::parse_tree("epoch 1 focused 3\n1 splith n=3 active=1 [0,0,1,1]\n  2 leaf surface=0 [0,0,0,0]\n  3* leaf surface=1 [0,0,1,1]\n  4 leaf surface=2 [0,0,1,1]\n");
        assert_eq!(pane_count(&t, |_| false), 3);
        assert_eq!(pane_count(&t, |x| x.leaf.surface.is_some() && x.leaf.id == 2), 2);
    }
}
