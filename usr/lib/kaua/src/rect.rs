// kaua::rect -- a rectangular screen region; the unit of layout + rendering.

/// A region in cell coordinates. `(x, y)` is the top-left; `width`/`height`
/// extend right/down. Coordinates are u16 (a console is far under 65535 cells
/// on a side).
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub struct Rect {
    pub x: u16,
    pub y: u16,
    pub width: u16,
    pub height: u16,
}

impl Rect {
    pub const fn new(x: u16, y: u16, width: u16, height: u16) -> Self {
        Rect {
            x,
            y,
            width,
            height,
        }
    }

    pub const fn area(self) -> u32 {
        self.width as u32 * self.height as u32
    }

    pub const fn is_empty(self) -> bool {
        self.width == 0 || self.height == 0
    }

    /// The x just past the right edge (saturating).
    pub const fn right(self) -> u16 {
        self.x.saturating_add(self.width)
    }

    /// The y just past the bottom edge (saturating).
    pub const fn bottom(self) -> u16 {
        self.y.saturating_add(self.height)
    }

    pub const fn contains(self, x: u16, y: u16) -> bool {
        x >= self.x && x < self.right() && y >= self.y && y < self.bottom()
    }

    /// Shrink by `margin` cells on every side, clamped so the result never
    /// inverts (an over-large margin yields a zero-extent rect at the centre).
    pub fn inner(self, margin: u16) -> Rect {
        let dx = margin.min(self.width / 2);
        let dy = margin.min(self.height / 2);
        Rect {
            x: self.x + dx,
            y: self.y + dy,
            width: self.width - 2 * dx,
            height: self.height - 2 * dy,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn area_and_edges() {
        let r = Rect::new(2, 3, 10, 4);
        assert_eq!(r.area(), 40);
        assert_eq!(r.right(), 12);
        assert_eq!(r.bottom(), 7);
        assert!(!r.is_empty());
        assert!(Rect::new(0, 0, 0, 5).is_empty());
    }

    #[test]
    fn contains_is_half_open() {
        let r = Rect::new(2, 3, 4, 2);
        assert!(r.contains(2, 3));
        assert!(r.contains(5, 4));
        assert!(!r.contains(6, 4)); // right edge is exclusive
        assert!(!r.contains(5, 5)); // bottom edge is exclusive
        assert!(!r.contains(1, 3));
    }

    #[test]
    fn inner_shrinks_and_clamps() {
        let r = Rect::new(0, 0, 10, 6).inner(1);
        assert_eq!(r, Rect::new(1, 1, 8, 4));
        // Over-large margin clamps to a centred zero-extent rect, never inverts.
        let z = Rect::new(0, 0, 4, 4).inner(100);
        assert_eq!(z.width, 0);
        assert_eq!(z.height, 0);
    }
}
