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
use cartoon::{Cartoon, Op};

use crate::chrome::NAME_PX;
use crate::layout::{LaidBlock, Sheet};
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
    step_run_with(flat, cursor, cur, forward, |fr| runs_on_row(t, fr))
}

/// `step_run` over any row source: `runs` gives a row's obj runs (a tile's
/// grid rows have theirs in the cell spans, not the transcript -- H-4d).
pub fn step_run_with(
    flat: &[FlatRow],
    cursor: usize,
    cur: Option<u16>,
    forward: bool,
    runs: impl Fn(FlatRow) -> Vec<ObjRun>,
) -> Option<(usize, u16)> {
    if flat.is_empty() {
        return None;
    }
    let cursor = cursor.min(flat.len() - 1);
    // Within the cursor row first.
    let here = runs(flat[cursor]);
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
            if let Some(r) = runs(flat[row]).first() {
                return Some((row, r.obj));
            }
        }
    } else {
        for row in (0..cursor).rev() {
            if let Some(r) = runs(flat[row]).last() {
                return Some((row, r.obj));
            }
        }
    }
    None
}

/// The run's pixel rect within a laid block (block-relative): the union of
/// its segs across the (possibly wrapped) lines of its item/row. None when
/// the run laid nothing (evicted / clipped away).
pub fn run_rect(
    laid: &LaidBlock,
    item: usize,
    row: usize,
    obj: u16,
) -> Option<(i32, i32, i32, i32)> {
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
    /// HALCYON-INSTRUMENT 14.2 / 14.9: an unavailable item is shown
    /// disabled -- `dim` ink, no fill, no mark, skipped by the keyboard,
    /// still legible -- never hidden.
    pub enabled: bool,
    /// A 1 px `separator` rule above this item (14.2).
    pub separator_before: bool,
}

impl MenuItem {
    pub fn new(label: &str, action: Action) -> MenuItem {
        MenuItem {
            label: String::from(label),
            action,
            enabled: true,
            separator_before: false,
        }
    }
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
            items.push(MenuItem::new(&r.label, Action::Internal(r.template.clone())));
        } else if let Some(cmd) = expand(&r.template, refv) {
            items.push(MenuItem::new(&r.label, Action::Command(cmd)));
        }
    }
    Menu {
        ty: String::from(ty),
        refv: String::from(refv),
        items,
        sel: 0,
    }
}

/// HALCYON-INSTRUMENT 14.9: the tile verb menu -- the program-provided
/// commands first (none registers yet: no `pill` mark is assumed built),
/// then the shell-owned Rename tile / Move to workspace / Restart / Close,
/// each an INTERNAL action the owner interprets (`tile <verb> <id>`), never
/// a shell command; unavailable ones disabled visibly. Restart is for a
/// retained tile (14.6: a distinct NEW process); Close needs a sibling
/// (6.5: the final tile is protected); Rename and Move wait on the dialog
/// family (I-7) and the workspace mechanism (I-4). The title row carries
/// `tile` + the tile's name (a label, bounded by the painter's ellipsis).
pub fn tile_menu(id: u32, name: &str, count: u32, retained: bool) -> Menu {
    let act = |verb: &str| {
        let mut a = String::from("tile ");
        a.push_str(verb);
        let _ = core::fmt::write(&mut a, format_args!(" {}", id));
        Action::Internal(a)
    };
    let mut restart = MenuItem::new("Restart", act("restart"));
    restart.enabled = retained;
    let mut close = MenuItem::new("Close", act("close"));
    close.enabled = count > 1;
    let mut rename = MenuItem::new("Rename tile\u{2026}", act("rename"));
    rename.enabled = false;
    rename.separator_before = true;
    let mut mv = MenuItem::new("Move to workspace\u{2026}", act("move"));
    mv.enabled = false;
    let items = alloc::vec![restart, close, rename, mv];
    let sel = items.iter().position(|i| i.enabled).unwrap_or(0);
    Menu {
        ty: String::from("tile"),
        refv: String::from(name),
        items,
        sel,
    }
}

/// HALCYON-INSTRUMENT 14.1: the workspace list the brand mark opens -- one
/// row per workspace (`01`..), the active one selected, each an INTERNAL
/// action `workspace <n>` (1-based) the owner interprets. Width 160 is the
/// painter's minimum-width clamp's business; the title reads `Workspaces`.
pub fn workspace_menu(numbers: &[u8], active: u8) -> Menu {
    // S4: one row per LIVE NUMBER, labelled and actioned by that number. A
    // row used to be labelled by its position plus one, which is the same
    // value only while the set is dense.
    //
    // An empty list floors to [1] rather than producing zero rows: `sel`
    // below would underflow on `items.len() - 1`, and a list with no
    // workspaces cannot describe a running compositor anyway.
    let fallback = [1u8];
    let numbers = if numbers.is_empty() { &fallback[..] } else { numbers };
    let mut items = Vec::new();
    for &n in numbers {
        let mut label = String::new();
        let _ = core::fmt::write(&mut label, format_args!("{:02}", n as u32));
        let mut act = String::from("workspace ");
        let _ = core::fmt::write(&mut act, format_args!("{}", n as u32));
        items.push(MenuItem::new(&label, Action::Internal(act)));
    }
    Menu {
        ty: String::from("Workspaces"),
        refv: String::new(),
        sel: (active as usize).min(items.len() - 1),
        items,
    }
}

