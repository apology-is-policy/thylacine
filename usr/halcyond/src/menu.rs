// menu -- the obj verb menu, the thinking half (HALCYON.md 5/6 + 13.6
// "Menus -- THE GATE"; BEACON.md 7). Pure: obj runs are read off the
// transcript model, the verb table comes in as parsed rules, a display list
// goes out. The surface, the `menu place` verb and the event pump are the
// bin's `menuset` (the 13.1 split).
//
// AN OBJ RUN is the cells of one transcript row that share one obj index
// (`Style.obj`, idx+1 into the block's obj table). An index is minted per
// `obj` frame and never shared, so the index IS the run's identity and the
// selection can name a run by (row, index) alone. `w`/`b` step runs across
// rows; Enter opens the menu for the selected run; a click on a run's
// glyphs opens the same menu (the last frame's laid geometry is the hit
// map). The menu shows the obj's TYPE and its RESOLVED ref first -- the
// anti-clickjack corollary: the text said one thing, the ref says another,
// the user sees the ref -- then the verbs the table offers for the type.

use alloc::string::String;
use alloc::vec::Vec;

use beacon::verbs::{expand, is_internal, rules_for, Rule};
use cartoon::{Cartoon, GlyphRef, Op};
use libhalcyon::theme::DAYLIGHT;

use crate::chrome::NAME_PX;
use crate::layout::LaidBlock;
use crate::raster::{GlyphSource, FACE_BODY, FACE_MONO};
use crate::select::FlatRow;
use crate::transcript::{Block, Item, TCell, Transcript};

/// One obj run on a row: the obj index (idx+1) and the run's text.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct ObjRun {
    pub obj: u16,
    pub text: String,
}

fn block_of(t: &Transcript, block: usize) -> Option<&Block> {
    if block == usize::MAX {
        Some(t.open_block())
    } else {
        t.frozen_blocks().get(block)
    }
}

fn push_cells(b: &Block, cells: &[TCell], runs: &mut Vec<ObjRun>, cur: &mut Option<ObjRun>) {
    for c in cells {
        let obj = b.styles.get(c.style as usize).map_or(0, |s| s.obj);
        match cur {
            Some(r) if r.obj == obj => r.text.push(c.ch),
            _ => {
                if let Some(r) = cur.take() {
                    runs.push(r);
                }
                if obj != 0 {
                    let mut text = String::new();
                    text.push(c.ch);
                    *cur = Some(ObjRun { obj, text });
                }
            }
        }
    }
}

/// The obj runs of one flat row, in cell order (a table row's cells are
/// walked in order; the plain realization's padding carries no obj).
pub fn runs_on_row(t: &Transcript, fr: FlatRow) -> Vec<ObjRun> {
    let mut runs = Vec::new();
    let b = match block_of(t, fr.block) {
        Some(b) => b,
        None => return runs,
    };
    let mut cur: Option<ObjRun> = None;
    match b.items.get(fr.item) {
        Some(Item::Line(l)) => push_cells(b, &l.cells, &mut runs, &mut cur),
        Some(Item::Table(tb)) => {
            if let Some(row) = tb.rows.get(fr.row) {
                for cell in row.iter() {
                    push_cells(b, cell, &mut runs, &mut cur);
                    if let Some(r) = cur.take() {
                        runs.push(r);
                    }
                }
            }
        }
        _ => {}
    }
    if let Some(r) = cur.take() {
        runs.push(r);
    }
    runs
}

/// The (type, resolved ref) of obj index `obj` in `block`.
pub fn obj_of(t: &Transcript, block: usize, obj: u16) -> Option<(&str, &str)> {
    let b = block_of(t, block)?;
    let o = b.objs.get((obj as usize).checked_sub(1)?)?;
    Some((o.ty.as_str(), o.refv.as_str()))
}

