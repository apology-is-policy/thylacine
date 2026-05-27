// libutopia::ansi -- ANSI SGR escape emission for the Pale Fire palette.
//
// Per UTOPIA-VISUAL.md section 4.3 -- 24-bit colour escapes
// (`ESC[38;2;R;G;Bm` foreground, `ESC[48;2;R;G;Bm` background,
// `ESC[0m` reset). Capability detection is deferred to v1.x;
// v1.0 emits unconditionally per UTOPIA-VISUAL.md section 4.4
// (terminals that don't grok 24-bit colour degrade in their
// own way).
//
// All emission goes through alloc::string::String -- libutopia is
// no_std + alloc, and the consumer (`ut`, a future coreutil) hands the
// rendered bytes to whatever stdout it uses (libthyla_rs::t_putstr,
// libthyla_rs::io::Write on Stdio, ...).

use crate::palette::{rgb_of, Rgb, Role};
use alloc::format;
use alloc::string::String;

/// The ANSI reset sequence -- `ESC[0m`. Every coloured emission ends
/// with this so colour cannot bleed into subsequent output.
pub const RESET: &str = "\x1b[0m";

/// Compose an ANSI foreground 24-bit colour escape for the given RGB.
/// Returns a heap String; the caller writes it to stdout.
///
/// Format per ECMA-48: `ESC[38;2;R;G;Bm`.
pub fn fg_seq(rgb: Rgb) -> String {
    format!("\x1b[38;2;{};{};{}m", rgb.r, rgb.g, rgb.b)
}

/// Compose an ANSI background 24-bit colour escape for the given RGB.
/// Format per ECMA-48: `ESC[48;2;R;G;Bm`.
pub fn bg_seq(rgb: Rgb) -> String {
    format!("\x1b[48;2;{};{};{}m", rgb.r, rgb.g, rgb.b)
}

/// Wrap `text` with the role's FG escape + reset, returning a fresh
/// String. Common case: emit a path segment in Path colour, the `⊢`
/// glyph in Glyph colour, etc.
pub fn fg(role: Role, text: &str) -> String {
    let rgb = rgb_of(role);
    let mut out = fg_seq(rgb);
    out.push_str(text);
    out.push_str(RESET);
    out
}

/// Wrap `text` with the role's BG escape + reset, returning a fresh
/// String. Reserved -- disciplined v1.0 programs do not emit BG
/// explicitly (the terminal renders the bg of every cell already).
pub fn bg(role: Role, text: &str) -> String {
    let rgb = rgb_of(role);
    let mut out = bg_seq(rgb);
    out.push_str(text);
    out.push_str(RESET);
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn fg_seq_matches_scripture() {
        // UTOPIA-VISUAL.md section 4 -- 24-bit colour escape format.
        // Path RGB 0x88 0x98 0xb4 -> 136 152 180 decimal.
        assert_eq!(fg_seq(Rgb::new(0x88, 0x98, 0xb4)),
                   "\x1b[38;2;136;152;180m");
    }

    #[test]
    fn bg_seq_matches_scripture() {
        // Background RGB 0x0e 0x10 0x18 -> 14 16 24 decimal.
        assert_eq!(bg_seq(Rgb::new(0x0e, 0x10, 0x18)),
                   "\x1b[48;2;14;16;24m");
    }

    #[test]
    fn fg_wraps_with_reset() {
        let s = fg(Role::Glyph, "\u{22a2}");
        // The wrapping shape: ESC[...m<text>ESC[0m
        assert!(s.starts_with("\x1b[38;2;"));
        assert!(s.contains("\u{22a2}"));
        assert!(s.ends_with("\x1b[0m"));
    }

    #[test]
    fn reset_is_canonical() {
        assert_eq!(RESET, "\x1b[0m");
    }
}
