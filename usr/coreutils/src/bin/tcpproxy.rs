// tcpproxy -- a sequential TCP port forwarder: bind a local port, accept a
// connection, dial an upstream, and splice the two together until both close.
// Useful (expose a loopback-only service; bridge two ports) AND a deliberate
// throughput TORTURE: every byte crosses netd's stack twice (client -> proxy ->
// upstream and back), so a `tcpproxy` in front of an echo/sink doubles the
// per-byte work and stresses the optimization the throughput arc is chasing.
//
//   tcpproxy LPORT HOST PORT   -- announce *!LPORT, forward each call to HOST:PORT
//
// Connections are served one at a time (the netd listener backlog is 1); the
// proxy loops forever (Ctrl-C to stop). The bidirectional splice lives in
// `coreutils::netpump` (shared with the splice half of the design). Status uses
// the Bonfire palette. netd owns the NIC (I-5); tcpproxy touches no hardware and
// reaches only the `/net` its territory grants (I-1/I-23/I-28).

#![no_std]
#![no_main]

extern crate alloc;
use alloc::format;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use coreutils::color::{self, ColorMode};
use coreutils::{netpump, palette};
use libthyla_rs::env::{self, Args};
use libthyla_rs::net::{self, Ipv4Addr, SocketAddrV4, TcpListener, TcpStream};
use libthyla_rs::time::Duration;
use libthyla_rs::{eprintln, println};

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

const USAGE: &str = "\
usage: tcpproxy [--color[=WHEN]] LPORT HOST PORT
  Forward each inbound connection on LPORT to HOST:PORT (one at a time).
  --color[=WHEN]  colorize status: always (default) | never | auto
  --help          show this help

Examples:
  tcpproxy 8080 127.0.0.1 80      # expose loopback :80 on :8080
  tcpproxy 9000 example.com 80    # bridge :9000 to a remote service
";

// Bounded upstream dial so an unreachable upstream drops the call, not the proxy.
const DIAL_TIMEOUT: Duration = Duration::from_secs(10);

fn run(args: Args) -> i64 {
    if let Some(rc) = coreutils::usage::help_if_requested(args, USAGE) {
        return rc;
    }

    let mut mode = ColorMode::Always;
    let mut pos: [&str; 3] = [""; 3];
    let mut npos = 0usize;
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
                None => return coreutils::usage::die("tcpproxy", &format!("invalid --color value -- '{}'", w)),
            }
            continue;
        }
        if a == "--" {
            continue;
        }
        if npos >= 3 {
            return coreutils::usage::die("tcpproxy", "too many operands");
        }
        pos[npos] = a;
        npos += 1;
    }
    if npos != 3 {
        return coreutils::usage::die("tcpproxy", "need LPORT HOST PORT");
    }
    let lport = match parse_port(pos[0]) {
        Some(p) => p,
        None => return coreutils::usage::die("tcpproxy", "invalid LPORT"),
    };
    let uport = match parse_port(pos[2]) {
        Some(p) => p,
        None => return coreutils::usage::die("tcpproxy", "invalid upstream PORT"),
    };

    let on = mode.resolve(stdout_is_console);
    let dim = color::col(palette::DIM, on);
    let rst = color::reset(on);
    let gold = color::col(palette::GOLD, on);
    let grn = color::col(palette::GREEN, on);
    let emb = color::col(palette::EMBER, on);

    // Resolve the upstream once, up front (fail fast if it cannot be resolved).
    let upaddr = match net::resolve(pos[1], uport) {
        Ok(a) => a,
        Err(_) => {
            eprintln!("{}tcpproxy:{} cannot resolve upstream '{}'", emb, rst, pos[1]);
            return 1;
        }
    };

    let listener = match TcpListener::bind(SocketAddrV4::new(Ipv4Addr::UNSPECIFIED, lport)) {
        Ok(l) => l,
        Err(_) => {
            eprintln!("{}tcpproxy:{} cannot announce port {} (is netd running?)", emb, rst, lport);
            return 1;
        }
    };
    println!("{}tcpproxy:{} listening on {}*:{}{} {}->{} {}{}{}", dim, rst, gold, lport, rst, dim, rst, gold, upaddr, rst);

    loop {
        let (mut client, peer) = match listener.accept() {
            Ok(x) => x,
            Err(_) => {
                eprintln!("{}tcpproxy:{} accept failed", emb, rst);
                return 1;
            }
        };
        println!("{}tcpproxy:{} {}+{} {}{}{}", dim, rst, grn, rst, gold, peer, rst);

        let mut up = match TcpStream::connect_timeout(upaddr, DIAL_TIMEOUT) {
            Ok(s) => s,
            Err(_) => {
                eprintln!("{}tcpproxy:{} upstream {} unreachable -- dropping {}", emb, rst, upaddr, peer);
                continue; // client drops here; serve the next call
            }
        };

        let _ = netpump::splice(&mut client, &mut up, "tcpproxy");
        println!("{}tcpproxy:{} {}-{} {}{}{}", dim, rst, dim, rst, gold, peer, rst);
    }
}

/// A TCP port is 1..=65535 (netd's `announce`/`connect` need a concrete,
/// non-zero port).
fn parse_port(s: &str) -> Option<u16> {
    match s.parse::<u16>() {
        Ok(p) if p > 0 => Some(p),
        _ => None,
    }
}

/// `--color=auto` stub; true until a kernel TTY check lands.
fn stdout_is_console() -> bool {
    true
}
