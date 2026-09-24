//! The Operator's Manual reader, pure half (docs/MANUAL-DESIGN.md): the
//! section format and its checker (section 3, `format`), the Beacon and plain
//! realization (section 4, `render`), word wrapping (4.3, `wrap`), and section
//! lookup (section 5, `catalog`). No I/O; the binary supplies files, the tier
//! and the width.

#![no_std]

extern crate alloc;

pub mod catalog;
pub mod format;
pub mod render;
pub mod wrap;

#[cfg(test)]
mod bounds;

use alloc::string::String;

/// The largest section the reader accepts (MANUAL-DESIGN.md 3.1).
pub const SECTION_MAX: usize = 1024 * 1024;

/// The reader's memory budget (MANUAL-DESIGN.md 8.1). The `bounds` test checks
/// and renders the sections most expensive to hold on the guest's heap and
/// asserts its peak footprint stays within half of it.
pub const HEAP_BYTES: usize = 16 * 1024 * 1024;

/// A console narrower than this is treated as width-unknown (4.3).
pub const WRAP_MIN_COLUMNS: usize = 20;

/// True for the characters section text must not carry (3.1) and the renderer
/// never emits (4.4): C0 controls, DEL, and the C1 range. LF and TAB are the
/// caller's to allow.
pub fn is_control(c: char) -> bool {
    let u = c as u32;
    u < 0x20 || u == 0x7f || (0x80..=0x9f).contains(&u)
}

/// True for the bidirectional embedding, override, and isolate controls (3.1),
/// which reorder how the text around them is displayed. The implicit marks
/// (U+061C, U+200E, U+200F) are not among them.
pub fn is_bidi_control(c: char) -> bool {
    matches!(c, '\u{202a}'..='\u{202e}' | '\u{2066}'..='\u{2069}')
}

/// True for every character output replaces with U+FFFD (4.4). Each site that
/// writes text it did not produce tests this, so the set has one definition.
pub fn is_replaced(c: char) -> bool {
    is_control(c) || is_bidi_control(c)
}

/// Replace every character of `is_replaced` with U+FFFD (4.4), keeping TAB when
/// `keep_tab` (code blocks). Section text therefore cannot open, close, or
/// imitate a Beacon frame, carry any other terminal control sequence, or reorder
/// its display -- independently of the checker, which rejects such text first.
pub fn sanitize(s: &str, keep_tab: bool) -> String {
    let mut out = String::with_capacity(s.len());
    for c in s.chars() {
        if is_replaced(c) && !(keep_tab && c == '\t') {
            out.push('\u{fffd}');
        } else {
            out.push(c);
        }
    }
    out
}

/// The wrap width from the contents of `/dev/winsize` (`winsize <cols>
/// <rows>`): `Some(cols)` when at least `WRAP_MIN_COLUMNS`, otherwise `None`
/// -- a serial console reports `winsize 0 0`, and a width that cannot be
/// parsed must not be guessed.
pub fn console_width(winsize: &[u8]) -> Option<usize> {
    let s = core::str::from_utf8(winsize).ok()?;
    let rest = s.trim().strip_prefix("winsize ")?;
    let mut fields = rest.split(' ');
    let cols: usize = fields.next()?.parse().ok()?;
    let _rows: usize = fields.next()?.parse().ok()?;
    if cols >= WRAP_MIN_COLUMNS {
        Some(cols)
    } else {
        None
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sanitize_replaces_controls_and_keeps_text() {
        assert_eq!(
            sanitize("a\x1b]b\x07c\x7f\u{9b}d", false),
            "a\u{fffd}]b\u{fffd}c\u{fffd}\u{fffd}d"
        );
        assert_eq!(sanitize("x\ty", false), "x\u{fffd}y");
        assert_eq!(sanitize("x\ty", true), "x\ty");
        assert_eq!(
            sanitize("caf\u{e9} \u{2014} ok", false),
            "caf\u{e9} \u{2014} ok"
        );
    }

    #[test]
    fn the_bidirectional_controls_are_exactly_nine() {
        let set: alloc::vec::Vec<u32> = (0..=0x10ffffu32)
            .filter_map(char::from_u32)
            .filter(|&c| is_bidi_control(c))
            .map(|c| c as u32)
            .collect();
        assert_eq!(
            set,
            [0x202a, 0x202b, 0x202c, 0x202d, 0x202e, 0x2066, 0x2067, 0x2068, 0x2069]
        );
        // The implicit marks reorder no letters and stay.
        for mark in ['\u{61c}', '\u{200e}', '\u{200f}'] {
            assert!(!is_replaced(mark));
        }
        assert_eq!(
            sanitize("a\u{202e}b\u{2066}c\u{200f}d", false),
            "a\u{fffd}b\u{fffd}c\u{200f}d"
        );
        assert_eq!(sanitize("\t\u{2069}", true), "\t\u{fffd}");
    }

    #[test]
    fn console_width_parses_the_winsize_leaf() {
        assert_eq!(console_width(b"winsize 160 50\n"), Some(160));
        assert_eq!(console_width(b"winsize 20 5"), Some(20));
        // The serial posture and anything narrower than the floor: no wrap.
        assert_eq!(console_width(b"winsize 0 0\n"), None);
        assert_eq!(console_width(b"winsize 19 40\n"), None);
        // Unparseable input is width-unknown, never a guess.
        assert_eq!(console_width(b"winsize x 40\n"), None);
        assert_eq!(console_width(b"size 80 24\n"), None);
        assert_eq!(console_width(b""), None);
    }
}
