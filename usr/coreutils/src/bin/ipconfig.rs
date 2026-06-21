// ipconfig -- read or set the network interface configuration (net-4c), the
// Plan 9 `ipconfig`/Unix `ip addr` equivalent. It is a thin client over netd's
// /net/ipifc tree (NET-DESIGN section 6): no kernel surface, just 9P file ops.
//
//   ipconfig                  print the interface status + the dynamic ndb
//   ipconfig add IP MASK [GW] assign a static address (MASK = 255.255.255.0,
//                             a /24, or a bare 24); replaces the current addr
//   ipconfig remove           clear the address + default route
//   ipconfig renew            re-acquire the DHCP lease (force a fresh DISCOVER)
//
// The status + ndb views are streamed verbatim (netd's authoritative text) under
// EMBER section headers; errors use the Bonfire palette. Reads go to
// /net/ipifc/0/status + /net/ndb; writes go to /net/ipifc/0/ctl (0666,
// SYSTEM-owned, world-accessible like cs/dns).

#![no_std]
#![no_main]

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

extern crate alloc;
use alloc::string::String;
use alloc::vec::Vec;

use coreutils::color::{self, ColorMode};
use coreutils::palette;
use libthyla_rs::env::{self, Args};
use libthyla_rs::fs::{File, OpenOptions};
use libthyla_rs::io::{self, Write};
use libthyla_rs::{eprintln, println};

const IPIFC_CTL: &str = "/net/ipifc/0/ctl";
const IPIFC_STATUS: &str = "/net/ipifc/0/status";
const NDB: &str = "/net/ndb";

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

const USAGE: &str = "\
usage: ipconfig [--color[=WHEN]] [add IP MASK [GW] | remove | renew]
  Read or set the network interface configuration (the /net/ipifc tree).
  (no args)         print the interface status + the dynamic ndb
  add IP MASK [GW]  assign a static address (MASK = 255.255.255.0, /24, or 24)
  remove            clear the address + default route
  renew             re-acquire the DHCP lease (force a fresh DISCOVER)
  --color[=WHEN]    colorize headers/errors: always (default) | never | auto
  --help            show this help

Examples:
  ipconfig                       # show the current lease + ndb
  ipconfig add 192.168.1.50 24   # a static address with a /24 mask
  ipconfig add 10.0.0.5 255.255.255.0 10.0.0.1   # with a gateway
  ipconfig remove                # clear the address
  ipconfig renew                 # re-acquire the lease via DHCP
";

fn run(args: Args) -> i64 {
    if let Some(rc) = coreutils::usage::help_if_requested(args, USAGE) {
        return rc;
    }

    let mut mode = ColorMode::Always;
    let mut ops: Vec<&str> = Vec::new();
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
                None => return coreutils::usage::die("ipconfig", "invalid --color value (use always/never/auto)"),
            }
            continue;
        }
        if a == "--" {
            continue;
        }
        ops.push(a);
    }

    let on = mode.resolve(stdout_is_console);
    match ops.first().copied() {
        None => show(on),
        Some("add") => add(&ops[1..], on),
        Some("remove") | Some("down") => ctl("remove", on),
        Some("renew") => ctl("renew", on),
        Some(_) => {
            let emb = color::col(palette::EMBER, on);
            let rst = color::reset(on);
            eprintln!(
                "{}ipconfig:{} usage: ipconfig [add IP MASK [GW] | remove | renew]",
                emb, rst
            );
            1
        }
    }
}

/// Print /net/ipifc/0/status then /net/ndb (the interface view + the live dynamic
/// database) under EMBER headers. A missing /net (netd down) is reported.
fn show(on: bool) -> i64 {
    let emb = color::col(palette::EMBER, on);
    let rst = color::reset(on);
    let mut out = io::stdout();
    let mut rc = 0;

    println!("{}interface{}", emb, rst);
    rc |= cat(&mut out, IPIFC_STATUS, on);
    println!("{}ndb{}", emb, rst);
    rc |= cat(&mut out, NDB, on);
    rc
}

/// Stream a file to stdout verbatim. Errors use the EMBER prefix.
fn cat(out: &mut impl Write, path: &str, on: bool) -> i64 {
    let emb = color::col(palette::EMBER, on);
    let rst = color::reset(on);
    match File::open(path) {
        Ok(mut f) => {
            if io::copy(&mut f, out).is_err() {
                eprintln!("{}ipconfig:{} {}: read error", emb, rst, path);
                return 1;
            }
            0
        }
        Err(e) => {
            eprintln!("{}ipconfig:{} {}: {}", emb, rst, path, e);
            1
        }
    }
}

/// `ipconfig add IP MASK [GW]` -> write "add IP MASK [GW]" to the ifc ctl.
fn add(rest: &[&str], on: bool) -> i64 {
    let emb = color::col(palette::EMBER, on);
    let rst = color::reset(on);
    if rest.len() < 2 || rest.len() > 3 {
        eprintln!("{}ipconfig:{} usage: ipconfig add IP MASK [GW]", emb, rst);
        return 1;
    }
    let mut cmd = String::from("add");
    for a in rest {
        cmd.push(' ');
        cmd.push_str(a);
    }
    ctl(&cmd, on)
}

/// Open the ifc ctl ORDWR and write one command (netd parses it; a malformed
/// command comes back as an Err from the write).
fn ctl(cmd: &str, on: bool) -> i64 {
    let emb = color::col(palette::EMBER, on);
    let rst = color::reset(on);
    match OpenOptions::new().read(true).write(true).open(IPIFC_CTL) {
        Ok(mut f) => match f.write_all(cmd.as_bytes()) {
            Ok(()) => 0,
            Err(e) => {
                eprintln!("{}ipconfig:{} {}: write: {}", emb, rst, IPIFC_CTL, e);
                1
            }
        },
        Err(e) => {
            eprintln!("{}ipconfig:{} {}: {}", emb, rst, IPIFC_CTL, e);
            1
        }
    }
}

/// `--color=auto` stub; true until a kernel TTY check lands.
fn stdout_is_console() -> bool {
    true
}
