// weft-bench -- the zero-copy dataplane yardstick (STAGED until Weft-6c).
//
// Weft (docs/NET-THROUGHPUT.md) is the committed throughput NOVEL: a per-flow,
// capability-scoped, zero-copy shared-page path to a confined Proc's /net flows
// -- bytes flow through a shared Burrow ring with NO per-op mediation and NO
// per-byte copy. This bench is its yardstick: how much faster is pushing a
// registered page than copying the payload through the byte ring?
//
// The native Weft API (push/pop/wait over a registered flow ring) lands at
// Weft-6c; it is not in the tree yet. So this is the aux Phase-B pattern -- a
// compiling skeleton + a baseline + a test plan, ready to fill in the moment the
// API lands:
//
//   * BASELINE (now): the memcpy "copy-tax" -- the userspace per-byte copy
//     bandwidth, the ceiling on how fast ANY copy-based dataplane can move bytes
//     and the cost Weft's zero-copy path removes. A real, motivating number.
//   * ZERO-COPY (Weft-6c): register a flow ring (an anon Burrow), push payload
//     descriptors (no copy), wait on the readiness ring; report ops/s and the
//     speedup over the copy-tax ceiling. Stubbed with a clear note below.
//
// Pure userspace; touches no hardware and (at the baseline) no network -- it
// measures the copy cost the network path pays.

#![no_std]
#![no_main]

extern crate alloc;
use alloc::{format, vec};

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use coreutils::color::{self, ColorMode};
use coreutils::{palette, ui};
use libthyla_rs::env::{self, Args};
use libthyla_rs::println;
use libthyla_rs::time::Instant;

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

const USAGE: &str = "\
usage: weft-bench [-n MB] [--color[=WHEN]]
  The zero-copy dataplane yardstick (STAGED until the native Weft API, Weft-6c).
  Reports the memcpy copy-tax baseline -- the per-byte copy cost the byte-copy
  ring pays, the ceiling Weft's zero-copy path removes.
  -n MB           megabytes to copy in the baseline (default 256)
  --color[=WHEN]  colorize the report: always (default) | never | auto
  --help          show this help

Examples:
  weft-bench                  # the copy-tax baseline over 256 MB
  weft-bench -n 1024          # over 1 GiB
";

const CHUNK: usize = 64 * 1024;
const DEFAULT_MB: u64 = 256;

fn run(args: Args) -> i64 {
    if let Some(rc) = coreutils::usage::help_if_requested(args, USAGE) {
        return rc;
    }

    let mut mode = ColorMode::Always;
    let mut mb = DEFAULT_MB;
    let mut i = 1;
    while let Some(a) = args.get_str(i) {
        i += 1;
        if a == "--color" {
            mode = ColorMode::Always;
            continue;
        }
        if let Some(w) = a.strip_prefix("--color=") {
            match ColorMode::parse_when(w) {
                Some(m) => mode = m,
                None => return coreutils::usage::die("weft-bench", &format!("invalid --color value -- '{}'", w)),
            }
            continue;
        }
        if a == "-n" {
            match args.get_str(i).and_then(|s| s.parse::<u64>().ok()) {
                Some(n) if n > 0 => {
                    mb = n;
                    i += 1;
                }
                _ => return coreutils::usage::die("weft-bench", "-n needs a positive megabyte count"),
            }
            continue;
        }
        return coreutils::usage::die("weft-bench", &format!("unexpected argument '{}'", a));
    }

    let on = mode.resolve(stdout_is_console);
    baseline_copy_tax(mb, on);
    staged_zero_copy_note(on);
    0
}

/// Measure the userspace memcpy bandwidth -- the per-byte copy ceiling Weft's
/// zero-copy path removes. `black_box` keeps the optimizer from eliding the copy.
fn baseline_copy_tax(mb: u64, on: bool) {
    let src = vec![0x5au8; CHUNK];
    let mut dst = vec![0u8; CHUNK];
    let target = mb * 1024 * 1024;
    let mut done = 0u64;

    let t0 = Instant::now();
    while done < target {
        let n = core::cmp::min(CHUNK as u64, target - done) as usize;
        dst[..n].copy_from_slice(&src[..n]);
        core::hint::black_box(&dst[..n]);
        done += n as u64;
    }
    let dt = t0.elapsed();
    ui::rate_card("weft-bench", "copy-tax (memcpy)", done, dt, on);
}

/// The staged zero-copy section -- printed until the native Weft API lands.
fn staged_zero_copy_note(on: bool) {
    let dim = color::col(palette::DIM, on);
    let rst = color::reset(on);
    let vio = color::col(palette::VIOLET, on);

    println!(
        "{}weft-bench:{} {}zero-copy path: STAGED{} (awaiting the native Weft API, Weft-6c)",
        dim, rst, vio, rst
    );
    println!(
        "{}  when it lands:{} register a flow ring (anon Burrow), push payload",
        dim, rst
    );
    println!(
        "{}              {} descriptors (no per-byte copy), wait on the readiness",
        dim, rst
    );
    println!(
        "{}              {} ring; report ops/s + the speedup vs the copy-tax above.",
        dim, rst
    );
}

/// `--color=auto` stub; true until a kernel TTY check lands.
fn stdout_is_console() -> bool {
    true
}
