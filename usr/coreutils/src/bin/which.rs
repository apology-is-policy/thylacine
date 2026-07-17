// which NAME... -- locate a command.
//
// A NAME containing '/' is treated as an explicit path (absolute, or
// relative-to-cwd since LS-4) and probed for existence. A bare name searches
// the $PATH environment variable (read from the per-Proc /env device -- login
// seeds PATH=/bin:/goroot/bin for a session, Stage 6), first hit wins. With
// no PATH in the environment (a bare-spawned boot context), a bare name is
// reported not-found (exit 1), the pre-Stage-6 behavior.
//
// NOTE: ut resolves commands by its own static $path (eval/stmt.rs
// resolve_command), not by $PATH -- login seeds the env var to MIRROR the
// shell's list, so `which` answers what the shell would run as long as the
// two stay in sync (both are Stage 6 surfaces; drift is a bug).

#![no_std]
#![no_main]

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

extern crate alloc;

use alloc::string::String;
use libthyla_rs::env::{self, Args};
use libthyla_rs::io;

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

const USAGE: &str = "\
usage: which NAME...
  Locate a command by printing its path. A bare NAME searches $PATH (from the
  /env device; login seeds /bin:/goroot/bin); a NAME containing '/' is probed
  as a path. Exit 1 if any NAME is not found.
  --help  show this help

Examples:
  which go              # /goroot/bin/go via $PATH
  which /bin/ut         # prints /bin/ut if it exists
";

fn run(args: Args) -> i64 {
    if let Some(rc) = coreutils::usage::help_if_requested(args, USAGE) {
        return rc;
    }

    let path_var = env::var("PATH");
    let mut status = 0;
    let mut had = false;
    for op in args.operands() {
        had = true;
        let name = match core::str::from_utf8(op) {
            Ok(n) => n,
            Err(_) => {
                status = 1;
                continue;
            }
        };
        if name.contains('/') {
            // Explicit path: report it if it exists (fs::exists resolves a
            // relative path against the cwd since LS-4).
            if libthyla_rs::fs::exists(name) {
                io::out(name.as_bytes());
                io::out(b"\n");
            } else {
                status = 1;
            }
        } else {
            let mut found = false;
            if let Some(path) = &path_var {
                for dir in path.split(':') {
                    if dir.is_empty() {
                        continue;
                    }
                    let mut cand = String::with_capacity(dir.len() + 1 + name.len());
                    cand.push_str(dir);
                    cand.push('/');
                    cand.push_str(name);
                    if libthyla_rs::fs::exists(&cand) {
                        io::out(cand.as_bytes());
                        io::out(b"\n");
                        found = true;
                        break;
                    }
                }
            }
            if !found {
                status = 1;
            }
        }
    }
    if !had {
        return 1;
    }
    status
}
