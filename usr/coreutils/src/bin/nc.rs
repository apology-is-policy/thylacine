// nc -- netcat: connect to (or listen on) a port and pump bytes between the
// connection and stdin/stdout. The universal network plumbing tool, and the
// simplest bulk LOAD generator for throughput work: a host `nc` feeding a guest
// `nc -l` (or the reverse) measures the real-NIC sustained transfer rate that
// netd's loopback micro-bench (netperf) deliberately excludes.
//
//   nc HOST PORT      -- active open: dial HOST:PORT, then pump.
//   nc -l PORT        -- passive open: announce *!PORT, accept ONE call, pump.
//   nc -u HOST PORT   -- UDP: connect to HOST:PORT, pump datagrams.
//
// The pumps (stdin <-> socket, half-duplex-buffered, POLLOUT under window
// backpressure for TCP; datagram-per-chunk for UDP) live in `coreutils::netpump`
// -- shared with `con`. The status banners (`-v`) use the Bonfire palette; the
// PAYLOAD on stdout is never colored (it's data a pipe consumes). netd owns the
// NIC (I-5); nc touches no hardware and reaches only the `/net` its territory
// grants (I-1/I-23/I-28).
//
// UDP is connect-only: a UDP server needs a bind-to-fixed-port API the native
// UdpSocket does not yet expose (DOC-GAP, see usr/apps/NET-APPS.md) -- so
// `nc -lu` is rejected; the guest is the UDP source, a host `nc -ul` the sink.

#![no_std]
#![no_main]

extern crate alloc;
use alloc::format;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use coreutils::color::{self, ColorMode};
use coreutils::{netpump, palette};
use libthyla_rs::env::{self, Args};
use libthyla_rs::eprintln;
use libthyla_rs::net::{self, Ipv4Addr, SocketAddrV4, TcpListener, TcpStream, UdpSocket};

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

const USAGE: &str = "\
usage: nc [-uv] [--color[=WHEN]] HOST PORT      (connect)
       nc -l [-v] [--color[=WHEN]] PORT         (listen, accept one connection)
  Pump bytes between a connection and stdin/stdout.
  -l              listen for one inbound TCP connection instead of dialing
  -u              use UDP datagrams instead of TCP (connect only)
  -v              print connection progress to stderr
  --color[=WHEN]  colorize -v/error status: always (default) | never | auto
  --help          show this help

Examples:
  echo -n hi | nc 127.0.0.1 7         # TCP: send 'hi', print the echo
  echo -n hi | nc -u 10.0.2.2 9       # UDP: send one datagram
  nc -l 9999 > out.bin                # save what the first peer sends
  nc -l 9999 < big.bin                # serve big.bin to the first peer
  # throughput: guest `nc -l 9999 > /dev/null` <- host `nc GUEST 9999 < big`
";

fn run(args: Args) -> i64 {
    if let Some(rc) = coreutils::usage::help_if_requested(args, USAGE) {
        return rc;
    }
    let mut listen = false;
    let mut udp = false;
    let mut verbose = false;
    let mut mode = ColorMode::Always;
    let mut pos: [&str; 2] = [""; 2];
    let mut npos = 0usize;
    let mut opts_done = false;

    let mut i = 1;
    while let Some(a) = args.get_str(i) {
        i += 1;
        if !opts_done {
            if a == "--" {
                opts_done = true;
                continue;
            }
            if a == "--color" {
                mode = ColorMode::Always;
                continue;
            }
            if let Some(w) = a.strip_prefix("--color=") {
                match ColorMode::parse_when(w) {
                    Some(m) => mode = m,
                    None => return coreutils::usage::die("nc", &format!("invalid --color value -- '{}'", w)),
                }
                continue;
            }
            // A short-flag cluster (`-luv`), but not a bare `-` (an operand).
            if a.len() > 1 && a.as_bytes()[0] == b'-' {
                for ch in a[1..].chars() {
                    match ch {
                        'l' => listen = true,
                        'u' => udp = true,
                        'v' => verbose = true,
                        _ => return coreutils::usage::die("nc", &format!("invalid option -- '{}'", ch)),
                    }
                }
                continue;
            }
        }
        if npos >= 2 {
            return coreutils::usage::die("nc", "too many operands");
        }
        pos[npos] = a;
        npos += 1;
    }

    if listen && udp {
        return coreutils::usage::die("nc", "UDP listen is unsupported (needs a UDP bind API -- see NET-APPS.md)");
    }

    let on = mode.resolve(stdout_is_console);

    if listen {
        if npos != 1 {
            return coreutils::usage::die("nc", "listen mode takes one PORT");
        }
        let port = match parse_port(pos[0]) {
            Some(p) => p,
            None => return coreutils::usage::die("nc", "invalid PORT"),
        };
        do_listen(port, verbose, on)
    } else {
        if npos != 2 {
            return coreutils::usage::die("nc", "connect mode takes HOST PORT");
        }
        let port = match parse_port(pos[1]) {
            Some(p) => p,
            None => return coreutils::usage::die("nc", "invalid PORT"),
        };
        do_connect(pos[0], port, udp, verbose, on)
    }
}

