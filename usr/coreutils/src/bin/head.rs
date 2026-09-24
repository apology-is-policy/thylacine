// head [-n N | -N] [FILE...] -- print the first N lines (default 10).
//
// Streams: stops reading after the N-th newline, so it does not slurp a
// whole large file or block forever on an endless stream. Multiple files
// get "==> name <==" banners (GNU style). No operand / "-" reads stdin. Once
// stdout's reader is gone, head stops, silently.

#![no_std]
#![no_main]

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::env::{self, Args};
use libthyla_rs::err::Error;
use libthyla_rs::fs::File;
use libthyla_rs::io::{self, Read, Write};
use libthyla_rs::eprintln;

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

const USAGE: &str = "\
usage: head [-n N | -c N | -N] [FILE...]
  Print the first N lines (default 10), or first N bytes with -c.
  -n N    first N lines       -c N    first N bytes
  --help  show this help

Examples:
  head file             # first 10 lines
  head -n 3 file        # first 3 lines
  head -c 20 file       # first 20 bytes
";

fn run(args: Args) -> i64 {
    if let Some(rc) = coreutils::usage::help_if_requested(args, USAGE) {
        return rc;
    }

    let mut n: usize = 10;
    let mut bytes = false;
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
                    eprintln!("head: invalid count");
                    return 1;
                }
            },
            Some(a)
                if a.len() > 2
                    && (a.starts_with("-c") || a.starts_with("-n"))
                    && a[2..].bytes().all(|c| c.is_ascii_digit()) =>
            {
                (&a[2..], a.starts_with("-c"), 1)
            }
            Some(a) if a.len() > 1 && a.starts_with('-') && a[1..].bytes().all(|c| c.is_ascii_digit()) => {
                (&a[1..], false, 1)
            }
            _ => break,
        };
        // A count too large to hold is refused, whichever form it came in.
        match text.parse::<usize>() {
            Ok(v) => {
                n = v;
                bytes = want_bytes;
                idx += took;
            }
            Err(_) => {
                eprintln!("head: invalid count");
                return 1;
            }
        }
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
                eprintln!("head: invalid UTF-8 path");
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
            emit(&mut io::stdin(), n, bytes)
        } else {
            match File::open(path) {
                Ok(mut f) => emit(&mut f, n, bytes),
                Err(e) => Err(Failed::Input(e)),
            }
        };
        if !settle(path, r, &mut status) {
            return status;
        }
    }

    if !had {
        settle("stdin", emit(&mut io::stdin(), n, bytes), &mut status);
    }
    status
}

/// The line naming each input when there are several, with a blank line
/// before all but the first, written whole or failed (GNU's header).
fn banner(path: &str, first: bool) -> Result<(), Failed> {
    let sep = if first { "" } else { "\n" };
    let name = if path == "-" { "standard input" } else { path };
    writeln!(io::stdout(), "{}==> {} <==", sep, name).map_err(Failed::Output)
}

/// Why one input's head was not all written: the input, or stdout.
enum Failed {
    Input(Error),
    Output(Error),
}

/// Report one input's outcome into `status`; false once stdout is gone.
fn settle(name: &str, r: Result<(), Failed>, status: &mut i64) -> bool {
    match r {
        Ok(()) => true,
        Err(Failed::Input(e)) => {
            eprintln!("head: {}: {}", name, e);
            *status = 1;
            true
        }
        // A reader that went away had what it wanted.
        Err(Failed::Output(Error::BrokenPipe)) => false,
        Err(Failed::Output(e)) => {
            eprintln!("head: write error: {}", e);
            *status = 1;
            false
        }
    }
}

fn emit<R: Read>(r: &mut R, n: usize, bytes: bool) -> Result<(), Failed> {
    if bytes {
        emit_head_bytes(r, n)
    } else {
        emit_head_lines(r, n)
    }
}

fn emit_head_lines<R: Read>(r: &mut R, n: usize) -> Result<(), Failed> {
    if n == 0 {
        return Ok(());
    }
    let mut out = io::stdout();
    let mut buf = [0u8; 4096];
    let mut lines = 0usize;
    loop {
        let got = r.read(&mut buf).map_err(Failed::Input)?;
        if got == 0 {
            return Ok(());
        }
        let chunk = &buf[..got];
        let mut emit_to = got;
        let mut done = false;
        for (j, &b) in chunk.iter().enumerate() {
            if b == b'\n' {
                lines += 1;
                if lines == n {
                    emit_to = j + 1;
                    done = true;
                    break;
                }
            }
        }
        out.write_all(&chunk[..emit_to]).map_err(Failed::Output)?;
        if done {
            return Ok(());
        }
    }
}

fn emit_head_bytes<R: Read>(r: &mut R, n: usize) -> Result<(), Failed> {
    if n == 0 {
        return Ok(());
    }
    let mut out = io::stdout();
    let mut buf = [0u8; 4096];
    let mut remaining = n;
    loop {
        let got = r.read(&mut buf).map_err(Failed::Input)?;
        if got == 0 {
            return Ok(());
        }
        let take = got.min(remaining);
        out.write_all(&buf[..take]).map_err(Failed::Output)?;
        remaining -= take;
        if remaining == 0 {
            return Ok(());
        }
    }
}
