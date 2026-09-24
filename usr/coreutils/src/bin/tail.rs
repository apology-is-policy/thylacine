// tail [-n [+]N | -c [+]N | -N] [FILE...] -- print the last N lines (default
// 10), or with +N everything from line (or byte) N on.
//
// Reads each input a buffer at a time, holding only what the last N lines (or
// bytes) need (coreutils::stream::Tail), so an input of any length has a
// tail; +N holds nothing (coreutils::stream::Skip). A trailing newline does NOT
// count as an extra empty line. Multiple files get "==> name <==" banners. No
// operand / "-" reads stdin. Once stdout's reader is gone, tail stops,
// silently.

#![no_std]
#![no_main]

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use coreutils::stream::{self, Skip, Tail};
use libthyla_rs::env::{self, Args};
use libthyla_rs::err::Error;
use libthyla_rs::fs::File;
use libthyla_rs::io::{self, Write};
use libthyla_rs::eprintln;

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

const USAGE: &str = "\
usage: tail [-n [+]N | -c [+]N | -N] [FILE...]
  Print the last N lines (default 10), or last N bytes with -c.
  -n N    last N lines        -c N    last N bytes
  -n +N   from line N on      -c +N   from byte N on
  --help  show this help

Examples:
  tail file             # last 10 lines
  tail -n 5 file        # last 5 lines
  tail -c 20 file       # last 20 bytes
  tail -n +2 file       # all but the first line
";

fn run(args: Args) -> i64 {
    if let Some(rc) = coreutils::usage::help_if_requested(args, USAGE) {
        return rc;
    }

    let mut n: usize = 10;
    let mut bytes = false;
    // +N: from the Nth line (or byte) on, rather than the last N.
    let mut from = false;
    let mut idx = 1;
    loop {
        // The count and where it was: `-n N`, attached `-nN`, or legacy `-N`.
        let (text, want_bytes, took) = match args.get_str(idx) {
            // `--` ends the options, so an operand may look like one.
            Some("--") => {
                idx += 1;
                break;
            }
            Some(flag @ ("-n" | "-c")) => match args.get_str(idx + 1) {
                Some(t) => (t, flag == "-c", 2),
                None => {
                    eprintln!("tail: invalid count");
                    return 1;
                }
            },
            Some(a) if a.len() > 2 && (a.starts_with("-c") || a.starts_with("-n")) && is_count(&a[2..]) => {
                (&a[2..], a.starts_with("-c"), 1)
            }
            Some(a) if a.len() > 1 && a.starts_with('-') && a[1..].bytes().all(|c| c.is_ascii_digit()) => (&a[1..], false, 1),
            _ => break,
        };
        match count(text) {
            Some((v, f)) => {
                n = v;
                from = f;
                bytes = want_bytes;
                idx += took;
            }
            None => {
                eprintln!("tail: invalid count");
                return 1;
            }
        }
    }

    // The last none of anything is nothing, so nothing is read (as GNU tail
    // reads nothing), and an input with no end does not keep tail running.
    if n == 0 && !from {
        return 0;
    }

    let mut count_ops = 0usize;
    {
        let mut k = idx;
        while args.get(k).is_some() {
            count_ops += 1;
            k += 1;
        }
    }

    let mut status = 0;
    let mut had = false;
    let mut first = true;
    let mut i = idx;
    while let Some(op) = args.get(i) {
        i += 1;
        had = true;
        let path = match core::str::from_utf8(op) {
            Ok(p) => p,
            Err(_) => {
                eprintln!("tail: invalid UTF-8 path");
                status = 1;
                continue;
            }
        };
        if count_ops > 1 {
            let r = banner(path, first);
            first = false;
            if !settle(path, r, &mut status) {
                return status;
            }
        }
        let r = if path == "-" {
            tail(&mut io::stdin(), n, bytes, from)
        } else {
            match File::open(path) {
                Ok(mut f) => tail(&mut f, n, bytes, from),
                Err(e) => Err(Failed::Input(stream::Error::Read(e))),
            }
        };
        if !settle(path, r, &mut status) {
            return status;
        }
    }

    if !had {
        settle("stdin", tail(&mut io::stdin(), n, bytes, from), &mut status);
    }
    status
}

/// The line naming each input when there are several, with a blank line
/// before all but the first, written whole or failed (GNU's header).
fn banner(path: &str, first: bool) -> core::result::Result<(), Failed> {
    let sep = if first { "" } else { "\n" };
    let name = if path == "-" { "standard input" } else { path };
    writeln!(io::stdout(), "{}==> {} <==", sep, name).map_err(Failed::Output)
}

/// Why one input's tail was not written: the input, or stdout.
enum Failed {
    Input(stream::Error<Error>),
    Output(Error),
}

/// Report one input's outcome into `status`; false once stdout is gone.
fn settle(name: &str, r: core::result::Result<(), Failed>, status: &mut i64) -> bool {
    match r {
        Ok(()) => true,
        Err(Failed::Input(e)) => {
            eprintln!("tail: {}: {}", name, e);
            *status = 1;
            true
        }
        // A reader that went away had what it wanted.
        Err(Failed::Output(Error::BrokenPipe)) => false,
        Err(Failed::Output(e)) => {
            eprintln!("tail: write error: {}", e);
            *status = 1;
            false
        }
    }
}

/// `[+|-]N`: a count, and whether it counts from the start (`+`); `-`, like no
/// sign, counts from the end (POSIX).
fn is_count(s: &str) -> bool {
    let digits = s.strip_prefix(['+', '-']).unwrap_or(s);
    !digits.is_empty() && digits.bytes().all(|c| c.is_ascii_digit())
}

fn count(s: &str) -> Option<(usize, bool)> {
    if !is_count(s) {
        return None;
    }
    let from = s.starts_with('+');
    s.strip_prefix(['+', '-']).unwrap_or(s).parse().ok().map(|n| (n, from))
}

/// Write one input's answer: its last `n` lines (or bytes), or with `from`
/// everything from the `n`th on, as it arrives.
fn tail<R: io::Read + ?Sized>(input: &mut R, n: usize, bytes: bool, from: bool) -> core::result::Result<(), Failed> {
    let mut buf = [0u8; stream::BUF];
    if from {
        let mut s = Skip::new(n, bytes);
        loop {
            let got = input.read(&mut buf).map_err(|e| Failed::Input(stream::Error::Read(e)))?;
            if got == 0 {
                return Ok(());
            }
            io::stdout().write_all(s.feed(&buf[..got])).map_err(Failed::Output)?;
        }
    }
    let mut t = Tail::new(n, bytes);
    loop {
        let got = input.read(&mut buf).map_err(|e| Failed::Input(stream::Error::Read(e)))?;
        if got == 0 {
            break;
        }
        t.feed(&buf[..got]).map_err(Failed::Input)?;
    }
    io::stdout().write_all(t.get()).map_err(Failed::Output)
}