/// A TCP/UDP port is 1..=65535 (netd's `announce`/`connect` need a concrete,
/// non-zero port).
fn parse_port(s: &str) -> Option<u16> {
    match s.parse::<u16>() {
        Ok(p) if p > 0 => Some(p),
        _ => None,
    }
}

fn do_connect(host: &str, port: u16, udp: bool, verbose: bool, on: bool) -> i64 {
    let dim = color::col(palette::DIM, on);
    let rst = color::reset(on);
    let gold = color::col(palette::GOLD, on);
    let grn = color::col(palette::GREEN, on);
    let emb = color::col(palette::EMBER, on);

    let addr = match net::resolve(host, port) {
        Ok(a) => a,
        Err(_) => {
            eprintln!("{}nc:{} cannot resolve '{}'", emb, rst, host);
            return 1;
        }
    };

    if udp {
        let mut sock = match UdpSocket::connect(addr) {
            Ok(s) => s,
            Err(_) => {
                eprintln!("{}nc:{} UDP connect to {} failed", emb, rst, addr);
                return 1;
            }
        };
        if verbose {
            eprintln!("{}nc:{} {}UDP connected{} to {}{}{}", dim, rst, grn, rst, gold, addr, rst);
        }
        return netpump::udp_pump(&mut sock, "nc");
    }

    let mut stream = match TcpStream::connect(addr) {
        Ok(s) => s,
        Err(_) => {
            eprintln!("{}nc:{} connect to {} failed", emb, rst, addr);
            return 1;
        }
    };
    if verbose {
        eprintln!("{}nc:{} {}connected{} to {}{}{}", dim, rst, grn, rst, gold, addr, rst);
    }
    netpump::stdio_pump(&mut stream, "nc")
}

fn do_listen(port: u16, verbose: bool, on: bool) -> i64 {
    let dim = color::col(palette::DIM, on);
    let rst = color::reset(on);
    let gold = color::col(palette::GOLD, on);
    let grn = color::col(palette::GREEN, on);
    let emb = color::col(palette::EMBER, on);

    let bind = SocketAddrV4::new(Ipv4Addr::UNSPECIFIED, port);
    let listener = match TcpListener::bind(bind) {
        Ok(l) => l,
        Err(_) => {
            eprintln!("{}nc:{} cannot announce port {} (is netd running?)", emb, rst, port);
            return 1;
        }
    };
    if verbose {
        eprintln!("{}nc:{} listening on port {}{}{}", dim, rst, gold, port, rst);
    }
    let (mut stream, peer) = match listener.accept() {
        Ok(x) => x,
        Err(_) => {
            eprintln!("{}nc:{} accept failed", emb, rst);
            return 1;
        }
    };
    if verbose {
        eprintln!("{}nc:{} {}connection{} from {}{}{}", dim, rst, grn, rst, gold, peer, rst);
    }
    netpump::stdio_pump(&mut stream, "nc")
}

/// `--color=auto` stub; true until a kernel TTY check lands (matches the
/// coreutils presentation tools).
fn stdout_is_console() -> bool {
    true
}