/// Step the run selection: from (cursor row, selected run or none) to the
/// next (`forward`) or previous run, crossing rows. None = no run that way.
pub fn step_run(
    t: &Transcript,
    flat: &[FlatRow],
    cursor: usize,
    cur: Option<u16>,
    forward: bool,
) -> Option<(usize, u16)> {
    if flat.is_empty() {
        return None;
    }
    let cursor = cursor.min(flat.len() - 1);
    // Within the cursor row first.
    let here = runs_on_row(t, flat[cursor]);
    let at = cur.and_then(|o| here.iter().position(|r| r.obj == o));
    let next_here = match (at, forward) {
        (Some(i), true) => here.get(i + 1),
        (Some(i), false) => i.checked_sub(1).and_then(|j| here.get(j)),
        (None, true) => here.first(),
        (None, false) => None,
    };
    if let Some(r) = next_here {
        return Some((cursor, r.obj));
    }
    // Then the following rows, in the step direction.
    if forward {
        for row in cursor + 1..flat.len() {
            if let Some(r) = runs_on_row(t, flat[row]).first() {
                return Some((row, r.obj));
            }
        }
    } else {
        for row in (0..cursor).rev() {
            if let Some(r) = runs_on_row(t, flat[row]).last() {
                return Some((row, r.obj));
            }
        }
    }
    None
}

/// The run's pixel rect within a laid block (block-relative): the union of
/// its segs across the (possibly wrapped) lines of its item/row. None when
/// the run laid nothing (evicted / clipped away).
pub fn run_rect(laid: &LaidBlock, item: usize, row: usize, obj: u16) -> Option<(i32, i32, i32, i32)> {
    let mut acc: Option<(i32, i32, i32, i32)> = None; // x0, y0, x1, y1
    for line in laid.lines.iter() {
        if line.src_item != item || line.src_row != row {
            continue;
        }
        for seg in line.segs.iter() {
            if seg.obj != obj || seg.refs.is_empty() {
                continue;
            }
            let (x0, y0, x1, y1) = (seg.x, line.y, seg.x_end, line.y + line.h);
            acc = Some(match acc {
                None => (x0, y0, x1, y1),
                Some((ax0, ay0, ax1, ay1)) => (ax0.min(x0), ay0.min(y0), ax1.max(x1), ay1.max(y1)),
            });
        }
    }
    acc.map(|(x0, y0, x1, y1)| (x0, y0, x1 - x0, y1 - y0))
}

/// Hit-test a block-relative point against a laid block: the obj run under
/// it, as (item, row, obj). None off any obj glyph.
pub fn hit_run(laid: &LaidBlock, x: i32, y: i32) -> Option<(usize, usize, u16)> {
    for line in laid.lines.iter() {
        if y < line.y || y >= line.y + line.h {
            continue;
        }
        for seg in line.segs.iter() {
            if seg.obj != 0 && x >= seg.x && x < seg.x_end {
                return Some((line.src_item, line.src_row, seg.obj));
            }
        }
    }
    None
}

/// What choosing a menu item does.
#[derive(Clone, PartialEq, Eq, Debug)]
pub enum Action {
    /// A command line for the shell (the expanded template).
    Command(String),
    /// A renderer-internal action (`#...`; test levers only).
    Internal(String),
}

#[derive(Clone, PartialEq, Eq, Debug)]
pub struct MenuItem {
    pub label: String,
    pub action: Action,
}

/// The open menu: the obj's type + resolved ref, the offered verbs, the
/// selected index.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct Menu {
    pub ty: String,
    pub refv: String,
    pub items: Vec<MenuItem>,
    pub sel: usize,
}

/// Build the menu for an obj from the verb table. A ref that cannot be
/// quoted safely (a control character) gets no command verbs at all --
/// only internal actions survive.
pub fn build_menu(rules: &[Rule], ty: &str, refv: &str) -> Menu {
    let mut items = Vec::new();
    for r in rules_for(rules, ty) {
        if is_internal(&r.template) {
            items.push(MenuItem { label: r.label.clone(), action: Action::Internal(r.template.clone()) });
        } else if let Some(cmd) = expand(&r.template, refv) {
            items.push(MenuItem { label: r.label.clone(), action: Action::Command(cmd) });
        }
    }
    Menu { ty: String::from(ty), refv: String::from(refv), items, sel: 0 }
}

