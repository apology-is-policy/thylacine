// netstat -- print the live network state (NET-DESIGN section 11), the Plan 9
// `netstat` realized as a thin walk of /net. No kernel surface, just 9P file ops
// over netd's tree: it reads the per-interface and per-protocol views and
// enumerates the live /net/<proto>/N connection dirs, reading each one's
// status/local/remote.
//
// A presentation tool -> the Bonfire palette by default (--color=never for
// plain). The interface + per-protocol views are streamed verbatim (netd's
// authoritative text); the connection table is rendered as a boxed card.

#![no_std]
#![no_main]

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

extern crate alloc;
use alloc::format;
use alloc::string::String;
use alloc::vec::Vec;

use coreutils::color::{self, ColorMode};
use coreutils::{palette, ui};
use libthyla_rs::env::{self, Args};
use libthyla_rs::fs::{self, File};
use libthyla_rs::io::{self, Read, Write};
use libthyla_rs::{eprintln, println};

const PROTOS: [&str; 3] = ["tcp", "udp", "icmp"];

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

const USAGE: &str = "\
usage: netstat [--color[=WHEN]]
  Print the live network state: interfaces, per-protocol stats, and the live
  connection table.
  --color[=WHEN]  colorize: always (default) | never | auto
  --help          show this help

Examples:
  netstat                     # interfaces + stats + the connection table
  netstat --color=never       # plain, for a pipe or a script
";

fn run(args: Args) -> i64 {
    if let Some(rc) = coreutils::usage::help_if_requested(args, USAGE) {
        return rc;
    }

    let mut mode = ColorMode::Always;
    let mut i = 1;
    while let Some(a) = args.get_str(i) {
        i += 1;
        if a == "--color" {
            mode = ColorMode::Always;
            continue;
        }
        if let Some(w) = a.strip_prefix("--color=") {
            match ColorMode::parse_when(w) {
                Some(m) => mode = m,
                None => return coreutils::usage::die("netstat", "invalid --color value (use always/never/auto)"),
            }
            continue;
        }
        return coreutils::usage::die("netstat", "netstat takes no operands");
    }

    let on = mode.resolve(stdout_is_console);
    let emb = color::col(palette::EMBER, on);
    let dim = color::col(palette::DIM, on);
    let rst = color::reset(on);

    let mut out = io::stdout();
    let mut rc = 0;

    // Interfaces + per-protocol stats: streamed verbatim under EMBER headers.
    println!("{}interfaces{}", emb, rst);
    rc |= cat(&mut out, "/net/ipifc/0/status");
    println!("{}stats{}", emb, rst);
    for p in PROTOS {
        rc |= cat(&mut out, &format!("/net/{}/stats", p));
    }
    println!();

    // The live connection table, as a boxed card. The header is a DIM row; each
    // connection is a colored row. An empty table is a card with just the header.
    let header = format!("{:<5} {:<5} {:<21} {:<21} {}", "proto", "conn", "local", "remote", "status");
    let mut rows: Vec<ui::Row> = Vec::new();
    rows.push(ui::Row::new(header.clone(), format!("{}{}{}", dim, header, rst)));
    let mut nconn = 0u32;
    for p in PROTOS {
        collect(p, on, &mut rows, &mut nconn);
    }
    let label = format!("{} conn{}", nconn, if nconn == 1 { "" } else { "s" });
    ui::card("connections", &label, &rows, on);

    rc
}

/// Stream a file to stdout verbatim (the per-interface / per-protocol views are
/// already cat-able). A missing /net (netd down / not mounted) is reported.
fn cat(out: &mut impl Write, path: &str) -> i64 {
    match File::open(path) {
        Ok(mut f) => {
            if io::copy(&mut f, out).is_err() {
                eprintln!("netstat: {}: read error", path);
                return 1;
            }
            0
        }
        Err(e) => {
            eprintln!("netstat: {}: {}", path, e);
            1
        }
    }
}

/// Enumerate the live /net/<proto>/N connection dirs and append one colored card
/// row each. `clone`/`stats` are non-numeric and filtered out; only live numbered
/// slots (netd resolves a numeric name only while its slot is live) are listed.
fn collect(proto: &str, on: bool, rows: &mut Vec<ui::Row>, nconn: &mut u32) {
    let dir = format!("/net/{}", proto);
    let rd = match fs::read_dir(&dir) {
        Ok(rd) => rd,
        Err(_) => return, // no /net or no such proto: nothing to list
    };
    let mut conns: Vec<u32> = Vec::new();
    for ent in rd {
        let ent = match ent {
            Ok(e) => e,
            Err(_) => break, // a readdir error ends enumeration (no partial lie)
        };
        if !ent.is_dir() {
            continue;
        }
        if let Ok(n) = ent.file_name().parse::<u32>() {
            conns.push(n);
        }
    }
    conns.sort_unstable();

    let slate = color::col(palette::SLATE, on);
    let gold = color::col(palette::GOLD, on);
    let grn = color::col(palette::GREEN, on);
    let rst = color::reset(on);
    for n in conns {
        let local = field(proto, n, "local");
        let remote = field(proto, n, "remote");
        let status = field(proto, n, "status");
        let (ld, rd_, sd) = (dash(&local), dash(&remote), dash(&status));
        let plain = format!("{:<5} {:<5} {:<21} {:<21} {}", proto, n, ld, rd_, sd);
        // Color each cell AFTER padding the plain value, so the SGR spans stay
        // zero-width and the columns align (the boxd compute-on-plain rule).
        let colored = format!(
            "{}{:<5}{} {}{:<5}{} {}{:<21}{} {}{:<21}{} {}{}{}",
            slate, proto, rst, gold, n, rst, gold, ld, rst, gold, rd_, rst, grn, sd, rst
        );
        rows.push(ui::Row::new(plain, colored));
        *nconn += 1;
    }
}

/// Read one per-connection field (local/remote/status), trimmed. A read error or
/// empty file yields "" (rendered as "-" by `dash`).
fn field(proto: &str, n: u32, name: &str) -> String {
    let path = format!("/net/{}/{}/{}", proto, n, name);
    let mut s = String::new();
    if let Ok(mut f) = File::open(&path) {
        let _ = f.read_to_string(&mut s);
    }
    s.trim().into()
}

fn dash(s: &str) -> &str {
    if s.is_empty() {
        "-"
    } else {
        s
    }
}

/// `--color=auto` stub; true until a kernel TTY check lands.
fn stdout_is_console() -> bool {
    true
}
