// A tile's model -- the live grid + the scrollback transcript (HALCYON 14.11.1).
//
// One `Tile` per leaf terminal. It holds two structures, not one (14.11.1):
//
//   - the live grid   -- the current screen, what CellDiff mutates.
//   - the scrollback  -- the existing block/zone `Transcript`, now fed by
//                        ScrollOff (lines that left the top of the grid) and
//                        Beacon frames, not by a raw VT byte stream. It is pure
//                        history: everything that has scrolled off.
//
// They are separate because the grid spans zone boundaries (14.11.1): the last
// `rows` lines routinely straddle a prompt, so the grid cannot be "one zone's
// block". The grid is zone-agnostic; the transcript carries the zone structure.
//
// `apply` is the record -> model dispatch (14.11.2). Record order is load-bearing
// and guaranteed by the producer (a pending CellDiff is flushed before every
// ScrollOff/Control/Mode), so a zone frame lands at the exact point between the
// cells it separates. This module runs NO VT parser and cuts NO zones itself:
// the kaua-term pre-digested the VT, and the Beacon cut rides the SAME
// `Transcript::feed` the console path uses (14.11.4), so the format-fuzz surface
// (parsing an untrusted per-tile stream) is one parser, audited once.

use alloc::collections::VecDeque;
use alloc::string::String;
use alloc::vec::Vec;

use crate::grid::Grid;
use crate::layout::{
    block_gap_between, caret_in_block, laid_line_for, layout_block, render_block, LaidBlock,
    LaidLine, Sheet,
};
use crate::menu::{run_rect, ObjRun};
use crate::raster::{GlyphSource, FACE_MONO};
use crate::transcript::{
    BlockKind, SpanMap, SpanTag, Transcript, DEFAULT_MAX_BLOCKS, DEFAULT_MAX_COST,
    DEFAULT_MAX_LINES_PER_BLOCK,
};
use cartoon::{Cartoon, Op};
use kaua_term::{Control, Record, ScreenMode};
use vt::{Palette, ATTR_REVERSE, ATTR_UNDERLINE};

/// PL-4b-ii: a render's cached proportional live tail -- the laid live block,
/// the per-grid-row provenance `(item, row within the item, start column)`
/// (the row is `usize::MAX` for a plain line; a rebuilt table's rows and a
/// pre's lines share one item and differ by row), and the tail's screen-y --
/// that a click inverts through (`Tile::live_laid`).
type LiveLaid = (LaidBlock, Vec<(usize, usize, usize)>, i32);

pub struct Tile {
    pub grid: Grid,
    pub scrollback: Transcript,
    pub mode: ScreenMode,
    /// OSC 0/2 title (the child's own; "" until it sets one).
    pub title: String,
    exit: Option<i32>,
    /// A pending bell affordance the render consumes once (no kernel bell).
    bell: bool,
    /// The frozen blocks' laid heights at `heights_width`, aligned to the
    /// scrollback's frozen deque (front-evicted, back-appended; block ids are
    /// strictly increasing along it). A frozen block's layout is width- and
    /// content-deterministic, so a few bytes per block are enough to position
    /// every block without laying it out -- a render then lays out ONLY the
    /// blocks that intersect the view. The old whole-history layout was a
    /// transient of ~1.8x the retained bytes, outside every budget: one tile
    /// with ~20K rows of history ended the whole session at its next paint.
    /// The entry carries the block's `exit` too: it is the ONE field a frozen
    /// block can still acquire (an exit mark floating in right after its
    /// zone closed lands on the last frozen block), and a non-zero code adds
    /// the badge line -- a height cached before it would misplace every block
    /// below. Any new post-freeze mutation must join this key.
    heights: VecDeque<(u64, Option<i64>, i32)>,
    heights_width: i32,
    /// H-4d: the last render's block placement -- (block id, or u64::MAX for
    /// the open block; screen y; height) for EVERY block, in transcript
    /// order -- the hit map for a click on an obj run and the anchor for the
    /// keyboard menu (the console's `frame`).
    pub frame: Vec<(u64, i32, i32)>,
    /// H-4d: serial -> span state, noted after every Beacon frame fed; the
    /// grid's cells and the scrolled-off rows resolve their spans through it.
    pub spans: SpanMap,
    /// Blocks laid out by the last `render` (the window's witness).
    pub laid_last: usize,
    /// Visual lines laid out by the last `render` (the transient's witness:
    /// bounded by the view plus the two whole blocks, never the history).
    pub laid_lines_last: usize,
    /// PL-4b-ii: the last NORMAL-mode render's proportional live tail --
    /// (the laid live block, the per-grid-row provenance, the tail's screen-y)
    /// -- so a click on the tail (`grid_hit` / `grid_run_rect`) inverts through
    /// the SAME geometry the render painted, not the mono cell grid. None in
    /// alt-screen (the tail is the mono `paint_grid`, hit by cell) and before
    /// the first render. Rebuilt every render (O(grid), never the history), so
    /// a stale cache never outlives one frame; a click uses the last frame's
    /// layout exactly as the block `frame` does.
    live_laid: Option<LiveLaid>,
}

impl Tile {
    pub fn new(cols: usize, rows: usize, pal: Palette) -> Tile {
        Tile::with_budget(cols, rows, pal, DEFAULT_MAX_COST)
    }

    /// A tile whose scrollback holds at most `max_cost` bytes -- a session's
    /// tiles share ONE budget (their sum, not each, must fit the heap).
    pub fn with_budget(cols: usize, rows: usize, pal: Palette, max_cost: usize) -> Tile {
        Tile {
            grid: Grid::new(cols, rows, pal.fg, pal.bg),
            scrollback: {
                // A tile's transcript is FRAME-fed (the text is grid cells):
                // structure is rebuilt from the tagged cells.
                let mut t = Transcript::with_caps(
                    pal,
                    DEFAULT_MAX_BLOCKS,
                    max_cost,
                    DEFAULT_MAX_LINES_PER_BLOCK,
                );
                t.set_cells_mode(true);
                t
            },
            mode: ScreenMode::Normal,
            title: String::new(),
            exit: None,
            bell: false,
            heights: VecDeque::new(),
            heights_width: 0,
            frame: Vec::new(),
            spans: SpanMap::new(),
            laid_last: 0,
            laid_lines_last: 0,
            live_laid: None,
        }
    }

    /// H-4d: the block under screen `y` in the last render, with its y:
    /// the click hit map. None on the grid tail or the gaps.
    pub fn hit(&self, y: i32) -> Option<(u64, i32)> {
        self.frame
            .iter()
            .find(|f| y >= f.1 && y < f.1 + f.2)
            .map(|f| (f.0, f.1))
    }

    /// H-4d: the span a grid cell was written under (None: no obj).
    fn grid_tag(&self, r: usize, c: usize) -> Option<SpanTag> {
        let cell = *self.grid.row(r).get(c)?;
        let t = self.spans.get(cell.span)?;
        if t.obj == 0 {
            None
        } else {
            Some(t)
        }
    }

    /// The obj runs on live-grid row `r`, keyed by their start column + 1
    /// (a row-unique u16, the grid's analogue of a block's obj index): the
    /// virtual trailing block's `runs_on_row`.
    pub fn grid_runs(&self, r: usize) -> Vec<ObjRun> {
        let row = self.grid.row(r);
        let mut runs = Vec::new();
        let mut c = 0;
        while c < row.len() {
            match self.grid_tag(r, c) {
                None => c += 1,
                Some(t) => {
                    let start = c;
                    let mut text = String::new();
                    while c < row.len() && self.grid_tag(r, c) == Some(t) {
                        text.push(row[c].ch);
                        c += 1;
                    }
                    runs.push(ObjRun {
                        obj: (start as u16).saturating_add(1),
                        text,
                    });
                }
            }
        }
        runs
    }

    /// A grid run by key: (start col, cols, its span).
    pub fn grid_run(&self, r: usize, key: u16) -> Option<(usize, usize, SpanTag)> {
        let start = (key as usize).checked_sub(1)?;
        let t = self.grid_tag(r, start)?;
        let row = self.grid.row(r);
        let mut c = start;
        while c < row.len() && self.grid_tag(r, c) == Some(t) {
            c += 1;
        }
        Some((start, c - start, t))
    }

    /// The (type, resolved ref) a grid run presents.
    pub fn grid_run_obj(&self, r: usize, key: u16) -> Option<(&str, &str)> {
        let (_, _, t) = self.grid_run(r, key)?;
        self.scrollback.obj_in_block(t.block, t.obj)
    }

    /// The run key (start col + 1) for the obj at grid cell (r, col), walking
    /// left to the run's start; None if the cell carries no obj.
    fn run_key_at(&self, r: usize, col: usize) -> Option<u16> {
        let t = self.grid_tag(r, col)?;
        let mut start = col;
        while start > 0 && self.grid_tag(r, start - 1) == Some(t) {
            start -= 1;
        }
        Some((start as u16).saturating_add(1))
    }

    /// The obj run under a tail-relative point: (grid row, run key). In NORMAL
    /// mode the tail is proportional (PL-4b), so the click inverts through the
    /// last render's cached layout -- `x`/`y` relative to the tail's top-left
    /// (the caller subtracts the tail's screen-y): the laid line under `y`, its
    /// logical column under `x`, then the `prov` inverse back to the grid
    /// (row, col). In alt-screen there is no cache, so it falls back to the mono
    /// cell grid (`cw` x `ch`), the geometry `paint_grid` uses.
    pub fn grid_hit(&self, x: i32, y: i32, cw: i32, ch: i32) -> Option<(usize, u16)> {
        if x < 0 || y < 0 {
            return None;
        }
        match &self.live_laid {
            Some((live_lb, prov, _)) => {
                let line = live_lb.lines.iter().find(|l| y >= l.y && y < l.y + l.h)?;
                let col = col_at_x(line, x)?;
                let cols = self.grid.dims().0;
                let (r, rc) = prov_inverse(prov, line.src_item, line.src_row, col, cols)?;
                self.run_key_at(r, rc).map(|k| (r, k))
            }
            None => {
                if cw <= 0 || ch <= 0 {
                    return None;
                }
                let (r, c) = ((y / ch) as usize, (x / cw) as usize);
                self.run_key_at(r, c).map(|k| (r, k))
            }
        }
    }