/// A key on the menu surface.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum MenuKey {
    Up,
    Down,
    Enter,
    None,
}

/// Map a KEY press on the menu surface (rune-first, then evdev code).
pub fn menu_key(code: u16, rune: u32) -> MenuKey {
    match rune {
        0x6b => MenuKey::Up,          // k
        0x6a => MenuKey::Down,        // j
        0x0d | 0x0a => MenuKey::Enter, // Enter
        _ => match code {
            103 => MenuKey::Up,
            108 => MenuKey::Down,
            28 | 96 => MenuKey::Enter,
            _ => MenuKey::None,
        },
    }
}

impl Menu {
    /// A wheel delta (evdev REL_WHEEL: +1 = away from the user = up): the
    /// selection moves by it, clamped -- the window `menu_list` lays
    /// follows the selection, so a list taller than its surface scrolls.
    pub fn wheel(&mut self, delta: i32) {
        if self.items.is_empty() {
            return;
        }
        let last = self.items.len() - 1;
        self.sel = if delta > 0 {
            self.sel.saturating_sub(delta as usize)
        } else {
            self.sel.saturating_add(delta.unsigned_abs() as usize).min(last)
        };
    }

    /// Apply a key: Up/Down move the selection (clamped); Enter yields the
    /// selected item's action (None on an empty menu).
    pub fn key(&mut self, k: MenuKey) -> Option<Action> {
        match k {
            MenuKey::Up => {
                self.sel = self.sel.saturating_sub(1);
                None
            }
            MenuKey::Down => {
                if self.sel + 1 < self.items.len() {
                    self.sel += 1;
                }
                None
            }
            MenuKey::Enter => self.items.get(self.sel).map(|i| i.action.clone()),
            MenuKey::None => None,
        }
    }
}

/// Menu metrics (Daylight: the tag bar's padding family; the menu is the
/// raised ground with the border stroke -- chrome, not content).
pub const MENU_PAD_X: i32 = 8;
pub const MENU_PAD_Y: i32 = 4;
pub const MENU_MAX_W: u32 = 640;
const ROW_PAD: i32 = 4;
const NO_VERBS: &str = "no verbs";

fn body_width(gs: &mut GlyphSource, s: &str) -> i32 {
    s.chars().filter_map(|c| gs.glyph(FACE_BODY, NAME_PX, c)).map(|g| g.advance).sum()
}

fn mono_width(gs: &GlyphSource, s: &str) -> i32 {
    let (cw, _, _) = gs.mono_cell();
    cw * s.chars().count() as i32
}

fn row_h(gs: &GlyphSource) -> i32 {
    let (_, ch, _) = gs.mono_cell();
    ch + ROW_PAD
}

/// The menu's surface size for its content: the widest of the title (type
/// label + ref) and the items, padded; one row per item (or the "no verbs"
/// row) under the title row and its rule. Capped at MENU_MAX_W wide and at
/// `max_h` tall (the display: the compositor refuses a taller surface -- the
/// H-3c round F3); past the cap the item list scrolls (`menu_list`).
pub fn menu_size(m: &Menu, gs: &mut GlyphSource, max_h: u32) -> (u32, u32) {
    let (cw, _, _) = gs.mono_cell();
    let title_w = body_width(gs, &m.ty) + 2 * cw + mono_width(gs, &m.refv);
    let mut w = title_w;
    if m.items.is_empty() {
        w = w.max(mono_width(gs, NO_VERBS));
    }
    for it in m.items.iter() {
        w = w.max(mono_width(gs, &it.label));
    }
    let rows = 1 + m.items.len().max(1) as i32;
    let h = 2 * MENU_PAD_Y + rows * row_h(gs) + 1;
    let w = (w + 2 * MENU_PAD_X).max(1) as u32;
    (w.min(MENU_MAX_W), (h.max(1) as u32).min(max_h.max(1)))
}

