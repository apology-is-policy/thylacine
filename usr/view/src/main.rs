// view [--check | --embed] <file | -> -- the inline-media viewer (I-47,
// HALCYON.md 14.7). Sniff the file; a recognized image decodes HERE (the
// sacrificial process), is reduced to the pane's current per-image limit, and
// its raster is handed to halcyond over the pane's channel. The interactive
// form passes anything else to `cat`. `--check` (decode, show nothing) and
// `--embed` (place, print only the reference) are for programs, and never do
// (HALCYON.md 14.7, the 2026-09-29 refinement).

#![no_std]
#![no_main]

extern crate alloc;

use alloc::format;
use alloc::string::String;
use alloc::vec::Vec;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::eprintln;
use libthyla_rs::env;
use libthyla_rs::err::Error;
use libthyla_rs::fs::File;
use libthyla_rs::io;
use libthyla_rs::process::Command;
use libthyla_rs::{t_close, t_open, t_read, t_write, T_OREAD, T_OWRITE, T_WALK_OPEN_FROM_ROOT};

use view::{
    decode_jpeg, decode_png, fit, jpeg_dimensions, png_dimensions, sniff, within_pixel_budget, Kind, Raster, READ_CAP,
    VIEW_MAX_PIXELS,
};

const USAGE: &str = "usage: view [--check | --embed] <file | ->\n";

const NO_CHANNEL: &str = "this pane has no inline-image channel (pictures show in a Halcyon session pane)";

/// The longest side the channel's header admits; a raster is held to it even
/// when the pane's limit cannot be read.
const MAX_SIDE: u32 = if inlinewire::MAX_W < inlinewire::MAX_H { inlinewire::MAX_W } else { inlinewire::MAX_H };

/// How the picture is used.
#[derive(Clone, Copy, PartialEq, Eq)]
enum Mode {
    /// Place it in this pane, with its caption and a status line; pass anything
    /// that is not a picture to `cat`.
    Show,
    /// Decode it and show nothing: the answer is the exit status.
    Check,
    /// Place it and print only its reference, for the caller to write where the
    /// picture belongs.
    Embed,
}

/// An argument as it may be echoed to a terminal: control characters shown as `?`.
fn printable(s: &str) -> String {
    s.chars().map(|c| if c.is_control() { '?' } else { c }).collect()
}

fn parse_args() -> Result<(Mode, String), i64> {
    let mut mode = Mode::Show;
    let mut operands: Vec<String> = Vec::new();
    let mut options_done = false;
    for raw in env::args().operands() {
        let Ok(arg) = core::str::from_utf8(raw) else {
            eprintln!("view: an argument is not valid UTF-8");
            return Err(2);
        };
        if !options_done && arg.starts_with('-') && arg != "-" {
            let chosen = match arg {
                "--" => {
                    options_done = true;
                    continue;
                }
                "--check" => Mode::Check,
                "--embed" => Mode::Embed,
                _ => {
                    eprintln!("view: unknown option '{}'", printable(arg));
                    io::err(USAGE.as_bytes());
                    return Err(2);
                }
            };
            if mode != Mode::Show && mode != chosen {
                eprintln!("view: --check and --embed exclude each other");
                return Err(2);
            }
            mode = chosen;
            continue;
        }
        operands.push(String::from(arg));
    }
    if operands.len() != 1 {
        io::err(USAGE.as_bytes());
        return Err(2);
    }
    Ok((mode, operands.remove(0)))
}

/// Report a failure and return its exit status: `view: <file>: <reason>` for a
/// person, the reason alone for a program, which names the file itself.
fn fail(mode: Mode, path: &str, why: &str) -> i64 {
    if mode == Mode::Show {
        eprintln!("view: {}: {}", printable(path), why);
    } else {
        eprintln!("{}", why);
    }
    1
}

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

fn read_error(e: Error) -> String {
    match e {
        Error::NoMemory => String::from("larger than the 16 MiB view reads"),
        e => format!("cannot read: {}", e),
    }
}

