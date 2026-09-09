// The live grid -- a tile's current terminal screen (HALCYON 14.11.1).
//
// A fixed rows x cols buffer of `vt::Cell`, the screen as it stands right now.
// It is what a tile's `CellDiff` records mutate (position-keyed cell writes +
// the cursor). Unlike the scrollback `Transcript`, the grid runs NO VT parser
// and cuts NO Beacon zones: the kaua-term already ran the VT and pre-digested
// the screen into diffs, so the grid is a pure cell store. It is the same
// live-screen both modes render (14.11.3): normal-mode as the fixed-height tail
// under the scrollback, alt-screen full-tile.
//
// halcyond is the geometry authority (it sizes the tile and sends Resize down),
// so a well-behaved kaua-term never addresses a cell outside the dims it was
// told. But a tile is untrusted (14.11.12, the format-fuzz class): a buggy or
// hostile producer's out-of-bounds cell write is DROPPED here, never allowed to
// index past the buffer. The cursor is stored as sent (a position marker, not a
// buffer index) and clamped by the render.

use alloc::vec;
use alloc::vec::Vec;
use vt::Cell;

pub struct Grid {
    cols: usize,
    rows: usize,
    /// row-major, `rows * cols` cells.
    cells: Vec<Cell>,
    /// `(row, col, visible)` as the producer last reported it (unclamped).
    cursor: (u16, u16, bool),
    /// PL-4: per-row soft-wrap flags -- the vt's live-screen `wrapped`, carried
    /// on the CellDiff. `wrapped[y]` is true iff grid row y ended by autowrap
    /// (a mid-`cols` break of a logical line) and continues into y+1, so the
    /// normal-mode proportional render can rejoin soft-wrapped rows into logical
    /// lines (the live analogue of `push_scrolled_rows`, PL-3). Length == `rows`.
    wrapped: Vec<bool>,
    /// The producer's flag for the row ABOVE row 0 (`Vt::top_continues`,
    /// carried on the CellDiff): the scrolled-off fragment the transcript
    /// holds continues into row 0. `wrapped[-1]`, the one flag the vector
    /// cannot carry.
    top_continues: bool,
    /// blank fill for clears / the grown region on resize.
    fg: u32,
    bg: u32,
}

impl Grid {
    pub fn new(cols: usize, rows: usize, fg: u32, bg: u32) -> Grid {
        Grid {
            cols,
            rows,
            cells: vec![
                Cell {
                    ch: ' ',
                    fg,
                    bg,
                    attrs: 0,
                    span: 0,
                };
                cols * rows
            ],
            cursor: (0, 0, true),
            wrapped: vec![false; rows],
            top_continues: false,
            fg,
            bg,
        }
    }

    #[inline]
    fn blank(&self) -> Cell {
        Cell {
            ch: ' ',
            fg: self.fg,
            bg: self.bg,
            attrs: 0,
            span: 0,
        }
    }

    pub fn dims(&self) -> (usize, usize) {
        (self.cols, self.rows)
    }

    /// The cursor clamped to a paintable coordinate (row in 0..rows, col in
    /// 0..cols); `visible` passes through. The render uses this so an
    /// out-of-range cursor from a misbehaving tile never indexes the buffer.
    pub fn cursor(&self) -> (usize, usize, bool) {
        let (r, c, v) = self.cursor;
        (
            (r as usize).min(self.rows.saturating_sub(1)),
            (c as usize).min(self.cols.saturating_sub(1)),
            v,
        )
    }

    /// The per-row soft-wrap flags (length == `rows`; PL-4). `wrapped()[y]` true
    /// iff grid row y ended by autowrap and continues into y+1.
    pub fn wrapped(&self) -> &[bool] {
        &self.wrapped
    }

    /// Whether the row that last scrolled off continues into row 0 (the
    /// producer's `Vt::top_continues`, as last reported): the live render
    /// joins the transcript's held fragment to row 0 exactly then.
    pub fn top_continues(&self) -> bool {
        self.top_continues
    }

    /// The whole grid, row-major (`rows * cols`) -- the PL-4 proportional render
    /// lays it as logical lines (via `Transcript::live_block`).
    pub fn cells(&self) -> &[Cell] {
        &self.cells
    }

    /// PL-4: the number of rows the proportional render lays -- through the last
    /// row with content OR the cursor row, whichever is lower, so a screen of
    /// trailing blank rows below the prompt is not painted (the bottom-anchored
    /// view would otherwise float the prompt mid-tile). At least 1.
    pub fn content_rows(&self) -> usize {
        let cur = self.cursor().0 + 1;
        let last = (0..self.rows)
            .rev()
            .find(|&r| self.row(r).iter().any(|c| c.ch != ' '))
            .map(|r| r + 1)
            .unwrap_or(0);
        last.max(cur).min(self.rows).max(1)
    }