/// How many item rows fit under the title row and its rule in `h`, and the
/// first item shown so the selection stays inside them.
pub fn item_window(m: &Menu, h: u32, gs: &GlyphSource) -> (usize, usize) {
    let rh = row_h(gs).max(1);
    let fit = ((h as i32 - 2 * MENU_PAD_Y - 1 - rh) / rh).max(1) as usize;
    let first = if m.sel >= fit { m.sel + 1 - fit } else { 0 };
    (first, fit)
}

fn push_body(cart: &mut Cartoon, gs: &mut GlyphSource, x: i32, baseline: i32, color: u32, s: &str) -> i32 {
    let mut refs: Vec<GlyphRef> = Vec::new();
    for c in s.chars() {
        if let Some(g) = gs.glyph(FACE_BODY, NAME_PX, c) {
            refs.push(g);
        }
    }
    let adv: i32 = refs.iter().map(|g| g.advance).sum();
    if !refs.is_empty() {
        cart.push_glyphs(gs.gen(), x, baseline, color, &refs);
    }
    adv
}

fn push_mono(cart: &mut Cartoon, gs: &mut GlyphSource, x: i32, baseline: i32, color: u32, s: &str) {
    let mut refs: Vec<GlyphRef> = Vec::new();
    for c in s.chars() {
        if let Some(g) = gs.glyph(FACE_MONO, 0.0, c) {
            refs.push(g);
        }
    }
    if !refs.is_empty() {
        cart.push_glyphs(gs.gen(), x, baseline, color, &refs);
    }
}

