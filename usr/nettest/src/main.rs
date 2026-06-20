// nettest -- the host-paired sustained-bandwidth bench: the OVER-THE-WIRE
// throughput number netd's loopback micro-bench (netperf) cannot produce.
// netperf isolates the per-op stack CPU on a resident 127.0.0.1 loopback (no
// NIC); nettest measures the real-NIC rate -- the figure that includes the
// RX-wake floor and the felt end-to-end win the throughput arc is chasing.
//
//   nettest -s [-p PORT]                 -- sink: accept one connection, drain
//                                           to EOF, report received MB/s.
//   nettest -c HOST [-p PORT] [-n MB]    -- source: send MB megabytes to
//                                           HOST:PORT as fast as the window
//                                           allows, report sent MB/s.
//
// Host-paired: a host load tool (`nc`, `iperf`, `dd | nc`) drives one end while
// nettest measures the other. The guest is the source for a NIC-out test and the
// sink for a NIC-in test. Rate math is integer-only (the no_std float path is not
// linked). netd owns the NIC (I-5); nettest touches no hardware and reaches only
// the `/net` its territory grants (I-1/I-23/I-28).
//
// Throughput only at v1.0; a wire-RTT percentile mode (p50/p99, the netperf-M1
// analog over the NIC) needs a paired echo peer and is a follow-on.

#![no_std]
#![no_main]

extern crate alloc;
use alloc::format;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use coreutils::color::{self, ColorMode};
use coreutils::{netpump, palette, ui};
use libthyla_rs::env::{self, Args};
use libthyla_rs::net::{self, Ipv4Addr, SocketAddrV4, TcpListener, TcpStream};
use libthyla_rs::time::{Duration, Instant};
use libthyla_rs::{eprintln, println};

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

const USAGE: &str = "\
usage: nettest -s [-p PORT] [--color[=WHEN]]
       nettest -c HOST [-p PORT] [-n MB] [--color[=WHEN]]
  Measure sustained TCP throughput over the real NIC (the over-the-wire number
  netperf's loopback design omits).
  -s              sink: accept one connection, drain to EOF, report MB/s
  -c HOST         source: send to HOST and report MB/s
  -p PORT         port (default 5555)
  -n MB           source: megabytes to send (default 64)
  --color[=WHEN]  colorize the report: always (default) | never | auto
  --help          show this help

Examples:
  nettest -s                          # guest sink; drive it host-side:
  #   host:  head -c 100000000 /dev/zero | nc <guest> 5555
  nettest -c 10.0.2.2 -n 128          # guest source -> a host sink on :5555
";

const CHUNK: usize = 16 * 1024;
const DEFAULT_PORT: u16 = 5555;
const DEFAULT_MB: u64 = 64;

fn run(args: Args) -> i64 {
    if let Some(rc) = coreutils::usage::help_if_requested(args, USAGE) {
        return rc;
    }

    let mut mode = ColorMode::Always;
    let mut server = false;
    let mut host: Option<&str> = None;
    let mut port = DEFAULT_PORT;
    let mut mb = DEFAULT_MB;

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
                None => return coreutils::usage::die("nettest", &format!("invalid --color value -- '{}'", w)),
            }
            continue;
        }
        match a {
            "-s" => server = true,
            "-c" => match args.get_str(i) {
                Some(h) => {
                    host = Some(h);
                    i += 1;
                }
                None => return coreutils::usage::die("nettest", "-c needs a HOST"),
            },
            "-p" => match args.get_str(i).and_then(parse_port) {
                Some(p) => {
                    port = p;
                    i += 1;
                }
                None => return coreutils::usage::die("nettest", "-p needs a port (1..=65535)"),
            },
            "-n" => match args.get_str(i).and_then(|s| s.parse::<u64>().ok()) {
                Some(n) if n > 0 => {
                    mb = n;
                    i += 1;
                }
                _ => return coreutils::usage::die("nettest", "-n needs a positive megabyte count"),
            },
            _ => return coreutils::usage::die("nettest", &format!("unexpected argument '{}'", a)),
        }
    }

    if server == host.is_some() {
        return coreutils::usage::die("nettest", "use exactly one of -s or -c HOST");
    }

    let on = mode.resolve(stdout_is_console);
    if server {
        sink(port, on)
    } else {
        source(host.unwrap(), port, mb, on)
    }
}