    /// Row `r`'s cells (empty slice if out of range).
    pub fn row(&self, r: usize) -> &[Cell] {
        if r < self.rows {
            &self.cells[r * self.cols..(r + 1) * self.cols]
        } else {
            &[]
        }
    }

    /// Apply a CellDiff: position-keyed writes (out-of-bounds dropped) then the
    /// cursor. Intra-batch order is irrelevant -- each entry is a full cell at a
    /// position, so a later write to the same cell simply wins (as it would on a
    /// real screen). The producer guarantees a CellDiff is flushed before every
    /// ScrollOff / Control / Mode, so the grid is coherent at every record
    /// boundary.
    pub fn apply_celldiff(
        &mut self,
        changed: &[(u16, u16, Cell)],
        cursor: (u16, u16, bool),
        wrapped: &[bool],
        top_continues: bool,
    ) {
        for &(r, c, cell) in changed {
            let (r, c) = (r as usize, c as usize);
            if r < self.rows && c < self.cols {
                self.cells[r * self.cols + c] = cell;
            }
            // else: out of bounds -> drop. halcyond owns the geometry; a tile
            // cannot corrupt the grid past its told dims.
        }
        self.cursor = cursor;
        // Pin the wrap snapshot to `rows` (never trust the wire's length -- the
        // format-fuzz class): a shorter vec pads false, a longer one truncates.
        self.wrapped.resize(self.rows, false);
        for i in 0..self.rows {
            self.wrapped[i] = wrapped.get(i).copied().unwrap_or(false);
        }
        self.top_continues = top_continues;
    }

