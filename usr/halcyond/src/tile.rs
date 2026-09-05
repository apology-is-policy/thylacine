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

use crate::grid::Grid;
use crate::layout::{layout_block, render_block, Sheet};
use crate::raster::{GlyphSource, FACE_MONO};
use crate::transcript::{
    Transcript, DEFAULT_MAX_BLOCKS, DEFAULT_MAX_COST, DEFAULT_MAX_LINES_PER_BLOCK,
};
use cartoon::{Cartoon, Op};
use kaua_term::{Control, Record, ScreenMode};
use vt::{Palette, ATTR_REVERSE, ATTR_UNDERLINE};

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
    /// Blocks laid out by the last `render` (the window's witness).
    pub laid_last: usize,
    /// Visual lines laid out by the last `render` (the transient's witness:
    /// bounded by the view plus the two whole blocks, never the history).
    pub laid_lines_last: usize,
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
            scrollback: Transcript::with_caps(
                pal,
                DEFAULT_MAX_BLOCKS,
                max_cost,
                DEFAULT_MAX_LINES_PER_BLOCK,
            ),
            mode: ScreenMode::Normal,
            title: String::new(),
            exit: None,
            bell: false,
            heights: VecDeque::new(),
            heights_width: 0,
            laid_last: 0,
            laid_lines_last: 0,
        }
    }

    /// The record -> model dispatch (14.11.2).
    pub fn apply(&mut self, rec: Record) {
        match rec {
            Record::CellDiff { changed, cursor } => self.grid.apply_celldiff(&changed, cursor),
            Record::ScrollOff { rows } => self.scrollback.push_scrolled_rows(&rows),
            Record::Control(c) => self.apply_control(c),
            Record::Mode(m) => self.mode = m,
        }
    }

    fn apply_control(&mut self, c: Control) {
        match c {
            // A Beacon frame is the COMPLETE ESC ] 1936 ; ... ST -- feed it to the
            // SAME beacon parser the console path uses; it drives the zone/block
            // cut + span state on the scrollback and touches no cells (14.11.4).
            Control::Osc1936Raw(frame) => self.scrollback.feed(&frame),
            Control::Title(t) => self.title = t,
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
        self.grid.resize(cols, rows);
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
    pub fn render(
        &mut self,
        cart: &mut Cartoon,
        w: usize,
        h: usize,
        gs: &mut GlyphSource,
        sheet: &Sheet,
        scroll_up: i32,
    ) -> i32 {
        cart.reset();
        cart.ops.push(Op::Clear {
            color: sheet.ground,
        });
        let (_cw, cell_h, _base) = gs.mono_cell();
        let grid_h = self.grid.dims().1 as i32 * cell_h;
        self.laid_last = 0;
        self.laid_lines_last = 0;

        if self.mode == ScreenMode::AltScreen {
            paint_grid(cart, &self.grid, 0, 0, gs, sheet);
            return grid_h;
        }

        let widthi = w as i32;
        let viewh = h as i32;
        self.laid_last += self.sync_heights(widthi, sheet, gs);

        // The exact content height from the cached heights: a leading gap,
        // every frozen block plus its trailing gap, then the open block (the
        // newest, un-frozen history; no trailing gap -- the grid follows it
        // directly as the live tail).
        let mut total = sheet.block_gap;
        for &(_, _, hgt) in self.heights.iter() {
            total += hgt + sheet.block_gap;
        }
        let open_lb = layout_block(self.scrollback.open_block(), widthi, sheet, gs);
        self.laid_last += 1;
        self.laid_lines_last += open_lb.lines.len();
        total += open_lb.height;

        let content_h = total + grid_h;
        let su = scroll_up.clamp(0, (content_h - viewh).max(0));
        let y0 = if content_h <= viewh {
            0
        } else {
            viewh - content_h + su
        };

        // Bottom-anchor [scrollback][grid]: walk the blocks by their cached
        // heights, laying out + rendering only those that intersect the view.
        let mut y = y0 + sheet.block_gap;
        for (b, &(_, _, hgt)) in self
            .scrollback
            .frozen_blocks()
            .iter()
            .zip(self.heights.iter())
        {
            if y + hgt >= 0 && y <= viewh {
                let lb = layout_block(b, widthi, sheet, gs);
                debug_assert_eq!(lb.height, hgt, "a frozen block's height is deterministic");
                render_block(cart, &lb, y, gs);
                self.laid_last += 1;
                self.laid_lines_last += lb.lines.len();
            }
            y += hgt + sheet.block_gap;
        }
        if y + open_lb.height >= 0 && y <= viewh {
            render_block(cart, &open_lb, y, gs);
        }
        y += open_lb.height;
        // `y` is now the grid tail's screen-y (== y0 + total).
        paint_grid(cart, &self.grid, 0, y, gs, sheet);
        content_h
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

/// Paint the live grid's cells at screen origin `(x0, y0)` into `cart` (a mono
/// cell store: per-cell bg rect when it differs from the ground, then the glyph,
/// then the underline; the block cursor beam last). Out-of-range is impossible
/// -- `Grid::row` and `Grid::cursor` are already clamped (grid.rs), the tile
/// trust boundary (14.11.12).
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
                if let Some(gref) = gs.glyph(FACE_MONO, 0.0, cell.ch) {
                    cart.push_glyphs(gen, cx, cy + base, fg, &[gref]);
                }
            }
            if cell.attrs & ATTR_UNDERLINE != 0 {
                cart.ops.push(Op::Rect {
                    x: cx,
                    y: cy + ch - 1,
                    w: cw as u32,
                    h: 1,
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
            w: 2,
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
        }
    }

    fn tile() -> Tile {
        Tile::new(20, 4, vt::BONFIRE)
    }

    #[test]
    fn celldiff_lands_on_the_grid_not_the_scrollback() {
        let mut t = tile();
        t.apply(Record::CellDiff {
            changed: vec![(0, 0, cell('h')), (0, 1, cell('i'))],
            cursor: (0, 2, true),
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

    #[test]
    fn beacon_frame_drives_the_scrollback_zone_cut_not_the_grid() {
        let mut t = tile();
        // an output zone with a cmd mark: the console path's own grammar. This
        // exercises the dispatch (Osc1936Raw -> scrollback.feed), reusing the
        // audited beacon parser; last_command is the observable effect.
        t.apply(Record::Control(Control::Osc1936Raw(
            b"\x1b]1936;v1;zone;k=output\x1b\\".to_vec(),
        )));
        t.apply(Record::Control(Control::Osc1936Raw(
            b"\x1b]1936;v1;mark;k=cmd;text=ls -l\x1b\\".to_vec(),
        )));
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
        });
        assert_eq!(t.scrollback.open_block().items.len(), 2);
        t.apply(Record::Control(Control::Osc1936Raw(
            b"\x1b]1936;v1;zone;k=prompt\x1b\\".to_vec(),
        )));
        // the zone cut froze the old block and opened a fresh one.
        t.apply(Record::ScrollOff {
            rows: vec![vec![cell('3')]],
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
        Tile::new(cols, rows, vt::DAYLIGHT)
    }

    #[test]
    fn render_alt_is_grid_only_and_emits_glyphs() {
        let mut gs = GlyphSource::new_vendored(512);
        let sheet = crate::layout::daylight_sheet();
        let (cw, ch, _) = gs.mono_cell();
        let mut t = daylight_tile(20, 4);
        t.apply(Record::Mode(ScreenMode::AltScreen));
        t.apply(Record::CellDiff {
            changed: vec![(0, 0, cell('h')), (0, 1, cell('i'))],
            cursor: (0, 2, true),
        });
        let mut cart = Cartoon::new();
        let (w, h) = ((20 * cw) as usize, (4 * ch) as usize);
        let content = t.render(&mut cart, w, h, &mut gs, &sheet, 0);
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
        let sheet = crate::layout::daylight_sheet();
        let (cw, ch, _) = gs.mono_cell();
        let mut t = daylight_tile(20, 4);
        t.apply(Record::CellDiff {
            changed: vec![(3, 0, cell('x'))],
            cursor: (3, 1, true),
        });
        let mut cart = Cartoon::new();
        let (w, h) = ((20 * cw) as usize, (4 * ch) as usize);
        let grid_only = t.render(&mut cart, w, h, &mut gs, &sheet, 0);
        // Three lines scroll off -> the scrollback grows -> the content is taller
        // than the grid tail alone (the flow renders above it, 14.11.3).
        t.apply(Record::ScrollOff {
            rows: vec![vec![cell('a')], vec![cell('b')], vec![cell('c')]],
        });
        let with_hist = t.render(&mut cart, w, h, &mut gs, &sheet, 0);
        assert!(
            with_hist > grid_only,
            "scrollback adds content height above the grid tail ({with_hist} > {grid_only})"
        );
    }

    /// A tile whose scrollback freezes a block every `lines` rows (zone cuts)
    /// and holds at most `max_blocks` frozen blocks.
    fn history_tile(cols: usize, rows: usize, max_blocks: usize) -> Tile {
        Tile {
            grid: Grid::new(cols, rows, vt::DAYLIGHT.fg, vt::DAYLIGHT.bg),
            scrollback: Transcript::with_caps(
                vt::DAYLIGHT,
                max_blocks,
                DEFAULT_MAX_COST,
                DEFAULT_MAX_LINES_PER_BLOCK,
            ),
            mode: ScreenMode::Normal,
            title: String::new(),
            exit: None,
            bell: false,
            heights: VecDeque::new(),
            heights_width: 0,
            laid_last: 0,
            laid_lines_last: 0,
        }
    }

    fn push_history(t: &mut Tile, blocks: usize, lines_per_block: usize, ch: char) {
        for _ in 0..blocks {
            let rows: Vec<Vec<Cell>> = (0..lines_per_block)
                .map(|_| vec![cell(ch), cell(ch), cell(ch)])
                .collect();
            t.apply(Record::ScrollOff { rows });
            // a zone cut freezes the open block and opens the next one
            t.apply(Record::Control(Control::Osc1936Raw(
                b"\x1b]1936;v1;zone;k=output\x1b\\".to_vec(),
            )));
        }
    }

    /// The exact content height by the OLD method (every block laid out).
    fn full_height(t: &Tile, w: usize, gs: &mut GlyphSource, sheet: &Sheet) -> i32 {
        let (_, ch, _) = gs.mono_cell();
        let mut total = sheet.block_gap;
        for b in t.scrollback.frozen_blocks().iter() {
            total += layout_block(b, w as i32, sheet, gs).height + sheet.block_gap;
        }
        total += layout_block(t.scrollback.open_block(), w as i32, sheet, gs).height;
        total + t.grid.dims().1 as i32 * ch
    }

    #[test]
    fn render_lays_out_only_the_blocks_in_view_once_the_heights_are_cached() {
        // B2-F1: a render's layout transient must be O(view), not O(history).
        let mut gs = GlyphSource::new_vendored(512);
        let sheet = crate::layout::daylight_sheet();
        let (cw, ch, _) = gs.mono_cell();
        let mut t = history_tile(20, 4, 1000);
        push_history(&mut t, 200, 3, 'h');
        assert_eq!(t.scrollback.frozen_blocks().len(), 200);
        let mut cart = Cartoon::new();
        let (w, h) = ((20 * cw) as usize, (4 * ch) as usize);

        // Cold: every frozen block is laid out ONCE for its height (and the
        // blocks in view again, plus the open block).
        let cold = t.render(&mut cart, w, h, &mut gs, &sheet, 0);
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
        let warm = t.render(&mut cart, w, h, &mut gs, &sheet, 0);
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
        let top = t.render(&mut cart, w, h, &mut gs, &sheet, i32::MAX);
        assert_eq!(top, cold);
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
        let grown = t.render(&mut cart, w, h, &mut gs, &sheet, 0);
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
        let sheet = crate::layout::daylight_sheet();
        let (cw, ch, _) = gs.mono_cell();
        // At most 5 frozen blocks: pushing 12 evicts 7 at the front.
        let mut t = history_tile(20, 4, 5);
        push_history(&mut t, 4, 2, 'a');
        let mut cart = Cartoon::new();
        let (w, h) = ((20 * cw) as usize, (4 * ch) as usize);
        let _ = t.render(&mut cart, w, h, &mut gs, &sheet, 0);
        assert_eq!(t.heights.len(), 4);
        push_history(&mut t, 8, 2, 'b');
        assert_eq!(
            t.scrollback.frozen_blocks().len(),
            5,
            "the block cap evicted"
        );
        let got = t.render(&mut cart, w, h, &mut gs, &sheet, 0);
        assert_eq!(t.heights.len(), 5);
        let ids: Vec<u64> = t.scrollback.frozen_blocks().iter().map(|b| b.id).collect();
        let cached: Vec<u64> = t.heights.iter().map(|e| e.0).collect();
        assert_eq!(cached, ids, "the cache is aligned to the frozen deque");
        assert_eq!(got, full_height(&t, w, &mut gs, &sheet));

        // A different width invalidates every cached height (a reflow).
        let w2 = (30 * cw) as usize;
        let narrow = t.render(&mut cart, w2, h, &mut gs, &sheet, 0);
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
        let sheet = crate::layout::daylight_sheet();
        let (cw, ch, _) = gs.mono_cell();
        let mut t = history_tile(20, 4, 1000);
        t.apply(Record::Control(Control::Osc1936Raw(
            b"\x1b]1936;v1;zone;k=output\x1b\\".to_vec(),
        )));
        t.apply(Record::ScrollOff {
            rows: vec![vec![cell('a')], vec![cell('b')]],
        });
        t.apply(Record::Control(Control::Osc1936Raw(
            b"\x1b]1936;v1;/zone\x1b\\".to_vec(),
        )));
        assert_eq!(t.scrollback.frozen_blocks().len(), 1);
        let mut cart = Cartoon::new();
        let (w, h) = ((20 * cw) as usize, (4 * ch) as usize);
        let before = t.render(&mut cart, w, h, &mut gs, &sheet, 0);
        assert_eq!(t.heights.len(), 1);
        assert_eq!(t.heights[0].1, None);
        // the floating exit mark: the open block is an empty Foreign one, so
        // the code lands on the frozen output block
        t.apply(Record::Control(Control::Osc1936Raw(
            b"\x1b]1936;v1;mark;k=exit;code=2\x1b\\".to_vec(),
        )));
        assert_eq!(t.scrollback.frozen_blocks()[0].exit, Some(2));
        let after = t.render(&mut cart, w, h, &mut gs, &sheet, 0);
        assert_eq!(t.heights[0].1, Some(2), "the cache re-keyed on the exit");
        assert!(
            after > before,
            "the badge line grew the content ({after} > {before})"
        );
        assert_eq!(after, full_height(&t, w, &mut gs, &sheet));
    }

    #[test]
    fn render_blank_daylight_grid_skips_bg_rects() {
        // A blank Daylight grid: every cell's bg == the sheet ground, so no bg
        // Rect is emitted (only the Clear) -- the paint_grid ground-skip. The
        // cursor beam is one Rect, so exactly one Rect total (the cursor).
        let mut gs = GlyphSource::new_vendored(512);
        let sheet = crate::layout::daylight_sheet();
        let (cw, ch, _) = gs.mono_cell();
        let mut t = daylight_tile(8, 2);
        t.apply(Record::Mode(ScreenMode::AltScreen)); // grid only, no scrollback flow
        let mut cart = Cartoon::new();
        let (w, h) = ((8 * cw) as usize, (2 * ch) as usize);
        t.render(&mut cart, w, h, &mut gs, &sheet, 0);
        let rects = cart
            .ops
            .iter()
            .filter(|o| matches!(o, Op::Rect { .. }))
            .count();
        assert_eq!(rects, 1, "only the cursor beam Rect (blank cells skip bg)");
    }
}
