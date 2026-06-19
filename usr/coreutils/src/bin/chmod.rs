// chmod MODE FILE... -- change file permission bits.
//
// MODE is either octal (e.g. 755, masked to 0o777) or symbolic
// ([ugoa]*[-+=][rwx]*, comma-separated -- e.g. `u+x`, `go-w`, `a=rx`). A
// symbolic mode reads the file's current bits (fs::metadata) and applies the
// delta. Backed by fs::chmod (SYS_WSTAT via a T_OPATH handle); the kernel
// enforces owner-only. setuid/setgid/sticky are unsupported (the kernel rejects
// any bit outside 0o777).

#![no_std]
#![no_main]

extern crate alloc;
use alloc::vec::Vec;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::env::{self, Args};
use libthyla_rs::{eprintln, fs};

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

const USAGE: &str = "\
usage: chmod MODE FILE...
  Change each FILE's permission bits. MODE is octal (e.g. 644) or symbolic
  ([ugoa]*[-+=][rwx]*, comma-separated -- e.g. u+x, go-w, a=rx). Owner-only;
  setuid/setgid/sticky are unsupported.
  --help  show this help

Examples:
  chmod 755 script    # rwxr-xr-x
  chmod +x script     # add execute for all
  chmod go-w file     # drop group + other write
";

fn run(args: Args) -> i64 {
    if let Some(rc) = coreutils::usage::help_if_requested(args, USAGE) {
        return rc;
    }

    let mut ops: Vec<&str> = Vec::new();
    for op in args.operands() {
        match core::str::from_utf8(op) {
            Ok(s) => ops.push(s),
            Err(_) => {
                eprintln!("chmod: invalid UTF-8 argument");
                return 1;
            }
        }
    }
    if ops.len() < 2 {
        eprintln!("usage: chmod MODE FILE...");
        return 1;
    }
    let spec = ops[0];
    let numeric = parse_octal(spec);
    if numeric.is_none() && !is_symbolic(spec) {
        eprintln!("chmod: invalid mode: '{}'", spec);
        return 1;
    }
    let mut status = 0;
    for path in &ops[1..] {
        let new_mode = match numeric {
            Some(m) => m,
            None => match fs::metadata(path) {
                Ok(md) => match apply_symbolic(md.mode() & 0o777, spec) {
                    Some(m) => m,
                    None => {
                        eprintln!("chmod: invalid mode: '{}'", spec);
                        return 1;
                    }
                },
                Err(e) => {
                    eprintln!("chmod: {}: {}", path, e);
                    status = 1;
                    continue;
                }
            },
        };
        if let Err(e) = fs::chmod(path, new_mode) {
            eprintln!("chmod: {}: {}", path, e);
            status = 1;
        }
    }
    status
}

/// An octal mode string (digits 0-7 only) -> `Some(bits & 0o777)`, else `None`
/// (it is then treated as a symbolic mode).
fn parse_octal(s: &str) -> Option<u32> {
    if s.is_empty() || !s.bytes().all(|b| (b'0'..=b'7').contains(&b)) {
        return None;
    }
    let mut m = 0u32;
    for b in s.bytes() {
        m = m * 8 + (b - b'0') as u32;
    }
    Some(m & 0o777)
}

/// A quick well-formedness check so a bad mode fails before touching any file.
fn is_symbolic(spec: &str) -> bool {
    spec.split(',').all(|c| apply_clause(0, c).is_some())
}

/// Apply comma-separated symbolic clauses to `mode` (the current 0o777 bits).
fn apply_symbolic(mut mode: u32, spec: &str) -> Option<u32> {
    for clause in spec.split(',') {
        mode = apply_clause(mode, clause)?;
    }
    Some(mode)
}

/// Apply one `[ugoa]*[-+=][rwx]*` clause to `mode`.
fn apply_clause(mode: u32, clause: &str) -> Option<u32> {
    let b = clause.as_bytes();
    let mut i = 0;
    let mut who = 0u32; // affected triples: 0o700 user / 0o070 group / 0o007 other
    let mut who_set = false;
    while i < b.len() {
        match b[i] {
            b'u' => who |= 0o700,
            b'g' => who |= 0o070,
            b'o' => who |= 0o007,
            b'a' => who |= 0o777,
            b'+' | b'-' | b'=' => break,
            _ => return None,
        }
        who_set = true;
        i += 1;
    }
    if i >= b.len() {
        return None; // no operator
    }
    let op = b[i];
    i += 1;
    if !who_set {
        who = 0o777; // no who -> all (POSIX default)
    }
    let mut perm = 0u32; // r=4 w=2 x=1 within one triple
    while i < b.len() {
        match b[i] {
            b'r' => perm |= 0o4,
            b'w' => perm |= 0o2,
            b'x' => perm |= 0o1,
            _ => return None,
        }
        i += 1;
    }
    // Expand the per-triple perm pattern across the affected triples.
    let mut bits = 0u32;
    if who & 0o700 != 0 {
        bits |= perm << 6;
    }
    if who & 0o070 != 0 {
        bits |= perm << 3;
    }
    if who & 0o007 != 0 {
        bits |= perm;
    }
    Some(match op {
        b'+' => mode | bits,
        b'-' => mode & !bits,
        b'=' => (mode & !who) | bits, // clear the affected triples, then set
        _ => return None,
    })
}
