// netstat -- print the live network state (NET-DESIGN section 11), the Plan 9
// `netstat` realized as a thin walk of /net. No kernel surface, just 9P file ops
// over netd's tree: it reads the per-interface and per-protocol views and
// enumerates the live /net/<proto>/N connection dirs, reading each one's
// status/local/remote. This is the one-shot aggregate as a CLIENT-SIDE walk
// (per-territory, independent of any server file); netd's `/net/summary` is the
// equivalent SERVER-SIDE rollup (`cat /net/summary`).
//
//   netstat      interfaces + per-protocol stats + the live connection table

#![no_std]
#![no_main]

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

extern crate alloc;
use alloc::format;
use alloc::string::String;
use alloc::vec::Vec;

use libthyla_rs::eprintln;
use libthyla_rs::fs::{self, File};
use libthyla_rs::io::{self, Read, Write};

const PROTOS: [&str; 3] = ["tcp", "udp", "icmp"];

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run()
}

fn run() -> i64 {
    let mut out = io::stdout();
    let mut rc = 0;

    // Interfaces -- the /net/ipifc view (address, gateway, dns, link state).
    let _ = writeln!(out, "interfaces");
    rc |= cat(&mut out, "/net/ipifc/0/status");

    // Per-protocol counters (active / opened).
    let _ = writeln!(out, "stats");
    for p in PROTOS {
        rc |= cat(&mut out, &format!("/net/{}/stats", p));
    }

    // The live connection table: walk each protocol dir, read each numeric
    // connection's local/remote/status. An empty table prints just the header.
    let _ = writeln!(out, "connections");
    let _ = writeln!(
        out,
        "{:<5} {:<5} {:<21} {:<21} status",
        "proto", "conn", "local", "remote"
    );
    for p in PROTOS {
        rc |= walk_proto(&mut out, p);
    }
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

/// Enumerate the live /net/<proto>/N connection dirs and print one line each.
/// `clone`/`stats` are non-numeric and filtered out; only live numbered slots
/// (netd resolves a numeric name only while its slot is live) are listed.
fn walk_proto(out: &mut impl Write, proto: &str) -> i64 {
    let dir = format!("/net/{}", proto);
    let rd = match fs::read_dir(&dir) {
        Ok(rd) => rd,
        // No /net (netd absent) or no such proto dir: nothing to list. The stats
        // `cat` above already surfaced a missing /net, so stay quiet here.
        Err(_) => return 0,
    };
    // Collect the numeric connection slots. netd lists them ascending; sort so
    // the output does not depend on the server's enumeration order.
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
    for n in conns {
        let local = field(proto, n, "local");
        let remote = field(proto, n, "remote");
        let status = field(proto, n, "status");
        let _ = writeln!(
            out,
            "{:<5} {:<5} {:<21} {:<21} {}",
            proto,
            n,
            dash(&local),
            dash(&remote),
            dash(&status),
        );
    }
    0
}

/// Read one per-connection field (local/remote/status), trimmed. A read error
/// or empty file yields "" (rendered as "-" by `dash`).
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