    /// The tail-relative display rect (x, y, w, h) of grid run (r, key): the
    /// proportional x-extent + laid-line y/h from the cached layout in NORMAL
    /// mode, or the mono cell rect (`cw` x `ch`) in alt-screen / before a
    /// render. A soft-wrapped run reports its FIRST laid piece (this rect only
    /// rides the menu-witness say line; the menu anchors at the pointer). None
    /// when the cell carries no run.
    pub fn grid_run_rect(&self, r: usize, key: u16, cw: i32, ch: i32) -> Option<(i32, i32, i32, i32)> {
        let (c0, n, _) = self.grid_run(r, key)?;
        match &self.live_laid {
            Some((live_lb, prov, _)) => {
                let &(item, row, start) = prov.get(r)?;
                let (a, b) = (start + c0, start + c0 + n);
                for line in live_lb.lines.iter() {
                    if line.src_item != item || line.src_row != row {
                        continue;
                    }
                    let lo = line.segs.first().map(|s| s.src_col).unwrap_or(0);
                    let hi = line
                        .segs
                        .last()
                        .map(|s| s.src_col + s.refs.len())
                        .unwrap_or(lo);
                    let (aa, bb) = (a.max(lo), b.min(hi));
                    if aa >= bb {
                        continue;
                    }
                    let x0 = line_col_x(line, aa);
                    let x1 = line_col_x(line, bb);
                    return Some((x0, line.y, (x1 - x0).max(1), line.h));
                }
                None
            }
            None => Some((c0 as i32 * cw, r as i32 * ch, (n as i32 * cw).max(1), ch)),
        }
    }

    /// The record -> model dispatch (14.11.2).
    pub fn apply(&mut self, rec: Record) {
        match rec {
            Record::CellDiff {
                changed,
                cursor,
                wrapped,
                top_continues,
            } => {
                // A cell written after the open `rule` frame (its serial at
                // or past the rule's) is the line the rule precedes: the
                // episode ends here, not at an op (transcript::end_rule). A
                // scroll re-reports older cells with their OLD serials, so
                // it does not end it; a bare newline changes no cell.
                if let Some(rule) = self.scrollback.rule_open() {
                    if changed.iter().any(|(_, _, c)| c.span >= rule && c.span != 0) {
                        self.scrollback.end_rule();
                    }
                }
                self.grid.apply_celldiff(&changed, cursor, &wrapped, top_continues);
                // The scrolled-off fragment the transcript holds continues
                // into row 0 only while the producer says so; the moment it
                // does not (row 0 restarted as a line of its own), the
                // fragment is complete as it stands and lands as a line --
                // never glued to whatever row scrolls off next. The alt
                // screen reports false throughout (it has no history) but
                // the main screen returns intact at alt-leave, so a fragment
                // rides out a TUI session and rejoins its row then.
                if !top_continues && self.mode == ScreenMode::Normal {
                    self.scrollback.flush_scroll_pending(&self.spans);
                }
            }
            Record::ScrollOff { rows, wrapped } => {
                self.scrollback
                    .push_scrolled_rows(&rows, &wrapped, &self.spans)
            }
            Record::Control(c) => self.apply_control(c),
            // The producer's top flag on the next normal-screen CellDiff says
            // whether a held soft-wrapped fragment still has its continuation
            // (PL-3); the mode flip itself decides nothing.
            Record::Mode(m) => self.mode = m,
        }
    }

    fn apply_control(&mut self, c: Control) {
        match c {
            // A Beacon frame is the COMPLETE ESC ] 1936 ; ... ST -- feed it to the
            // SAME beacon parser the console path uses; it drives the zone/block
            // cut + span state on the scrollback and touches no cells (14.11.4).
            Control::Osc1936Raw { serial, frame } => {
                self.scrollback.feed_frame(&frame, serial);
                // H-4d: the cells the producer writes next carry `serial`;
                // they mean THIS state (after the frame) -- incl. the
                // structure (pre / table cell / rule) the tile rebuilds from.
                self.spans.note(serial, self.scrollback.span_tag());
                // BEACON.md 12.12: a program's `mark k=prog` names the tile
                // exactly as an OSC title does -- one `title`, latest wins
                // across both channels (the record stream keeps their order).
                if let Some(p) = self.scrollback.take_prog() {
                    self.title = p;
                }
            }
            Control::Title(t) => self.title = t,
            // The cwd report, forwarded raw (BEACON.md 12.11): the transcript's
            // one decoder applies it, as on the console path.
            Control::Osc7Raw(body) => self.scrollback.apply_cwd_report(&body),
            Control::Bell => self.bell = true,
            Control::Exit(code) => self.exit = Some(code),
            // The down-channel resize was applied on the pts; no model state here.
            Control::WinsizeAck => {}
        }
    }

    /// Resize the tile (halcyond drives geometry, 14.11.6): the grid reshapes
    /// now; the kaua-term replies with a full CellDiff. The scrollback is
    /// flow-based and reflows at layout, so it takes no dims here.
    pub fn resize(&mut self, cols: usize, rows: usize) {
        // The normal screen reflows (the transcript's content model: a
        // soft-wrapped row is half of one logical line); the alt screen is
        // the TUI's to repaint.
        self.grid.resize(cols, rows, self.mode == ScreenMode::Normal);
    }

    /// `Some(code)` once the hosted child has exited (the teardown trigger,
    /// 14.11.10).
    pub fn exited(&self) -> Option<i32> {
        self.exit
    }

    /// Take + clear the pending bell affordance (the render rings it once).
    pub fn take_bell(&mut self) -> bool {
        core::mem::replace(&mut self.bell, false)
    }

    /// Paint the tile into `cart` (HALCYON.md 14.11.3). Returns the total
    /// content height in px (for scroll clamping by the caller).
    ///
    /// Normal mode: the scrollback flow renders above, the live grid renders as
    /// a fixed-height tail (`grid_rows * cell_h`) at the bottom; the content is
    /// bottom-anchored, raised by `scroll_up` px (0 = the grid sits at the view
    /// bottom, history off the top; scrolling up reveals history). Alt-screen
    /// mode: the grid alone, full-tile from the top-left, scrollback frozen +
    /// hidden. The `cart` is `reset()` first, so the caller passes one reusable
    /// display list. Grid glyphs come from FACE_MONO (the tile is a terminal);
    /// the scrollback flows through the proportional `layout_block`/`render_block`
    /// exactly as the console transcript does.
    ///
    /// Layout is windowed: the frozen blocks' heights come from the cache
    /// (filled once per block per width), the exact content height and every
    /// block's screen-y follow from them, and only the blocks intersecting
    /// the view are laid out, each dropped after it renders. The open block
    /// (the one block no cache can position: it changes) is laid out whole
    /// every render and stays alive across the walk, so at most TWO laid
    /// blocks exist at once, and a block that merely touches the view is
    /// laid out whole -- the transient is O(view + 2 x the open-block cap),
    /// `OPEN_BLOCK_MAX_COST` bounding both, whatever the history holds and
    /// wherever the view scrolled.
    ///
    /// `scroll_up` is the view's offset from the bottom, in pixels; a Normal
    /// mode `mark` (the cursor row + its selected obj run) drags it so the
    /// row is visible (Helix: the view follows the cursor), and the clamped
    /// result is written back. The mark paints the row's band and, for a
    /// selected run, the ember underline (the console renderer's pass).
    pub fn render(
        &mut self,
        cart: &mut Cartoon,
        w: usize,
        h: usize,
        gs: &mut GlyphSource,
        sheet: &Sheet,
        scroll_up: &mut i32,
        mark: Option<Mark>,
    ) -> i32 {
        cart.reset();
        cart.ops.push(Op::Clear {
            color: sheet.ground,
        });
        let (_cw, cell_h, _base) = gs.mono_cell();
        let grid_h = self.grid.dims().1 as i32 * cell_h;
        self.laid_last = 0;
        self.laid_lines_last = 0;
        self.frame.clear();

        if self.mode == ScreenMode::AltScreen {
            // The tail is the mono grid; a click hits it by cell, not through a
            // proportional cache -- drop any stale normal-mode layout so
            // `grid_hit` takes the mono path.
            self.live_laid = None;
            paint_grid(cart, &self.grid, 0, 0, gs, sheet);
            return grid_h;
        }

        let widthi = w as i32;
        let viewh = h as i32;
        self.laid_last += self.sync_heights(widthi, sheet, gs);

        // The exact content height from the cached heights: a leading gap,
        // every frozen block plus its trailing gap, then the open block (the
        // newest, un-frozen history; no trailing gap -- the grid follows it
        // directly as the live tail). The gap after a block depends on the
        // pair (a prompt runs into its output as one entry), so it is read
        // per index by the three walks below.
        let frozen_kinds: Vec<BlockKind> = self
            .scrollback
            .frozen_blocks()
            .iter()
            .map(|b| b.kind)
            .collect();
        let open_kind = self.scrollback.open_block().kind;
        // A frozen block that laid nothing (cells mode keeps a zone-less
        // block alive for its obj table even when its text is still on the
        // grid) takes no gap either -- else every such block is a phantom
        // band.
        let gap_after = |i: usize, hgt: i32| -> i32 {
            if hgt == 0 {
                return 0;
            }
            let next = frozen_kinds.get(i + 1).copied().unwrap_or(open_kind);
            let this = frozen_kinds.get(i).copied().unwrap_or(open_kind);
            block_gap_between(this, next, sheet)
        };
        let mut total = sheet.block_gap;
        for (i, &(_, _, hgt)) in self.heights.iter().enumerate() {
            total += hgt + gap_after(i, hgt);
        }
        let open_lb = layout_block(self.scrollback.open_block(), widthi, sheet, gs);
        self.laid_last += 1;
        self.laid_lines_last += open_lb.lines.len();
        total += open_lb.height;

        // PL-4: the live grid renders PROPORTIONALLY as the normal-mode tail
        // (HALCYON 14.13) -- its soft-wrapped rows joined into logical lines and
        // re-wrapped at the tile width, replacing the fixed mono grid. Laid only
        // through the content rows so a screen of trailing blanks below the
        // prompt is not painted (the bottom-anchored view would else float the
        // prompt mid-tile). `prov` maps a grid row -> (logical line, start col).
        let live_cols = self.grid.dims().0;
        let live_rows = self.grid.content_rows();
        let live_wrapped = self.grid.wrapped();
        let (live_b, prov) = self.scrollback.live_block(
            self.grid.cells(),
            live_cols,
            live_rows,
            live_wrapped,
            &self.spans,
            self.grid.top_continues(),
        );
        let live_lb = layout_block(&live_b, widthi, sheet, gs);
        self.laid_last += 1;
        self.laid_lines_last += live_lb.lines.len();

        let content_h = total + live_lb.height;

        // The mark's row drags the view: locate its content-relative span
        // (a frozen block's from the cached heights; the open block's is
        // laid already) and adjust scroll_up so it is visible.
        if let Some(m) = mark {
            let mut rel = sheet.block_gap;
            let mut span: Option<(i32, i32)> = None;
            for (i, (b, &(_, _, hgt))) in self
                .scrollback
                .frozen_blocks()
                .iter()
                .zip(self.heights.iter())
                .enumerate()
            {
                if b.id == m.block {
                    let lb = layout_block(b, widthi, sheet, gs);
                    span = Some(match laid_line_for(&lb, m.item, m.row) {
                        Some((ly, lh)) => (rel + ly, lh),
                        None => (rel, hgt.max(1)),
                    });
                    break;
                }
                rel += hgt + gap_after(i, hgt);
            }
            if span.is_none() && m.block == u64::MAX {
                span = Some(match laid_line_for(&open_lb, m.item, m.row) {
                    Some((ly, lh)) => (rel + ly, lh),
                    None => (rel, open_lb.height.max(1)),
                });
            }
            if span.is_none() && m.block == GRID_KEY {
                let sp = live_row_spans(&live_lb, &prov, m.item, live_cols);
                if let (Some(&(y0, _)), Some(&(y1, h1))) = (sp.first(), sp.last()) {
                    span = Some((total + y0, (y1 + h1) - y0));
                }
            }
            if let Some((r, lh)) = span {
                // Visible iff scroll_up <= from_bottom <= scroll_up + viewh - lh.
                let from_bottom = content_h - r - lh;
                if from_bottom < *scroll_up {
                    *scroll_up = from_bottom;
                } else if from_bottom > *scroll_up + viewh - lh {
                    *scroll_up = from_bottom - (viewh - lh).max(0);
                }
            }
        }
        *scroll_up = (*scroll_up).clamp(0, (content_h - viewh).max(0));
        let su = *scroll_up;
        let y0 = if content_h <= viewh {
            0
        } else {
            viewh - content_h + su
        };

        // Bottom-anchor [scrollback][grid]: walk the blocks by their cached
        // heights, laying out + rendering only those that intersect the view.
        let mut y = y0 + sheet.block_gap;
        for (i, (b, &(_, _, hgt))) in self
            .scrollback
            .frozen_blocks()
            .iter()
            .zip(self.heights.iter())
            .enumerate()
        {
            self.frame.push((b.id, y, hgt));
            if y + hgt >= 0 && y <= viewh {
                let lb = layout_block(b, widthi, sheet, gs);
                debug_assert_eq!(lb.height, hgt, "a frozen block's height is deterministic");
                paint_mark(cart, &lb, y, w, sheet, mark.filter(|m| m.block == b.id));
                render_block(cart, &lb, y, gs);
                paint_run(cart, &lb, y, sheet, mark.filter(|m| m.block == b.id));
                self.laid_last += 1;
                self.laid_lines_last += lb.lines.len();
            }
            y += hgt + gap_after(i, hgt);
        }
        self.frame.push((u64::MAX, y, open_lb.height));
        if y + open_lb.height >= 0 && y <= viewh {
            let m = mark.filter(|m| m.block == u64::MAX);
            paint_mark(cart, &open_lb, y, w, sheet, m);
            render_block(cart, &open_lb, y, gs);
            paint_run(cart, &open_lb, y, sheet, m);
        }
        y += open_lb.height;
        // `y` is now the grid tail's screen-y (== y0 + total). H-4d: the
        // live grid is the virtual trailing block (14.11.5) -- in the frame
        // under GRID_KEY, its marked row banded under the cells, its
        // selected run underlined over them.
        self.frame.push((GRID_KEY, y, live_lb.height));
        let gm = mark.filter(|m| m.block == GRID_KEY);
        // The marked grid row's band, proportional (a soft-wrapped row spans
        // several laid lines) -- painted UNDER the cells.
        if let Some(m) = gm {
            for (by, bh) in live_row_spans(&live_lb, &prov, m.item, live_cols) {
                cart.ops.push(Op::Rect {
                    x: 0,
                    y: y + by,
                    w: w as u32,
                    h: bh as u32,
                    color: sheet.sel_bg,
                });
            }
        }
        render_block(cart, &live_lb, y, gs);
        // The caret: ONE source of truth (the grid cursor), placed at the
        // proportional x of its character boundary (HALCYON 14.13; subsumes s2,
        // the stray cursor adrift from the rows).
        let (cr, cc, cvis) = self.grid.cursor();
        if cvis {
            if let Some(&(item, row, start)) = prov.get(cr) {
                let (cx, cy, chh) = caret_in_block(&live_lb, item, row, start + cc, sheet);
                cart.ops.push(Op::Rect {
                    x: cx,
                    y: y + cy,
                    w: sheet.mark_w as u32,
                    h: chh as u32,
                    color: libhalcyon::theme::DAYLIGHT.ember,
                });
            }
        }
        // The selected obj run underlined, proportional -- each laid piece of a
        // wrapped run over its real x-extent, painted OVER the cells.
        if let Some(Mark {
            item,
            obj: Some(key),
            ..
        }) = gm
        {
            if let Some((c0, n, _)) = self.grid_run(item, key) {
                for (by, x0, x1) in live_run_underline(&live_lb, &prov, item, c0, n, sheet.mark_w) {
                    cart.ops.push(Op::Rect {
                        x: x0,
                        y: y + by,
                        w: (x1 - x0).max(1) as u32,
                        h: sheet.mark_w as u32,
                        color: libhalcyon::theme::DAYLIGHT.ember,
                    });
                }
            }
        }
        // Cache this frame's proportional tail for the click inverse (`y` is the
        // tail's screen-y). Moved in AFTER every read above (`live_lb` / `prov`
        // are done being borrowed); `grid_hit` / `grid_run_rect` invert through
        // it until the next render replaces it.
        self.live_laid = Some((live_lb, prov, y));
        content_h
    }

