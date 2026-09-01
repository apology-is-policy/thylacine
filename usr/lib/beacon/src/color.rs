//! The color gate. `ColorMode` parses `--color[=WHEN]`; `col` / `reset` emit an
//! SGR code or nothing, so the SAME formatting code stays byte-clean when off.
//!
//! Pattern: `write!(w, "{}{}{}", col(SLATE, on), name, reset(on))` colors when
//! `on` and is byte-identical to `name` when off. The `on` flag is resolved once
//! per run from `--color=WHEN` (the bin's job; `auto` needs a syscall probe).

use crate::palette;

/// When to colorize, from `--color=WHEN`.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum ColorMode {
    /// Always emit color.
    Always,
    /// Never emit color (the clean escape hatch for pipes / scripts).
    Never,
    /// Color iff stdout is a terminal (the bin supplies the probe).
    Auto,
}

impl ColorMode {
    /// Parse a `--color=WHEN` value. A bare `--color` (empty `WHEN`) is
    /// `Always`, matching GNU. `None` for an unrecognized value.
    pub fn parse_when(s: &str) -> Option<ColorMode> {
        match s {
            "" | "always" | "yes" | "force" => Some(ColorMode::Always),
            "never" | "no" | "none" => Some(ColorMode::Never),
            "auto" | "tty" | "if-tty" => Some(ColorMode::Auto),
            _ => None,
        }
    }

    /// Resolve to a concrete on/off, given the bin's TTY probe for `Auto`.
    #[inline]
    pub fn resolve(self, is_tty: impl FnOnce() -> bool) -> bool {
        match self {
            ColorMode::Always => true,
            ColorMode::Never => false,
            ColorMode::Auto => is_tty(),
        }
    }
}

/// The SGR `code` if `on`, else `""`.
#[inline]
pub fn col(code: &'static str, on: bool) -> &'static str {
    if on {
        code
    } else {
        ""
    }
}

/// `palette::RESET` if `on`, else `""`.
#[inline]
pub fn reset(on: bool) -> &'static str {
    if on {
        palette::RESET
    } else {
        ""
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_when_with_synonyms() {
        assert_eq!(ColorMode::parse_when("always"), Some(ColorMode::Always));
        assert_eq!(ColorMode::parse_when(""), Some(ColorMode::Always)); // bare --color
        assert_eq!(ColorMode::parse_when("never"), Some(ColorMode::Never));
        assert_eq!(ColorMode::parse_when("auto"), Some(ColorMode::Auto));
        assert_eq!(ColorMode::parse_when("bogus"), None);
    }

    #[test]
    fn resolve_honors_mode_and_probe() {
        assert!(ColorMode::Always.resolve(|| false));
        assert!(!ColorMode::Never.resolve(|| true));
        assert!(ColorMode::Auto.resolve(|| true));
        assert!(!ColorMode::Auto.resolve(|| false));
    }

    #[test]
    fn col_and_reset_gate_on_the_flag() {
        assert_eq!(col(palette::SLATE, true), palette::SLATE);
        assert_eq!(col(palette::SLATE, false), "");
        assert_eq!(reset(true), palette::RESET);
        assert_eq!(reset(false), "");
    }
}
