// kaua::style -- cell appearance: truecolor fg/bg + text attributes.
//
// The default theme maps these to the committed Bonfire palette
// (docs/UTOPIA-VISUAL.md); the backend (T-1b) emits the matching SGR escape
// for each changed cell.

/// A cell colour. `Reset` is the terminal's default; `Rgb` is truecolor,
/// emitted as an SGR `38;2;r;g;b` (fg) / `48;2;r;g;b` (bg) sequence.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub enum Color {
    #[default]
    Reset,
    Rgb(u8, u8, u8),
}

/// Text attributes as an OR-able bitset; the backend emits the matching SGR.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub struct Attr(u16);

impl Attr {
    pub const NONE: Attr = Attr(0);
    pub const BOLD: Attr = Attr(1 << 0);
    pub const DIM: Attr = Attr(1 << 1);
    pub const ITALIC: Attr = Attr(1 << 2);
    pub const UNDERLINE: Attr = Attr(1 << 3);
    pub const REVERSE: Attr = Attr(1 << 4);

    pub const fn bits(self) -> u16 {
        self.0
    }
    pub const fn is_empty(self) -> bool {
        self.0 == 0
    }
    pub const fn contains(self, other: Attr) -> bool {
        self.0 & other.0 == other.0
    }
}

impl core::ops::BitOr for Attr {
    type Output = Attr;
    fn bitor(self, rhs: Attr) -> Attr {
        Attr(self.0 | rhs.0)
    }
}

impl core::ops::BitOrAssign for Attr {
    fn bitor_assign(&mut self, rhs: Attr) {
        self.0 |= rhs.0;
    }
}

/// A cell's full appearance.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub struct Style {
    pub fg: Color,
    pub bg: Color,
    pub attr: Attr,
}

impl Style {
    pub const fn new() -> Self {
        Style {
            fg: Color::Reset,
            bg: Color::Reset,
            attr: Attr::NONE,
        }
    }

    pub fn fg(mut self, c: Color) -> Self {
        self.fg = c;
        self
    }

    pub fn bg(mut self, c: Color) -> Self {
        self.bg = c;
        self
    }

    pub fn attr(mut self, a: Attr) -> Self {
        self.attr = a;
        self
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn attr_is_an_or_able_bitset() {
        let a = Attr::BOLD | Attr::UNDERLINE;
        assert!(a.contains(Attr::BOLD));
        assert!(a.contains(Attr::UNDERLINE));
        assert!(!a.contains(Attr::ITALIC));
        assert!(!a.is_empty());
        assert!(Attr::NONE.is_empty());
    }

    #[test]
    fn style_builders_compose() {
        let s = Style::new()
            .fg(Color::Rgb(224, 120, 64))
            .bg(Color::Reset)
            .attr(Attr::BOLD);
        assert_eq!(s.fg, Color::Rgb(224, 120, 64));
        assert_eq!(s.bg, Color::Reset);
        assert!(s.attr.contains(Attr::BOLD));
    }

    #[test]
    fn default_is_reset_reset_none() {
        let s = Style::default();
        assert_eq!(s.fg, Color::Reset);
        assert_eq!(s.bg, Color::Reset);
        assert!(s.attr.is_empty());
    }
}