    /// Drop every cached height: a SHEET change (a display scale change,
    /// HALCYON-SCALE 6) re-sizes every block at the same width, which the
    /// width key alone cannot see.
    pub fn invalidate_heights(&mut self) {
        self.heights.clear();
        self.heights_width = 0;
    }

    /// Bring the height cache in line with the frozen deque at `width`:
    /// a width change invalidates everything; blocks evicted at the front
    /// drop off; blocks frozen since the last render are laid out ONCE for
    /// their height and dropped. Returns the number of blocks laid out.
    fn sync_heights(&mut self, width: i32, sheet: &Sheet, gs: &mut GlyphSource) -> usize {
        if self.heights_width != width {
            self.heights.clear();
            self.heights_width = width;
        }
        let frozen = self.scrollback.frozen_blocks();
        // Front eviction: ids are strictly increasing along the deque, so
        // every cached id below the oldest live block's is gone.
        if let Some(oldest) = frozen.front().map(|b| b.id) {
            while self.heights.front().is_some_and(|&(id, _, _)| id < oldest) {
                self.heights.pop_front();
            }
        } else {
            self.heights.clear();
        }
        // Pairwise alignment on (id, exit): any disagreement truncates the
        // cache there, and the tail is re-laid below -- a floating exit mark
        // landing on the last frozen block re-lays exactly that block.
        let mut keep = 0;
        for (cached, b) in self.heights.iter().zip(frozen.iter()) {
            if cached.0 != b.id || cached.1 != b.exit {
                break;
            }
            keep += 1;
        }
        self.heights.truncate(keep);
        let mut laid = 0;
        for b in frozen.iter().skip(self.heights.len()) {
            let lb = layout_block(b, width, sheet, gs);
            self.heights.push_back((b.id, b.exit, lb.height));
            laid += 1;
        }
        laid
    }
}

/// H-4d: the frame / `Mark` key of the live grid, the virtual trailing block
/// (`Mark.item` is then the grid row, `Mark.obj` a `grid_runs` key).
pub const GRID_KEY: u64 = u64::MAX - 1;

/// H-4d: a Normal-mode cursor position in a tile's transcript, as `render`
/// paints it: the block (id, u64::MAX for the open block, or GRID_KEY), the
/// row (an item, and a table row when the item is a table; the grid row
/// under GRID_KEY), and the selected obj run on it, if any.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Mark {
    pub block: u64,
    pub item: usize,
    pub row: usize,
    pub obj: Option<u16>,
}

/// The cursor row's band under the text (`sel_bg`, full width).
fn paint_mark(
    cart: &mut Cartoon,
    lb: &LaidBlock,
    y: i32,
    w: usize,
    sheet: &Sheet,
    m: Option<Mark>,
) {
    if let Some(m) = m {
        if let Some((ly, lh)) = laid_line_for(lb, m.item, m.row) {
            cart.ops.push(Op::Rect {
                x: 0,
                y: y + ly,
                w: w as u32,
                h: lh.max(0) as u32,
                color: sheet.sel_bg,
            });
        }
    }
}

/// The selected run's ember underline (the sheet's 2-px mark) over the text.
fn paint_run(cart: &mut Cartoon, lb: &LaidBlock, y: i32, sheet: &Sheet, m: Option<Mark>) {
    if let Some(Mark {
        item,
        row,
        obj: Some(obj),
        ..
    }) = m
    {
        if let Some(r) = run_rect(lb, item, row, obj) {
            cart.ops.push(Op::Rect {
                x: r.0,
                y: y + r.1 + r.3 - sheet.mark_w,
                w: r.2.max(1) as u32,
                h: sheet.mark_w as u32,
                color: libhalcyon::theme::DAYLIGHT.ember,
            });
        }
    }
}

/// Paint the live grid's cells at screen origin `(x0, y0)` into `cart` (a mono
/// cell store: per-cell bg rect when it differs from the ground, then the glyph,
/// then the underline; the block cursor beam last). Out-of-range is impossible
/// -- `Grid::row` and `Grid::cursor` are already clamped (grid.rs), the tile
/// trust boundary (14.11.12).
/// PL-4: the x of column `col` within ONE laid line (line-scoped, for the
/// per-line banding + underline geometry). Past the line's content -> its end.
fn line_col_x(line: &LaidLine, col: usize) -> i32 {
    for seg in line.segs.iter() {
        let n = seg.refs.len();
        if col >= seg.src_col && col < seg.src_col + n {
            return seg.xs[col - seg.src_col];
        }
    }
    line.segs.last().map(|s| s.x_end).unwrap_or(0)
}

/// PL-4b: the logical column of laid line `line` under block-relative x `x` --
/// the seg whose [x, x_end) contains it, then the glyph cell within it (each
/// glyph owns [xs[i], xs[i+1])). None past the content: a click beyond the last
/// glyph, or in a gap between runs, hits no cell (so no run).
fn col_at_x(line: &LaidLine, x: i32) -> Option<usize> {
    for seg in line.segs.iter() {
        if x >= seg.x && x < seg.x_end {
            let n = seg.refs.len();
            for i in 0..n {
                let hi = if i + 1 < n { seg.xs[i + 1] } else { seg.x_end };
                if x < hi {
                    return Some(seg.src_col + i);
                }
            }
            return Some(seg.src_col + n.saturating_sub(1));
        }
    }
    None
}

