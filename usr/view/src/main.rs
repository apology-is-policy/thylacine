// view <path> -- the inline-media viewer (I-47, HALCYON.md 14.7). Sniff the
// file; a recognized image decodes HERE (the sacrificial process) and its
// raster is handed to halcyond over the per-pane channel (slice 3, not yet
// wired -- this slice reports the decode); anything else falls back to `cat`.

#![no_std]
#![no_main]

extern crate alloc;

use alloc::vec::Vec;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::eprintln;
use libthyla_rs::env;
use libthyla_rs::fs::File;
use libthyla_rs::io;
use libthyla_rs::process::Command;
use libthyla_rs::{t_close, t_open, t_write, T_OREAD, T_OWRITE, T_WALK_OPEN_FROM_ROOT};

use view::{decode_png, sniff, Kind, Raster};

// A generous cap on the file we will read into memory to decode (the channel's
// per-pane quota is the real bound, slice 3; this just refuses a pathological
// slurp). 64 MiB holds any real inline image's compressed bytes.
const READ_CAP: usize = 64 * 1024 * 1024;

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
        Kind::Png => match decode_png(&bytes) {
            Ok(r) => match place_on_halcyon(&r) {
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
            },
            Err(e) => {
                eprintln!("view: {}: {}", path, e);
                1
            }
        },
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