/// A key on the menu surface.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum MenuKey {
    Up,
    Down,
    Home,
    End,
    Enter,
    None,
}

/// Map a KEY press on the menu surface (rune-first, then evdev code).
pub fn menu_key(code: u16, rune: u32) -> MenuKey {
    match rune {
        0x6b => MenuKey::Up,           // k
        0x6a => MenuKey::Down,         // j
        0x0d | 0x0a => MenuKey::Enter, // Enter
        _ => match code {
            103 => MenuKey::Up,
            108 => MenuKey::Down,
            102 => MenuKey::Home,
            107 => MenuKey::End,
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
            self.sel
                .saturating_add(delta.unsigned_abs() as usize)
                .min(last)
        };
    }

    /// Apply a key: Up/Down move the selection (clamped), skipping
    /// disabled items (14.2; a menu whose every item is disabled keeps
    /// its selection where it is); Home/End go to the first/last enabled;
    /// Enter yields the selected item's action -- None on an empty menu or
    /// a disabled selection (a disabled item is never activated).
    pub fn key(&mut self, k: MenuKey) -> Option<Action> {
        let enabled = |i: usize, items: &[MenuItem]| items.get(i).is_some_and(|it| it.enabled);
        match k {
            MenuKey::Up => {
                let mut i = self.sel;
                while i > 0 {
                    i -= 1;
                    if enabled(i, &self.items) {
                        self.sel = i;
                        break;
                    }
                }
                None
            }
            MenuKey::Down => {
                let mut i = self.sel;
                while i + 1 < self.items.len() {
                    i += 1;
                    if enabled(i, &self.items) {
                        self.sel = i;
                        break;
                    }
                }
                None
            }
            MenuKey::Home => {
                if let Some(i) = self.items.iter().position(|it| it.enabled) {
                    self.sel = i;
                }
                None
            }
            MenuKey::End => {
                if let Some(i) = self.items.iter().rposition(|it| it.enabled) {
                    self.sel = i;
                }
                None
            }
            MenuKey::Enter => self
                .items
                .get(self.sel)
                .filter(|i| i.enabled)
                .map(|i| i.action.clone()),
            MenuKey::None => None,
        }
    }
}

/// Menu metrics (Daylight: the tag bar's padding family; the menu is the
/// raised ground with the border stroke -- chrome, not content), LOGICAL:
/// the sheet scales them (HALCYON-SCALE 6).
pub const MENU_PAD_X: i32 = 8;
pub const MENU_PAD_Y: i32 = 4;
pub const MENU_MAX_W: i32 = 640;
const ROW_PAD: i32 = 4;
const NO_VERBS: &str = "no verbs";

fn body_width(gs: &mut GlyphSource, sheet: &Sheet, s: &str) -> i32 {
    // The sub-pixel pen's width, so the menu measures what it paints
    // (HALCYON-TYPE 4.3; `push_body` shapes with the same call).
    let px = sheet.px(NAME_PX);
    gs.shape_run(FACE_BODY, px, s.chars()).1
}

fn mono_width(gs: &GlyphSource, s: &str) -> i32 {
    let (cw, _, _) = gs.island_cell();
    cw * s.chars().count() as i32
}

fn row_h(gs: &GlyphSource, sheet: &Sheet) -> i32 {
    if sheet.profile == libhalcyon::instrument::Profile::Instrument {
        return sheet.ipx(INST_ROW_H);
    }
    let (_, ch, _) = gs.island_cell();
    ch + sheet.ipx(ROW_PAD)
}

/// The title row's height: the legacy one is an item row; the Instrument
/// one is 14.2's 24.
fn title_h(gs: &GlyphSource, sheet: &Sheet) -> i32 {
    if sheet.profile == libhalcyon::instrument::Profile::Instrument {
        return sheet.ipx(INST_TITLE_H);
    }
    row_h(gs, sheet)
}

/// A separator row's height under Instrument (14.2: 1 px with a 4 px
/// vertical margin either side); legacy menus have no separators.
fn sep_h(sheet: &Sheet) -> i32 {
    if sheet.profile == libhalcyon::instrument::Profile::Instrument {
        return sheet.hairline + 2 * sheet.ipx(INST_SEP_MARGIN);
    }
    0
}

