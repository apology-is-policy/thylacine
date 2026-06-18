// ipconfig -- read or set the network interface configuration (net-4c), the
// Plan 9 `ipconfig`/Unix `ip addr` equivalent. It is a thin client over netd's
// /net/ipifc tree (NET-DESIGN section 6): no kernel surface, just 9P file ops.
//
//   ipconfig                 print the interface status + the dynamic ndb
//   ipconfig add IP MASK [GW] assign a static address (MASK = 255.255.255.0
//                            or a /24 or a bare 24); replaces the current addr
//   ipconfig remove          clear the address + default route
//
// The DHCP path is netd's own (it drives the lease at bring-up and folds it
// into /net/ipifc + /net/ndb); `ipconfig add` is the static / bridged-network
// path. Reads go to /net/ipifc/0/status + /net/ndb; writes go to
// /net/ipifc/0/ctl (0666, SYSTEM-owned, world-accessible like cs/dns).

#![no_std]
#![no_main]

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

extern crate alloc;
use alloc::vec::Vec;

use libthyla_rs::env::{self, Args};
use libthyla_rs::fs::{File, OpenOptions};
use libthyla_rs::io::{self, Write};
use libthyla_rs::eprintln;

const IPIFC_CTL: &str = "/net/ipifc/0/ctl";
const IPIFC_STATUS: &str = "/net/ipifc/0/status";
const NDB: &str = "/net/ndb";

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

fn run(args: Args) -> i64 {
    let ops: Vec<&[u8]> = args.operands().collect();
    match ops.first().copied() {
        None => show(),
        Some(sub) if sub == b"add" => add(&ops[1..]),
        Some(sub) if sub == b"remove" || sub == b"down" => ctl(b"remove"),
        Some(_) => {
            eprintln!("ipconfig: usage: ipconfig [add <ip> <mask> [gw] | remove]");
            1
        }
    }
}

/// Print /net/ipifc/0/status then /net/ndb (the interface view + the live
/// dynamic database). A missing /net (netd down / not mounted) is reported.
fn show() -> i64 {
    let mut out = io::stdout();
    let mut rc = 0;
    for path in [IPIFC_STATUS, NDB] {
        match File::open(path) {
            Ok(mut f) => {
                if io::copy(&mut f, &mut out).is_err() {
                    eprintln!("ipconfig: {}: read error", path);
                    rc = 1;
                }
            }
            Err(e) => {
                eprintln!("ipconfig: {}: {}", path, e);
                rc = 1;
            }
        }
    }
    rc
}

/// `ipconfig add IP MASK [GW]` -> write "add IP MASK [GW]" to the ifc ctl.
fn add(rest: &[&[u8]]) -> i64 {
    if rest.len() < 2 || rest.len() > 3 {
        eprintln!("ipconfig: usage: ipconfig add <ip> <mask> [gw]");
        return 1;
    }
    let mut cmd: Vec<u8> = Vec::new();
    cmd.extend_from_slice(b"add");
    for a in rest {
        cmd.push(b' ');
        cmd.extend_from_slice(a);
    }
    ctl(&cmd)
}

/// Open the ifc ctl ORDWR and write one command (the netd handler parses it;
/// a malformed command comes back as an Err from the write).
fn ctl(cmd: &[u8]) -> i64 {
    match OpenOptions::new().read(true).write(true).open(IPIFC_CTL) {
        Ok(mut f) => match f.write_all(cmd) {
            Ok(()) => 0,
            Err(e) => {
                eprintln!("ipconfig: {}: write: {}", IPIFC_CTL, e);
                1
            }
        },
        Err(e) => {
            eprintln!("ipconfig: {}: {}", IPIFC_CTL, e);
            1
        }
    }
}
