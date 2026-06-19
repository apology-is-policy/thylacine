// clear -- clear the terminal screen.
//
// Emits the ANSI control sequence to fd 1: cursor-home (ESC[H), erase the
// entire display (ESC[2J), and erase the scrollback (ESC[3J, xterm). This is
// the ncurses `clear` convention. Authored natively (no aux source); any
// operands are ignored.

#![no_std]
#![no_main]

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::env::{self, Args};
use libthyla_rs::io;

const USAGE: &str = "\
usage: clear
  Clear the terminal screen and scrollback.
  --help  show this help
";

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

fn run(args: Args) -> i64 {
    if let Some(rc) = coreutils::usage::help_if_requested(args, USAGE) {
        return rc;
    }
    io::out(b"\x1b[H\x1b[2J\x1b[3J");
    0
}