// HALCYON-INSTRUMENT 14.2, the object verb menu in the round-2 look
// (logical): square; min-width 224, max-width 320, padding 4; the title
// row 24 tall, horizontal padding 10, Sans 500 11; items 28 tall, padding
// 10, Sans 13; the 2 px `amber` mark at y 6..22 of the focused row;
// separators 1 px with margin 4, inset 8.
const INST_MIN_W: i32 = 224;
const INST_MAX_W: i32 = 320;
const INST_PAD: i32 = 4;
const INST_PAD_X: i32 = 10;
const INST_TITLE_H: i32 = 24;
const INST_TITLE_PX: f32 = 11.0;
const INST_ROW_H: i32 = 28;
const INST_ITEM_PX: f32 = 13.0;
const INST_MARK_W: i32 = 2;
const INST_MARK_Y: i32 = 6;
const INST_MARK_H: i32 = 16;
const INST_SEP_MARGIN: i32 = 4;
const INST_SEP_INSET: i32 = 8;

/// The menu's surface size for its content at the sheet's scale: the
/// widest of the title (type label + ref) and the items, padded; one row
/// per item (or the "no verbs" row) under the title row and its rule.
/// Capped at MENU_MAX_W (scaled) wide and at `max_h` tall (the display: the
/// compositor refuses a taller surface -- the H-3c round F3); past the cap
/// the item list scrolls (`menu_list`).
pub fn menu_size(m: &Menu, sheet: &Sheet, gs: &mut GlyphSource, max_h: u32) -> (u32, u32) {
    if sheet.profile == libhalcyon::instrument::Profile::Instrument {
        return menu_size_inst(m, sheet, gs, max_h);
    }
    let (cw, _, _) = gs.island_cell();
    let title_w = body_width(gs, sheet, &m.ty) + 2 * cw + mono_width(gs, &m.refv);
    let mut w = title_w;
    if m.items.is_empty() {
        w = w.max(mono_width(gs, NO_VERBS));
    }
    for it in m.items.iter() {
        w = w.max(mono_width(gs, &it.label));
    }
    let rows = 1 + m.items.len().max(1) as i32;
    let h = 2 * sheet.ipx(MENU_PAD_Y) + rows * row_h(gs, sheet) + sheet.hairline;
    let w = (w + 2 * sheet.ipx(MENU_PAD_X)).max(1) as u32;
    (
        w.min(sheet.ipx(MENU_MAX_W).max(1) as u32),
        (h.max(1) as u32).min(max_h.max(1)),
    )
}

/// How many item rows fit under the title row and its rule in `h`, and the
/// first item shown so the selection stays inside them.
pub fn item_window(m: &Menu, h: u32, sheet: &Sheet, gs: &GlyphSource) -> (usize, usize) {
    let rh = row_h(gs, sheet).max(1);
    let pad_y = if sheet.profile == libhalcyon::instrument::Profile::Instrument {
        sheet.ipx(INST_PAD)
    } else {
        sheet.ipx(MENU_PAD_Y)
    };
    let fit = ((h as i32 - 2 * pad_y - sheet.hairline - title_h(gs, sheet)) / rh).max(1) as usize;
    let first = if m.sel >= fit { m.sel + 1 - fit } else { 0 };
    (first, fit)
}

/// 14.2: the Instrument menu's surface size -- the widest of the title and
/// the items in their faces plus the padding, clamped to [224, 320]; the
/// title row, its rule, one row per item (or the "no verbs" row), the
/// separators, the padding; capped at `max_h`.
fn menu_size_inst(m: &Menu, sheet: &Sheet, gs: &mut GlyphSource, max_h: u32) -> (u32, u32) {
    let tpx = sheet.px(INST_TITLE_PX);
    let ipx = sheet.px(INST_ITEM_PX);
    let (title_face, item_face) = (sheet.face_medium, sheet.face_body);
    let mut w = gs.shape_run(title_face, tpx, m.ty.chars()).1
        + sheet.ipx(INST_PAD_X)
        + gs.shape_run(title_face, tpx, m.refv.chars()).1;
    if m.items.is_empty() {
        w = w.max(gs.shape_run(item_face, ipx, NO_VERBS.chars()).1);
    }
    for it in m.items.iter() {
        w = w.max(gs.shape_run(item_face, ipx, it.label.chars()).1);
    }
    let w = (w + 2 * sheet.ipx(INST_PAD_X) + 2 * sheet.ipx(INST_PAD))
        .clamp(sheet.ipx(INST_MIN_W), sheet.ipx(INST_MAX_W))
        .max(1) as u32;
    let seps = m.items.iter().filter(|i| i.separator_before).count() as i32;
    let h = 2 * sheet.ipx(INST_PAD)
        + title_h(gs, sheet)
        + sheet.hairline
        + m.items.len().max(1) as i32 * row_h(gs, sheet)
        + seps * sep_h(sheet);
    (w, (h.max(1) as u32).min(max_h.max(1)))
}

