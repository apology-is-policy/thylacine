// halcyond (lib) -- the brain (HALCYON.md section 13.1): everything that
// thinks, and nothing that syscalls. Pure logic over injected bytes, so
// every module here is host-testable; the bin half (main.rs, the `guest`
// feature) owns the Surface, the console, and the event loop.
//
// H-2c landed the rasterization floor: the vendored IBM Plex Sans faces (the
// DejaVu -> Public Sans -> IBM Plex Sans supersessions closed 2026-09-07) + a
// glyph source that fills a cartoon atlas on demand -- since HALCYON-TYPE
// TY-1 through the outline path (`outline`: skrifa + zeno, the smoothing
// stroke) rather than fontdue. The transcript model, layout, theme, and
// Beacon parsing arrived at H-2d.

#![no_std]

extern crate alloc;

pub mod chrome;
pub mod downq;
pub mod grid;
pub mod input;
pub mod layout;
pub mod menu;
pub mod outline;
pub mod raster;
pub mod select;
pub mod session_init;
pub mod status;
pub mod tile;
pub mod tiles;
pub mod transcript;
pub mod viewtest;

/// The vendored proportional faces (third_party/ibm-plex; HALCYON-VISUAL.md
/// section 7 + HALCYON.md section 4 -- IBM Plex Sans, operator-ratified
/// 2026-09-07, superseding Public Sans which had superseded DejaVu Sans
/// Condensed). The operator's weight rule: baseline body/chrome is Text (450);
/// bigger type (headings) is Regular (400), italic in full (section 8.1). So
/// TEXT is the baseline upright, TEXT_ITALIC carries em--emph (baseline-size
/// inline italic), BOLD is the one bold (em--strong), and HEADING_ITALIC is
/// the Regular-weight (400) italic headings use. Cornucopia (raster) owns
/// preformatted/terminal content; nothing here does.
pub const IBM_PLEX_SANS_TEXT: &[u8] =
    include_bytes!("../../../third_party/ibm-plex/ttf/IBMPlexSans-Text.ttf");
pub const IBM_PLEX_SANS_TEXT_ITALIC: &[u8] =
    include_bytes!("../../../third_party/ibm-plex/ttf/IBMPlexSans-TextItalic.ttf");
pub const IBM_PLEX_SANS_BOLD: &[u8] =
    include_bytes!("../../../third_party/ibm-plex/ttf/IBMPlexSans-Bold.ttf");
pub const IBM_PLEX_SANS_HEADING_ITALIC: &[u8] =
    include_bytes!("../../../third_party/ibm-plex/ttf/IBMPlexSans-Italic.ttf");