/// PL-4b: the inverse of `prov` -- the grid (row, col-in-row) that logical
/// column `col` of laid line (`item`, `row`) came from. The joined rows of one
/// logical line hold disjoint column ranges ([start, start+cols)), so at most
/// one row matches; the rows of one rebuilt table (the lines of one pre) share
/// the item and are told apart by `row` -- matching on the item alone named
/// the FIRST row for every click on the structure. None if no row covers it.
fn prov_inverse(
    prov: &[(usize, usize, usize)],
    item: usize,
    row: usize,
    col: usize,
    cols: usize,
) -> Option<(usize, usize)> {
    for (r, &(it, rw, start)) in prov.iter().enumerate() {
        if it == item && rw == row && start <= col && col < start + cols {
            return Some((r, col - start));
        }
    }
    None
}

/// PL-4: the (block-relative y, h) spans of `live`'s laid lines that show grid
/// row `r`'s columns. A soft-wrapped row can span several laid lines; a laid
/// line shared by two grid rows bands for both (matching the mono full-row
/// band). Empty for a row past `prov`.
fn live_row_spans(
    live: &LaidBlock,
    prov: &[(usize, usize, usize)],
    r: usize,
    cols: usize,
) -> Vec<(i32, i32)> {
    let Some(&(item, row, start)) = prov.get(r) else {
        return Vec::new();
    };
    let end = start + cols;
    let mut spans = Vec::new();
    for line in live.lines.iter() {
        if line.src_item != item || line.src_row != row {
            continue;
        }
        let lo = line.segs.first().map(|s| s.src_col).unwrap_or(0);
        let hi = line
            .segs
            .last()
            .map(|s| s.src_col + s.refs.len())
            .unwrap_or(lo);
        if lo < end && hi > start {
            spans.push((line.y, line.h));
        }
    }
    spans
}

/// PL-4: the (block-relative line-bottom y, x0, x1) underline segments for grid
/// row `r`'s obj run at grid columns [rc0, rc0+n) -- one per laid line the run
/// crosses, so a wrapped run underlines each piece over its real x-extent.
fn live_run_underline(
    live: &LaidBlock,
    prov: &[(usize, usize, usize)],
    r: usize,
    rc0: usize,
    n: usize,
    mark_w: i32,
) -> Vec<(i32, i32, i32)> {
    let Some(&(item, row, start)) = prov.get(r) else {
        return Vec::new();
    };
    let c0 = start + rc0;
    let c1 = c0 + n;
    let mut out = Vec::new();
    for line in live.lines.iter() {
        if line.src_item != item || line.src_row != row {
            continue;
        }
        let lo = line.segs.first().map(|s| s.src_col).unwrap_or(0);
        let hi = line
            .segs
            .last()
            .map(|s| s.src_col + s.refs.len())
            .unwrap_or(lo);
        let a = c0.max(lo);
        let b = c1.min(hi);
        if a >= b {
            continue;
        }
        out.push((line.y + line.h - mark_w, line_col_x(line, a), line_col_x(line, b)));
    }
    out
}

