// echo [-n] [ARG...] -- write arguments to stdout, space-separated, with a
// trailing newline unless -n. Bytes pass through verbatim (no backslash
// escapes -- that is `echo -e`, a bash extension out of scope here).
//
// Output goes to fd 1 (io::out). v1.0 caveat (DOC-GAP G06): standalone, fd
// 1 may be unwired, so output is visible only in a pipeline / redirect /
// parent-inherited fd.

#![no_std]
#![no_main]

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::env::{self, Args};
use libthyla_rs::io;

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

const USAGE: &str = "\
usage: echo [-n] [ARG...]
  Print the ARGs separated by spaces, then a newline. Bytes pass through
  verbatim (no backslash-escape processing).
  -n      omit the trailing newline
  --help  show this help

Examples:
  echo hello world      # hello world
  echo -n 'prompt: '    # no trailing newline
";

fn run(args: Args) -> i64 {
    if let Some(rc) = coreutils::usage::help_if_requested(args, USAGE) {
        return rc;
    }

    let mut idx = 1; // argv[0] is the program name
    let mut newline = true;

    if args.get(idx) == Some(b"-n") {
        newline = false;
        idx += 1;
    }

    let mut first = true;
    while let Some(a) = args.get(idx) {
        if !first {
            io::out(b" ");
        }
        io::out(a);
        first = false;
        idx += 1;
    }

    if newline {
        io::out(b"\n");
    }
    0
}