/// The file's bytes, at most `READ_CAP`; `-` is standard input, so a caller can
/// hand view a file it has opened itself.
fn read_input(path: &str) -> Result<Vec<u8>, String> {
    if path == "-" {
        let mut stdin = io::stdin();
        return io::slurp_capped(&mut stdin, READ_CAP).map_err(read_error);
    }
    let mut f = File::open(path).map_err(|e| format!("cannot open: {}", e))?;
    io::slurp_capped(&mut f, READ_CAP).map_err(read_error)
}

/// Refuse an over-budget image from its headers BEFORE the heap-hungry decode
/// (the heap grows to hold whatever the decode asks, so the pixel budget is its
/// only bound); malformed headers are refused here too.
fn within_budget(dims: Result<(u32, u32), &'static str>) -> Result<(), String> {
    match dims {
        Ok((w, h)) if !within_pixel_budget(w, h, VIEW_MAX_PIXELS) => Err(format!(
            "image too large ({}x{}; over the {} Mpx budget)",
            w,
            h,
            VIEW_MAX_PIXELS >> 20
        )),
        Ok(_) => Ok(()),
        Err(e) => Err(String::from(e)),
    }
}

/// Decode HERE (the sacrificial process, the blast-radius amendment): a PNG or
/// JPEG becomes a raster, each checked against the budget from its headers first.
fn decode(bytes: &[u8], kind: Kind) -> Result<Raster, String> {
    match kind {
        Kind::Png => {
            within_budget(png_dimensions(bytes))?;
            decode_png(bytes).map_err(String::from)
        }
        Kind::Jpeg => {
            within_budget(jpeg_dimensions(bytes))?;
            decode_jpeg(bytes).map_err(String::from)
        }
        Kind::Other => Err(String::from("not a PNG or JPEG picture")),
    }
}

/// True when `/env/HALCYON_PLACE` names this pane's session channel (14.7.2).
fn session_channel() -> bool {
    env::var("HALCYON_PLACE").is_some_and(|s| !s.is_empty())
}

/// Connect to the renderer's place service (I-47, HALCYON.md 14.7): the pane's
/// SESSION address when `/env/HALCYON_PLACE` names one (14.7.2,
/// `/srv/halcyon-<user>/<hex>/place`), else the console's `/srv/halcyon`.
/// There is NO console fallback once a session address is present -- a session
/// has no global `/srv/halcyon`, and falling back would place into the wrong
/// channel; a stale/dead address just fails. A `/srv` posted service CONNECTS on
/// open, and the resolver WALKS intermediate components rather than opening
/// them, so a single deep open cannot cross the service to reach the token dir:
/// open the service root (connect), then name `place` relative to it. Returns
/// the connected root and that relative path.
fn connect() -> Result<(i64, String), &'static str> {
    if let Some(addr) = env::var("HALCYON_PLACE") {
        if !addr.is_empty() {
            let (root_path, sub_path) = match split_service_addr(&addr) {
                Some((r, s)) => (String::from(r), String::from(s)),
                None => return Err("halcyon: malformed HALCYON_PLACE"),
            };
            let root = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, root_path.as_ptr(), root_path.len(), T_OREAD) };
            if root < 0 {
                return Err("halcyon: pane service unreachable");
            }
            return Ok((root, sub_path));
        }
    }
    const SRV: &[u8] = b"/srv/halcyon";
    let root = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, SRV.as_ptr(), SRV.len(), T_OREAD) };
    if root < 0 {
        return Err("no /srv/halcyon (renderer not posting)");
    }
    Ok((root, String::from("place")))
}

/// Split `/srv/halcyon-<user>/<hex>/place` into the service root
/// (`/srv/halcyon-<user>`) and the in-service subpath (`<hex>/place`). `None` if
/// the address does not have at least two trailing components.
fn split_service_addr(addr: &str) -> Option<(&str, &str)> {
    let last = addr.rfind('/')?; // the '/' before "place"
    let prev = addr[..last].rfind('/')?; // the '/' before "<hex>"
    if prev == 0 {
        return None;
    }
    Some((&addr[..prev], &addr[prev + 1..]))
}

