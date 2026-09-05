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

use alloc::string::String;
use alloc::vec::Vec;

use crate::grid::Grid;
use crate::layout::{layout_block, render_block, LaidBlock, Sheet};
use crate::raster::{GlyphSource, FACE_MONO};
use crate::transcript::Transcript;
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
}

impl Tile {
    pub fn new(cols: usize, rows: usize, pal: Palette) -> Tile {
        Tile {
            grid: Grid::new(cols, rows, pal.fg, pal.bg),
            scrollback: Transcript::new(pal),
            mode: ScreenMode::Normal,
            title: String::new(),
            exit: None,
            bell: false,
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
    pub fn render(
        &self,
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

        if self.mode == ScreenMode::AltScreen {
            paint_grid(cart, &self.grid, 0, 0, gs, sheet);
            return grid_h;
        }

        // Normal: lay the scrollback flow (pass 1, stored), then bottom-anchor
        // [scrollback][grid] and render (pass 2). Two passes so the grid tail's
        // screen-y is known before any block is emitted -- layout is done once
        // per block, its result reused.
        let widthi = w as i32;
        let viewh = h as i32;
        let mut laid: Vec<LaidBlock> = Vec::new();
        let mut total = sheet.block_gap; // a leading gap above the first block
        for b in self.scrollback.frozen_blocks().iter() {
            let lb = layout_block(b, widthi, sheet, gs);
            total += lb.height + sheet.block_gap;
            laid.push(lb);
        }
        // The open block is the newest (un-frozen) history; no trailing gap --
        // the grid follows it directly as the live tail.
        let open_lb = layout_block(self.scrollback.open_block(), widthi, sheet, gs);
        total += open_lb.height;
        laid.push(open_lb);

        let content_h = total + grid_h;
        let su = scroll_up.clamp(0, (content_h - viewh).max(0));
        let y0 = if content_h <= viewh {
            0
        } else {
            viewh - content_h + su
        };

        let mut y = y0 + sheet.block_gap;
        let n = laid.len();
        for (i, lb) in laid.iter().enumerate() {
            if y + lb.height >= 0 && y <= viewh {
                render_block(cart, lb, y, gs);
            }
            // Every block but the last (the open block) carries a trailing gap,
            // mirroring the `total` accumulation above so `y` lands at the grid.
            y += lb.height;
            if i + 1 < n {
                y += sheet.block_gap;
            }
        }
        // `y` is now the grid tail's screen-y (== y0 + total).
        paint_grid(cart, &self.grid, 0, y, gs, sheet);
        content_h
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
