// dial -- the Plan 9 connection tester: resolve a dial string through netd's
// connection server (/net/cs) and open a TCP connection, reporting the resolved
// address and the round-trip time to ESTABLISHED. The canonical "can I reach
// X?", and Thylacine-distinctive: it showcases /net/cs, which resolves both the
// host (numeric -> ndb-static -> DNS) AND the service name (http -> 80) in one
// query.
//
//   dial HOST PORT             -- tcp!HOST!PORT
//   dial [tcp!]HOST!SERVICE    -- a Plan 9 dial string (proto defaults to tcp)
//
// A presentation tool -> the Bonfire palette is on by default (--color=never for
// a plain report). The connect is bounded (a few seconds), so an unreachable host
// reports "unreachable" rather than hanging on netd's 15 s connect timeout. netd
// owns the NIC (I-5); dial touches no hardware and reaches only the `/net` its
// territory grants (I-1/I-23/I-28).

#![no_std]
#![no_main]

extern crate alloc;
use alloc::format;
use alloc::string::String;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use coreutils::color::{self, ColorMode};
use coreutils::{netpump, palette};
use libthyla_rs::env::{self, Args};
use libthyla_rs::err::Error;
use libthyla_rs::net::TcpStream;
use libthyla_rs::time::{Duration, Instant};
use libthyla_rs::{eprintln, println};

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

const USAGE: &str = "\
usage: dial [--color[=WHEN]] HOST PORT
       dial [--color[=WHEN]] [tcp!]HOST!SERVICE
  Resolve through /net/cs and open a TCP connection, reporting the address
  and the round-trip time to ESTABLISHED.
  --color[=WHEN]  colorize: always (default) | never | auto
  --help          show this help

Examples:
  dial 127.0.0.1 7            # the in-guest echo (loopback)
  dial 10.0.2.2 79            # the slirp gateway, port 79
  dial tcp!example.com!http   # resolve host + service name via /net/cs
";

// Bounded connect: an unreachable host reports a timeout instead of blocking on
// netd's 15 s connect deadline.
const CONNECT_TIMEOUT: Duration = Duration::from_secs(5);

fn run(args: Args) -> i64 {
    if let Some(rc) = coreutils::usage::help_if_requested(args, USAGE) {
        return rc;
    }

    let mut mode = ColorMode::Always;
    let mut pos: [&str; 2] = [""; 2];
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
                None => return coreutils::usage::die("dial", &format!("invalid --color value -- '{}'", w)),
            }
            continue;
        }
        if a == "--" {
            continue;
        }
        if npos >= 2 {
            return coreutils::usage::die("dial", "too many operands");
        }
        pos[npos] = a;
        npos += 1;
    }

    let dialstr = match build_dialstring(&pos[..npos]) {
        Ok(s) => s,
        Err(msg) => return coreutils::usage::die("dial", msg),
    };

    let on = mode.resolve(stdout_is_console);
    let dim = color::col(palette::DIM, on);
    let rst = color::reset(on);
    let gold = color::col(palette::GOLD, on);
    let grn = color::col(palette::GREEN, on);
    let vio = color::col(palette::VIOLET, on);
    let emb = color::col(palette::EMBER, on);

    let addr = match netpump::cs_resolve(&dialstr) {
        Ok(a) => a,
        Err(_) => {
            eprintln!("{}dial:{} cannot resolve '{}' (via /net/cs)", emb, rst, dialstr);
            return 1;
        }
    };
    println!("{}dial:{} {}{}{} {}->{} {}{}{} {}(via /net/cs){}", dim, rst, vio, dialstr, rst, dim, rst, gold, addr, rst, dim, rst);

    let t0 = Instant::now();
    match TcpStream::connect_timeout(addr, CONNECT_TIMEOUT) {
        Ok(_stream) => {
            let rtt = t0.elapsed();
            println!("{}dial:{} {}connected{} to {}{}{} in {}{}{}", dim, rst, grn, rst, gold, addr, rst, grn, FmtMs(rtt), rst);
            0
        }
        Err(Error::TimedOut) => {
            eprintln!("{}dial:{} {} unreachable (no answer in {}s)", emb, rst, addr, CONNECT_TIMEOUT.as_secs());
            1
        }
        Err(_) => {
            eprintln!("{}dial:{} connection to {} refused", emb, rst, addr);
            1
        }
    }
}

/// Build a Plan 9 dial string `proto!host!service` from the operands:
///   - two operands -> `tcp!{host}!{service}`,
///   - one operand with `!` -> used as-is (a bare `host!service` gets `tcp!`),
///   - anything else -> a usage error.
fn build_dialstring(operands: &[&str]) -> core::result::Result<String, &'static str> {
    match operands.len() {
        2 => Ok(format!("tcp!{}!{}", operands[0], operands[1])),
        1 => {
            let s = operands[0];
            // proto!host!service = 3 fields; host!service = 2.
            match s.split('!').count() {
                3.. => Ok(String::from(s)),
                2 => Ok(format!("tcp!{}", s)),
                _ => Err("a single operand must be a dial string (HOST!SERVICE)"),
            }
        }
        _ => Err("need HOST PORT or a dial string"),
    }
}

/// `--color=auto` stub (see the SYS_FD_DEVCLASS spec); true until a kernel TTY
/// check lands, matching the coreutils presentation tools.
fn stdout_is_console() -> bool {
    true
}

/// Format a `Duration` as `X.YYY ms` (the ping/dial convention); sub-millisecond
/// RTTs (the loopback case) render as `0.NNN ms`.
struct FmtMs(Duration);

impl core::fmt::Display for FmtMs {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        let us = self.0.as_micros();
        write!(f, "{}.{:03} ms", us / 1000, us % 1000)
    }
}