/// The pane's current per-image limit, read from `place` (the 2026-09-29
/// refinement). A handle of its own, so the upload's handle still starts at
/// offset 0, which the channel requires. `None` when the read fails or does not
/// parse: the raster is then held only to the header's side bound, and the
/// server judges its size.
fn read_limit(root: i64, place: &str) -> Option<u64> {
    let fd = unsafe { t_open(root, place.as_ptr(), place.len(), T_OREAD) };
    if fd < 0 {
        return None;
    }
    let mut buf = [0u8; inlinewire::LIMIT_TEXT_MAX + 1];
    let mut n = 0usize;
    while n < buf.len() {
        let got = unsafe { t_read(fd, buf[n..].as_mut_ptr(), buf.len() - n) };
        if got <= 0 {
            break;
        }
        n += got as usize;
    }
    let _ = unsafe { t_close(fd) };
    inlinewire::parse_limit(&buf[..n])
}

/// Hand a decoded raster to halcyond: connect, reduce the raster to the pane's
/// limit, then write the inlinewire header and the ARGB payload in bounded
/// chunks. `Ok` -- the id and the size placed -- only when the whole message was
/// accepted.
fn place_on_halcyon(r: Raster, mode: Mode) -> Result<(u128, u32, u32), &'static str> {
    let id = if session_channel() {
        // The reference lands in this pane's output only when stdout IS the
        // pane; `--embed` hands it to its caller instead, which composes it.
        if mode == Mode::Show {
            let tier = env::var("BEACON").and_then(|v| beacon::Tier::parse(&v)).unwrap_or(beacon::Tier::None);
            if beacon::effective_tier(tier, libthyla_rs::fd_devclass(1), beacon::BeaconMode::Auto) != beacon::Tier::Rich {
                return Err("stdout is not a rich Halcyon pane");
            }
        }
        let mut bytes = [0u8; 16];
        if unsafe { libthyla_rs::t_getrandom(bytes.as_mut_ptr(), bytes.len(), 0) } != 16 {
            return Err("cannot create image reference");
        }
        let id = u128::from_le_bytes(bytes);
        if id == 0 {
            return Err("cannot create image reference");
        }
        id
    } else if mode == Mode::Embed {
        // The console channel orders nothing: there is no reference to give.
        return Err(NO_CHANNEL);
    } else {
        0
    };
    let (root, place) = connect()?;
    let r = fit(r, read_limit(root, &place).unwrap_or(u64::MAX), MAX_SIDE);
    let fd = unsafe { t_open(root, place.as_ptr(), place.len(), T_OWRITE) };
    let _ = unsafe { t_close(root) };
    if fd < 0 {
        return Err("halcyon: pane channel unreachable");
    }
    let mut header = inlinewire::PlaceHeader::argb(r.w, r.h);
    header.id = id;
    let hdr = header.pack();
    // The payload is the ARGB u32s as their LE bytes -- aarch64 is little-endian,
    // so the in-memory bytes ARE the wire bytes the reader reconstructs with
    // from_le_bytes. SAFETY: argb is a live &[u32] (4-aligned, contiguous); the
    // reinterpreted &[u8] covers exactly its bytes and is read-only + dropped
    // before argb.
    let payload: &[u8] = unsafe { core::slice::from_raw_parts(r.argb.as_ptr() as *const u8, r.argb.len() * 4) };
    let ok = write_all(fd, &hdr) && write_all(fd, payload);
    let _ = unsafe { t_close(fd) };
    if ok {
        Ok((id, r.w, r.h))
    } else {
        Err("halcyon: the channel refused the picture")
    }
}

