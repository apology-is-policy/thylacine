// basename PATH [SUFFIX] -- strip directory and (optional) suffix.
//
// Exercises libthyla-rs::fs::Path::file_name(), which returns None for "/",
// ".", and ".." (DOC-GAP G08: it diverges from POSIX basename, which yields
// "/", ".", ".." respectively). We recover the POSIX answer for the None
// case via posix_base.

#![no_std]
#![no_main]

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::env::{self, Args};
use libthyla_rs::fs::Path;
use libthyla_rs::{eprintln, io};

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

const USAGE: &str = "\
usage: basename PATH [SUFFIX]
  Strip the directory prefix (and an optional trailing SUFFIX) from PATH.
  --help  show this help

Examples:
  basename /usr/bin/ut         # ut
  basename /tmp/file.txt .txt  # file
";

fn run(args: Args) -> i64 {
    if let Some(rc) = coreutils::usage::help_if_requested(args, USAGE) {
        return rc;
    }

    let path = match args.get_str(1) {
        Some(p) => p,
        None => {
            eprintln!("basename: missing operand");
            return 1;
        }
    };

    let base: &str = Path::new(path).file_name().unwrap_or_else(|| posix_base(path));

    let out = match args.get_str(2) {
        Some(suf) if !suf.is_empty() && base != suf && base.ends_with(suf) => {
            &base[..base.len() - suf.len()]
        }
        _ => base,
    };

    io::out(out.as_bytes());
    io::out(b"\n");
    0
}

fn posix_base(path: &str) -> &str {
    if path.is_empty() {
        return ""; // POSIX: basename '' -> empty
    }
    let trimmed = path.trim_end_matches('/');
    if trimmed.is_empty() {
        return "/"; // path was "/", "//", ...
    }
    match trimmed.rfind('/') {
        Some(i) => &trimmed[i + 1..],
        None => trimmed, // ".", "..", or a bare name
    }
}
