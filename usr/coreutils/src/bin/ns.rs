// ns [pid] -- print a process's namespace (its territory mount list), the
// Plan 9 `ns` tool. Reads /proc/<pid>/ns, which the kernel renders as one
// "mount <mountpoint> <source>" line per mount entry plus a trailing
// "binds: <N>" count (devproc -> territory_format_ns; #66, invariant I-33).
//
// The mount-point column is the namespace name the directory was mounted onto
// (a Spoor.path, #66a); the source column is the mounted tree's name, or a
// Plan 9 device spec "#<dc>" (e.g. "#9"=9P, "#s"=srv, "#p"=proc) when the
// source is a device root with no namespace name.
//
// At v1.0 there is no getpid syscall, so `ns` with no operand shows kproc's
// namespace (pid 0 -- the system root namespace); `ns <pid>` shows a specific
// Proc's. (A /proc/self alias + a self-pid default land with the #66c
// /proc/<pid>/fd work.)

#![no_std]
#![no_main]

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

extern crate alloc;
use alloc::format;

use libthyla_rs::env::{self, Args};
use libthyla_rs::fs::File;
use libthyla_rs::{eprintln, io};

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

fn run(args: Args) -> i64 {
    let mut pid: i64 = 0;
    let mut seen = 0;
    for op in args.operands() {
        seen += 1;
        if seen > 1 {
            eprintln!("ns: usage: ns [pid]");
            return 1;
        }
        match core::str::from_utf8(op).ok().and_then(parse_pid) {
            Some(v) => pid = v,
            None => {
                eprintln!("ns: invalid pid operand");
                return 1;
            }
        }
    }

    let path = format!("/proc/{}/ns", pid);
    let mut out = io::stdout();
    match File::open(&path) {
        Ok(mut f) => match io::copy(&mut f, &mut out) {
            Ok(_) => 0,
            Err(e) => {
                eprintln!("ns: {}: {}", path, e);
                1
            }
        },
        Err(e) => {
            eprintln!("ns: {}: {}", path, e);
            1
        }
    }
}

// Parse a non-negative decimal pid. Rejects empty / non-digit / overflow.
fn parse_pid(s: &str) -> Option<i64> {
    if s.is_empty() {
        return None;
    }
    let mut v: i64 = 0;
    for b in s.bytes() {
        if !b.is_ascii_digit() {
            return None;
        }
        v = v.checked_mul(10)?.checked_add((b - b'0') as i64)?;
    }
    Some(v)
}
