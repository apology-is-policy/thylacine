//! The Bonfire palette as truecolor SGR escapes (docs/UTOPIA-VISUAL.md U-2).
//!
//! RGB values match `nora::theme` so the editor, the shell, and the tools share
//! one identity. A truecolor terminal (the QEMU serial host TTY today; the
//! future Aurora framebuffer) renders `\x1b[38;2;R;G;Bm`; a degrade to 256/16
//! colors for a poorer terminal is a seam.

/// Reset all attributes (the close of every colored span).
pub const RESET: &str = "\x1b[0m";
/// Bold weight.
pub const BOLD: &str = "\x1b[1m";

/// Body text -- Bonfire `fg` (e4ddd8).
pub const FG: &str = "\x1b[38;2;228;221;216m";
/// Dim furniture: box rules, column headers, labels -- Bonfire `fg_muted` (9a8f8a).
pub const DIM: &str = "\x1b[38;2;154;143;138m";
/// Accent: titles, counts, the program name -- Bonfire `ember` (e07840).
pub const EMBER: &str = "\x1b[38;2;224;120;64m";
/// Directories -- Bonfire `slate` (8a9ac8).
pub const SLATE: &str = "\x1b[38;2;138;154;200m";
/// Executables (any `x` bit) -- Bonfire `moss` (b8d098).
pub const GREEN: &str = "\x1b[38;2;184;208;152m";
/// Grafts (live kernel namespaces) -- Bonfire `dusk` (a898c8).
pub const VIOLET: &str = "\x1b[38;2;168;152;200m";
/// Devices (char devices) -- Bonfire `sand` (c8a882).
pub const GOLD: &str = "\x1b[38;2;200;168;130m";

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn codes_are_well_formed_sgr() {
        // Every fg color is a CSI ...m sequence; RESET closes it.
        for c in [FG, DIM, EMBER, SLATE, GREEN, VIOLET, GOLD] {
            assert!(c.starts_with("\x1b[38;2;"));
            assert!(c.ends_with('m'));
        }
        assert_eq!(RESET, "\x1b[0m");
    }
}
