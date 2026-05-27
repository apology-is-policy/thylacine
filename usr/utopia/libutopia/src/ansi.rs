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

/// Count the visible-column width of `s`, treating ANSI CSI escapes as
/// zero-width. Used by the line editor (libutopia::line_editor::
/// LineEditor::render) for cursor positioning + by future prompt-emit
/// helpers for prompt-width accounting.
///
/// Algorithm: walk the bytes; when an `ESC [` is seen, consume the
/// CSI body (parameter bytes 0x30..=0x3f, intermediate bytes
/// 0x20..=0x2f) then the terminating byte (0x40..=0x7e). All bytes
/// inside the sequence count as 0 columns. Bytes outside the sequence
/// count as one column each per UTF-8 char (v1.0 approximation; real
/// grapheme-cluster + wcwidth support is v1.x). Other ANSI escapes
/// (OSC, DCS, ...) are NOT recognized at v1.0; they would over-count
/// width if a program emits them. Disciplined Utopia programs emit
/// only CSI 24-bit-color SGR + reset, which this handles.
pub fn visible_width(s: &str) -> usize {
    let bytes = s.as_bytes();
    let mut i = 0;
    let mut cols = 0usize;
    while i < bytes.len() {
        // CSI sequence: ESC [ ... final-byte
        if bytes[i] == 0x1b && i + 1 < bytes.len() && bytes[i + 1] == b'[' {
            i += 2;
            // Parameter bytes 0x30..=0x3f (digits, :, ;, <, =, >, ?)
            // + intermediate bytes 0x20..=0x2f (space, !, ", ...).
            while i < bytes.len() && (0x20..=0x3f).contains(&bytes[i]) {
                i += 1;
            }
            // Terminating byte 0x40..=0x7e.
            if i < bytes.len() {
                i += 1;
            }
            continue;
        }
        // Non-escape byte. ASCII -> 1 column; UTF-8 leading byte ->
        // 1 column (the v1.0 approximation; v1.x can add wcwidth).
        // Continuation bytes (0x80..=0xbf) DON'T count.
        if (bytes[i] & 0xc0) != 0x80 {
            cols += 1;
        }
        i += 1;
    }
    cols
}

#[cfg(test)]
mod width_tests {
    use super::*;

    #[test]
    fn plain_ascii_width() {
        assert_eq!(visible_width("hello"), 5);
        assert_eq!(visible_width(""), 0);
    }

    #[test]
    fn csi_escape_is_zero_width() {
        assert_eq!(visible_width("\x1b[38;2;255;255;255m"), 0);
        assert_eq!(visible_width("\x1b[0m"), 0);
    }

    #[test]
    fn wrapped_text_yields_inner_width() {
        let s = fg(Role::Glyph, "x");
        // fg wraps with CSI + 'x' + RESET; visible width is 1.
        assert_eq!(visible_width(&s), 1);
    }

    #[test]
    fn pale_fire_banner_visible_width() {
        // The U-3 banner shape: ESC[<rgb>m ⊢ ESC[0m
        // The turnstile is 1 visible column.
        let s = fg(Role::Glyph, "\u{22a2}");
        assert_eq!(visible_width(&s), 1);
    }

    #[test]
    fn utf8_multibyte_counts_one() {
        // "é" is 2 bytes UTF-8 but 1 column.
        assert_eq!(visible_width("é"), 1);
        assert_eq!(visible_width("héllo"), 5);
    }
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
