// nslookup -- resolve a host name to an IPv4 address via netd's connection
// server (`/net/cs`: numeric -> ndb-static -> DNS, NET-DESIGN 5). A numeric host
// passes through; `localhost` resolves from the static ndb; a real name goes to
// the slirp-forwarded DNS resolver. A native `/net` client (libthyla-rs
// `net::resolve`): it touches no hardware (netd owns the NIC, I-5) and reaches
// only the `/net` its territory grants (I-1/I-23/I-28).
//
// A presentation tool -> the Bonfire palette + a boxed result card by default
// (--color=never for plain). v1.0 reports the single A-record the resolver
// returns (cs/dns is first-match); record-type queries (MX, AAAA, PTR) are a
// v1.x `dig` refinement.

#![no_std]
#![no_main]

extern crate alloc;
use alloc::format;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use coreutils::color::{self, ColorMode};
use coreutils::{palette, ui};
use libthyla_rs::env::{self, Args};
use libthyla_rs::eprintln;
use libthyla_rs::net;

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

const USAGE: &str = "\
usage: nslookup [--color[=WHEN]] HOST
  Resolve HOST to an IPv4 address via /net/cs (numeric -> ndb -> DNS).
  --color[=WHEN]  colorize: always (default) | never | auto
  --help          show this help

Examples:
  nslookup localhost          # 127.0.0.1 (from the static ndb)
  nslookup 10.0.2.2           # 10.0.2.2  (numeric passthrough)
  nslookup example.com        # the slirp-forwarded DNS answer
";

fn run(args: Args) -> i64 {
    if let Some(rc) = coreutils::usage::help_if_requested(args, USAGE) {
        return rc;
    }

    let mut mode = ColorMode::Always;
    let mut host: Option<&str> = None;
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
                None => return coreutils::usage::die("nslookup", "invalid --color value (use always/never/auto)"),
            }
            continue;
        }
        if a == "--" {
            continue;
        }
        if host.is_none() {
            host = Some(a);
        } else {
            return coreutils::usage::die("nslookup", "too many operands");
        }
    }
    let host = match host {
        Some(h) => h,
        None => return coreutils::usage::die("nslookup", "missing HOST operand"),
    };

    let on = mode.resolve(stdout_is_console);
    let dim = color::col(palette::DIM, on);
    let rst = color::reset(on);
    let fg = color::col(palette::FG, on);
    let gold = color::col(palette::GOLD, on);
    let emb = color::col(palette::EMBER, on);

    // cs's dial (`tcp!host!service`) needs a VALID service: a 0 service falls
    // through to an ndb lookup of "0" and misses. Use 80 -- we print only the
    // resolved ip, so the port is immaterial.
    match net::resolve(host, 80) {
        Ok(addr) => {
            let rows = [
                ui::Row::new(
                    format!("Name:    {}", host),
                    format!("{}Name:{}    {}{}{}", dim, rst, fg, host, rst),
                ),
                ui::Row::new(
                    format!("Address: {}", addr.ip()),
                    format!("{}Address:{} {}{}{}", dim, rst, gold, addr.ip(), rst),
                ),
            ];
            ui::card("lookup", "", &rows, on);
            0
        }
        Err(_) => {
            eprintln!("{}nslookup:{} can't resolve '{}'", emb, rst, host);
            1
        }
    }
}

/// `--color=auto` stub; true until a kernel TTY check lands (matches the
/// coreutils presentation tools).
fn stdout_is_console() -> bool {
    true
}
