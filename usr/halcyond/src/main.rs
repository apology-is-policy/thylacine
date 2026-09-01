// halcyond (bin) -- the body: syscalls, the tapestry Surface, the event
// loop. H-2c is the skeleton (builds for the bare target, links the lib
// + the vendored fontdue, exits clean); the transcript pane arrives at
// H-2d and joey's renderer choice decides who owns the console.

#![no_std]
#![no_main]

extern crate alloc;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::t_putstr;

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    // Prove the whole floor links and runs on-device: parse the vendored
    // faces + rasterize one glyph through the lib. Cheap (one 'H' at
    // 16px), and it turns a silent link/FP regression into a loud line.
    let mut gs = halcyond::raster::GlyphSource::new_vendored(128);
    let ok = gs.face_count() == 2
        && gs.glyph(halcyond::raster::FACE_BODY, 16.0, 'H').is_some();
    if ok {
        t_putstr("halcyond: skeleton OK (faces + raster live; the transcript arrives at H-2d)\n");
        0
    } else {
        t_putstr("halcyond: raster floor FAILED\n");
        1
    }
}
