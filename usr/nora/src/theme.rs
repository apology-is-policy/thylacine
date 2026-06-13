// nora::theme -- the Bonfire palette for the editor (docs/UTOPIA-VISUAL.md).
//
// The four committed Bonfire roles (BG / FG / PATH / GLYPH) mirror
// libutopia::palette; nora carries its own copies rather than depend on the
// whole shell library (it needs only these RGB values). The three mode-chip
// accents are nora-local picks in the same warm-dark family.

use kaua::style::{Attr, Color, Style};

/// Editor background -- Bonfire BG (deep blue-black).
pub const BG: Color = Color::Rgb(0x0e, 0x10, 0x18);
/// Body text -- Bonfire FG (off-white).
pub const FG: Color = Color::Rgb(0xd8, 0xe4, 0xf4);
/// Line numbers / dim furniture -- Bonfire PATH (muted blue-gray).
pub const DIM: Color = Color::Rgb(0x88, 0x98, 0xb4);
/// Accent / Normal-mode chip -- Bonfire GLYPH (ember orange).
pub const EMBER: Color = Color::Rgb(0xe0, 0x78, 0x40);
/// Status-bar background (a lifted BG).
pub const BAR: Color = Color::Rgb(0x1a, 0x1e, 0x2a);
/// Insert-mode chip (green).
pub const GREEN: Color = Color::Rgb(0x7f, 0xb0, 0x69);
/// Visual-mode chip + selection (violet).
pub const VIOLET: Color = Color::Rgb(0xb0, 0x8f, 0xe0);
/// Command-mode chip (gold).
pub const GOLD: Color = Color::Rgb(0xe0, 0xc0, 0x40);

/// Body text over the editor background.
pub fn text() -> Style {
    Style::new().fg(FG).bg(BG)
}

/// A blank editor cell (the background fill).
pub fn blank() -> Style {
    Style::new().bg(BG)
}

/// Gutter line numbers.
pub fn gutter() -> Style {
    Style::new().fg(DIM).bg(BG)
}

/// The `~` past-end-of-buffer markers (vim style).
pub fn tilde() -> Style {
    Style::new().fg(DIM).bg(BG)
}

/// The status-bar fill.
pub fn statusbar() -> Style {
    Style::new().fg(FG).bg(BAR)
}

/// A transient status message on the bar.
pub fn status_msg() -> Style {
    Style::new().fg(EMBER).bg(BAR)
}

/// The command/search line (`:`/`/`), drawn over the editor background.
pub fn cmdline() -> Style {
    Style::new().fg(FG).bg(BG)
}

/// A visual-mode selected cell.
pub fn selection() -> Style {
    Style::new().fg(BG).bg(VIOLET)
}

/// A mode chip: dark text on the mode's accent colour, bold.
pub fn mode_chip(accent: Color) -> Style {
    Style::new().fg(BG).bg(accent).attr(Attr::BOLD)
}