fn paint_grid(
    cart: &mut Cartoon,
    grid: &Grid,
    x0: i32,
    y0: i32,
    gs: &mut GlyphSource,
    sheet: &Sheet,
) {
    let (cw, ch, base) = gs.mono_cell();
    let (cols, rows) = grid.dims();
    let gen = gs.gen(); // stable across this frame: glyph() inserts never regen
    for r in 0..rows {
        let cy = y0 + r as i32 * ch;
        for c in 0..cols {
            let cell = grid.row(r)[c];
            let cx = x0 + c as i32 * cw;
            let (fg, bg) = if cell.attrs & ATTR_REVERSE != 0 {
                (cell.bg, cell.fg)
            } else {
                (cell.fg, cell.bg)
            };
            if bg != sheet.ground {
                cart.ops.push(Op::Rect {
                    x: cx,
                    y: cy,
                    w: cw as u32,
                    h: ch as u32,
                    color: bg,
                });
            }
            if cell.ch != ' ' && cell.ch != '\0' {
                // The GRID mono (advance 10 at 100%): a full-screen program
                // owns its cells at the pts geometry, not the document's
                // island size.
                if let Some(gref) = gs.glyph(FACE_MONO, sheet.mono_grid_px, cell.ch) {
                    cart.push_glyphs(gen, cx, cy + base, fg, &[gref]);
                }
            }
            if cell.attrs & ATTR_UNDERLINE != 0 {
                cart.ops.push(Op::Rect {
                    x: cx,
                    y: cy + ch - sheet.hairline,
                    w: cw as u32,
                    h: sheet.hairline as u32,
                    color: fg,
                });
            }
        }
    }
    let (curx, cury, vis) = grid.cursor();
    if vis {
        cart.ops.push(Op::Rect {
            x: x0 + curx as i32 * cw,
            y: y0 + cury as i32 * ch,
            w: sheet.mark_w as u32,
            h: ch as u32,
            color: sheet.accent,
        });
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::transcript::Item;
    use alloc::vec;
    use alloc::vec::Vec;
    use vt::Cell;

    fn cell(ch: char) -> Cell {
        Cell {
            ch,
            fg: 0x00FF00,
            bg: 0,
            attrs: 0,
            span: 0,
        }
    }

    fn tile() -> Tile {
        Tile::new(20, 4, vt::BONFIRE)
    }

    #[test]
    fn a_celldiff_clearing_the_top_flag_finalizes_the_held_fragment() {
        // A soft-wrapped row scrolls off (held as a fragment); the producer's
        // CellDiffs say row 0 continues it (still held, joined live); then
        // row 0 restarts -- the flag clears and the fragment lands as a line
        // of its own, never glued to whatever scrolls off next.
        let mut t = tile();
        let row: Vec<Cell> = "abcdefgh".chars().map(cell).collect();
        t.apply(Record::ScrollOff {
            rows: vec![row],
            wrapped: vec![true],
        });
        assert!(t.scrollback.open_block().items.is_empty(), "held");
        t.apply(Record::CellDiff {
            changed: vec![(0, 0, cell('i')), (0, 1, cell('j'))],
            cursor: (0, 2, true),
            wrapped: vec![false; 4],
            top_continues: true,
        });
        assert!(t.scrollback.open_block().items.is_empty(), "still held while row 0 continues it");
        assert!(t.grid.top_continues());
        t.apply(Record::CellDiff {
            changed: vec![(0, 0, cell('z'))],
            cursor: (0, 1, true),
            wrapped: vec![false; 4],
            top_continues: false,
        });
        assert!(!t.grid.top_continues());
        let items = &t.scrollback.open_block().items;
        assert_eq!(items.len(), 1, "the fragment is a line now");
        match &items[0] {
            crate::transcript::Item::Line(l) => {
                assert_eq!(l.cells.iter().map(|c| c.ch).collect::<String>(), "abcdefgh")
            }
            _ => panic!("a line"),
        }
    }

    #[test]
    fn a_held_fragment_rides_out_a_tui_session_and_rejoins_its_row_at_leave() {
        // The alt screen's CellDiffs carry a clear flag (no history there)
        // but the main screen comes back whole: the fragment held at entry
        // is still held at leave, and the restored main's flag joins it.
        let mut t = tile();
        let row: Vec<Cell> = "abcdefgh".chars().map(cell).collect();
        t.apply(Record::ScrollOff {
            rows: vec![row],
            wrapped: vec![true],
        });
        t.apply(Record::CellDiff {
            changed: vec![(0, 0, cell('i'))],
            cursor: (0, 1, true),
            wrapped: vec![false; 4],
            top_continues: true,
        });
        t.apply(Record::Mode(ScreenMode::AltScreen));
        t.apply(Record::CellDiff {
            changed: vec![(0, 0, cell('T'))],
            cursor: (0, 1, true),
            wrapped: vec![false; 4],
            top_continues: false,
        });
        assert!(t.scrollback.open_block().items.is_empty(), "held through the TUI");
        t.apply(Record::Mode(ScreenMode::Normal));
        t.apply(Record::CellDiff {
            changed: vec![(0, 0, cell('i'))],
            cursor: (0, 1, true),
            wrapped: vec![false; 4],
            top_continues: true,
        });
        assert!(t.scrollback.open_block().items.is_empty(), "still held: row 0 continues it");
        assert!(t.grid.top_continues());
        let (lb, prov) = t.scrollback.live_block(t.grid.cells(), 20, 1, &[false], &t.spans, true);
        match &lb.items[0] {
            crate::transcript::Item::Line(l) => {
                assert_eq!(l.cells.iter().map(|c| c.ch).collect::<String>(), "abcdefghi")
            }
            _ => panic!("a line"),
        }
        assert_eq!(prov[0].2, 8, "row 0 sits after the eight held cells");
    }

    #[test]
    fn celldiff_lands_on_the_grid_not_the_scrollback() {
        let mut t = tile();
        t.apply(Record::CellDiff {
            changed: vec![(0, 0, cell('h')), (0, 1, cell('i'))],
            cursor: (0, 2, true),
            wrapped: vec![],
            top_continues: false,
        });
        assert_eq!(t.grid.row(0)[0].ch, 'h');
        assert_eq!(t.grid.row(0)[1].ch, 'i');
        assert_eq!(t.grid.cursor(), (0, 2, true));
        // the grid is not history: the scrollback's open block stays empty.
        assert!(t.scrollback.open_block().items.is_empty());
    }

    #[test]
    fn scrolloff_appends_rows_to_the_scrollback_as_lines() {
        let mut t = tile();
        t.apply(Record::ScrollOff {
            rows: vec![vec![cell('a'), cell('b')], vec![cell('c')]],
            wrapped: vec![false, false],
        });
        let items = &t.scrollback.open_block().items;
        assert_eq!(items.len(), 2, "two scrolled rows -> two Line items");
        let line_txt = |it: &Item| -> Vec<char> {
            match it {
                Item::Line(l) => l.cells.iter().map(|c| c.ch).collect(),
                _ => Vec::new(),
            }
        };
        assert_eq!(line_txt(&items[0]), vec!['a', 'b']);
        assert_eq!(line_txt(&items[1]), vec!['c']);
    }

    #[test]
    fn mode_flip_is_recorded() {
        let mut t = tile();
        assert_eq!(t.mode, ScreenMode::Normal);
        t.apply(Record::Mode(ScreenMode::AltScreen));
        assert_eq!(t.mode, ScreenMode::AltScreen);
        t.apply(Record::Mode(ScreenMode::Normal));
        assert_eq!(t.mode, ScreenMode::Normal);
    }

    #[test]
    fn control_records_latch_title_exit_bell() {
        let mut t = tile();
        assert_eq!(t.title, "");
        assert_eq!(t.exited(), None);
        assert!(!t.take_bell());

        t.apply(Record::Control(Control::Title(String::from(
            "edit - foo.rs",
        ))));
        assert_eq!(t.title, "edit - foo.rs");

        t.apply(Record::Control(Control::Bell));
        assert!(t.take_bell(), "bell latched");
        assert!(!t.take_bell(), "bell cleared after one take");

        t.apply(Record::Control(Control::WinsizeAck)); // no-op, must not disturb state
        assert_eq!(t.exited(), None);

        t.apply(Record::Control(Control::Exit(0)));
        assert_eq!(t.exited(), Some(0));
    }

    // BEACON.md 12.11 on the tile path: the cwd report crosses the wire raw
    // and the transcript's one decoder applies it -- a good report sets the
    // directory, a foreign host's or a malformed one changes nothing, and
    // the grid is untouched either way.
    #[test]
    fn a_raw_cwd_report_sets_the_scrollback_directory_through_the_one_decoder() {
        let mut t = tile();
        assert_eq!(t.scrollback.cwd(), "");
        t.apply(Record::Control(Control::Osc7Raw(
            b"file://localhost/lib/aurora".to_vec(),
        )));
        assert_eq!(t.scrollback.cwd(), "/lib/aurora");
        t.apply(Record::Control(Control::Osc7Raw(b"file:///a%20b".to_vec())));
        assert_eq!(t.scrollback.cwd(), "/a b", "percent-decoded, an empty host is ours");
        t.apply(Record::Control(Control::Osc7Raw(
            b"file://otherhost/elsewhere".to_vec(),
        )));
        assert_eq!(t.scrollback.cwd(), "/a b", "another host's report is not ours");
        t.apply(Record::Control(Control::Osc7Raw(b"file://localhost/no\x01ctl".to_vec())));
        assert_eq!(t.scrollback.cwd(), "/a b", "a control byte: rejected");
        t.apply(Record::Control(Control::Osc7Raw(b"garbage".to_vec())));
        assert_eq!(t.scrollback.cwd(), "/a b", "not a file URL: dropped whole");
        assert!(
            t.grid.cells().iter().all(|c| c.ch == ' ' || c.ch == '\0'),
            "the report wrote no cell"
        );
    }

    // BEACON.md 12.12: `mark k=prog` names the tile like an OSC title does;
    // the two channels share one `title`, latest wins in record order; an
    // empty or oversize name is dropped whole.
    #[test]
    fn a_prog_mark_names_the_tile_and_an_osc_title_after_it_wins() {
        let mut t = tile();
        let mut f: Vec<u8> = Vec::new();
        beacon::wire::point(&mut f, beacon::wire::Op::Mark, &[("k", "prog"), ("text", "ut")]);
        t.apply(Record::Control(Control::Osc1936Raw {
            serial: 1,
            frame: f.clone(),
        }));
        assert_eq!(t.title, "ut");
        t.apply(Record::Control(Control::Title(String::from("nora"))));
        assert_eq!(t.title, "nora", "a program's OSC title after the mark wins");
        t.apply(Record::Control(Control::Osc1936Raw { serial: 2, frame: f }));
        assert_eq!(t.title, "ut", "the shell re-asserts at its next prompt");
        let mut e: Vec<u8> = Vec::new();
        beacon::wire::point(&mut e, beacon::wire::Op::Mark, &[("k", "prog"), ("text", "  ")]);
        t.apply(Record::Control(Control::Osc1936Raw { serial: 3, frame: e }));
        assert_eq!(t.title, "ut", "an empty name is dropped whole");
        let long = alloc::string::String::from_utf8(alloc::vec![b'x'; 300]).unwrap();
        let mut l: Vec<u8> = Vec::new();
        beacon::wire::point(&mut l, beacon::wire::Op::Mark, &[("k", "prog"), ("text", &long)]);
        t.apply(Record::Control(Control::Osc1936Raw { serial: 4, frame: l }));
        assert_eq!(t.title, "ut", "an oversize name is dropped whole");
    }

    #[test]
    fn beacon_frame_drives_the_scrollback_zone_cut_not_the_grid() {
        let mut t = tile();
        // an output zone with a cmd mark: the console path's own grammar. This
        // exercises the dispatch (Osc1936Raw -> scrollback.feed), reusing the
        // audited beacon parser; last_command is the observable effect.
        t.apply(Record::Control(Control::Osc1936Raw {
            serial: 0,
            frame: b"\x1b]1936;v1;zone;k=output\x1b\\".to_vec(),
        }));
        t.apply(Record::Control(Control::Osc1936Raw {
            serial: 0,
            frame: b"\x1b]1936;v1;mark;k=cmd;text=ls -l\x1b\\".to_vec(),
        }));
        assert_eq!(t.scrollback.last_command(), Some("ls -l"));
        // the grid is untouched by a control frame.
        assert_eq!(t.grid.row(0)[0].ch, ' ');
    }

    #[test]
    fn resize_reshapes_the_grid() {
        let mut t = tile();
        assert_eq!(t.grid.dims(), (20, 4));
        t.resize(40, 10);
        assert_eq!(t.grid.dims(), (40, 10));
    }

    #[test]
    fn record_order_a_zone_after_a_scrolloff_lands_in_the_right_block() {
        // stream order: two output lines scroll off, THEN a prompt zone opens,
        // THEN one more line scrolls off. The first two belong to the old block,
        // the third to the new prompt block (14.11.2 order guarantee).
        let mut t = tile();
        t.apply(Record::ScrollOff {
            rows: vec![vec![cell('1')], vec![cell('2')]],
            wrapped: vec![false, false],
        });
        assert_eq!(t.scrollback.open_block().items.len(), 2);
        t.apply(Record::Control(Control::Osc1936Raw {
            serial: 0,
            frame: b"\x1b]1936;v1;zone;k=prompt\x1b\\".to_vec(),
        }));
        // the zone cut froze the old block and opened a fresh one.
        t.apply(Record::ScrollOff {
            rows: vec![vec![cell('3')]],
            wrapped: vec![false],
        });
        assert_eq!(
            t.scrollback.open_block().items.len(),
            1,
            "the post-zone scroll-off is alone in the new block"
        );
    }

    // The render (14.11.3): a Cartoon is composed with a real GlyphSource. These
    // are shape assertions (a Clear, then glyph runs, plus the height contract),
    // not pixel checks -- the pixels are the ls-gfx-session E2E's job.
    fn daylight_tile(cols: usize, rows: usize) -> Tile {
        Tile::new(cols, rows, libhalcyon::theme::daylight_palette())
    }

    #[test]
    fn render_alt_is_grid_only_and_emits_glyphs() {
        let mut gs = GlyphSource::new_vendored(512);
        let sheet = crate::layout::daylight_sheet(100);
        let (cw, ch, _) = gs.mono_cell();
        let mut t = daylight_tile(20, 4);
        t.apply(Record::Mode(ScreenMode::AltScreen));
        t.apply(Record::CellDiff {
            changed: vec![(0, 0, cell('h')), (0, 1, cell('i'))],
            cursor: (0, 2, true),
            wrapped: vec![],
            top_continues: false,
        });
        let mut cart = Cartoon::new();
        let (w, h) = ((20 * cw) as usize, (4 * ch) as usize);
        let content = t.render(&mut cart, w, h, &mut gs, &sheet, &mut 0, None);
        assert_eq!(content, 4 * ch, "alt-screen content height == grid height");
        assert!(matches!(cart.ops.first(), Some(Op::Clear { .. })));
        assert!(
            cart.ops.iter().any(|o| matches!(o, Op::Glyphs { .. })),
            "the grid's glyphs are emitted"
        );
    }

    #[test]
    fn render_normal_scrollback_adds_height_above_the_grid() {
        let mut gs = GlyphSource::new_vendored(512);
        let sheet = crate::layout::daylight_sheet(100);
        let (cw, ch, _) = gs.mono_cell();
        let mut t = daylight_tile(20, 4);
        t.apply(Record::CellDiff {
            changed: vec![(3, 0, cell('x'))],
            cursor: (3, 1, true),
            wrapped: vec![],
            top_continues: false,
        });
        let mut cart = Cartoon::new();
        let (w, h) = ((20 * cw) as usize, (4 * ch) as usize);
        let grid_only = t.render(&mut cart, w, h, &mut gs, &sheet, &mut 0, None);
        // Three lines scroll off -> the scrollback grows -> the content is taller
        // than the grid tail alone (the flow renders above it, 14.11.3).
        t.apply(Record::ScrollOff {
            rows: vec![vec![cell('a')], vec![cell('b')], vec![cell('c')]],
            wrapped: vec![false, false, false],
        });
        let with_hist = t.render(&mut cart, w, h, &mut gs, &sheet, &mut 0, None);
        assert!(
            with_hist > grid_only,
            "scrollback adds content height above the grid tail ({with_hist} > {grid_only})"
        );
    }

    #[test]
    fn render_normal_tail_is_proportional_with_a_caret() {
        // PL-4b: the normal-mode tail renders PROPORTIONAL (via live_block, not
        // the mono paint_grid); a 2px ember caret marks the grid cursor; and a
        // mostly-blank TALL grid TRIMS -- the content height is far below
        // rows*cell_h.
        let mut gs = GlyphSource::new_vendored(512);
        let sheet = crate::layout::daylight_sheet(100);
        let (_, ch, _) = gs.mono_cell();
        let mut t = daylight_tile(20, 24); // tall grid, one line of content
        t.apply(Record::CellDiff {
            changed: vec![(0, 0, cell('h')), (0, 1, cell('i'))],
            cursor: (0, 2, true),
            wrapped: vec![],
            top_continues: false,
        });
        let mut cart = Cartoon::new();
        let (w, h) = (20 * 8, (24 * ch) as usize);
        let content = t.render(&mut cart, w, h, &mut gs, &sheet, &mut 0, None);
        assert!(
            content < 24 * ch,
            "the trimmed proportional tail is far below the full mono grid ({content} < {})",
            24 * ch
        );
        let caret = cart.ops.iter().any(|op| {
            matches!(op, Op::Rect { w: 2, color, .. } if *color == libhalcyon::theme::DAYLIGHT.ember)
        });
        assert!(caret, "a 2px ember caret beam is painted at the grid cursor");
    }

    /// A tile whose scrollback freezes a block every `lines` rows (zone cuts)
    /// and holds at most `max_blocks` frozen blocks.
    fn history_tile(cols: usize, rows: usize, max_blocks: usize) -> Tile {
        Tile {
            grid: Grid::new(cols, rows, libhalcyon::theme::daylight_palette().fg, libhalcyon::theme::daylight_palette().bg),
            scrollback: {
                let mut t = Transcript::with_caps(
                    libhalcyon::theme::daylight_palette(),
                    max_blocks,
                    DEFAULT_MAX_COST,
                    DEFAULT_MAX_LINES_PER_BLOCK,
                );
                t.set_cells_mode(true);
                t
            },
            mode: ScreenMode::Normal,
            title: String::new(),
            exit: None,
            bell: false,
            heights: VecDeque::new(),
            heights_width: 0,
            frame: Vec::new(),
            spans: SpanMap::new(),
            laid_last: 0,
            laid_lines_last: 0,
            live_laid: None,
        }
    }

    fn push_history(t: &mut Tile, blocks: usize, lines_per_block: usize, ch: char) {
        for _ in 0..blocks {
            let rows: Vec<Vec<Cell>> = (0..lines_per_block)
                .map(|_| vec![cell(ch), cell(ch), cell(ch)])
                .collect();
            t.apply(Record::ScrollOff {
                wrapped: vec![false; rows.len()],
                rows,
            });
            // a zone cut freezes the open block and opens the next one
            t.apply(Record::Control(Control::Osc1936Raw {
                serial: 0,
                frame: b"\x1b]1936;v1;zone;k=output\x1b\\".to_vec(),
            }));
        }
    }

    /// The exact content height by the OLD method (every block laid out).
    fn full_height(t: &Tile, w: usize, gs: &mut GlyphSource, sheet: &Sheet) -> i32 {
        let mut total = sheet.block_gap;
        let frozen = t.scrollback.frozen_blocks();
        for (i, b) in frozen.iter().enumerate() {
            let next = frozen
                .get(i + 1)
                .map(|n| n.kind)
                .unwrap_or(t.scrollback.open_block().kind);
            let h = layout_block(b, w as i32, sheet, gs).height;
            total += h + if h == 0 { 0 } else { block_gap_between(b.kind, next, sheet) };
        }
        total += layout_block(t.scrollback.open_block(), w as i32, sheet, gs).height;
        // PL-4: the tail is the proportional live grid (its content rows laid
        // as logical lines), not rows*cell_h.
        let (live_b, _) = t.scrollback.live_block(
            t.grid.cells(),
            t.grid.dims().0,
            t.grid.content_rows(),
            t.grid.wrapped(),
            &t.spans,
        false,
        );
        total + layout_block(&live_b, w as i32, sheet, gs).height
    }

    #[test]
    fn render_lays_out_only_the_blocks_in_view_once_the_heights_are_cached() {
        // B2-F1: a render's layout transient must be O(view), not O(history).
        let mut gs = GlyphSource::new_vendored(512);
        let sheet = crate::layout::daylight_sheet(100);
        let (cw, ch, _) = gs.mono_cell();
        let mut t = history_tile(20, 4, 1000);
        push_history(&mut t, 200, 3, 'h');
        assert_eq!(t.scrollback.frozen_blocks().len(), 200);
        let mut cart = Cartoon::new();
        let (w, h) = ((20 * cw) as usize, (4 * ch) as usize);

        // Cold: every frozen block is laid out ONCE for its height (and the
        // blocks in view again, plus the open block).
        let cold = t.render(&mut cart, w, h, &mut gs, &sheet, &mut 0, None);
        assert!(
            t.laid_last >= 200,
            "cold render fills the cache ({})",
            t.laid_last
        );
        assert_eq!(
            cold,
            full_height(&t, w, &mut gs, &sheet),
            "content height is exact"
        );

        // Warm: only the open block and the (at most two) frozen blocks that
        // touch a 4-row view are laid out -- not the 200 in history.
        let warm = t.render(&mut cart, w, h, &mut gs, &sheet, &mut 0, None);
        assert_eq!(warm, cold);
        assert!(
            t.laid_last <= 4,
            "warm render laid out {} blocks for a 4-row view",
            t.laid_last
        );
        // The transient in LINES: the in-view blocks (3 lines each) plus the
        // (empty) open block -- never the 600 lines of history.
        assert!(
            t.laid_lines_last <= 12,
            "warm render laid out {} lines for a 4-row view",
            t.laid_lines_last
        );

        // Scrolled to the very top: the window follows the scroll -- still a
        // handful of blocks, never the whole history.
        let mut su = i32::MAX;
        let top = t.render(&mut cart, w, h, &mut gs, &sheet, &mut su, None);
        assert_eq!(top, cold);
        assert!(
            (0..i32::MAX).contains(&su),
            "the clamped scroll offset is written back: {}",
            su
        );
        assert!(
            (1..=6).contains(&t.laid_last),
            "top-of-history render laid out {} blocks",
            t.laid_last
        );
        assert!(
            cart.ops.iter().any(|o| matches!(o, Op::Glyphs { .. })),
            "the oldest history renders at the top"
        );

        // New history since the last render: only the NEW frozen blocks join
        // the cache (plus the view's blocks).
        push_history(&mut t, 3, 3, 'j');
        let grown = t.render(&mut cart, w, h, &mut gs, &sheet, &mut 0, None);
        assert!(grown > cold);
        assert!(
            t.laid_last <= 3 + 4,
            "incremental cache fill laid out {}",
            t.laid_last
        );
        assert_eq!(grown, full_height(&t, w, &mut gs, &sheet));
    }

    #[test]
    fn height_cache_follows_eviction_and_width_changes() {
        let mut gs = GlyphSource::new_vendored(512);
        let sheet = crate::layout::daylight_sheet(100);
        let (cw, ch, _) = gs.mono_cell();
        // At most 5 frozen blocks: pushing 12 evicts 7 at the front.
        let mut t = history_tile(20, 4, 5);
        push_history(&mut t, 4, 2, 'a');
        let mut cart = Cartoon::new();
        let (w, h) = ((20 * cw) as usize, (4 * ch) as usize);
        let _ = t.render(&mut cart, w, h, &mut gs, &sheet, &mut 0, None);
        assert_eq!(t.heights.len(), 4);
        push_history(&mut t, 8, 2, 'b');
        assert_eq!(
            t.scrollback.frozen_blocks().len(),
            5,
            "the block cap evicted"
        );
        let got = t.render(&mut cart, w, h, &mut gs, &sheet, &mut 0, None);
        assert_eq!(t.heights.len(), 5);
        let ids: Vec<u64> = t.scrollback.frozen_blocks().iter().map(|b| b.id).collect();
        let cached: Vec<u64> = t.heights.iter().map(|e| e.0).collect();
        assert_eq!(cached, ids, "the cache is aligned to the frozen deque");
        assert_eq!(got, full_height(&t, w, &mut gs, &sheet));

        // A different width invalidates every cached height (a reflow).
        let w2 = (30 * cw) as usize;
        let narrow = t.render(&mut cart, w2, h, &mut gs, &sheet, &mut 0, None);
        assert!(
            t.laid_last >= 5,
            "a new width re-lays every block ({})",
            t.laid_last
        );
        assert_eq!(narrow, full_height(&t, w2, &mut gs, &sheet));
        assert_eq!(t.heights_width, w2 as i32);
    }

    #[test]
    fn a_floating_exit_mark_re_lays_the_frozen_block_it_lands_on() {
        // The one post-freeze mutation: an exit mark arriving right AFTER its
        // output zone closed lands on the last FROZEN block (the floating
        // order the transcript tolerates). A non-zero code adds the badge
        // line, so a height cached before it would misplace every block below.
        let mut gs = GlyphSource::new_vendored(512);
        let sheet = crate::layout::daylight_sheet(100);
        let (cw, ch, _) = gs.mono_cell();
        let mut t = history_tile(20, 4, 1000);
        t.apply(Record::Control(Control::Osc1936Raw {
            serial: 0,
            frame: b"\x1b]1936;v1;zone;k=output\x1b\\".to_vec(),
        }));
        t.apply(Record::ScrollOff {
            rows: vec![vec![cell('a')], vec![cell('b')]],
            wrapped: vec![false, false],
        });
        t.apply(Record::Control(Control::Osc1936Raw {
            serial: 0,
            frame: b"\x1b]1936;v1;/zone\x1b\\".to_vec(),
        }));
        assert_eq!(t.scrollback.frozen_blocks().len(), 1);
        let mut cart = Cartoon::new();
        let (w, h) = ((20 * cw) as usize, (4 * ch) as usize);
        let before = t.render(&mut cart, w, h, &mut gs, &sheet, &mut 0, None);
        assert_eq!(t.heights.len(), 1);
        assert_eq!(t.heights[0].1, None);
        // the floating exit mark: the open block is an empty Foreign one, so
        // the code lands on the frozen output block
        t.apply(Record::Control(Control::Osc1936Raw {
            serial: 0,
            frame: b"\x1b]1936;v1;mark;k=exit;code=2\x1b\\".to_vec(),
        }));
        assert_eq!(t.scrollback.frozen_blocks()[0].exit, Some(2));
        let after = t.render(&mut cart, w, h, &mut gs, &sheet, &mut 0, None);
        assert_eq!(t.heights[0].1, Some(2), "the cache re-keyed on the exit");
        assert!(
            after > before,
            "the badge line grew the content ({after} > {before})"
        );
        assert_eq!(after, full_height(&t, w, &mut gs, &sheet));
    }

    #[test]
    fn a_mark_drags_the_view_to_its_row_and_the_frame_maps_every_block() {
        // H-4d: the Normal-mode cursor is visible after every render (Helix:
        // the view follows the cursor), the block frame covers the whole
        // history in transcript order (the click hit map), and a marked row
        // paints its band.
        let mut gs = GlyphSource::new_vendored(512);
        let sheet = crate::layout::daylight_sheet(100);
        let (cw, ch, _) = gs.mono_cell();
        let mut t = history_tile(20, 4, 1000);
        push_history(&mut t, 40, 3, 'm');
        let ids: Vec<u64> = t.scrollback.frozen_blocks().iter().map(|b| b.id).collect();
        let mut cart = Cartoon::new();
        // A 12-row view over a 4-row grid: eight rows of history show.
        let (w, h) = ((20 * cw) as usize, (12 * ch) as usize);
        let viewh = h as i32;
        let band = |cart: &Cartoon| {
            cart.ops
                .iter()
                .any(|o| matches!(o, Op::Rect { color, .. } if *color == sheet.sel_bg))
        };

        // Unmarked, at the bottom: every block is in the frame, in order, y
        // ascending without overlap; the oldest is above the view, the
        // newest frozen one in it; no band.
        let mut su = 0;
        t.render(&mut cart, w, h, &mut gs, &sheet, &mut su, None);
        assert_eq!(su, 0);
        // ... then the open block, then the live grid (the virtual trailing block).
        let want: Vec<u64> = ids.iter().copied().chain([u64::MAX, GRID_KEY]).collect();
        assert_eq!(t.frame.iter().map(|f| f.0).collect::<Vec<_>>(), want);
        assert!(
            t.frame.windows(2).all(|p| p[0].1 + p[0].2 <= p[1].1),
            "frame y ascends without overlap"
        );
        assert!(
            t.frame[0].1 + t.frame[0].2 <= 0,
            "the oldest block is above the view"
        );
        let newest = t.frame[ids.len() - 1];
        assert!(
            newest.1 >= 0 && newest.1 + newest.2 <= viewh,
            "the newest frozen block is in view: y={} h={}",
            newest.1,
            newest.2
        );
        assert_eq!(t.hit(newest.1), Some((newest.0, newest.1)));
        assert_eq!(t.hit(newest.1 + newest.2 - 1), Some((newest.0, newest.1)));
        assert_eq!(
            t.hit(t.frame[0].1 - 1),
            None,
            "the leading gap hits nothing"
        );
        assert!(!band(&cart), "no band without a mark");

        // A mark on the oldest block's first line drags the view up to it and
        // paints the band.
        let mark = Mark {
            block: ids[0],
            item: 0,
            row: usize::MAX,
            obj: None,
        };
        t.render(&mut cart, w, h, &mut gs, &sheet, &mut su, Some(mark));
        assert!(su > 0, "the view scrolled up: {}", su);
        // The MARKED ROW is in view -- not the whole block. row=usize::MAX
        // resolves (via laid_line_for) to item 0's line, a proportional body
        // line now (14.13, ~15px), SHORTER than the mono cell (`ch`=22), so
        // `ch` is no longer a valid row-height proxy; a 3-line block trails
        // below the row and need not fit. Take the row's actual span.
        let oldest = t.frame[0];
        let olb = layout_block(
            t.scrollback.frozen_blocks().front().unwrap(),
            w as i32,
            &sheet,
            &mut gs,
        );
        let (ory, orh) = laid_line_for(&olb, 0, usize::MAX).unwrap();
        assert!(
            oldest.1 + ory >= 0 && oldest.1 + ory + orh <= viewh,
            "the oldest block's marked row is in view: rowy={} rowh={}",
            oldest.1 + ory,
            orh
        );
        assert!(band(&cart), "the marked row paints its band");

        // A mark back on the newest frozen block drags the view down again:
        // the marked row is visible, and the offset written back is within
        // the content.
        let mark = Mark {
            block: *ids.last().unwrap(),
            item: 0,
            row: usize::MAX,
            obj: None,
        };
        t.render(&mut cart, w, h, &mut gs, &sheet, &mut su, Some(mark));
        let newest = t.frame[ids.len() - 1];
        let nlb = layout_block(
            t.scrollback.frozen_blocks().back().unwrap(),
            w as i32,
            &sheet,
            &mut gs,
        );
        let (nry, nrh) = laid_line_for(&nlb, 0, usize::MAX).unwrap();
        assert!(
            newest.1 + nry >= 0 && newest.1 + nry + nrh <= viewh,
            "the newest block's marked row is back in view: rowy={} rowh={}",
            newest.1 + nry,
            nrh
        );
        assert!(
            (0..viewh * 40).contains(&su),
            "the offset is clamped: {}",
            su
        );
        assert!(band(&cart));
    }

    #[test]
    fn grid_cells_carry_their_obj_runs_and_scroll_them_into_the_landing_block() {
        // H-4d: a cell stamped with a frame's serial resolves to the span
        // state after that frame -- an obj run on the LIVE GRID (the virtual
        // trailing block), keyed by its start column; when the row scrolls
        // off into a LATER block than the obj's, the obj is copied into the
        // landing block so the row's run resolves there too.
        let cs = |ch: char, span: u32| Cell {
            ch,
            fg: 0xFFFFFF,
            bg: 0,
            attrs: 0,
            span,
        };
        let mut t = daylight_tile(8, 2);
        // Content before the obj, so the zone cut below freezes a block.
        t.apply(Record::ScrollOff {
            rows: vec![vec![cs('p', 0), cs('q', 0)]],
            wrapped: vec![false],
        });
        let b0 = t.scrollback.open_block().id;
        t.apply(Record::Control(Control::Osc1936Raw {
            serial: 1,
            frame: b"\x1b]1936;v1;obj;type=path;ref=/bin\x1b\\".to_vec(),
        }));
        assert_eq!(
            t.spans.get(1),
            Some(SpanTag {
                block: b0,
                obj: 1,
                em: 0,
                hdr: 0
            })
        );
        t.apply(Record::CellDiff {
            changed: vec![(0, 0, cs('b', 1)), (0, 1, cs('i', 1)), (0, 2, cs('n', 1))],
            cursor: (0, 3, true),
            wrapped: vec![],
            top_continues: false,
        });
        t.apply(Record::Control(Control::Osc1936Raw {
            serial: 2,
            frame: b"\x1b]1936;v1;/obj\x1b\\".to_vec(),
        }));
        t.apply(Record::CellDiff {
            changed: vec![(0, 4, cs('x', 2))],
            cursor: (0, 5, true),
            wrapped: vec![],
            top_continues: false,
        });
        let runs = t.grid_runs(0);
        assert_eq!(runs.len(), 1, "one run on the grid row: {:?}", runs);
        assert_eq!((runs[0].obj, runs[0].text.as_str()), (1, "bin"));
        assert_eq!(t.grid_run(0, 1).map(|(c, n, _)| (c, n)), Some((0, 3)));
        assert_eq!(t.grid_run_obj(0, 1), Some(("path", "/bin")));
        assert_eq!(t.grid_hit(15, 3, 10, 20), Some((0, 1)), "col 1 of the run");
        assert_eq!(t.grid_hit(45, 3, 10, 20), None, "the x after the close");
        assert!(t.grid_runs(1).is_empty());

        // A zone-open freezes b0 (it has content) and opens b1; the row then
        // scrolls off INTO b1 carrying b0's obj: copied, and resolvable.
        t.apply(Record::Control(Control::Osc1936Raw {
            serial: 3,
            frame: b"\x1b]1936;v1;zone;k=prompt\x1b\\".to_vec(),
        }));
        assert_ne!(t.scrollback.open_block().id, b0, "the zone cut froze b0");
        t.apply(Record::ScrollOff {
            rows: vec![vec![
                cs('b', 1),
                cs('i', 1),
                cs('n', 1),
                cs(' ', 0),
                cs('x', 2),
            ]],
            wrapped: vec![false],
        });
        let fr = crate::select::FlatRow {
            block: usize::MAX,
            item: 0,
            row: usize::MAX,
        };
        let runs = crate::menu::runs_on_row(&t.scrollback, fr);
        assert_eq!(runs.len(), 1, "the scrolled row keeps its run: {:?}", runs);
        assert_eq!(runs[0].text, "bin");
        assert_eq!(
            crate::menu::obj_of(&t.scrollback, usize::MAX, runs[0].obj),
            Some(("path", "/bin")),
            "the obj was copied into the landing block"
        );
        assert_eq!(t.scrollback.open_block().objs.len(), 1);
        assert_eq!(
            t.scrollback.block_by_id(b0).map(|b| b.objs.len()),
            Some(1),
            "the source block keeps its own"
        );
    }

    #[test]
    fn grid_hit_inverts_a_proportional_click_to_the_run() {
        // PL-4b-ii-b: after a proportional render, a click on the live tail
        // inverts through the CACHED layout (not the mono cell grid) to the
        // right grid run, and the run's display rect is its real x-extent.
        let cs = |ch: char, span: u32| Cell {
            ch,
            fg: 0xFFFFFF,
            bg: 0,
            attrs: 0,
            span,
        };
        let mut gs = GlyphSource::new_vendored(512);
        let sheet = crate::layout::daylight_sheet(100);
        let mut t = daylight_tile(16, 4);
        // An obj run "bin" on grid row 0 (cols 0..3, serial 1), then a plain
        // 'x' at col 4 (serial 2, obj closed) -- the mono test's shape.
        t.apply(Record::Control(Control::Osc1936Raw {
            serial: 1,
            frame: b"\x1b]1936;v1;obj;type=path;ref=/bin\x1b\\".to_vec(),
        }));
        t.apply(Record::CellDiff {
            changed: vec![(0, 0, cs('b', 1)), (0, 1, cs('i', 1)), (0, 2, cs('n', 1))],
            cursor: (0, 3, true),
            wrapped: vec![],
            top_continues: false,
        });
        t.apply(Record::Control(Control::Osc1936Raw {
            serial: 2,
            frame: b"\x1b]1936;v1;/obj\x1b\\".to_vec(),
        }));
        t.apply(Record::CellDiff {
            changed: vec![(0, 4, cs('x', 2))],
            cursor: (0, 5, true),
            wrapped: vec![],
            top_continues: false,
        });
        // Render populates the proportional cache (live_laid Some).
        let mut cart = Cartoon::new();
        let (w, h) = (16 * 8, 4 * 20);
        t.render(&mut cart, w, h, &mut gs, &sheet, &mut 0, None);

        // The run's tail-relative rect is proportional (a real x-extent, one
        // laid line high), and its centre inverts back to (row 0, key 1).
        let (rx, ry, rw, rh) = t.grid_run_rect(0, 1, 8, 20).expect("the run has a rect");
        assert!(rw > 0 && rh > 0, "a non-empty proportional rect: {rw}x{rh}");
        let (cx, cy) = (rx + rw / 2, ry + rh / 2);
        assert_eq!(
            t.grid_hit(cx, cy, 8, 20),
            Some((0, 1)),
            "a click on 'bin' inverts to its run"
        );
        assert_eq!(
            t.grid_run_obj(0, 1),
            Some(("path", "/bin")),
            "the returned key resolves the obj"
        );
        // A click far right of every glyph, or below the trimmed tail, hits no
        // run (the proportional inverse finds no cell there).
        assert_eq!(t.grid_hit(w as i32 - 1, cy, 8, 20), None, "past the content");
        assert_eq!(t.grid_hit(cx, h as i32 - 1, 8, 20), None, "below the tail");

        // Alt-screen drops the cache -> grid_hit falls back to the mono grid
        // (the same (row, key) by cell), and grid_run_rect to the mono cell.
        t.apply(Record::Mode(ScreenMode::AltScreen));
        t.render(&mut cart, w, h, &mut gs, &sheet, &mut 0, None);
        assert_eq!(
            t.grid_hit(5, 5, 8, 20),
            Some((0, 1)),
            "alt-screen: mono hit on 'bin'"
        );
        assert_eq!(
            t.grid_run_rect(0, 1, 8, 20),
            Some((0, 0, 24, 20)),
            "alt-screen: the mono 3-cell rect"
        );
    }

    #[test]
    fn render_blank_daylight_grid_skips_bg_rects() {
        // A blank Daylight grid: every cell's bg == the sheet ground, so no bg
        // Rect is emitted (only the Clear) -- the paint_grid ground-skip. The
        // cursor beam is one Rect, so exactly one Rect total (the cursor).
        let mut gs = GlyphSource::new_vendored(512);
        let sheet = crate::layout::daylight_sheet(100);
        let (cw, ch, _) = gs.mono_cell();
        let mut t = daylight_tile(8, 2);
        t.apply(Record::Mode(ScreenMode::AltScreen)); // grid only, no scrollback flow
        let mut cart = Cartoon::new();
        let (w, h) = ((8 * cw) as usize, (2 * ch) as usize);
        t.render(&mut cart, w, h, &mut gs, &sheet, &mut 0, None);
        let rects = cart
            .ops
            .iter()
            .filter(|o| matches!(o, Op::Rect { .. }))
            .count();
        assert_eq!(rects, 1, "only the cursor beam Rect (blank cells skip bg)");
    }

    // --- the composition-round audit F1: a click on a rebuilt structure ---
    //
    // In cells mode every row of one rebuilt table (every line of one pre)
    // shares an ITEM; the provenance must carry the ROW, and the laid runs
    // their SOURCE columns, or the inverse names the structure's first row
    // for every click on it (`ps`: a click on row N's pid opened row 0's; a
    // `kill` from that menu killed the wrong process).

    fn cs(ch: char, span: u32) -> Cell {
        Cell {
            ch,
            fg: 0xFFFFFF,
            bg: 0,
            attrs: 0,
            span,
        }
    }

    fn frame(t: &mut Tile, serial: u32, body: &[u8]) {
        let mut f = Vec::new();
        f.extend_from_slice(b"\x1b]1936;v1;");
        f.extend_from_slice(body);
        f.extend_from_slice(b"\x1b\\");
        t.apply(Record::Control(Control::Osc1936Raw { serial, frame: f }));
    }

    fn write(t: &mut Tile, cells: Vec<(u16, u16, Cell)>, cur: (u16, u16)) {
        t.apply(Record::CellDiff {
            changed: cells,
            cursor: (cur.0, cur.1, true),
            wrapped: vec![],
            top_continues: false,
        });
    }

    /// A `ps`-shaped Beacon table on the LIVE grid: two rows, an obj (pid)
    /// cell in column 0 of each. A click on row 1's pid opens row 1's
    /// object; row 1's run rect is row 1's laid line.
    #[test]
    fn live_grid_table_click_hits_the_clicked_row() {
        let mut gs = GlyphSource::new_vendored(512);
        let sheet = crate::layout::daylight_sheet(100);
        let mut t = Tile::new(40, 4, libhalcyon::theme::daylight_palette());
        frame(&mut t, 1, b"zone;k=output");
        frame(&mut t, 2, b"table;cols=lr;hdr=0");
        frame(&mut t, 3, b"row");
        frame(&mut t, 4, b"cell");
        frame(&mut t, 5, b"obj;type=pid;ref=100");
        write(&mut t, vec![(0, 0, cs('1', 5)), (0, 1, cs('0', 5)), (0, 2, cs('0', 5))], (0, 3));
        frame(&mut t, 6, b"/obj");
        frame(&mut t, 7, b"/cell");
        write(&mut t, vec![(0, 3, cs(' ', 7)), (0, 4, cs(' ', 7))], (0, 5));
        frame(&mut t, 8, b"cell");
        write(&mut t, vec![(0, 5, cs('u', 8)), (0, 6, cs('t', 8))], (0, 7));
        frame(&mut t, 9, b"/cell");
        frame(&mut t, 10, b"/row");
        frame(&mut t, 11, b"row");
        frame(&mut t, 12, b"cell");
        frame(&mut t, 13, b"obj;type=pid;ref=200");
        write(&mut t, vec![(1, 0, cs('2', 13)), (1, 1, cs('0', 13)), (1, 2, cs('0', 13))], (1, 3));
        frame(&mut t, 14, b"/obj");
        frame(&mut t, 15, b"/cell");
        write(&mut t, vec![(1, 3, cs(' ', 15)), (1, 4, cs(' ', 15))], (1, 5));
        frame(&mut t, 16, b"cell");
        write(&mut t, vec![(1, 5, cs('s', 16)), (1, 6, cs('h', 16))], (1, 7));
        frame(&mut t, 17, b"/cell");
        frame(&mut t, 18, b"/row");
        frame(&mut t, 19, b"/table");
        write(&mut t, vec![], (2, 0));

        // The keyboard path (cell spans) names each row's own object.
        assert_eq!(t.grid_run_obj(1, 1), Some(("pid", "200")));
        assert_eq!(t.grid_run_obj(0, 1), Some(("pid", "100")));

        let mut cart = Cartoon::new();
        let (cw, ch, _) = gs.mono_cell();
        let (w, h) = (320usize, (4 * ch) as usize);
        t.render(&mut cart, w, h, &mut gs, &sheet, &mut 0, None);

        // The live block IS a rebuilt table, both rows on one item.
        let (lb, prov) = t.scrollback.live_block(
            t.grid.cells(),
            40,
            t.grid.content_rows(),
            t.grid.wrapped(),
            &t.spans,
        false,
        );
        assert!(matches!(lb.items[0], Item::Table(_)), "the live grid rebuilt the table");
        assert_eq!((prov[0].0, prov[0].1), (0, 0));
        assert_eq!((prov[1].0, prov[1].1), (0, 1), "the same item, the next ROW");
        let laid = crate::layout::layout_block(&lb, w as i32, &sheet, &mut gs);
        let row1 = laid.lines.iter().find(|l| l.src_row == 1).expect("row 1 laid");
        let pid_seg = row1.segs.iter().find(|s| s.obj != 0).expect("row 1's pid seg");
        assert_eq!(pid_seg.src_col, 0, "the pid cell starts at grid column 0");
        let sh_seg = row1.segs.iter().find(|s| s.obj == 0).expect("row 1's second cell");
        assert_eq!(sh_seg.src_col, 5, "the second cell starts at its grid column");
        let (x, y) = ((pid_seg.x + pid_seg.x_end) / 2, row1.y + row1.h / 2);

        // The click on row 1's pid glyphs.
        let hit = t.grid_hit(x, y, cw, ch);
        let obj = hit.and_then(|(r, k)| t.grid_run_obj(r, k).map(|(a, b)| (r, a, b)));
        assert_eq!(
            obj,
            Some((1usize, "pid", "200")),
            "a click on row 1's pid opens row 1's object (hit {:?})",
            hit
        );
        // And row 1's run rect sits on row 1's laid line, not row 0's.
        let r0 = t.grid_run_rect(0, 1, cw, ch).expect("row 0 rect");
        let r1 = t.grid_run_rect(1, 1, cw, ch).expect("row 1 rect");
        assert_ne!(r0.1, r1.1, "row 0 and row 1 rects sit on different laid lines: {:?} vs {:?}", r0, r1);
        assert_eq!(r1.1, row1.y, "row 1's rect is row 1's laid line");
    }

    /// The `la` shape: a `pre` box on the live grid with an obj path per
    /// row. A click on row 1's path opens row 1's object.
    #[test]
    fn live_grid_pre_click_hits_the_clicked_row() {
        let mut gs = GlyphSource::new_vendored(512);
        let sheet = crate::layout::daylight_sheet(100);
        let mut t = Tile::new(40, 4, libhalcyon::theme::daylight_palette());
        frame(&mut t, 1, b"pre");
        write(&mut t, vec![(0, 0, cs('|', 1)), (0, 1, cs(' ', 1))], (0, 2));
        frame(&mut t, 2, b"obj;type=path;ref=/aa");
        write(&mut t, vec![(0, 2, cs('a', 2)), (0, 3, cs('a', 2))], (0, 4));
        frame(&mut t, 3, b"/obj");
        write(&mut t, vec![(0, 4, cs(' ', 3)), (0, 5, cs('|', 3))], (0, 6));
        write(&mut t, vec![(1, 0, cs('|', 3)), (1, 1, cs(' ', 3))], (1, 2));
        frame(&mut t, 4, b"obj;type=path;ref=/bb");
        write(&mut t, vec![(1, 2, cs('b', 4)), (1, 3, cs('b', 4))], (1, 4));
        frame(&mut t, 5, b"/obj");
        write(&mut t, vec![(1, 4, cs(' ', 5)), (1, 5, cs('|', 5))], (1, 6));
        frame(&mut t, 6, b"/pre");
        write(&mut t, vec![], (2, 0));
        assert_eq!(t.grid_run_obj(1, 3), Some(("path", "/bb")));

        let mut cart = Cartoon::new();
        let (cw, ch, _) = gs.mono_cell();
        let (w, h) = (320usize, (4 * ch) as usize);
        t.render(&mut cart, w, h, &mut gs, &sheet, &mut 0, None);
        let (lb, prov) = t.scrollback.live_block(
            t.grid.cells(),
            40,
            t.grid.content_rows(),
            t.grid.wrapped(),
            &t.spans,
        false,
        );
        assert!(matches!(lb.items[0], Item::Pre(_)), "the live grid rebuilt the pre");
        assert_eq!(prov[1].0, prov[0].0, "both grid rows map to the ONE pre item");
        assert_eq!((prov[0].1, prov[1].1), (0, 1), "each names its pre line");
        let laid = crate::layout::layout_block(&lb, w as i32, &sheet, &mut gs);
        let pre_lines: Vec<&crate::layout::LaidLine> =
            laid.lines.iter().filter(|l| l.src_item == 0).collect();
        assert_eq!(pre_lines.len(), 2);
        assert_eq!((pre_lines[0].src_row, pre_lines[1].src_row), (0, 1), "pre lines carry their row");
        let row1 = pre_lines[1];
        let seg = row1.segs.iter().find(|s| s.obj != 0).expect("row 1's obj seg");
        let (x, y) = ((seg.x + seg.x_end) / 2, row1.y + row1.h / 2);
        let hit = t.grid_hit(x, y, cw, ch);
        let obj = hit.and_then(|(r, k)| t.grid_run_obj(r, k).map(|(a, b)| (r, a, b)));
        assert_eq!(
            obj,
            Some((1usize, "path", "/bb")),
            "a click on row 1's path opens /bb (hit {:?})",
            hit
        );
    }

    /// Audit F3, through the tile: `rule`, a text line, a newline, then an
    /// `em` open starting the next line -- the text line's cells ended the
    /// rule episode, so the open carries no rule and one rule frame places
    /// ONE rule on the live grid.
    #[test]
    fn one_rule_frame_places_one_rule_on_the_live_grid() {
        let mut t = Tile::new(20, 4, libhalcyon::theme::daylight_palette());
        frame(&mut t, 1, b"rule");
        assert_eq!(t.scrollback.rule_open(), Some(1));
        write(&mut t, vec![(0, 0, cs('a', 1)), (0, 1, cs('b', 1))], (1, 0));
        assert_eq!(t.scrollback.rule_open(), None, "cells after the rule end its episode");
        frame(&mut t, 2, b"em;class=dim");
        write(&mut t, vec![(1, 0, cs('x', 2)), (1, 1, cs('y', 2))], (1, 2));
        frame(&mut t, 3, b"/em");
        let (lb, _) = t.scrollback.live_block(
            t.grid.cells(),
            20,
            t.grid.content_rows(),
            t.grid.wrapped(),
            &t.spans,
        false,
        );
        let rules = lb.items.iter().filter(|i| matches!(i, Item::Rule)).count();
        assert_eq!(rules, 1, "one rule frame, one rule; items {}", lb.items.len());
        assert!(matches!(lb.items[0], Item::Rule), "the rule precedes the text line");
    }
}
