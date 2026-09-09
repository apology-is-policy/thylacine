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

use view::{decode_png, sniff, Kind};

// A generous cap on the file we will read into memory to decode (the channel's
// per-pane quota is the real bound, slice 3; this just refuses a pathological
// slurp). 64 MiB holds any real inline image's compressed bytes.
const READ_CAP: usize = 64 * 1024 * 1024;

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
            Ok(r) => {
                // Slice 3 hands `r.argb` to halcyond over the per-pane channel.
                // Until then, report the decode -- the witness that zune runs
                // in-guest and produces a well-formed raster.
                libthyla_rs::println!(
                    "view: {} decoded PNG {}x{} ({} argb px)",
                    path,
                    r.w,
                    r.h,
                    r.argb.len()
                );
                0
            }
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
