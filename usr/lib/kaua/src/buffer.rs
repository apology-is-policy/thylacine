// kaua::buffer -- the cell grid + the diff that drives flicker-free rendering.
//
// Immediate-mode: each frame the app redraws the whole back buffer; the
// backend (T-1b) diffs it against the front buffer and emits only the changed
// cells (the ratatui model). A Buffer is a pure value -- no terminal, fully
// host-testable.

use alloc::vec::Vec;

use crate::rect::Rect;
use crate::style::Style;

/// One screen cell: a character + its appearance. Width is assumed 1 at v1.0;
/// the full unicode-width table (wide CJK / combining clusters) is a documented
/// seam (KAUA.md section 3.1).
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct Cell {
    pub symbol: char,
    pub style: Style,
}

impl Cell {
    pub fn new(symbol: char, style: Style) -> Self {
        Cell { symbol, style }
    }
}

impl Default for Cell {
    fn default() -> Self {
        Cell {
            symbol: ' ',
            style: Style::new(),
        }
    }
}

/// A row-major grid of `Cell`s covering `area`.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct Buffer {
    pub area: Rect,
    pub content: Vec<Cell>,
}

impl Buffer {
    /// An empty (space-filled, default-style) buffer over `area`.
    pub fn empty(area: Rect) -> Self {
        Self::filled(area, Cell::default())
    }

    pub fn filled(area: Rect, cell: Cell) -> Self {
        let n = area.area() as usize;
        Buffer {
            area,
            content: alloc::vec![cell; n],
        }
    }

    /// Linear index of `(x, y)`; `None` if outside `area`.
    pub fn index_of(&self, x: u16, y: u16) -> Option<usize> {
        if !self.area.contains(x, y) {
            return None;
        }
        let col = (x - self.area.x) as usize;
        let row = (y - self.area.y) as usize;
        Some(row * self.area.width as usize + col)
    }

    pub fn get(&self, x: u16, y: u16) -> Option<&Cell> {
        self.index_of(x, y).map(|i| &self.content[i])
    }

    pub fn get_mut(&mut self, x: u16, y: u16) -> Option<&mut Cell> {
        match self.index_of(x, y) {
            Some(i) => Some(&mut self.content[i]),
            None => None,
        }
    }

    /// Set one cell; a no-op if `(x, y)` is out of bounds.
    pub fn set_cell(&mut self, x: u16, y: u16, cell: Cell) {
        if let Some(i) = self.index_of(x, y) {
            self.content[i] = cell;
        }
    }

    /// Write `s` from `(x, y)`, one cell per char, clipped to the row's right
    /// edge. Returns the x just past the last written cell.
    pub fn set_str(&mut self, x: u16, y: u16, s: &str, style: Style) -> u16 {
        let mut cx = x;
        let max_x = self.area.right();
        for ch in s.chars() {
            if cx >= max_x {
                break;
            }
            self.set_cell(cx, y, Cell::new(ch, style));
            cx = cx.saturating_add(1);
        }
        cx
    }

    /// Reset every cell to the default (space, default style).
    pub fn reset(&mut self) {
        for c in self.content.iter_mut() {
            *c = Cell::default();
        }
    }

    /// Resize to `area`, reallocating + clearing. A resize discards content;
    /// the caller redraws (immediate-mode, so that is expected).
    pub fn resize(&mut self, area: Rect) {
        self.area = area;
        self.content.clear();
        self.content.resize(area.area() as usize, Cell::default());
    }

    /// The cells that differ from `prev` (the on-screen front buffer), as
    /// `(x, y, &Cell)` updates -- exactly what the backend emits. Areas must
    /// match; a size change is a full repaint (empty diff + a caller-driven
    /// clear), so this returns nothing in that case.
    pub fn diff<'a>(&'a self, prev: &Buffer) -> Vec<(u16, u16, &'a Cell)> {
        let mut out = Vec::new();
        if self.area != prev.area {
            return out;
        }
        let w = self.area.width as usize;
        for (i, (cur, old)) in self.content.iter().zip(prev.content.iter()).enumerate() {
            if cur != old {
                let x = self.area.x + (i % w) as u16;
                let y = self.area.y + (i / w) as u16;
                out.push((x, y, cur));
            }
        }
        out
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::style::{Color, Style};

    fn area() -> Rect {
        Rect::new(0, 0, 10, 3)
    }

    #[test]
    fn set_str_and_get() {
        let mut b = Buffer::empty(area());
        b.set_str(0, 0, "hi", Style::new());
        assert_eq!(b.get(0, 0).unwrap().symbol, 'h');
        assert_eq!(b.get(1, 0).unwrap().symbol, 'i');
        assert_eq!(b.get(2, 0).unwrap().symbol, ' ');
    }

    #[test]
    fn set_str_clips_to_row() {
        let mut b = Buffer::empty(area());
        let end = b.set_str(8, 0, "abcd", Style::new());
        assert_eq!(end, 10); // clipped at the right edge
        assert_eq!(b.get(8, 0).unwrap().symbol, 'a');
        assert_eq!(b.get(9, 0).unwrap().symbol, 'b');
        assert!(b.get(10, 0).is_none());
    }

    #[test]
    fn out_of_bounds_is_none() {
        let b = Buffer::empty(area());
        assert!(b.get(10, 0).is_none());
        assert!(b.get(0, 3).is_none());
        assert!(b.index_of(10, 0).is_none());
    }

    #[test]
    fn diff_reports_only_changed_cells() {
        let front = Buffer::empty(area());
        let mut back = front.clone();
        let ember = Style::new().fg(Color::Rgb(224, 120, 64));
        back.set_cell(3, 1, Cell::new('X', ember));
        let d = back.diff(&front);
        assert_eq!(d.len(), 1);
        assert_eq!((d[0].0, d[0].1), (3, 1));
        assert_eq!(d[0].2.symbol, 'X');
        assert_eq!(d[0].2.style.fg, Color::Rgb(224, 120, 64));
    }

    #[test]
    fn diff_empty_when_identical() {
        let front = Buffer::empty(area());
        let back = front.clone();
        assert!(back.diff(&front).is_empty());
    }

    #[test]
    fn diff_is_empty_across_a_resize() {
        let front = Buffer::empty(area());
        let back = Buffer::empty(Rect::new(0, 0, 4, 4));
        assert!(back.diff(&front).is_empty());
    }

    #[test]
    fn resize_changes_area_and_clears() {
        let mut b = Buffer::empty(area());
        b.set_cell(0, 0, Cell::new('Z', Style::new()));
        b.resize(Rect::new(0, 0, 4, 4));
        assert_eq!(b.area.width, 4);
        assert_eq!(b.content.len(), 16);
        assert_eq!(b.get(0, 0).unwrap().symbol, ' ');
    }
}
