// uniq [-cdui] [FILE] -- collapse ADJACENT duplicate lines.
//
// -c prefixes each output line with its run count. -d prints only duplicated
// groups (count > 1); -u prints only unique lines (count == 1); -i compares
// case-insensitively (the first line of each group is emitted as-is). Reads one
// FILE (or stdin), a line at a time. Like GNU uniq, only ADJACENT duplicates
// are merged (sort first to dedupe globally), and with nothing to count or
// select a line goes out as its run begins, not once the run has ended.

#![no_std]
#![no_main]

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use core::fmt::Write as _;
use coreutils::stream;
use libthyla_rs::env::{self, Args};
use libthyla_rs::err;
use libthyla_rs::fs::File;
use libthyla_rs::{eprintln, io};

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

fn emit(out: &mut io::OutSink, line: &[u8], count: usize, with_count: bool) {
    if with_count {
        let _ = write!(out, "{:>7} ", count);
    }
    out.put(line);
    out.put(b"\n");
}

/// Adjacent-line equality, optionally ASCII-case-insensitive.
fn same(a: &[u8], b: &[u8], ignore_case: bool) -> bool {
    if ignore_case {
        a.len() == b.len() && a.iter().zip(b).all(|(x, y)| x.eq_ignore_ascii_case(y))
    } else {
        a == b
    }
}

/// Collapse one input's adjacent duplicates, until the input ends or stdout is
/// gone: every run's line as the run begins (coreutils::stream::firsts), or with
/// `want` the runs it selects once each has ended (coreutils::stream::runs).
fn collapse<R: io::Read + ?Sized>(out: &mut io::OutSink, input: &mut R, ignore_case: bool, with_count: bool, want: Option<&dyn Fn(usize) -> bool>) -> Result<(), stream::Error<err::Error>> {
    let read = |buf: &mut [u8]| input.read(buf);
    let eq = |a: &[u8], b: &[u8]| same(a, b, ignore_case);
    match want {
        None => stream::firsts(read, eq, |first| {
            emit(out, first, 1, false);
            !out.failed()
        }),
        Some(want) => stream::runs(read, eq, |first, count| {
            if want(count) {
                emit(out, first, count, with_count);
            }
            !out.failed()
        }),
    }
}

const USAGE: &str = "\
usage: uniq [-cdui] [FILE]
  Collapse adjacent duplicate lines.
  -c      prefix each line with its run count
  -d      only print duplicated lines (count > 1)
  -u      only print unique lines (count == 1)
  -i      ignore case when comparing
  --help  show this help

Examples:
  sort f | uniq         # drop adjacent duplicates
  sort f | uniq -c      # with a run count
  uniq -d file          # only the duplicated lines
";

fn run(args: Args) -> i64 {
    if let Some(rc) = coreutils::usage::help_if_requested(args, USAGE) {
        return rc;
    }

    let mut idx = 1;
    let mut with_count = false;
    let mut only_dup = false;
    let mut only_uniq = false;
    let mut ignore_case = false;
    while let Some(a) = args.get_str(idx) {
        if a == "--" {
            idx += 1;
            break;
        }
        if a.starts_with('-') && a != "-" && a.len() > 1 {
            for ch in a[1..].chars() {
                match ch {
                    'c' => with_count = true,
                    'd' => only_dup = true,
                    'u' => only_uniq = true,
                    'i' => ignore_case = true,
                    _ => {
                        eprintln!("uniq: invalid option -- '{}'", ch);
                        return 1;
                    }
                }
            }
            idx += 1;
        } else {
            break;
        }
    }

    // Whether a group of `count` lines should be emitted. -d and -u together
    // select nothing (a line cannot be both duplicated and unique).
    let want = |count: usize| match (only_dup, only_uniq) {
        (true, true) => false,
        (true, false) => count > 1,
        (false, true) => count == 1,
        (false, false) => true,
    };
    // Counting or selecting needs each run's end; showing every run does not.
    let want = (with_count || only_dup || only_uniq).then_some(&want as &dyn Fn(usize) -> bool);

    let mut out = io::OutSink::new();
    let (name, r) = match args.get(idx) {
        Some(op) => {
            let path = match core::str::from_utf8(op) {
                Ok(p) => p,
                Err(_) => {
                    eprintln!("uniq: invalid UTF-8 path");
                    return 1;
                }
            };
            let r = File::open(path)
                .map_err(stream::Error::Read)
                .and_then(|mut f| collapse(&mut out, &mut f, ignore_case, with_count, want));
            (path, r)
        }
        None => ("stdin", collapse(&mut out, &mut io::stdin(), ignore_case, with_count, want)),
    };
    if let Err(e) = r {
        eprintln!("uniq: {}: {}", name, e);
        return 1;
    }
    out.finish("uniq", 0)
}
