// con -- the Plan 9 interactive connect: resolve a dial string through /net/cs
// and bridge the console to a TCP service. Where `nc` is the raw HOST:PORT
// plumbing tool (and a server, with `-l`), `con` is the interactive client --
// it takes a Plan 9 dial string (service names resolve through /net/cs) and
// bridges stdin/stdout to the connection so you can talk to a line protocol
// (echo, SMTP, an HTTP service) by hand. Type Ctrl-D to half-close the send
// side; the session ends when the peer closes.
//
//   con HOST PORT             -- tcp!HOST!PORT
//   con [tcp!]HOST!SERVICE    -- a Plan 9 dial string (proto defaults to tcp)
//
// The pump (stdin <-> socket, half-duplex-buffered, POLLOUT under backpressure)
// is shared with `nc` (coreutils::netpump). The status banners use the Bonfire
// palette (--color=never for plain). netd owns the NIC (I-5); con touches no
// hardware and reaches only the `/net` its territory grants (I-1/I-23/I-28).

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
use libthyla_rs::time::Duration;
use libthyla_rs::eprintln;

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

const USAGE: &str = "\
usage: con [--color[=WHEN]] HOST PORT
       con [--color[=WHEN]] [tcp!]HOST!SERVICE
  Open an interactive TCP connection (resolved via /net/cs) and bridge it to
  the console. Ctrl-D half-closes the send side; the peer's close ends it.
  --color[=WHEN]  colorize status: always (default) | never | auto
  --help          show this help

Examples:
  con 127.0.0.1 7            # talk to the in-guest echo
  con tcp!example.com!http   # then type:  GET / HTTP/1.0  <enter> <enter>
";

// Bounded connect so an unreachable host fails fast for an interactive user.
const CONNECT_TIMEOUT: Duration = Duration::from_secs(10);

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
                None => return coreutils::usage::die("con", &format!("invalid --color value -- '{}'", w)),
            }
            continue;
        }
        if a == "--" {
            continue;
        }
        if npos >= 2 {
            return coreutils::usage::die("con", "too many operands");
        }
        pos[npos] = a;
        npos += 1;
    }

    let dialstr = match build_dialstring(&pos[..npos]) {
        Ok(s) => s,
        Err(msg) => return coreutils::usage::die("con", msg),
    };

    let on = mode.resolve(stdout_is_console);
    let dim = color::col(palette::DIM, on);
    let rst = color::reset(on);
    let gold = color::col(palette::GOLD, on);
    let grn = color::col(palette::GREEN, on);
    let emb = color::col(palette::EMBER, on);

    let addr = match netpump::cs_resolve(&dialstr) {
        Ok(a) => a,
        Err(_) => {
            eprintln!("{}con:{} cannot resolve '{}' (via /net/cs)", emb, rst, dialstr);
            return 1;
        }
    };

    let mut stream = match TcpStream::connect_timeout(addr, CONNECT_TIMEOUT) {
        Ok(s) => s,
        Err(Error::TimedOut) => {
            eprintln!("{}con:{} {} unreachable (no answer in {}s)", emb, rst, addr, CONNECT_TIMEOUT.as_secs());
            return 1;
        }
        Err(_) => {
            eprintln!("{}con:{} connection to {} refused", emb, rst, addr);
            return 1;
        }
    };
    eprintln!("{}con:{} {}connected{} to {}{}{} {}(Ctrl-D to close){}", dim, rst, grn, rst, gold, addr, rst, dim, rst);

    let rc = netpump::stdio_pump(&mut stream, "con");
    eprintln!("{}con:{} connection closed", dim, rst);
    rc
}

/// Build a Plan 9 dial string `proto!host!service` from the operands (see
/// `dial`): two operands -> `tcp!host!service`; one with `!` -> as-is (a bare
/// `host!service` gets `tcp!`); otherwise a usage error.
fn build_dialstring(operands: &[&str]) -> core::result::Result<String, &'static str> {
    match operands.len() {
        2 => Ok(format!("tcp!{}!{}", operands[0], operands[1])),
        1 => {
            let s = operands[0];
            match s.split('!').count() {
                3.. => Ok(String::from(s)),
                2 => Ok(format!("tcp!{}", s)),
                _ => Err("a single operand must be a dial string (HOST!SERVICE)"),
            }
        }
        _ => Err("need HOST PORT or a dial string"),
    }
}

/// `--color=auto` stub; true until a kernel TTY check lands (matches the
/// coreutils presentation tools).
fn stdout_is_console() -> bool {
    true
}
