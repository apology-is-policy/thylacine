// cut -f LIST [-d DELIM] [FILE...]  /  cut -c LIST [FILE...]
//
// -f LIST: select delimiter-separated fields (default delim TAB).
// -c LIST: select 1-based byte positions.
// LIST is comma-separated single positions and N-M / N- / -M ranges.
// Reads each FILE (or stdin), one transformed line at a time.

#![no_std]
#![no_main]

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use coreutils::select::{self, Range};
use coreutils::stream;
use libthyla_rs::env::{self, Args};
use libthyla_rs::err;
use libthyla_rs::fs::File;
use libthyla_rs::{eprintln, io};

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

/// Cut one input a line at a time (coreutils::stream), holding only the line,
/// until the input ends or stdout is gone.
fn process<R: io::Read + ?Sized>(out: &mut io::OutSink, input: &mut R, by_field: bool, delim: u8, ranges: &[Range]) -> Result<(), stream::Error<err::Error>> {
    stream::lines(
        |buf| input.read(buf),
        |line, _| {
            let put = |piece: &[u8]| {
                out.put(piece);
                !out.failed()
            };
            if by_field {
                select::fields(line, delim, ranges, put);
            } else {
                select::bytes(line, ranges, put);
            }
            out.put(b"\n");
            !out.failed()
        },
    )
}

const USAGE: &str = "\
usage: cut -f LIST [-d DELIM] [FILE...]
       cut -c LIST [FILE...]
  Print selected parts of each line. LIST is comma-separated positions and
  ranges (1,3 or 2-5 or 4- or -3).
  -f LIST   select delimiter-separated fields
  -c LIST   select 1-based byte positions
  -d DELIM  field delimiter for -f (default TAB)
  --help    show this help

Examples:
  cut -d: -f1 /etc/passwd   # first colon-field of each line
  cut -c1-3 file            # first three bytes of each line
";

fn run(args: Args) -> i64 {
    if let Some(rc) = coreutils::usage::help_if_requested(args, USAGE) {
        return rc;
    }

    let mut idx = 1;
    let mut delim = b'\t';
    let mut flist: Option<&str> = None;
    let mut clist: Option<&str> = None;

    while let Some(a) = args.get_str(idx) {
        if a == "--" {
            idx += 1;
            break;
        }
        if let Some(rest) = a.strip_prefix("-d") {
            let d = if rest.is_empty() {
                idx += 1;
                args.get_str(idx).unwrap_or("\t")
            } else {
                rest
            };
            delim = d.as_bytes().first().copied().unwrap_or(b'\t');
            idx += 1;
        } else if let Some(rest) = a.strip_prefix("-f") {
            flist = Some(if rest.is_empty() {
                idx += 1;
                args.get_str(idx).unwrap_or("")
            } else {
                rest
            });
            idx += 1;
        } else if let Some(rest) = a.strip_prefix("-c") {
            clist = Some(if rest.is_empty() {
                idx += 1;
                args.get_str(idx).unwrap_or("")
            } else {
                rest
            });
            idx += 1;
        } else if a.starts_with('-') && a != "-" {
            eprintln!("cut: invalid option {}", a);
            return 1;
        } else {
            break;
        }
    }

    let (by_field, list_str) = match (flist, clist) {
        (Some(l), None) => (true, l),
        (None, Some(l)) => (false, l),
        _ => {
            eprintln!("cut: specify exactly one of -f or -c");
            return 1;
        }
    };
    let ranges = match select::parse_list(list_str) {
        Some(r) => r,
        None => {
            eprintln!("cut: invalid list '{}'", list_str);
            return 1;
        }
    };

    let mut status = 0;
    let mut out = io::OutSink::new();
    let mut had = false;
    let mut i = idx;
    while let Some(op) = args.get(i) {
        if out.failed() {
            break;
        }
        i += 1;
        had = true;
        let path = match core::str::from_utf8(op) {
            Ok(p) => p,
            Err(_) => {
                eprintln!("cut: invalid UTF-8 path");
                status = 1;
                continue;
            }
        };
        match File::open(path)
            .map_err(stream::Error::Read)
            .and_then(|mut f| process(&mut out, &mut f, by_field, delim, &ranges))
        {
            Ok(()) => {}
            Err(e) => {
                eprintln!("cut: {}: {}", path, e);
                status = 1;
            }
        }
    }
    if !had {
        match process(&mut out, &mut io::stdin(), by_field, delim, &ranges) {
            Ok(()) => {}
            Err(e) => {
                eprintln!("cut: stdin: {}", e);
                status = 1;
            }
        }
    }
    out.finish("cut", status)
}