/// Sink: accept one connection and drain it to EOF, timing from the first byte
/// (so idle time before the peer starts sending is excluded).
fn sink(port: u16, on: bool) -> i64 {
    let dim = color::col(palette::DIM, on);
    let rst = color::reset(on);
    let gold = color::col(palette::GOLD, on);
    let emb = color::col(palette::EMBER, on);

    let listener = match TcpListener::bind(SocketAddrV4::new(Ipv4Addr::UNSPECIFIED, port)) {
        Ok(l) => l,
        Err(_) => {
            eprintln!("{}nettest:{} cannot announce port {} (is netd running?)", emb, rst, port);
            return 1;
        }
    };
    println!("{}nettest:{} sink on {}*:{}{} -- drive it host-side", dim, rst, gold, port, rst);

    let (mut stream, peer) = match listener.accept() {
        Ok(x) => x,
        Err(_) => {
            eprintln!("{}nettest:{} accept failed", emb, rst);
            return 1;
        }
    };
    println!("{}nettest:{} receiving from {}{}{}", dim, rst, gold, peer, rst);

    let mut buf = [0u8; CHUNK];
    let mut total = 0u64;
    let mut t0: Option<Instant> = None;
    loop {
        match stream.read(&mut buf) {
            Ok(0) => break,
            Ok(n) => {
                if t0.is_none() {
                    t0 = Some(Instant::now());
                }
                total += n as u64;
            }
            Err(_) => break,
        }
    }
    let dt = t0.map(|t| t.elapsed()).unwrap_or(Duration::from_micros(1));
    ui::rate_card("nettest", "received", total, dt, on);
    0
}

/// Source: connect and send `mb` megabytes as fast as the window allows, using
/// the backpressure-aware send_all, then half-close and report.
fn source(host: &str, port: u16, mb: u64, on: bool) -> i64 {
    let dim = color::col(palette::DIM, on);
    let rst = color::reset(on);
    let gold = color::col(palette::GOLD, on);
    let emb = color::col(palette::EMBER, on);

    let addr = match net::resolve(host, port) {
        Ok(a) => a,
        Err(_) => {
            eprintln!("{}nettest:{} cannot resolve '{}'", emb, rst, host);
            return 1;
        }
    };
    let mut stream = match TcpStream::connect(addr) {
        Ok(s) => s,
        Err(_) => {
            eprintln!("{}nettest:{} connect to {} failed", emb, rst, addr);
            return 1;
        }
    };
    let ready = match stream.ready_fd() {
        Ok(r) => r,
        Err(_) => {
            eprintln!("{}nettest:{} cannot open the readiness fd", emb, rst);
            return 1;
        }
    };
    let rfd = ready.as_raw_fd();
    println!("{}nettest:{} sending {}{}{} MB to {}{}{}", dim, rst, gold, mb, rst, gold, addr, rst);

    let buf = [0x5au8; CHUNK];
    let target = mb * 1024 * 1024;
    let mut sent = 0u64;
    let t0 = Instant::now();
    while sent < target {
        let want = core::cmp::min(CHUNK as u64, target - sent) as usize;
        if !netpump::send_all(&mut stream, &buf[..want], rfd) {
            eprintln!("{}nettest:{} send failed at {} bytes", emb, rst, sent);
            break;
        }
        sent += want as u64;
    }
    let _ = stream.shutdown(); // FIN so the peer's sink sees EOF
    let dt = t0.elapsed();
    ui::rate_card("nettest", "sent", sent, dt, on);
    0
}

/// A TCP port is 1..=65535.
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
