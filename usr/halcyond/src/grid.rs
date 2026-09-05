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
    pub fn apply_celldiff(&mut self, changed: &[(u16, u16, Cell)], cursor: (u16, u16, bool)) {
        for &(r, c, cell) in changed {
            let (r, c) = (r as usize, c as usize);
            if r < self.rows && c < self.cols {
                self.cells[r * self.cols + c] = cell;
            }
            // else: out of bounds -> drop. halcyond owns the geometry; a tile
            // cannot corrupt the grid past its told dims.
        }
        self.cursor = cursor;
    }

    /// Resize to new dims (halcyond drives this on a tile relayout; the tile
    /// then repaints with a full CellDiff). Preserve the overlapping top-left
    /// block so the frame between resize and that repaint does not flash, blank
    /// the grown region, and clamp the cursor into the new dims.
    pub fn resize(&mut self, cols: usize, rows: usize) {
        let blank = self.blank();
        let mut next = vec![blank; cols * rows];
        let copy_rows = self.rows.min(rows);
        let copy_cols = self.cols.min(cols);
        for r in 0..copy_rows {
            for c in 0..copy_cols {
                next[r * cols + c] = self.cells[r * self.cols + c];
            }
        }
        self.cells = next;
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
        g.apply_celldiff(&[(0, 1, c('h')), (1, 3, c('i'))], (1, 3, true));
        assert_eq!(g.row(0)[1].ch, 'h');
        assert_eq!(g.row(1)[3].ch, 'i');
        assert_eq!(g.row(0)[0].ch, ' ', "untouched cell stays blank");
        assert_eq!(g.cursor(), (1, 3, true));
    }

    #[test]
    fn celldiff_last_write_to_a_cell_wins() {
        let mut g = Grid::new(3, 1, 0xFFFFFF, 0);
        g.apply_celldiff(&[(0, 0, c('a')), (0, 0, c('b'))], (0, 1, true));
        assert_eq!(g.row(0)[0].ch, 'b');
    }

    #[test]
    fn out_of_bounds_write_is_dropped_no_panic() {
        let mut g = Grid::new(2, 2, 0xFFFFFF, 0);
        // row 9 and col 9 are past the 2x2 grid: dropped, no index panic.
        g.apply_celldiff(
            &[(9, 0, c('x')), (0, 9, c('y')), (1, 1, c('z'))],
            (9, 9, true),
        );
        assert_eq!(g.row(1)[1].ch, 'z', "the in-bounds write still landed");
        // an out-of-range cursor is clamped by the accessor, never indexes.
        assert_eq!(g.cursor(), (1, 1, true));
    }

    #[test]
    fn resize_preserves_overlap_blanks_growth_clamps_cursor() {
        let mut g = Grid::new(3, 2, 0xFFFFFF, 0);
        g.apply_celldiff(&[(0, 0, c('a')), (1, 2, c('b'))], (1, 2, true));
        // shrink to 2x1: (0,0)='a' kept; (1,2)='b' falls outside; cursor clamps.
        g.resize(2, 1);
        assert_eq!(g.dims(), (2, 1));
        assert_eq!(g.row(0)[0].ch, 'a');
        assert_eq!(g.row(0)[1].ch, ' ');
        assert_eq!(g.cursor(), (0, 1, true), "cursor clamped into 2x1");
        // grow to 4x3: old top-left kept, new region blank.
        g.resize(4, 3);
        assert_eq!(g.dims(), (4, 3));
        assert_eq!(g.row(0)[0].ch, 'a', "overlap preserved across grow");
        assert_eq!(g.row(2)[3].ch, ' ', "grown region blank");
    }

    #[test]
    fn row_out_of_range_is_empty() {
        let g = Grid::new(3, 2, 0xFFFFFF, 0);
        assert!(g.row(5).is_empty());
    }
}
