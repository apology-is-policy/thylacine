// gallery <image> -- the fullscreen inline-media viewer (I-47, the `view
// --fullscreen` variant, HALCYON.md 14.7). Decodes a recognized image HERE (the
// sacrificial process, like `view` -- hostile bytes never parse in the
// compositor) and blits it letterboxed to a fullscreen tapestryd surface (the
// DOSBox/Quake client pattern; rides I-40/I-45, no new compositor code). Esc or
// q exits. Unlike `view`, a non-image is an error -- a fullscreen `cat` is
// meaningless, so there is no fallback.

#![no_std]
#![no_main]

extern crate alloc;

use alloc::vec::Vec;

// An image decoder's working set far exceeds the default 4 MiB heap, and the
// worst case is a PROGRESSIVE JPEG: zune holds a full-image coefficient buffer
// per input component (~2 B * components * npx, up to 4 for CMYK, zune
// mcu_prog.rs) ALONGSIDE the output during decode -- peak ~= READ_CAP + 12*npx,
// vs a baseline/PNG ~8*npx. To view a ~12 Mpx photo (12*12M + 16 MiB = 160 MiB)
// the heap is 192 MiB (lazy demand-zero overcommit -- only touched pages commit,
// and 160 MiB is well under the 256 MiB per-AddrSpace page budget, I-32).
// GALLERY_MAX_PIXELS rejects anything larger up front, so the bound is REAL, not
// a phantom the allocator OOMs past (the former 128 MiB OOM-exited a 12 Mpx
// progressive JPEG -- holotype F1).
#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAllocN<{ 192 * 1024 * 1024 }> =
    libthyla_rs::alloc::ThylaAllocN;

use libthyla_rs::env;
use libthyla_rs::eprintln;
use libthyla_rs::fs::File;
use libthyla_rs::io;
use libthyla_rs::time::{sleep, Duration};

use tapestry::{FrameIntent, Surface, TapError, TEV_CLOSE, TEV_CONFIGURE, TEV_KEY};
use view::{decode_jpeg, decode_png, jpeg_dimensions, png_dimensions, sniff, within_pixel_budget, Kind};

macro_rules! say {
    ($($a:tt)*) => {{
        let mut s = alloc::format!($($a)*);
        s.push('\n');
        let _ = libthyla_rs::t_putstr(&s);
    }};
}

// The compressed-input cap. It is held ACROSS the decode (zune borrows the input
// slice), so it is part of the peak working set -- counted in the budget below.
// 16 MiB holds any real image's compressed bytes with room to spare.
const READ_CAP: usize = 16 * 1024 * 1024;

// The decode pixel budget, sized to the 192 MiB heap for the worst-case
// progressive-JPEG peak (~= READ_CAP + 12*npx; see the allocator note): 12*12M +
// 16 MiB = 160 MiB, inside the heap with margin. Rejected BEFORE decode via a
// headers-only dimension read, so an over-budget image gets a clean error instead
// of a silent OOM-exit.
const GALLERY_MAX_PIXELS: u64 = 12 * 1024 * 1024;

// tapestryd is warden-spawned well before this, but a slow bring-up must not
// flake a manual/menu run.
const CONNECT_TRIES: u32 = 25;
const CONNECT_DELAY_MS: u64 = 200;