/// The menu display list for a w x h surface: raised ground, 1px border
/// stroke, the title row (type in the proportional face, muted; the
/// resolved ref in monospace, full ink), a rule, then the items in
/// monospace -- the selected one on a `header` band.
pub fn menu_list(m: &Menu, w: u32, h: u32, gs: &mut GlyphSource) -> Cartoon {
    let d = &DAYLIGHT;
    let mut cart = Cartoon::new();
    if w == 0 || h == 0 {
        return cart;
    }
    cart.ops.push(Op::Clear { color: d.raised });
    let (wi, hi) = (w as i32, h as i32);
    for r in [(0, 0, w, 1), (0, hi - 1, w, 1), (0, 0, 1, h), (wi - 1, 0, 1, h)] {
        cart.ops.push(Op::Rect { x: r.0, y: r.1, w: r.2, h: r.3, color: d.border });
    }
    let (cw, _, mono_base) = gs.mono_cell();
    let rh = row_h(gs);
    let body_asc = gs.line_metrics(FACE_BODY, NAME_PX).map(|lm| lm.ascent).unwrap_or(8);
    let mut y = MENU_PAD_Y;
    // Title: "<type>  <ref>".
    let mut x = MENU_PAD_X;
    x += push_body(&mut cart, gs, x, y + ROW_PAD / 2 + body_asc, d.fg_muted, &m.ty);
    x += 2 * cw;
    push_mono(&mut cart, gs, x, y + ROW_PAD / 2 + mono_base, d.fg, &m.refv);
    y += rh;
    cart.ops.push(Op::Rect { x: 1, y, w: w - 2, h: 1, color: d.border });
    y += 1;
    if m.items.is_empty() {
        push_mono(&mut cart, gs, MENU_PAD_X, y + ROW_PAD / 2 + mono_base, d.fg_muted, NO_VERBS);
        return cart;
    }
    let (first, fit) = item_window(m, h, gs);
    for (i, it) in m.items.iter().enumerate().skip(first).take(fit) {
        if i == m.sel {
            cart.ops.push(Op::Rect { x: 1, y, w: w - 2, h: rh as u32, color: d.header });
        }
        let ink = match it.action {
            Action::Command(_) => d.fg,
            Action::Internal(_) => d.fg_dim,
        };
        push_mono(&mut cart, gs, MENU_PAD_X, y + ROW_PAD / 2 + mono_base, ink, &it.label);
        y += rh;
    }
    cart
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::layout::{daylight_sheet, layout_block};
    use crate::select::flatten;
    use beacon::verbs::parse;
    use beacon::wire::{self, Op as BOp};

    fn corpus() -> Transcript {
        let mut t = Transcript::new(libhalcyon::theme::daylight_palette());
        let mut buf = Vec::new();
        wire::open(&mut buf, BOp::Zone, &[("k", "prompt")]);
        buf.extend_from_slice(b"$ ls /lib\n");
        wire::close(&mut buf, BOp::Zone);
        wire::open(&mut buf, BOp::Zone, &[("k", "output")]);
        wire::open(&mut buf, BOp::Obj, &[("type", "path"), ("ref", "/lib/aurora")]);
        buf.extend_from_slice(b"aurora");
        wire::close(&mut buf, BOp::Obj);
        buf.extend_from_slice(b"  ");
        wire::open(&mut buf, BOp::Obj, &[("type", "path"), ("ref", "/lib/ndb")]);
        buf.extend_from_slice(b"ndb");
        wire::close(&mut buf, BOp::Obj);
        buf.extend_from_slice(b"\nplain line\n");
        wire::open(&mut buf, BOp::Table, &[("cols", "lr"), ("hdr", "0")]);
        wire::open(&mut buf, BOp::Row, &[]);
        wire::open(&mut buf, BOp::Cell, &[]);
        wire::open(&mut buf, BOp::Obj, &[("type", "pid"), ("ref", "42")]);
        buf.extend_from_slice(b"42");
        wire::close(&mut buf, BOp::Obj);
        wire::close(&mut buf, BOp::Cell);
        wire::open(&mut buf, BOp::Cell, &[]);
        buf.extend_from_slice(b"ut");
        wire::close(&mut buf, BOp::Cell);
        wire::close(&mut buf, BOp::Row);
        buf.extend_from_slice(b"\n");
        wire::close(&mut buf, BOp::Table);
        wire::close(&mut buf, BOp::Zone);
        t.feed(&buf);
        t
    }

    #[test]
    fn runs_are_per_obj_index_in_cell_order() {
        let t = corpus();
        let flat = flatten(&t);
        // rows: "$ ls /lib", "aurora  ndb", "plain line", the table row.
        assert_eq!(flat.len(), 4, "{:?}", flat);
        let r = runs_on_row(&t, flat[1]);
        assert_eq!(r.len(), 2);
        assert_eq!(r[0].text, "aurora");
        assert_eq!(r[1].text, "ndb");
        assert_ne!(r[0].obj, r[1].obj, "an obj index is minted per frame");
        assert_eq!(obj_of(&t, flat[1].block, r[0].obj), Some(("path", "/lib/aurora")));
        assert_eq!(obj_of(&t, flat[1].block, r[1].obj), Some(("path", "/lib/ndb")));
        assert!(runs_on_row(&t, flat[2]).is_empty(), "a plain line has no runs");
        let tr = runs_on_row(&t, flat[3]);
        assert_eq!(tr.len(), 1, "the table row's obj cell is one run");
        assert_eq!(obj_of(&t, flat[3].block, tr[0].obj), Some(("pid", "42")));
        assert_eq!(obj_of(&t, flat[3].block, 0), None, "0 is the no-obj sentinel");
    }

    #[test]
    fn stepping_crosses_rows_both_ways() {
        let t = corpus();
        let flat = flatten(&t);
        let (r1, a) = step_run(&t, &flat, 0, None, true).expect("first run forward from the prompt row");
        assert_eq!(r1, 1);
        let (r2, b) = step_run(&t, &flat, r1, Some(a), true).unwrap();
        assert_eq!((r2, b == a), (1, false), "the second run on the same row");
        let (r3, c) = step_run(&t, &flat, r2, Some(b), true).unwrap();
        assert_eq!(r3, 3, "then the table row's run, skipping the plain line");
        assert_eq!(step_run(&t, &flat, r3, Some(c), true), None, "nothing past the last run");
        assert_eq!(step_run(&t, &flat, r3, Some(c), false), Some((1, b)), "back lands on the LAST run of the previous obj row");
        assert_eq!(step_run(&t, &flat, 1, Some(b), false), Some((1, a)));
        assert_eq!(step_run(&t, &flat, 1, Some(a), false), None);
        assert_eq!(step_run(&t, &flat, 3, None, false), Some((1, b)), "no run selected: back skips the row and lands on the previous obj row's last run");
        assert_eq!(step_run(&t, &flat, 2, None, true), Some((3, c)), "forward from a plain row finds the next obj row");
    }

    #[test]
    fn run_rect_and_hit_agree_on_the_laid_geometry() {
        let t = corpus();
        let flat = flatten(&t);
        let b = &t.frozen_blocks()[flat[1].block];
        let sheet = daylight_sheet();
        let mut gs = GlyphSource::new_vendored(64);
        let laid = layout_block(b, 800, &sheet, &mut gs);
        let runs = runs_on_row(&t, flat[1]);
        let (x, y, w, h) = run_rect(&laid, flat[1].item, flat[1].row, runs[1].obj).expect("ndb laid");
        assert!(w > 0 && h > 0);
        let hit = hit_run(&laid, x + w / 2, y + h / 2);
        assert_eq!(hit, Some((flat[1].item, flat[1].row, runs[1].obj)), "the rect's centre hits its own run");
        let (ax, _, aw, _) = run_rect(&laid, flat[1].item, flat[1].row, runs[0].obj).unwrap();
        assert!(ax + aw <= x, "aurora lies left of ndb");
        assert_eq!(hit_run(&laid, x - 1, y + h / 2), None, "the padding between runs hits nothing");
    }

    #[test]
    fn menu_shows_the_resolved_ref_and_the_typed_verbs() {
        let rules = parse("path ls ls -l {}\npath cat cat {}\npid kill kill {}\npath t #wedge 10\n", true);
        let m = build_menu(&rules, "path", "/lib/o'k");
        assert_eq!(m.refv, "/lib/o'k", "the ref is displayed verbatim");
        let labels: Vec<&str> = m.items.iter().map(|i| i.label.as_str()).collect();
        assert_eq!(labels, ["ls", "cat", "t"]);
        assert_eq!(m.items[0].action, Action::Command(String::from("ls -l '/lib/o''k'")));
        assert_eq!(m.items[2].action, Action::Internal(String::from("#wedge 10")));
        let none = build_menu(&rules, "commit", "abc");
        assert!(none.items.is_empty());
        let unsafe_ref = build_menu(&rules, "path", "a\nb");
        assert_eq!(unsafe_ref.items.len(), 1, "only the internal action survives an unquotable ref");
    }

    #[test]
    fn keys_move_clamped_and_enter_chooses() {
        let rules = parse("path ls ls {}\npath cat cat {}\n", false);
        let mut m = build_menu(&rules, "path", "/x");
        assert_eq!(m.key(menu_key(0, 0x6b)), None);
        assert_eq!(m.sel, 0, "up at the top stays");
        m.key(menu_key(108, 0));
        m.key(menu_key(108, 0));
        assert_eq!(m.sel, 1, "down clamps at the last item");
        assert_eq!(m.key(menu_key(28, 0x0d)), Some(Action::Command(String::from("cat '/x'"))));
        assert_eq!(menu_key(30, 0x61), MenuKey::None);
        let mut empty = build_menu(&rules, "pid", "1");
        assert_eq!(empty.key(MenuKey::Enter), None);
    }

    #[test]
    fn list_is_raised_ground_with_a_border_and_grows_with_items() {
        let rules = parse("path ls ls {}\npath cat cat {}\n", false);
        let m = build_menu(&rules, "path", "/lib/aurora/config");
        let mut gs = GlyphSource::new_vendored(64);
        let (w, h) = menu_size(&m, &mut gs, 800);
        assert!(w > 40 && h > 20, "{}x{}", w, h);
        let (w0, h0) = menu_size(&build_menu(&rules, "pid", "1"), &mut gs, 800);
        assert!(h0 < h, "no verbs = one placeholder row; two verbs = two rows");
        assert!(w0 > 0);
        let c = menu_list(&m, w, h, &mut gs);
        assert!(matches!(c.ops[0], Op::Clear { color: 0xFFBDB0A0 }), "raised ground");
        assert!(matches!(c.ops[1], Op::Rect { y: 0, h: 1, color: 0xFFA89880, .. }), "border stroke");
        assert!(c.ops.iter().any(|o| matches!(o, Op::Rect { color: 0xFFCEC4B6, .. })), "the selected item's header band");
        assert!(c.ops.iter().filter(|o| matches!(o, Op::Glyphs { .. })).count() >= 4, "type + ref + two labels");
        assert!(menu_list(&m, 0, 0, &mut gs).ops.is_empty());
    }

    // The H-3c round F3: a verb-rich type must not ask the compositor for a
    // surface taller than the display (refused = no menu at all); the list
    // scrolls inside the cap instead, the selection always in the window.
    #[test]
    fn a_tall_list_caps_at_the_display_and_scrolls_to_the_selection() {
        let text: String = (0..40).map(|i| alloc::format!("path v{} echo {} {{}}\n", i, i)).collect();
        let rules = parse(&text, false);
        let mut m = build_menu(&rules, "path", "/x");
        assert_eq!(m.items.len(), 40);
        let mut gs = GlyphSource::new_vendored(64);
        let (_, uncapped) = menu_size(&m, &mut gs, u32::MAX);
        let (w, h) = menu_size(&m, &mut gs, 200);
        assert!(uncapped > 200 && h == 200, "uncapped {} capped {}", uncapped, h);
        let (first, fit) = item_window(&m, h, &gs);
        assert_eq!(first, 0);
        assert!(fit >= 2 && fit < 40, "fit {}", fit);
        for _ in 0..39 {
            m.key(MenuKey::Down);
        }
        assert_eq!(m.sel, 39);
        let (first, _) = item_window(&m, h, &gs);
        assert_eq!(first, 40 - fit, "the window ends at the selection");
        // The selected band lies inside the surface.
        let c = menu_list(&m, w, h, &mut gs);
        let band = c.ops.iter().find_map(|o| match o {
            Op::Rect { y, h: bh, color: 0xFFCEC4B6, .. } => Some((*y, *bh as i32)),
            _ => None,
        }).expect("the selected item's band");
        assert!(band.0 >= 0 && band.0 + band.1 <= h as i32, "band {:?} in h {}", band, h);
        // Glyph rows drawn = the window, not the whole list.
        let glyph_ops = c.ops.iter().filter(|o| matches!(o, Op::Glyphs { .. })).count();
        assert!(glyph_ops <= fit + 2, "{} glyph ops for a {}-row window", glyph_ops, fit);
        // The wheel: up moves toward the top, clamped; down clamps at the end.
        m.wheel(3);
        assert_eq!(m.sel, 36);
        m.wheel(-100);
        assert_eq!(m.sel, 39);
        m.wheel(1000);
        assert_eq!(m.sel, 0);
        let mut empty = build_menu(&rules, "pid", "1");
        empty.wheel(-1);
        assert_eq!(empty.sel, 0);
    }
}