    /// Resize to new dims (halcyond drives this on a tile relayout; the tile
    /// then repaints with a full CellDiff), so the frame between the resize
    /// and that repaint does not flash. With `reflow` (the normal screen) the
    /// mirror re-cuts its rows exactly as the producer's vt will
    /// (`vt::reflow`, the one algorithm both run), so that frame already
    /// shows the re-wrapped lines the repaint then confirms; the rows the
    /// cursor anchor slides past are dropped here -- the producer's own
    /// ScrollOff delivers them -- and until it does the top flag is cleared,
    /// since the transcript's held fragment is not yet row 0's head.
    /// Without it (the alt screen, which the TUI repaints) the overlapping
    /// top-left block is preserved, the grown region blanked, and the
    /// cursor clamped into the new dims.
    pub fn resize(&mut self, cols: usize, rows: usize, reflow: bool) {
        if reflow && cols > 0 && rows > 0 {
            let (cr, cc, cv) = self.cursor();
            let rf = vt::reflow(
                &self.cells,
                &self.wrapped,
                self.cols,
                self.rows,
                (cc, cr),
                self.top_continues,
                cols,
                rows,
                self.blank(),
            );
            self.cells = rf.cells;
            self.wrapped = rf.wrapped;
            self.cols = cols;
            self.rows = rows;
            self.cursor = (rf.cursor.1 as u16, rf.cursor.0.min(cols - 1) as u16, cv);
            self.top_continues = rf.scrolled.is_empty() && rf.top_continues;
            return;
        }
        let blank = self.blank();
        let mut next = vec![blank; cols * rows];
        let copy_rows = self.rows.min(rows);
        let copy_cols = self.cols.min(cols);
        for r in 0..copy_rows {
            for c in 0..copy_cols {
                next[r * cols + c] = self.cells[r * self.cols + c];
            }
        }
        // Mirror the wrap flags for the preserved rows; a grown row is not
        // soft-wrapped until the producer's repaint says so.
        let mut next_wrapped = vec![false; rows];
        for r in 0..copy_rows {
            next_wrapped[r] = self.wrapped.get(r).copied().unwrap_or(false);
        }
        self.cells = next;
        self.wrapped = next_wrapped;
        self.cols = cols;
        self.rows = rows;
        let (cr, cc, cv) = self.cursor;
        self.cursor = (
            cr.min(rows.saturating_sub(1) as u16),
            cc.min(cols.saturating_sub(1) as u16),
            cv,
        );
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn c(ch: char) -> Cell {
        Cell {
            ch,
            fg: 0x00FF00,
            bg: 0x000000,
            attrs: 0,
            span: 0,
        }
    }

    #[test]
    fn new_is_blank_with_dims() {
        let g = Grid::new(4, 2, 0xAAAAAA, 0x111111);
        assert_eq!(g.dims(), (4, 2));
        for r in 0..2 {
            for cell in g.row(r) {
                assert_eq!(cell.ch, ' ');
                assert_eq!(cell.fg, 0xAAAAAA);
                assert_eq!(cell.bg, 0x111111);
            }
        }
        // a blank grid's cursor is home + visible.
        assert_eq!(g.cursor(), (0, 0, true));
    }

    #[test]
    fn celldiff_writes_and_moves_cursor() {
        let mut g = Grid::new(4, 2, 0xFFFFFF, 0);
        g.apply_celldiff(&[(0, 1, c('h')), (1, 3, c('i'))], (1, 3, true), &[false, false], false);
        assert_eq!(g.row(0)[1].ch, 'h');
        assert_eq!(g.row(1)[3].ch, 'i');
        assert_eq!(g.row(0)[0].ch, ' ', "untouched cell stays blank");
        assert_eq!(g.cursor(), (1, 3, true));
    }

    #[test]
    fn celldiff_last_write_to_a_cell_wins() {
        let mut g = Grid::new(3, 1, 0xFFFFFF, 0);
        g.apply_celldiff(&[(0, 0, c('a')), (0, 0, c('b'))], (0, 1, true), &[false], false);
        assert_eq!(g.row(0)[0].ch, 'b');
    }

    #[test]
    fn out_of_bounds_write_is_dropped_no_panic() {
        let mut g = Grid::new(2, 2, 0xFFFFFF, 0);
        // row 9 and col 9 are past the 2x2 grid: dropped, no index panic.
        g.apply_celldiff(
            &[(9, 0, c('x')), (0, 9, c('y')), (1, 1, c('z'))],
            (9, 9, true),
            &[false, false],
            false,
        );
        assert_eq!(g.row(1)[1].ch, 'z', "the in-bounds write still landed");
        // an out-of-range cursor is clamped by the accessor, never indexes.
        assert_eq!(g.cursor(), (1, 1, true));
    }

    #[test]
    fn resize_preserves_overlap_blanks_growth_clamps_cursor() {
        let mut g = Grid::new(3, 2, 0xFFFFFF, 0);
        g.apply_celldiff(&[(0, 0, c('a')), (1, 2, c('b'))], (1, 2, true), &[false, false], false);
        // shrink to 2x1: (0,0)='a' kept; (1,2)='b' falls outside; cursor clamps.
        g.resize(2, 1, false);
        assert_eq!(g.dims(), (2, 1));
        assert_eq!(g.row(0)[0].ch, 'a');
        assert_eq!(g.row(0)[1].ch, ' ');
        assert_eq!(g.cursor(), (0, 1, true), "cursor clamped into 2x1");
        // grow to 4x3: old top-left kept, new region blank.
        g.resize(4, 3, false);
        assert_eq!(g.dims(), (4, 3));
        assert_eq!(g.row(0)[0].ch, 'a', "overlap preserved across grow");
        assert_eq!(g.row(2)[3].ch, ' ', "grown region blank");
    }

    #[test]
    fn celldiff_stores_wrap_and_resize_mirrors_it() {
        // PL-4: the grid holds the per-row soft-wrap snapshot the CellDiff
        // carries, pinned to `rows`; resize preserves it for the kept rows.
        let mut g = Grid::new(4, 3, 0xFFFFFF, 0);
        g.apply_celldiff(&[], (0, 0, true), &[true, false, true], false);
        assert_eq!(g.wrapped(), &[true, false, true]);
        // a short wire vec pads false to `rows` (never indexes past the grid).
        g.apply_celldiff(&[], (0, 0, true), &[true], false);
        assert_eq!(g.wrapped(), &[true, false, false]);
        // shrink keeps the top rows' flags; grow blanks the new rows false.
        g.apply_celldiff(&[], (0, 0, true), &[true, true, true], false);
        g.resize(4, 2, false);
        assert_eq!(g.wrapped(), &[true, true]);
        g.resize(4, 4, false);
        assert_eq!(g.wrapped(), &[true, true, false, false]);
    }

    #[test]
    fn content_rows_trims_trailing_blanks() {
        // PL-4: the proportional render lays through the last content row or the
        // cursor row, not the full grid -- so trailing blanks below the prompt
        // are not painted.
        let mut g = Grid::new(4, 4, 0xFFFFFF, 0);
        assert_eq!(g.content_rows(), 1, "blank grid, cursor home -> 1 row");
        g.apply_celldiff(&[(1, 0, c('x'))], (1, 1, true), &[], false);
        assert_eq!(g.content_rows(), 2, "content + cursor on row 1 -> 2 rows");
        // cursor past the content still extends through the cursor.
        let mut g2 = Grid::new(4, 4, 0xFFFFFF, 0);
        g2.apply_celldiff(&[], (3, 0, true), &[], false);
        assert_eq!(g2.content_rows(), 4, "cursor at row 3 -> 4 rows even if blank");
    }

    #[test]
    fn row_out_of_range_is_empty() {
        let g = Grid::new(3, 2, 0xFFFFFF, 0);
        assert!(g.row(5).is_empty());
    }

    #[test]
    fn a_normal_screen_resize_reflows_and_the_alt_screen_crops() {
        // The mirror at a CONFIGURE, before the producer's repaint lands:
        // the normal screen re-cuts its soft-wrapped line at the new width
        // (the frame in between shows the line whole), the alt screen keeps
        // the old top-left crop (its TUI repaints).
        let row = |s: &str, r: u16| -> Vec<(u16, u16, Cell)> {
            s.chars().enumerate().map(|(i, ch)| (r, i as u16, c(ch))).collect()
        };
        let mut g = Grid::new(10, 4, 0xFFFFFF, 0);
        let mut changed = row("prompt> ab", 0);
        changed.extend(row("c", 1));
        g.apply_celldiff(&changed, (1, 1, true), &[true, false, false, false], false);
        g.resize(6, 4, true);
        let text = |g: &Grid, r: usize| g.row(r).iter().map(|c| c.ch).collect::<alloc::string::String>();
        assert_eq!(text(&g, 0), "prompt");
        assert_eq!(text(&g, 1), "> abc ");
        assert_eq!(g.wrapped(), &[true, false, false, false]);
        assert_eq!(g.cursor(), (1, 5, true), "the cursor keeps its logical cell");
        // The same grid as an alt screen: cropped, flags kept, cursor clamped.
        let mut g = Grid::new(10, 4, 0xFFFFFF, 0);
        g.apply_celldiff(&changed, (1, 1, true), &[true, false, false, false], false);
        g.resize(6, 4, false);
        assert_eq!(text(&g, 0), "prompt");
        assert_eq!(text(&g, 1), "c     ");
        assert_eq!(g.wrapped(), &[true, false, false, false]);
    }

    #[test]
    fn the_mirror_reflow_agrees_with_the_producer_cell_for_cell() {
        // One algorithm, two runners: whatever the producer's vt makes of a
        // resize, the mirror makes of the same screen -- cells, flags and
        // cursor -- so the frame before the repaint IS the repaint.
        let mut v = vt::Vt::new(40, 6);
        v.feed(b"Super+H / Super+V split, Super+F zooms; halcyon layout save <name> keeps an arrangement\r\nls\r\n");
        let mut g = Grid::new(40, 6, v.pal.fg, v.pal.bg);
        let all: Vec<(u16, u16, Cell)> = v
            .cells
            .iter()
            .enumerate()
            .map(|(i, c)| ((i / 40) as u16, (i % 40) as u16, *c))
            .collect();
        g.apply_celldiff(&all, (v.cy as u16, v.cx as u16, true), v.wrapped(), false);
        for &(w, h) in &[(17usize, 6usize), (100, 3), (9, 30), (40, 6)] {
            v.resize(w, h);
            g.resize(w, h, true);
            assert_eq!(g.cells(), &v.cells[..], "{w}x{h}");
            assert_eq!(g.wrapped(), v.wrapped(), "{w}x{h}");
            assert_eq!(g.cursor().0, v.cy, "{w}x{h}");
            assert_eq!(g.cursor().1, v.cx.min(w - 1), "{w}x{h}");
        }
    }

    #[test]
    fn a_mirror_reflow_that_slides_rows_off_clears_the_top_flag_until_the_repaint() {
        // The rows the anchor slides past are the producer's ScrollOff to
        // deliver; until it lands the transcript's held fragment is not row
        // 0's head, so the join is off. A reflow that slides nothing keeps
        // the flag it was told.
        let row = |s: &str, r: u16| -> Vec<(u16, u16, Cell)> {
            s.chars().enumerate().map(|(i, ch)| (r, i as u16, c(ch))).collect()
        };
        let mut g = Grid::new(8, 2, 0xFFFFFF, 0);
        let mut changed = row("abcdefgh", 0);
        changed.extend(row("ij", 1));
        g.apply_celldiff(&changed, (1, 2, true), &[true, false], true);
        assert!(g.top_continues());
        g.resize(10, 2, true); // "abcdefghij" fits row 0: nothing slides
        assert!(g.top_continues(), "kept: row 0 still starts the same line");
        g.resize(4, 2, true); // abcd|efgh|ij: the cursor row 2 slides one off
        assert!(!g.top_continues(), "cleared until the producer's ScrollOff + repaint");
        g.apply_celldiff(&[], (1, 2, true), &[true, false], true);
        assert!(g.top_continues(), "the repaint restores the producer's answer");
    }
}
