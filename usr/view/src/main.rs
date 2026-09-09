// view <path> -- the inline-media viewer (I-47, HALCYON.md 14.7). Sniff the
// file; a recognized image decodes HERE (the sacrificial process) and its
// raster is handed to halcyond over the per-pane channel (slice 3, not yet
// wired -- this slice reports the decode); anything else falls back to `cat`.

#![no_std]
#![no_main]

extern crate alloc;

use alloc::vec::Vec;

// Decoding an image needs far more than the default 4 MiB heap (peak ~= 8*npx +
// the compressed input, all live during decode); size it for a ~6 Mpx inline
// image, with VIEW_MAX_PIXELS the REAL bound (a bare MAX_PIXELS that exceeds the
// heap is a phantom the allocator OOMs past).
#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAllocN<{ 64 * 1024 * 1024 }> =
    libthyla_rs::alloc::ThylaAllocN;

use libthyla_rs::eprintln;
use libthyla_rs::env;
use libthyla_rs::fs::File;
use libthyla_rs::io;
use libthyla_rs::process::Command;
use libthyla_rs::{t_close, t_open, t_write, T_OREAD, T_OWRITE, T_WALK_OPEN_FROM_ROOT};

use view::{decode_png, png_dimensions, sniff, within_pixel_budget, Kind, Raster};

// The compressed-input cap, coherent with the 64 MiB heap (a 6 Mpx image's
// decode peak is ~48 MiB, so the input must stay well under the remainder).
const READ_CAP: usize = 16 * 1024 * 1024;

// view's own decode pixel budget, sized to the heap (peak ~= 8*npx + input),
// checked from the headers BEFORE decode so an over-budget image is a clean
// report rather than a silent OOM-exit. halcyond re-caps the CHANNEL downstream
// (display-adaptive); this only bounds view's local decode.
const VIEW_MAX_PIXELS: u64 = 6 * 1024 * 1024;

/// Write the whole buffer to `fd` in bounded chunks, looping on the returned
/// count (a 9P-backed fid caps a Twrite at the negotiated msize). False on any
/// write error or a zero-progress return.
fn write_all(fd: i64, buf: &[u8]) -> bool {
    const CHUNK: usize = 60 * 1024; // conservatively under the 9P msize
    let mut off = 0usize;
    while off < buf.len() {
        let end = (off + CHUNK).min(buf.len());
        let n = unsafe { t_write(fd, buf[off..end].as_ptr(), end - off) };
        if n <= 0 {
            return false;
        }
        off += n as usize;
    }
    true
}

/// Hand the decoded raster to halcyond over the per-pane channel (I-47,
/// HALCYON.md 14.7): open /srv/halcyon (9p-mode -> a root), walk to `place`,
/// write the inlinewire header then the ARGB payload in bounded chunks. `Ok`
/// only when the whole message was accepted; `Err(reason)` lets the caller fall
/// back to a report -- notably when no renderer posted the service.
fn place_on_halcyon(r: &Raster) -> Result<(), &'static str> {
    const SRV: &[u8] = b"/srv/halcyon";
    const PLACE: &[u8] = b"place";
    let root = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, SRV.as_ptr(), SRV.len(), T_OREAD) };
    if root < 0 {
        return Err("no /srv/halcyon (renderer not posting)");
    }
    let place = unsafe { t_open(root, PLACE.as_ptr(), PLACE.len(), T_OWRITE) };
    let _ = unsafe { t_close(root) };
    if place < 0 {
        return Err("halcyon: no place file");
    }
    let hdr = inlinewire::PlaceHeader::argb(r.w, r.h).pack();
    // The payload is the ARGB u32s as their LE bytes -- aarch64 is little-endian,
    // so the in-memory bytes ARE the wire bytes the reader reconstructs with
    // from_le_bytes. SAFETY: argb is a live &[u32] (4-aligned, contiguous); the
    // reinterpreted &[u8] covers exactly its bytes and is read-only + dropped
    // before argb.
    let payload: &[u8] =
        unsafe { core::slice::from_raw_parts(r.argb.as_ptr() as *const u8, r.argb.len() * 4) };
    let ok = write_all(place, &hdr) && write_all(place, payload);
    let _ = unsafe { t_close(place) };
    if ok {
        Ok(())
    } else {
        Err("halcyon: short write on the channel")
    }
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    let path = match env::args().operands().next() {
        Some(p) => match core::str::from_utf8(p) {
            Ok(s) => s,
            Err(_) => {
                eprintln!("view: non-UTF-8 path");
                return 2;
            }
        },
        None => {
            eprintln!("usage: view <file>");
            return 2;
        }
    };

    let mut f = match File::open(path) {
        Ok(f) => f,
        Err(e) => {
            eprintln!("view: {}: {:?}", path, e);
            return 1;
        }
    };
    let bytes: Vec<u8> = match io::slurp_capped(&mut f, READ_CAP) {
        Ok(b) => b,
        Err(e) => {
            eprintln!("view: {}: read failed: {:?}", path, e);
            return 1;
        }
    };

    match sniff(&bytes) {
        Kind::Png => {
            // Reject an over-budget image up front (headers-only) so a huge PNG
            // is a clean report, not a silent OOM-exit mid-decode.
            match png_dimensions(&bytes) {
                Ok((w, h)) if !within_pixel_budget(w, h, VIEW_MAX_PIXELS) => {
                    eprintln!(
                        "view: {}: image too large ({}x{}; over the {} Mpx budget)",
                        path,
                        w,
                        h,
                        VIEW_MAX_PIXELS >> 20
                    );
                    return 1;
                }
                Ok(_) => {}
                Err(e) => {
                    eprintln!("view: {}: {}", path, e);
                    return 1;
                }
            }
            let r = match decode_png(&bytes) {
                Ok(r) => r,
                Err(e) => {
                    eprintln!("view: {}: {}", path, e);
                    return 1;
                }
            };
            // The compressed input is done; free it before the (blocking) channel
            // write so only the raster is held.
            drop(bytes);
            match place_on_halcyon(&r) {
                Ok(()) => {
                    libthyla_rs::println!("view: {} placed inline ({}x{})", path, r.w, r.h);
                    0
                }
                Err(why) => {
                    // No renderer channel (or a write error): report the decode
                    // so `view` is still useful standalone, and say why it did
                    // not display. Not an error exit -- the decode succeeded.
                    libthyla_rs::println!(
                        "view: {} decoded PNG {}x{} ({} argb px); not displayed ({})",
                        path,
                        r.w,
                        r.h,
                        r.argb.len(),
                        why
                    );
                    0
                }
            }
        }
        Kind::Jpeg => {
            eprintln!("view: {}: JPEG decode lands in a later slice", path);
            1
        }
        // Not a recognized image: fall back to cat (the operator's spec --
        // "text would fallback to cat"). cat reads the path itself.
        Kind::Other => match Command::new("cat").arg(path).spawn() {
            Ok(mut child) => match child.wait() {
                Ok(status) => status.code().unwrap_or(1) as i64,
                Err(e) => {
                    eprintln!("view: cat {}: wait failed: {:?}", path, e);
                    1
                }
            },
            Err(e) => {
                eprintln!("view: cat {}: spawn failed: {:?}", path, e);
                1
            }
        },
    }
}