fn push_body(
    cart: &mut Cartoon,
    gs: &mut GlyphSource,
    sheet: &Sheet,
    x: i32,
    baseline: i32,
    color: u32,
    s: &str,
) -> i32 {
    let px = sheet.px(NAME_PX);
    let (refs, adv) = gs.shape_run(FACE_BODY, px, s.chars());
    if !refs.is_empty() {
        cart.push_glyphs(gs.gen(), x, baseline, color, &refs);
    }
    adv
}

fn push_mono(
    cart: &mut Cartoon,
    gs: &mut GlyphSource,
    sheet: &Sheet,
    x: i32,
    baseline: i32,
    color: u32,
    s: &str,
) {
    // FACE_MONO refuses a phase (a fixed cell has none), so this is the
    // whole-cell run it always was -- routed through the one shaper so
    // there is a single place the pen lives.
    let (refs, _) = gs.shape_run(FACE_MONO, sheet.mono_island_px, s.chars());
    if !refs.is_empty() {
        cart.push_glyphs(gs.gen(), x, baseline, color, &refs);
    }
}

/// The menu display list for a w x h surface at the sheet's scale: raised
/// ground, the hairline border stroke, the title row (type in the
/// proportional face, muted; the resolved ref in monospace, full ink), a
/// rule, then the items in monospace -- the selected one on a `header`
/// band.
pub fn menu_list(m: &Menu, w: u32, h: u32, sheet: &Sheet, gs: &mut GlyphSource) -> Cartoon {
    // The source follows the sheet in force at every painter entry (r2 A-F2).
    gs.set_kerning(sheet.kerning);
    if sheet.profile == libhalcyon::instrument::Profile::Instrument {
        return menu_list_inst(m, w, h, sheet, gs);
    }
    let d = &sheet.theme;
    let mut cart = Cartoon::new();
    if w == 0 || h == 0 {
        return cart;
    }
    cart.ops.push(Op::Clear { color: d.raised });
    let (wi, hi) = (w as i32, h as i32);
    let hair = sheet.hairline;
    let hu = hair as u32;
    for r in [
        (0, 0, w, hu),
        (0, (hi - hair).max(0), w, hu),
        (0, 0, hu, h),
        ((wi - hair).max(0), 0, hu, h),
    ] {
        cart.ops.push(Op::Rect {
            x: r.0,
            y: r.1,
            w: r.2,
            h: r.3,
            color: d.border,
        });
    }
    let (cw, _, mono_base) = gs.island_cell();
    let rh = row_h(gs, sheet);
    let (pad_x, pad_y, row_pad) = (
        sheet.ipx(MENU_PAD_X),
        sheet.ipx(MENU_PAD_Y),
        sheet.ipx(ROW_PAD),
    );
    let body_asc = gs
        .line_metrics(FACE_BODY, sheet.px(NAME_PX))
        .map(|lm| lm.ascent)
        .unwrap_or(8);
    let mut y = pad_y;
    // Title: "<type>  <ref>".
    let mut x = pad_x;
    x += push_body(
        &mut cart,
        gs,
        sheet,
        x,
        y + row_pad / 2 + body_asc,
        d.fg_muted,
        &m.ty,
    );
    x += 2 * cw;
    push_mono(&mut cart, gs, sheet, x, y + row_pad / 2 + mono_base, d.fg, &m.refv);
    y += rh;
    cart.ops.push(Op::Rect {
        x: hair,
        y,
        w: (wi - 2 * hair).max(0) as u32,
        h: hu,
        color: d.border,
    });
    y += hair;
    if m.items.is_empty() {
        push_mono(
            &mut cart,
            gs,
            sheet,
            pad_x,
            y + row_pad / 2 + mono_base,
            d.fg_muted,
            NO_VERBS,
        );
        return cart;
    }
    let (first, fit) = item_window(m, h, sheet, gs);
    for (i, it) in m.items.iter().enumerate().skip(first).take(fit) {
        if i == m.sel {
            cart.ops.push(Op::Rect {
                x: hair,
                y,
                w: (wi - 2 * hair).max(0) as u32,
                h: rh as u32,
                color: d.header,
            });
        }
        let ink = match it.action {
            Action::Command(_) if it.enabled => d.fg,
            _ => d.fg_dim,
        };
        push_mono(
            &mut cart,
            gs,
            sheet,
            pad_x,
            y + row_pad / 2 + mono_base,
            ink,
            &it.label,
        );
        y += rh;
    }
    cart
}

