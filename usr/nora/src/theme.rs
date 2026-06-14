// nora::theme -- the Bonfire palette for the editor (docs/UTOPIA-VISUAL.md U-2).
//
// nora carries its own copies of the Bonfire RGB values rather than depend on
// the whole shell library (it needs only these). The constant NAMES are
// nora-local roles; each maps to a canonical Bonfire role (noted inline). The
// mode-chip accents reuse Bonfire's syntax/diagnostic hues (moss/dusk/sand).
//
// (Pre-#124 these carried the retired U-1 "Pale Fire" cold values --
// `#0e1018`/`#d8e4f4`/`#8898b4`, the exact hexes UTOPIA-VISUAL.md section 8 flags
// as residue; corrected to Bonfire here alongside the palette-box styles.)

use kaua::style::{Attr, Color, Style};

/// Editor background -- Bonfire `bg` (warm near-black).
pub const BG: Color = Color::Rgb(0x0e, 0x0c, 0x0c);
/// Body text -- Bonfire `fg` (warm off-white).
pub const FG: Color = Color::Rgb(0xe4, 0xdd, 0xd8);
/// Line numbers / dim furniture -- Bonfire `fg_muted`.
pub const DIM: Color = Color::Rgb(0x9a, 0x8f, 0x8a);
/// Accent / Normal-mode chip / cursor -- Bonfire `ember`.
pub const EMBER: Color = Color::Rgb(0xe0, 0x78, 0x40);
/// Status-bar + popup background -- Bonfire `surface` (lifted warm dark).
pub const BAR: Color = Color::Rgb(0x18, 0x0f, 0x0e);
/// Popup / divider border -- Bonfire `border`.
pub const BORDER: Color = Color::Rgb(0x3a, 0x2a, 0x26);
/// Insert-mode chip -- Bonfire `moss` (green).
pub const GREEN: Color = Color::Rgb(0xb8, 0xd0, 0x98);
/// Visual-mode chip + selection -- Bonfire `dusk` (purple).
pub const VIOLET: Color = Color::Rgb(0xa8, 0x98, 0xc8);
/// Command-mode chip -- Bonfire `sand` (amber).
pub const GOLD: Color = Color::Rgb(0xc8, 0xa8, 0x82);

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

/// The `[Space]` command-palette popup surface (entry text on Bonfire surface).
pub fn palette_surface() -> Style {
    Style::new().fg(FG).bg(BAR)
}

/// The palette popup border.
pub fn palette_border() -> Style {
    Style::new().fg(BORDER).bg(BAR)
}

/// The palette popup title.
pub fn palette_title() -> Style {
    Style::new().fg(EMBER).bg(BAR).attr(Attr::BOLD)
}

/// The selected palette entry (a full-width highlight bar: dark text on ember).
pub fn palette_selected() -> Style {
    Style::new().fg(BG).bg(EMBER).attr(Attr::BOLD)
}
