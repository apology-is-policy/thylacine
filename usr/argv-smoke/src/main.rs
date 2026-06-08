// /argv-smoke -- U-6e-pre-a milestone: the FIRST runtime exercise of the
// native-argv path (libthyla_rs::env::args, DOC-GAP G03) and the std-stream
// handles (libthyla_rs::io::{stdout}, DOC-GAP G05).
//
// joey spawns this binary via SYS_SPAWN_FULL_ARGV with
//   argv = ["argv-smoke", "alpha", "beta-2"]   (argc = 3)
// and a pipe wired as fd 0 + fd 1 (the pouch_smoke_one_argv harness). The
// binary reads its own argv through env::args(), echoes argc + every argv[i]
// to stdout (fd 1, which flows back to joey through the pipe), and exits 0
// iff the argv it received matches what joey passed. joey content-checks the
// per-position markers AND the exit status.
//
// This proves, end to end and at runtime:
//   * _start captured the kernel-populated Shape-B startup frame and
//     env::args() reads argc + each argv[i] pointer correctly (G03);
//   * io::stdout() writes reach the inherited fd 1 (G05).
//
// Until this ran, the native-argv capture in libthyla-rs's _start had only
// ever been disassembly-verified (the aux track's aux-rt note); this is its
// first execution.

#![no_std]
#![no_main]

extern crate alloc;

use libthyla_rs::alloc::ThylaAlloc;
use libthyla_rs::env;
use libthyla_rs::io::{self, Write};
use libthyla_rs::t_putstr;

#[global_allocator]
static GLOBAL_ALLOCATOR: ThylaAlloc = ThylaAlloc;

// What joey passes. Keep in sync with usr/joey/joey.c's argv-smoke block.
const EXPECT: [&str; 3] = ["argv-smoke", "alpha", "beta-2"];

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    let args = env::args();
    let mut out = io::stdout();

    // Echo argc + every argv[i] to fd 1 as per-position markers. `args` is
    // Copy, so iterating a copy below leaves the original usable.
    let _ = writeln!(out, "argv-smoke: argc={}", args.len());
    for (i, a) in args.enumerate() {
        // a: &[u8]. Render as UTF-8 (the test argv is ASCII); fall back to a
        // sentinel if a future caller passes non-UTF-8 bytes.
        match core::str::from_utf8(a) {
            Ok(s) => {
                let _ = writeln!(out, "argv-smoke: argv[{}]={}", i, s);
            }
            Err(_) => {
                let _ = writeln!(out, "argv-smoke: argv[{}]=<non-utf8>", i);
            }
        }
    }

    // Self-check (the second, status-based proof): the argv we received must
    // match EXACTLY what joey passed -- count and each value.
    if args.len() != EXPECT.len() {
        t_putstr("argv-smoke: argc MISMATCH\n");
        return 1;
    }
    for (i, want) in EXPECT.iter().enumerate() {
        if args.get_str(i) != Some(*want) {
            t_putstr("argv-smoke: argv MISMATCH\n");
            return 1;
        }
    }

    let _ = out.write_all(b"argv-smoke: native argv OK\n");
    0
}