/// 14.2: the Instrument menu's display list -- `pane` ground, a 1 px
/// `structure` border; the title row (`tile` + the label in Sans 11
/// `text`), a 1 px `separator` rule; items in Sans 13: `text`, or `dim`
/// when disabled (no fill, no mark); the selected row on `hover` with the
/// 2 px `amber` mark at y 6..22; a disabled selection paints nothing (the
/// keyboard never rests on one). Separators 1 px `separator`, margin 4,
/// inset 8. The list window follows the selection (`item_window`).
fn menu_list_inst(m: &Menu, w: u32, h: u32, sheet: &Sheet, gs: &mut GlyphSource) -> Cartoon {
    let i = &sheet.inst;
    let mut cart = Cartoon::new();
    if w == 0 || h == 0 {
        return cart;
    }
    cart.ops.push(Op::Clear { color: i.pane });
    let (wi, hi) = (w as i32, h as i32);
    let hair = sheet.hairline;
    let hu = hair as u32;
    for r in [
        (0, 0, w, hu),
        (0, (hi - hair).max(0), w, hu),
        (0, 0, hu, h),
        ((wi - hair).max(0), 0, hu, h),
    ] {
        cart.ops.push(Op::Rect {
            x: r.0,
            y: r.1,
            w: r.2,
            h: r.3,
            color: i.structure,
        });
    }
    let gen = gs.gen();
    let (pad, pad_x) = (sheet.ipx(INST_PAD), sheet.ipx(INST_PAD_X));
    let (tpx, ipx) = (sheet.px(INST_TITLE_PX), sheet.px(INST_ITEM_PX));
    // 14.2: the title row Sans 500 11, the items Sans 400 13 -- the
    // sheet's `face_medium` / `face_body` (since I-5).
    let (title_face, item_face) = (sheet.face_medium, sheet.face_body);
    let th = title_h(gs, sheet);
    let rh = row_h(gs, sheet);
    let centre = |gs: &mut GlyphSource, face: u8, px: f32, rows: i32| -> i32 {
        let (asc, desc) = gs
            .line_metrics(face, px)
            .map(|lm| (lm.ascent, lm.descent))
            .unwrap_or((8, 2));
        (rows - (asc + desc)) / 2 + asc
    };
    let mut y = pad;
    // The title: the type, then the label.
    let mut x = pad + pad_x;
    let (refs, adv) = gs.shape_run(title_face, tpx, m.ty.chars());
    let tbase = y + centre(gs, title_face, tpx, th);
    if !refs.is_empty() {
        cart.push_glyphs(gen, x, tbase, i.secondary, &refs);
    }
    x += adv + pad_x;
    let avail = wi - pad - pad_x - x;
    if avail > 0 && !m.refv.is_empty() {
        let label = crate::chrome::fit_end_pub(gs, title_face, tpx, &m.refv, avail);
        let (refs, _) = gs.shape_run(title_face, tpx, label.chars());
        if !refs.is_empty() {
            cart.push_glyphs(gen, x, tbase, i.text, &refs);
        }
    }
    y += th;
    cart.ops.push(Op::Rect {
        x: hair,
        y,
        w: (wi - 2 * hair).max(0) as u32,
        h: hu,
        color: i.separator,
    });
    y += hair;
    if m.items.is_empty() {
        let (refs, _) = gs.shape_run(item_face, ipx, NO_VERBS.chars());
        if !refs.is_empty() {
            let base = y + centre(gs, item_face, ipx, rh);
            cart.push_glyphs(gen, pad + pad_x, base, i.dim, &refs);
        }
        return cart;
    }
    let (first, fit) = item_window(m, h, sheet, gs);
    for (idx, it) in m.items.iter().enumerate().skip(first).take(fit) {
        if it.separator_before && idx > first {
            let margin = sheet.ipx(INST_SEP_MARGIN);
            let inset = sheet.ipx(INST_SEP_INSET);
            cart.ops.push(Op::Rect {
                x: pad + inset,
                y: y + margin,
                w: (wi - 2 * pad - 2 * inset).max(0) as u32,
                h: hu,
                color: i.separator,
            });
            y += sep_h(sheet);
        }
        if idx == m.sel && it.enabled {
            cart.ops.push(Op::Rect {
                x: pad,
                y,
                w: (wi - 2 * pad).max(0) as u32,
                h: rh as u32,
                color: i.hover,
            });
            cart.ops.push(Op::Rect {
                x: pad,
                y: y + sheet.ipx(INST_MARK_Y),
                w: sheet.ipx(INST_MARK_W).max(1) as u32,
                h: sheet.ipx(INST_MARK_H).max(1) as u32,
                color: i.amber,
            });
        }
        let (refs, _) = gs.shape_run(item_face, ipx, it.label.chars());
        if !refs.is_empty() {
            let base = y + centre(gs, item_face, ipx, rh);
            let ink = if it.enabled { i.text } else { i.dim };
            cart.push_glyphs(gen, pad + pad_x, base, ink, &refs);
        }
        y += rh;
    }
    cart
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::layout::{daylight_sheet, layout_block};

    fn sheet() -> Sheet {
        daylight_sheet(100)
    }
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
        wire::open(
            &mut buf,
            BOp::Obj,
            &[("type", "path"), ("ref", "/lib/aurora")],
        );
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
        assert_eq!(
            obj_of(&t, flat[1].block, r[0].obj),
            Some(("path", "/lib/aurora"))
        );
        assert_eq!(
            obj_of(&t, flat[1].block, r[1].obj),
            Some(("path", "/lib/ndb"))
        );
        assert!(
            runs_on_row(&t, flat[2]).is_empty(),
            "a plain line has no runs"
        );
        let tr = runs_on_row(&t, flat[3]);
        assert_eq!(tr.len(), 1, "the table row's obj cell is one run");
        assert_eq!(obj_of(&t, flat[3].block, tr[0].obj), Some(("pid", "42")));
        assert_eq!(
            obj_of(&t, flat[3].block, 0),
            None,
            "0 is the no-obj sentinel"
        );
    }

    #[test]
    fn stepping_crosses_rows_both_ways() {
        let t = corpus();
        let flat = flatten(&t);
        let (r1, a) =
            step_run(&t, &flat, 0, None, true).expect("first run forward from the prompt row");
        assert_eq!(r1, 1);
        let (r2, b) = step_run(&t, &flat, r1, Some(a), true).unwrap();
        assert_eq!((r2, b == a), (1, false), "the second run on the same row");
        let (r3, c) = step_run(&t, &flat, r2, Some(b), true).unwrap();
        assert_eq!(r3, 3, "then the table row's run, skipping the plain line");
        assert_eq!(
            step_run(&t, &flat, r3, Some(c), true),
            None,
            "nothing past the last run"
        );
        assert_eq!(
            step_run(&t, &flat, r3, Some(c), false),
            Some((1, b)),
            "back lands on the LAST run of the previous obj row"
        );
        assert_eq!(step_run(&t, &flat, 1, Some(b), false), Some((1, a)));
        assert_eq!(step_run(&t, &flat, 1, Some(a), false), None);
        assert_eq!(
            step_run(&t, &flat, 3, None, false),
            Some((1, b)),
            "no run selected: back skips the row and lands on the previous obj row's last run"
        );
        assert_eq!(
            step_run(&t, &flat, 2, None, true),
            Some((3, c)),
            "forward from a plain row finds the next obj row"
        );
    }

    #[test]
    fn run_rect_and_hit_agree_on_the_laid_geometry() {
        let t = corpus();
        let flat = flatten(&t);
        let b = &t.frozen_blocks()[flat[1].block];
        let sheet = daylight_sheet(100);
        let mut gs = GlyphSource::new_vendored(64);
        let laid = layout_block(b, 800, &sheet, &mut gs);
        let runs = runs_on_row(&t, flat[1]);
        let (x, y, w, h) =
            run_rect(&laid, flat[1].item, flat[1].row, runs[1].obj).expect("ndb laid");
        assert!(w > 0 && h > 0);
        let hit = hit_run(&laid, x + w / 2, y + h / 2);
        assert_eq!(
            hit,
            Some((flat[1].item, flat[1].row, runs[1].obj)),
            "the rect's centre hits its own run"
        );
        let (ax, _, aw, _) = run_rect(&laid, flat[1].item, flat[1].row, runs[0].obj).unwrap();
        assert!(ax + aw <= x, "aurora lies left of ndb");
        assert_eq!(
            hit_run(&laid, x - 1, y + h / 2),
            None,
            "the padding between runs hits nothing"
        );
    }

    #[test]
    fn menu_shows_the_resolved_ref_and_the_typed_verbs() {
        let rules = parse(
            "path ls ls -l {}\npath cat cat {}\npid kill kill {}\npath t #wedge 10\n",
            true,
        );
        let m = build_menu(&rules, "path", "/lib/o'k");
        assert_eq!(m.refv, "/lib/o'k", "the ref is displayed verbatim");
        let labels: Vec<&str> = m.items.iter().map(|i| i.label.as_str()).collect();
        assert_eq!(labels, ["ls", "cat", "t"]);
        assert_eq!(
            m.items[0].action,
            Action::Command(String::from("ls -l '/lib/o''k'"))
        );
        assert_eq!(
            m.items[2].action,
            Action::Internal(String::from("#wedge 10"))
        );
        let none = build_menu(&rules, "commit", "abc");
        assert!(none.items.is_empty());
        let unsafe_ref = build_menu(&rules, "path", "a\nb");
        assert_eq!(
            unsafe_ref.items.len(),
            1,
            "only the internal action survives an unquotable ref"
        );
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
        assert_eq!(
            m.key(menu_key(28, 0x0d)),
            Some(Action::Command(String::from("cat '/x'")))
        );
        assert_eq!(menu_key(30, 0x61), MenuKey::None);
        let mut empty = build_menu(&rules, "pid", "1");
        assert_eq!(empty.key(MenuKey::Enter), None);
    }

    #[test]
    fn list_is_raised_ground_with_a_border_and_grows_with_items() {
        let rules = parse("path ls ls {}\npath cat cat {}\n", false);
        let m = build_menu(&rules, "path", "/lib/aurora/config");
        let mut gs = GlyphSource::new_vendored(64);
        let (w, h) = menu_size(&m, &sheet(), &mut gs, 800);
        assert!(w > 40 && h > 20, "{}x{}", w, h);
        let (w0, h0) = menu_size(&build_menu(&rules, "pid", "1"), &sheet(), &mut gs, 800);
        assert!(
            h0 < h,
            "no verbs = one placeholder row; two verbs = two rows"
        );
        assert!(w0 > 0);
        let c = menu_list(&m, w, h, &sheet(), &mut gs);
        assert!(
            matches!(c.ops[0], Op::Clear { color: 0xFFBDB0A0 }),
            "raised ground"
        );
        assert!(
            matches!(
                c.ops[1],
                Op::Rect {
                    y: 0,
                    h: 1,
                    color: 0xFFA89880,
                    ..
                }
            ),
            "border stroke"
        );
        assert!(
            c.ops.iter().any(|o| matches!(
                o,
                Op::Rect {
                    color: 0xFFCEC4B6,
                    ..
                }
            )),
            "the selected item's header band"
        );
        assert!(
            c.ops
                .iter()
                .filter(|o| matches!(o, Op::Glyphs { .. }))
                .count()
                >= 4,
            "type + ref + two labels"
        );
        assert!(menu_list(&m, 0, 0, &sheet(), &mut gs).ops.is_empty());
    }

    // The H-3c round F3: a verb-rich type must not ask the compositor for a
    // surface taller than the display (refused = no menu at all); the list
    // scrolls inside the cap instead, the selection always in the window.
    #[test]
    fn a_tall_list_caps_at_the_display_and_scrolls_to_the_selection() {
        let text: String = (0..40)
            .map(|i| alloc::format!("path v{} echo {} {{}}\n", i, i))
            .collect();
        let rules = parse(&text, false);
        let mut m = build_menu(&rules, "path", "/x");
        assert_eq!(m.items.len(), 40);
        let mut gs = GlyphSource::new_vendored(64);
        let (_, uncapped) = menu_size(&m, &sheet(), &mut gs, u32::MAX);
        let (w, h) = menu_size(&m, &sheet(), &mut gs, 200);
        assert!(
            uncapped > 200 && h == 200,
            "uncapped {} capped {}",
            uncapped,
            h
        );
        let (first, fit) = item_window(&m, h, &sheet(), &gs);
        assert_eq!(first, 0);
        assert!(fit >= 2 && fit < 40, "fit {}", fit);
        for _ in 0..39 {
            m.key(MenuKey::Down);
        }
        assert_eq!(m.sel, 39);
        let (first, _) = item_window(&m, h, &sheet(), &gs);
        assert_eq!(first, 40 - fit, "the window ends at the selection");
        // The selected band lies inside the surface.
        let c = menu_list(&m, w, h, &sheet(), &mut gs);
        let band = c
            .ops
            .iter()
            .find_map(|o| match o {
                Op::Rect {
                    y,
                    h: bh,
                    color: 0xFFCEC4B6,
                    ..
                } => Some((*y, *bh as i32)),
                _ => None,
            })
            .expect("the selected item's band");
        assert!(
            band.0 >= 0 && band.0 + band.1 <= h as i32,
            "band {:?} in h {}",
            band,
            h
        );
        // Glyph rows drawn = the window, not the whole list.
        let glyph_ops = c
            .ops
            .iter()
            .filter(|o| matches!(o, Op::Glyphs { .. }))
            .count();
        assert!(
            glyph_ops <= fit + 2,
            "{} glyph ops for a {}-row window",
            glyph_ops,
            fit
        );
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
    /// HALCYON-INSTRUMENT 14.9: the tile verb menu's four shell-owned items,
    /// enabled by the tile's state (Restart for a retained tile, Close with
    /// a sibling), the two unbuilt ones disabled behind a separator; the
    /// keyboard skips disabled items and never activates one.
    #[test]
    fn the_tile_menu_offers_restart_and_close_by_state_and_skips_the_disabled() {
        let m = tile_menu(7, "ut", 1, false);
        assert_eq!((m.ty.as_str(), m.refv.as_str()), ("tile", "ut"));
        let labels: Vec<&str> = m.items.iter().map(|i| i.label.as_str()).collect();
        assert_eq!(labels, ["Restart", "Close", "Rename tile\u{2026}", "Move to workspace\u{2026}"]);
        assert!(!m.items[0].enabled, "a live tile does not restart");
        assert!(!m.items[1].enabled, "a lone tile is protected");
        assert!(!m.items[2].enabled && !m.items[3].enabled);
        assert!(m.items[2].separator_before && !m.items[1].separator_before);
        let mut m2 = tile_menu(7, "ut", 3, true);
        assert!(m2.items[0].enabled && m2.items[1].enabled);
        assert_eq!(m2.sel, 0);
        assert_eq!(m2.key(MenuKey::Down), None);
        assert_eq!(m2.sel, 1);
        assert_eq!(m2.key(MenuKey::Down), None);
        assert_eq!(m2.sel, 1, "the disabled tail is skipped");
        assert_eq!(m2.key(MenuKey::End), None);
        assert_eq!(m2.sel, 1);
        assert_eq!(m2.key(MenuKey::Home), None);
        assert_eq!(m2.sel, 0);
        assert_eq!(m2.key(MenuKey::Enter), Some(Action::Internal(String::from("tile restart 7"))));
        m2.sel = 1;
        assert_eq!(m2.key(MenuKey::Enter), Some(Action::Internal(String::from("tile close 7"))));
        m2.sel = 2;
        assert_eq!(m2.key(MenuKey::Enter), None, "a disabled item never activates");
        assert_eq!(m2.key(MenuKey::Up), None);
        assert_eq!(m2.sel, 1);
        // Every item disabled: Enter yields nothing, the selection stays.
        let mut m3 = tile_menu(1, "x", 1, false);
        assert_eq!(m3.sel, 0);
        assert_eq!(m3.key(MenuKey::Enter), None);
        assert_eq!(m3.key(MenuKey::Down), None);
        assert_eq!(m3.sel, 0);
        assert_eq!(menu_key(102, 0), MenuKey::Home);
        assert_eq!(menu_key(107, 0), MenuKey::End);
    }

    /// 14.2 on Carbon: the Instrument menu's geometry and inks -- `pane`
    /// ground, the `structure` border, the 24 title row, 28 rows, the
    /// selected row on `hover` with the 2 x 16 `amber` mark at y 6, a
    /// separator inset 8 with a 4 margin, disabled labels in `dim`; a
    /// disabled selection paints no band; the legacy list is unchanged.
    #[test]
    fn the_instrument_menu_is_the_round_two_look() {
        let s = crate::layout::sheet_for(
            &libhalcyon::instrument::Bundle::builtin(libhalcyon::instrument::Profile::Instrument),
            100,
            crate::layout::TEST_DISPLAY_W,
        );
        let mut gs = GlyphSource::new_vendored(64);
        let m = tile_menu(7, "renderer.rs", 3, true);
        let (w, h) = menu_size(&m, &s, &mut gs, 900);
        assert!((224..=320).contains(&w), "clamped to 14.2's width: {}", w);
        // 4 + 24 + 1 + 4 x 28 + (4 + 1 + 4) + 4
        assert_eq!(h, 154);
        let c = menu_list(&m, w, h, &s, &mut gs);
        assert!(matches!(c.ops[0], Op::Clear { color: 0xFF0B_0D0E }), "pane ground");
        let rects: Vec<(i32, i32, u32, u32, u32)> = c
            .ops
            .iter()
            .filter_map(|op| match *op {
                Op::Rect { x, y, w, h, color } => Some((x, y, w, h, color)),
                _ => None,
            })
            .collect();
        assert!(rects.contains(&(0, 0, w, 1, 0xFF45_4B48)), "the structure border: {:?}", rects);
        assert!(rects.contains(&(4, 29, w - 8, 28, 0xFF19_1C1D)), "the selected row's band: {:?}", rects);
        assert!(rects.contains(&(4, 35, 2, 16, 0xFFC7_B98B)), "the amber mark: {:?}", rects);
        assert!(rects.contains(&(12, 89, w - 24, 1, 0xFF29_2D2B)), "the separator: {:?}", rects);
        let inks: Vec<u32> = c
            .ops
            .iter()
            .filter_map(|op| match *op {
                Op::Glyphs { color, .. } => Some(color),
                _ => None,
            })
            .collect();
        assert_eq!(inks.iter().filter(|&&k| k == 0xFF73_7A76).count(), 2, "two disabled labels: {:?}", inks);
        assert!(inks.iter().filter(|&&k| k == 0xFFF2_F3EF).count() >= 3, "the label + two enabled items: {:?}", inks);
        let mut m2 = m.clone();
        m2.sel = 2;
        let c2 = menu_list(&m2, w, h, &s, &mut gs);
        assert!(!c2.ops.iter().any(|op| matches!(op, Op::Rect { color: 0xFF19_1C1D, .. })));
        let ls = sheet();
        let lc = menu_list(&m, 200, 100, &ls, &mut gs);
        assert!(matches!(lc.ops[0], Op::Clear { color } if color == ls.theme.raised), "the legacy ground");
        let (lw, lh) = menu_size(&m, &ls, &mut gs, 900);
        assert!(lw < 224 || lh < 154, "the legacy size is the legacy size");
    }
}