/// The reference that shows a placed picture: a standalone Beacon object naming
/// the raster by id, carrying readable text for anything that cannot resolve it.
fn caption(id: u128, w: u32, h: u32) -> Vec<u8> {
    let key = format!("{:032x}", id);
    let mut out = Vec::new();
    beacon::wire::open(&mut out, beacon::wire::Op::Obj, &[("type", "inline-image"), ("ref", &key)]);
    out.extend_from_slice(format!("image {}x{}", w, h).as_bytes());
    beacon::wire::close(&mut out, beacon::wire::Op::Obj);
    out.push(b'\n');
    out
}

/// The interactive form: place the picture, then report. Exit 0 only when it is
/// displayed; a decode that could not be shown says why and exits 1.
fn show(path: &str, r: Raster) -> i64 {
    let (w0, h0, px) = (r.w, r.h, r.argb.len());
    match place_on_halcyon(r, Mode::Show) {
        Ok((id, w, h)) => {
            // The object caption rides stdout, preserving its position across
            // live cells, soft wrapping and frozen scrollback.
            // Placed but unreferenced, the raster stays in the pane's cache
            // undrawn: not displayed, and said so.
            if id != 0 && !write_all(1, &caption(id, w, h)) {
                return fail(Mode::Show, path, "placed, but its reference could not be written, so it is not displayed");
            }
            if (w, h) == (w0, h0) {
                libthyla_rs::println!("view: {} placed inline ({}x{})", printable(path), w, h);
            } else {
                libthyla_rs::println!(
                    "view: {} placed inline ({}x{}, reduced from {}x{})",
                    printable(path),
                    w,
                    h,
                    w0,
                    h0
                );
            }
            0
        }
        Err(why) => {
            libthyla_rs::println!(
                "view: {} decoded {}x{} ({} argb px); not displayed ({})",
                printable(path),
                w0,
                h0,
                px,
                why
            );
            1
        }
    }
}

/// Named by its absolute path: a spawn resolves a relative name against the
/// working directory, never a search path, so a bare `cat` would run whatever
/// file of that name sits where `view` was started.
const CAT: &str = "/bin/cat";

/// Not a picture, in the interactive form: the operator's spec is "text would
/// fall back to cat". Standard input has already been read, so it is written
/// back out as it came; a file is handed to `cat`, which reads it itself, after
/// `--`, so a name that starts with '-' is not read as cat's options.
fn cat_fallback(path: &str, bytes: &[u8]) -> i64 {
    if path == "-" {
        return if write_all(1, bytes) { 0 } else { 1 };
    }
    match Command::new(CAT).arg("--").arg(path).spawn() {
        Ok(mut child) => match child.wait() {
            Ok(status) => status.code().unwrap_or(1) as i64,
            Err(e) => {
                eprintln!("view: cat {}: wait failed: {:?}", printable(path), e);
                1
            }
        },
        Err(e) => {
            eprintln!("view: cat {}: spawn failed: {:?}", printable(path), e);
            1
        }
    }
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    let (mode, path) = match parse_args() {
        Ok(a) => a,
        Err(status) => return status,
    };
    // Asked before the decode, which is the expensive part.
    if mode == Mode::Embed && !session_channel() {
        return fail(mode, &path, NO_CHANNEL);
    }
    let bytes = match read_input(&path) {
        Ok(b) => b,
        Err(why) => return fail(mode, &path, &why),
    };
    let kind = sniff(&bytes);
    if kind == Kind::Other && mode == Mode::Show {
        return cat_fallback(&path, &bytes);
    }
    let r = match decode(&bytes, kind) {
        Ok(r) => r,
        Err(why) => return fail(mode, &path, &why),
    };
    // The compressed input is done; free it before the (blocking) channel write
    // so only the raster is held.
    drop(bytes);
    match mode {
        Mode::Check => 0,
        Mode::Show => show(&path, r),
        Mode::Embed => match place_on_halcyon(r, mode) {
            Ok((id, w, h)) => {
                if write_all(1, &caption(id, w, h)) {
                    0
                } else {
                    fail(mode, &path, "cannot write the reference")
                }
            }
            Err(why) => fail(mode, &path, why),
        },
    }
}