/// Reject an over-budget image from its headers BEFORE the heap-hungry decode
/// (the decode peak can dwarf a fixed heap; a pixel cap above the heap is a
/// phantom the allocator OOMs past). `Err(code)` bails with that exit code; a
/// failed dimension read (malformed headers) also bails clean.
fn check_budget(path: &str, dims: Result<(u32, u32), &'static str>, max: u64) -> Result<(), i64> {
    match dims {
        Ok((w, h)) if !within_pixel_budget(w, h, max) => {
            eprintln!(
                "gallery: {}: image too large ({}x{}; over the {} Mpx budget)",
                path,
                w,
                h,
                max >> 20
            );
            Err(1)
        }
        Ok(_) => Ok(()),
        Err(e) => {
            eprintln!("gallery: {}: {}", path, e);
            Err(1)
        }
    }
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    let path = match env::args().operands().next() {
        Some(p) => match core::str::from_utf8(p) {
            Ok(s) => s,
            Err(_) => {
                eprintln!("gallery: non-UTF-8 path");
                return 2;
            }
        },
        None => {
            eprintln!("usage: gallery <image>");
            return 2;
        }
    };

    let mut f = match File::open(path) {
        Ok(f) => f,
        Err(e) => {
            eprintln!("gallery: {}: {:?}", path, e);
            return 1;
        }
    };
    let bytes: Vec<u8> = match io::slurp_capped(&mut f, READ_CAP) {
        Ok(b) => b,
        Err(e) => {
            eprintln!("gallery: {}: read failed: {:?}", path, e);
            return 1;
        }
    };

    // Decode HERE (the blast-radius amendment). Unlike view, a non-image is an
    // error -- there is no fullscreen fallback. Both image arms reject an
    // over-budget image from its headers before the heap-hungry decode.
    let raster = match sniff(&bytes) {
        Kind::Png => {
            if let Err(c) = check_budget(path, png_dimensions(&bytes), GALLERY_MAX_PIXELS) {
                return c;
            }
            match decode_png(&bytes) {
                Ok(r) => r,
                Err(e) => {
                    eprintln!("gallery: {}: {}", path, e);
                    return 1;
                }
            }
        }
        Kind::Jpeg => {
            if let Err(c) = check_budget(path, jpeg_dimensions(&bytes), GALLERY_MAX_PIXELS) {
                return c;
            }
            match decode_jpeg(&bytes) {
                Ok(r) => r,
                Err(e) => {
                    eprintln!("gallery: {}: {}", path, e);
                    return 1;
                }
            }
        }
        Kind::Other => {
            eprintln!("gallery: {}: not a recognized image", path);
            return 1;
        }
    };
    // The compressed input is done; free it before the event loop so only the
    // raster occupies the heap for the viewer's lifetime (F2).
    drop(bytes);

    // Connect a fullscreen tapestryd surface (bounded retry, the tapestry-demo
    // pattern). The loop YIELDS the Surface directly, so there is no post-loop
    // `unwrap` whose soundness would silently depend on CONNECT_TRIES being
    // non-zero (F3).
    let mut surf = 'connect: {
        for i in 0..CONNECT_TRIES {
            match Surface::fullscreen() {
                Ok(s) => break 'connect s,
                Err(e) => {
                    if i + 1 == CONNECT_TRIES {
                        eprintln!("gallery: no compositor: {:?}", e);
                        return 1;
                    }
                    let _ = sleep(Duration::from_millis(CONNECT_DELAY_MS));
                }
            }
        }
        // Reachable only if CONNECT_TRIES were 0 (the loop never runs); a clean
        // exit rather than a panic.
        eprintln!("gallery: no compositor (no connect attempts)");
        return 1;
    };
    // A still image: declare Static so the compositor does not pace us a frame
    // clock (we present once, and again only on a resize).
    let _ = surf.intent(FrameIntent::Static);

    let (mut dw, mut dh) = (surf.w, surf.h);
    let fit = gallery::paint(surf.pixels(), dw, dh, &raster.argb, raster.w, raster.h);
    if let Err(e) = surf.present(None) {
        eprintln!("gallery: present failed: {:?}", e);
        return 1;
    }
    say!(
        "gallery: {} {}x{} shown {}x{} at {},{} on {}x{}",
        path,
        raster.w,
        raster.h,
        fit.fw,
        fit.fh,
        fit.ox,
        fit.oy,
        dw,
        dh
    );

    loop {
        let ev = match surf.wait_event() {
            Ok(ev) => ev,
            Err(e) => {
                eprintln!("gallery: event stream ended: {:?}", e);
                return 1;
            }
        };
        match ev.kind {
            TEV_KEY => {
                if gallery::is_exit_key(ev.code, ev.value, ev.rune) {
                    return 0;
                }
            }
            TEV_CLOSE => return 0,
            // A CONFIGURE is either a resize (handle_configure reweaves + returns
            // true) or a same-size full-REDRAW request (returns false after
            // invalidating the slots). Either Ok obliges a full repaint +
            // present (libtapestry's contract). Every present paints the FULL
            // buffer, so the multi-slot rotation caveat (GPU-DESIGN 4.5.8b) never
            // applies -- no partial rect relies on stale slot content. Busy
            // means the offer went stale mid-ack: drop it, a newer CONFIGURE
            // carries the live geometry.
            TEV_CONFIGURE => match surf.handle_configure(&ev) {
                Ok(changed) => {
                    if changed {
                        dw = surf.w;
                        dh = surf.h;
                    }
                    let _ = gallery::paint(surf.pixels(), dw, dh, &raster.argb, raster.w, raster.h);
                    if let Err(e) = surf.present(None) {
                        eprintln!("gallery: re-present failed: {:?}", e);
                        return 1;
                    }
                }
                Err(TapError::Busy) => {}
                Err(e) => {
                    eprintln!("gallery: configure failed: {:?}", e);
                    return 1;
                }
            },
            _ => {}
        }
    }
}
