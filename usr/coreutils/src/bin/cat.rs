// cat [-AbEnstTuv] [FILE...] -- concatenate files (or stdin) to stdout.
//
// With no operands (or "-"), copies stdin. With no flags the bytes go out as
// they are read (byte-clean -- cat is a pipe payload tool). Any of -n/-b
// (number), -E/-T/-v/-A (show ends/tabs/nonprinting), or -s (squeeze blank
// runs) passes each read through coreutils::stream::CatLines: a user-requested
// transform, still plain text and pipe-safe, that holds no line, so a line with
// no end streams like any other input. The line counter and the squeeze state
// are continuous across every operand, which is numbered as one input.
// Absolute paths only (no cwd resolution).

#![no_std]
#![no_main]

extern crate alloc;
use alloc::vec::Vec;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use coreutils::stream::{self, CatLines};
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
usage: cat [-AbEnstTuv] [FILE...]
  Concatenate FILE(s) (or stdin) to stdout.
  -n      number all output lines
  -b      number nonblank output lines (overrides -n)
  -s      squeeze repeated blank output lines
  -E      show line ends as '$'
  -T      show tabs as '^I'
  -v      show nonprinting characters (^X and M- notation)
  -e      = -vE       -t      = -vT       -A      = -vET
  -u      ignored (always unbuffered)
  --help  show this help

Examples:
  cat file              # print a file
  cat -n file           # with line numbers
  cat a b > both        # concatenate two files
";

fn run(args: Args) -> i64 {
    if let Some(rc) = coreutils::usage::help_if_requested(args, USAGE) {
        return rc;
    }

    let mut idx = 1;
    let mut lines = CatLines::default();
    while let Some(a) = args.get_str(idx) {
        if a == "--" {
            idx += 1;
            break;
        }
        if a.starts_with('-') && a.len() > 1 {
            for ch in a[1..].chars() {
                match ch {
                    'n' => lines.number = true,
                    'b' => lines.number_nonblank = true,
                    's' => lines.squeeze = true,
                    'E' => lines.show_ends = true,
                    'T' => lines.show_tabs = true,
                    'v' => lines.show_nonprint = true,
                    'e' => {
                        lines.show_nonprint = true;
                        lines.show_ends = true;
                    }
                    't' => {
                        lines.show_nonprint = true;
                        lines.show_tabs = true;
                    }
                    'A' => {
                        lines.show_nonprint = true;
                        lines.show_ends = true;
                        lines.show_tabs = true;
                    }
                    'u' => {} // unbuffered: already effectively so
                    _ => {
                        eprintln!("cat: invalid option -- '{}'", ch);
                        return 1;
                    }
                }
            }
            idx += 1;
        } else {
            break;
        }
    }

    let mut status = 0;
    let mut had = false;
    let mut staged = Vec::new();
    let mut i = idx;
    while let Some(op) = args.get(i) {
        i += 1;
        had = true;
        let path = match core::str::from_utf8(op) {
            Ok(p) => p,
            Err(_) => {
                eprintln!("cat: invalid UTF-8 in path operand");
                status = 1;
                continue;
            }
        };
        let r = if path == "-" {
            cat_reader(&mut io::stdin(), &mut lines, &mut staged)
        } else {
            match File::open(path) {
                Ok(mut f) => cat_reader(&mut f, &mut lines, &mut staged),
                Err(e) => Err(Failed::Input(e)),
            }
        };
        if !settle(path, r, &mut status) {
            return status;
        }
    }

    if !had {
        settle("stdin", cat_reader(&mut io::stdin(), &mut lines, &mut staged), &mut status);
    }
    status
}

/// Why one input was not all written: the input, or stdout.
enum Failed {
    Input(Error),
    Output(Error),
}

/// Report one input's outcome into `status`; false once stdout is gone.
fn settle(name: &str, r: Result<(), Failed>, status: &mut i64) -> bool {
    match r {
        Ok(()) => true,
        Err(Failed::Input(e)) => {
            eprintln!("cat: {}: {}", name, e);
            *status = 1;
            true
        }
        // A reader that went away had what it wanted.
        Err(Failed::Output(Error::BrokenPipe)) => false,
        Err(Failed::Output(e)) => {
            eprintln!("cat: write error: {}", e);
            *status = 1;
            false
        }
    }
}

/// Copy one input to stdout a read at a time, through `lines` when a transform
/// is on (`staged` holds one read's transformed bytes).
fn cat_reader<R: Read + ?Sized>(r: &mut R, lines: &mut CatLines, staged: &mut Vec<u8>) -> Result<(), Failed> {
    let mut buf = [0u8; stream::BUF];
    let mut out = io::stdout();
    loop {
        let got = r.read(&mut buf).map_err(Failed::Input)?;
        if got == 0 {
            return Ok(());
        }
        let bytes = if lines.active() {
            staged.clear();
            lines.feed(&buf[..got], staged);
            &staged[..]
        } else {
            &buf[..got]
        };
        out.write_all(bytes).map_err(Failed::Output)?;
    }
}
