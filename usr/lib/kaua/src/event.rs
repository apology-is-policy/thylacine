// kaua::event -- the terminal-agnostic key model the backend produces.
//
// A `KeyEvent` is what the VT/ANSI parser (kaua::input) yields from raw fd-0
// bytes; widgets + the editor match on it. Deliberately terminal-independent:
// the parser already folded escape sequences + C0 controls into these variants,
// so app code never sees a raw byte. The `EventSource` loop abstraction (the
// Loom seam, KAUA.md section 4.4) lands in T-2; T-1 defines the event the
// backend produces.

/// A logical key. Modifiers ride alongside in `KeyEvent::mods`; `Char` carries
/// the already-cased grapheme (Shift is baked into `'A'` vs `'a'`, matching the
/// crossterm convention), so `SHIFT` in `mods` is only set for the non-text keys
/// where a terminal encodes it (modified arrows, BackTab).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum KeyCode {
    /// A printable character (one Unicode scalar). For a Ctrl-letter the parser
    /// emits the *lowercase* letter with `Mods::CTRL` (e.g. Ctrl-C -> `Char('c')`
    /// + CTRL); for Alt-letter, the letter with `Mods::ALT`.
    Char(char),
    Enter,
    Esc,
    Backspace,
    Tab,
    /// Shift-Tab (`ESC[Z`).
    BackTab,
    Left,
    Right,
    Up,
    Down,
    Home,
    End,
    PageUp,
    PageDown,
    Delete,
    Insert,
    /// A function key, `F(1)`..`F(12)`.
    F(u8),
}

/// Keyboard modifier bitset. Recovered only where the encoding carries it:
/// C0 control bytes -> `CTRL`; `ESC`-prefixed printables -> `ALT`; CSI/SS3
/// modifier params -> the decoded `SHIFT`/`ALT`/`CTRL` combination.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub struct Mods(u8);

impl Mods {
    pub const NONE: Mods = Mods(0);
    pub const SHIFT: Mods = Mods(1 << 0);
    pub const ALT: Mods = Mods(1 << 1);
    pub const CTRL: Mods = Mods(1 << 2);

    #[inline]
    pub const fn bits(self) -> u8 {
        self.0
    }
    #[inline]
    pub const fn is_empty(self) -> bool {
        self.0 == 0
    }
    #[inline]
    pub const fn contains(self, other: Mods) -> bool {
        self.0 & other.0 == other.0
    }
}

impl core::ops::BitOr for Mods {
    type Output = Mods;
    #[inline]
    fn bitor(self, rhs: Mods) -> Mods {
        Mods(self.0 | rhs.0)
    }
}

impl core::ops::BitOrAssign for Mods {
    #[inline]
    fn bitor_assign(&mut self, rhs: Mods) {
        self.0 |= rhs.0;
    }
}

/// One key press: a `KeyCode` plus the modifiers the encoding revealed.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct KeyEvent {
    pub code: KeyCode,
    pub mods: Mods,
}

impl KeyEvent {
    /// A key with no modifiers.
    #[inline]
    pub const fn new(code: KeyCode) -> Self {
        KeyEvent {
            code,
            mods: Mods::NONE,
        }
    }

    /// A key with the given modifiers.
    #[inline]
    pub const fn with(code: KeyCode, mods: Mods) -> Self {
        KeyEvent { code, mods }
    }

    /// Convenience: `Char(c)` with no modifiers.
    #[inline]
    pub const fn char(c: char) -> Self {
        KeyEvent::new(KeyCode::Char(c))
    }

    /// `true` iff this is `Char(c)` carrying exactly `Mods::CTRL`.
    #[inline]
    pub fn is_ctrl(self, c: char) -> bool {
        self.code == KeyCode::Char(c) && self.mods == Mods::CTRL
    }
}

/// The event-loop message (KAUA.md sections 3.4 / 4.1). `Key` is what the
/// `EventSource` (kaua::source) produces from the VT parser. `Resize` + `Tick`
/// are v1.0 seams -- there is no winsize signal nor a timer source yet -- kept
/// in the type for the loop's match and the future.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Event {
    Key(KeyEvent),
    Resize(u16, u16),
    Tick,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn mods_is_an_or_able_bitset() {
        let m = Mods::CTRL | Mods::ALT;
        assert!(m.contains(Mods::CTRL));
        assert!(m.contains(Mods::ALT));
        assert!(!m.contains(Mods::SHIFT));
        assert!(!m.is_empty());
        assert!(Mods::NONE.is_empty());
    }

    #[test]
    fn key_constructors() {
        assert_eq!(KeyEvent::char('a'), KeyEvent::new(KeyCode::Char('a')));
        let c = KeyEvent::with(KeyCode::Char('c'), Mods::CTRL);
        assert!(c.is_ctrl('c'));
        assert!(!c.is_ctrl('x'));
        assert!(!KeyEvent::char('c').is_ctrl('c')); // no CTRL -> not a ctrl-c
    }
}
