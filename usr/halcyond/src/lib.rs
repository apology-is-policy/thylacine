// halcyond (lib) -- the brain (HALCYON.md section 13.1): everything that
// thinks, and nothing that syscalls. Pure logic over injected bytes, so
// every module here is host-testable; the bin half (main.rs, the `guest`
// feature) owns the Surface, the console, and the event loop.
//
// H-2c lands the rasterization floor: the vendored DejaVu Sans Condensed
// faces + the fontdue wrapper that fills a cartoon atlas on demand. The
// transcript model, layout, theme, and Beacon parsing arrive at H-2d.

#![no_std]

extern crate alloc;

pub mod chrome;
pub mod input;
pub mod layout;
pub mod menu;
pub mod raster;
pub mod select;
pub mod transcript;

/// The vendored proportional faces (third_party/dejavu-fonts; HALCYON.md
/// section 3 -- DejaVu Sans Condensed, operator-chosen). Oblique +
/// BoldOblique are vendored beside these; they get included the day the
/// stylesheet takes an italic role.
pub const DEJAVU_SANS_CONDENSED: &[u8] =
    include_bytes!("../../../third_party/dejavu-fonts/ttf/DejaVuSansCondensed.ttf");
pub const DEJAVU_SANS_CONDENSED_BOLD: &[u8] =
    include_bytes!("../../../third_party/dejavu-fonts/ttf/DejaVuSansCondensed-Bold.ttf");
