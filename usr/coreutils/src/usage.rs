//! Shared `--help` / bad-usage plumbing so every tool behaves the GNU way: a
//! `--help` anywhere prints the tool's usage to stdout and exits 0; a usage
//! error prints to stderr with a `Try 'tool --help'` hint and exits 2.
//!
//! Backend-gated (uses libthyla-rs io); each tool keeps its own `USAGE` string.

use libthyla_rs::env::Args;
use libthyla_rs::{eprintln, print};

/// If any argument (before a `--` terminator) is `--help`, print `usage` to
/// stdout and return `Some(0)` -- the caller `return`s it. Position-independent.
pub fn help_if_requested(args: Args, usage: &str) -> Option<i64> {
    let mut i = 1;
    while let Some(a) = args.get_str(i) {
        if a == "--" {
            break;
        }
        if a == "--help" {
            print!("{}", usage);
            return Some(0);
        }
        i += 1;
    }
    None
}

/// The `Try 'PROG --help' for more information.` hint (stderr). Pair it with the
/// tool's specific error message + a `return 2`.
pub fn hint(prog: &str) {
    eprintln!("Try '{} --help' for more information.", prog);
}

/// `PROG: MSG` + the hint, returning 2 (GNU's bad-usage exit). The one-call form
/// for an option/operand error.
pub fn die(prog: &str, msg: &str) -> i64 {
    eprintln!("{}: {}", prog, msg);
    hint(prog);
    2
}
