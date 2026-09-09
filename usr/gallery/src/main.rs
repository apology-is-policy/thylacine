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

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::env;
use libthyla_rs::eprintln;
use libthyla_rs::fs::File;
use libthyla_rs::io;
use libthyla_rs::time::{sleep, Duration};

use tapestry::{FrameIntent, Surface, TapError, TEV_CLOSE, TEV_CONFIGURE, TEV_KEY};
use view::{decode_png, sniff, Kind};

macro_rules! say {
    ($($a:tt)*) => {{
        let mut s = alloc::format!($($a)*);
        s.push('\n');
        let _ = libthyla_rs::t_putstr(&s);
    }};
}

// A generous cap on the file slurped to decode (the decoder's own MAX_PIXELS is
// the pixel bound; this refuses a pathological compressed slurp). 64 MiB holds
// any real image's compressed bytes.
const READ_CAP: usize = 64 * 1024 * 1024;

// tapestryd is warden-spawned well before this, but a slow bring-up must not
// flake a manual/menu run.
const CONNECT_TRIES: u32 = 25;
const CONNECT_DELAY_MS: u64 = 200;

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
    // error -- there is no fullscreen fallback.
    let raster = match sniff(&bytes) {
        Kind::Png => match decode_png(&bytes) {
            Ok(r) => r,
            Err(e) => {
                eprintln!("gallery: {}: {}", path, e);
                return 1;
            }
        },
        Kind::Jpeg => {
            eprintln!("gallery: {}: JPEG decode lands in a later slice", path);
            return 1;
        }
        Kind::Other => {
            eprintln!("gallery: {}: not a recognized image", path);
            return 1;
        }
    };

    // Connect a fullscreen tapestryd surface (bounded retry, the tapestry-demo
    // pattern).
    let mut surf: Option<Surface> = None;
    for i in 0..CONNECT_TRIES {
        match Surface::fullscreen() {
            Ok(s) => {
                surf = Some(s);
                break;
            }
            Err(e) => {
                if i == CONNECT_TRIES - 1 {
                    eprintln!("gallery: no compositor: {:?}", e);
                    return 1;
                }
                let _ = sleep(Duration::from_millis(CONNECT_DELAY_MS));
            }
        }
    }
    let mut surf = surf.unwrap();
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
