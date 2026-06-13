// kaua::layout -- split a Rect into sub-Rects under simple constraints.
//
// A single-axis greedy solver (KAUA.md section 3.2), NOT a cassowary-class
// constraint system: fixed sizes (Length, Pct) are resolved first, then the
// remainder is shared equally among the flexible slots (Min, Fill). This covers
// every v1.0 layout (an editor body + a status line; a list pane + a detail
// pane); the richer solver is a documented seam. Pure + host-testable.

use alloc::vec::Vec;

use crate::rect::Rect;

/// One slot's sizing rule along the split axis.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Constraint {
    /// Exactly `n` cells (clamped to what remains).
    Length(u16),
    /// At least `n` cells; grows with its share of the remainder.
    Min(u16),
    /// `n` percent of the axis extent (clamped at 100).
    Pct(u16),
    /// An equal share of the remainder after fixed slots.
    Fill,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Direction {
    Vertical,
    Horizontal,
}

/// A constraint set + a direction; `split` applies it to a `Rect`.
#[derive(Clone, Copy)]
pub struct Layout<'a> {
    direction: Direction,
    constraints: &'a [Constraint],
    margin: u16,
}

impl<'a> Layout<'a> {
    pub const fn vertical(constraints: &'a [Constraint]) -> Self {
        Layout {
            direction: Direction::Vertical,
            constraints,
            margin: 0,
        }
    }

    pub const fn horizontal(constraints: &'a [Constraint]) -> Self {
        Layout {
            direction: Direction::Horizontal,
            constraints,
            margin: 0,
        }
    }

    /// Shrink the area by `m` cells on every side before splitting.
    pub fn margin(mut self, m: u16) -> Self {
        self.margin = m;
        self
    }

    /// One `Rect` per constraint, tiling the (margin-shrunk) area along the
    /// axis. Sizes are clamped cumulatively so no rect spills past the area; if
    /// no Fill/Min absorbs the remainder, trailing space is left unassigned.
    pub fn split(&self, area: Rect) -> Vec<Rect> {
        let inner = area.inner(self.margin);
        let extent = match self.direction {
            Direction::Vertical => inner.height,
            Direction::Horizontal => inner.width,
        };
        let sizes = solve(self.constraints, extent);
        let mut out = Vec::with_capacity(self.constraints.len());
        let mut off: u16 = 0;
        for &s in &sizes {
            let start = off.min(extent);
            let len = s.min(extent - start);
            let r = match self.direction {
                Direction::Vertical => Rect::new(inner.x, inner.y + start, inner.width, len),
                Direction::Horizontal => Rect::new(inner.x + start, inner.y, len, inner.height),
            };
            out.push(r);
            off = start.saturating_add(len);
        }
        out
    }
}

/// Resolve each constraint to a size along an axis of `extent` cells. Length +
/// Pct are fixed; Min is a floor that also flexes; Fill is pure flex. The
/// remainder after the fixed sizes + Min floors is shared equally across the
/// flex set (Min and Fill), the first slots taking the +1 from any remainder.
fn solve(cs: &[Constraint], extent: u16) -> Vec<u16> {
    let e = extent as u32;
    let mut sizes: Vec<u16> = Vec::with_capacity(cs.len());
    let mut consumed: u32 = 0;
    for &c in cs {
        let v = match c {
            Constraint::Length(n) => n,
            Constraint::Pct(p) => (p.min(100) as u32 * e / 100) as u16,
            Constraint::Min(n) => n,
            Constraint::Fill => 0,
        };
        consumed += v as u32;
        sizes.push(v);
    }
    let remainder = e.saturating_sub(consumed);
    let flex: Vec<usize> = cs
        .iter()
        .enumerate()
        .filter(|(_, c)| matches!(c, Constraint::Min(_) | Constraint::Fill))
        .map(|(i, _)| i)
        .collect();
    if !flex.is_empty() && remainder > 0 {
        let share = remainder / flex.len() as u32;
        let extra = remainder % flex.len() as u32;
        for (k, &i) in flex.iter().enumerate() {
            let add = share + if (k as u32) < extra { 1 } else { 0 };
            sizes[i] = sizes[i].saturating_add(add as u16);
        }
    }
    sizes
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn editor_body_plus_status_line() {
        let v = Layout::vertical(&[Constraint::Min(1), Constraint::Length(1)])
            .split(Rect::new(0, 0, 20, 10));
        assert_eq!(v, [Rect::new(0, 0, 20, 9), Rect::new(0, 9, 20, 1)]);
    }

    #[test]
    fn pct_then_fill_horizontal() {
        let v = Layout::horizontal(&[Constraint::Pct(30), Constraint::Fill])
            .split(Rect::new(0, 0, 10, 5));
        assert_eq!(v, [Rect::new(0, 0, 3, 5), Rect::new(3, 0, 7, 5)]);
    }

    #[test]
    fn two_fill_share_the_remainder() {
        // 11 / 2 -> 6 + 5 (the first flex slot takes the +1).
        let v =
            Layout::horizontal(&[Constraint::Fill, Constraint::Fill]).split(Rect::new(0, 0, 11, 1));
        assert_eq!(v, [Rect::new(0, 0, 6, 1), Rect::new(6, 0, 5, 1)]);
    }

    #[test]
    fn fixed_only_leaves_trailing_space() {
        let v = Layout::vertical(&[Constraint::Length(1), Constraint::Length(1)])
            .split(Rect::new(0, 0, 4, 10));
        // No flex slot -> the last 8 rows are unassigned (by design).
        assert_eq!(v, [Rect::new(0, 0, 4, 1), Rect::new(0, 1, 4, 1)]);
    }

    #[test]
    fn oversubscribed_clamps_to_the_area() {
        let v = Layout::vertical(&[Constraint::Length(8), Constraint::Length(8)])
            .split(Rect::new(0, 0, 4, 10));
        assert_eq!(v, [Rect::new(0, 0, 4, 8), Rect::new(0, 8, 4, 2)]);
    }

    #[test]
    fn margin_shrinks_all_sides() {
        let v = Layout::vertical(&[Constraint::Fill])
            .margin(1)
            .split(Rect::new(0, 0, 10, 10));
        assert_eq!(v, [Rect::new(1, 1, 8, 8)]);
    }

    #[test]
    fn empty_constraints_yield_no_rects() {
        let v = Layout::vertical(&[]).split(Rect::new(0, 0, 10, 10));
        assert!(v.is_empty());
    }

    #[test]
    fn min_floor_grows_with_remainder() {
        // Min(2) floors at 2 then takes the whole 6-cell remainder.
        let v = Layout::horizontal(&[Constraint::Length(2), Constraint::Min(2)])
            .split(Rect::new(0, 0, 10, 1));
        assert_eq!(v, [Rect::new(0, 0, 2, 1), Rect::new(2, 0, 8, 1)]);
    }
}
