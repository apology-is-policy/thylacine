// ping -- send ICMP echo requests to a host and report the round-trip time,
// over netd's `/net/icmp` tree (a native libthyla-rs `net::IcmpSocket`). The
// name is resolved via /net/cs (numeric -> ndb-static -> DNS, like nslookup);
// each request waits up to one second for the matching reply (a never-answering
// host times out, it does not hang). RTT is measured with the LS-K monotonic
// clock.
//
// A presentation tool -> the Bonfire palette by default (--color=never for
// plain), with the closing statistics in a boxed card. netd owns the NIC (I-5),
// so ping touches no hardware and reaches only the `/net` its territory grants
// (I-1/I-23/I-28). A loopback target (127.0.0.1) auto-answers in-guest (net-8a);
// an external echo is host-dependent (slirp may answer internally or proxy to a
// host ping socket that the host may forbid) -- best-effort, never a tool failure.

#![no_std]
#![no_main]

extern crate alloc;
use alloc::format;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use coreutils::color::{self, ColorMode};
use coreutils::{palette, ui};
use libthyla_rs::env::{self, Args};
use libthyla_rs::net::{self, IcmpSocket};
use libthyla_rs::poll::{PollEvents, PollSet, PollTimeout};
use libthyla_rs::time::{self, Duration, Instant};
use libthyla_rs::{eprintln, println};

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

const USAGE: &str = "\
usage: ping [-c COUNT] [--color[=WHEN]] HOST
  Send ICMP echo requests to HOST and report the round-trip time.
  -c COUNT        stop after COUNT requests (default 4)
  --color[=WHEN]  colorize: always (default) | never | auto
  --help          show this help

Examples:
  ping 127.0.0.1        # the in-guest loopback (always answers)
  ping 10.0.2.2         # the slirp gateway (host-dependent)
  ping example.com      # resolved via /net/cs
";

// The ICMP echo payload: 56 data bytes (the classic `ping` default), an
// incrementing pattern the reply echoes back verbatim.
const PAYLOAD_LEN: usize = 56;
// Per-request reply timeout. A host that never answers times out here rather
// than blocking the tool forever.
const REPLY_TIMEOUT_MS: u32 = 1000;

fn run(args: Args) -> i64 {
    if let Some(rc) = coreutils::usage::help_if_requested(args, USAGE) {
        return rc;
    }

    let mut count: u32 = 4;
    let mut host: Option<&str> = None;
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
                None => return coreutils::usage::die("ping", "invalid --color value (use always/never/auto)"),
            }
            continue;
        }
        if a == "-c" {
            let c = match args.get_str(i) {
                Some(c) => c,
                None => return coreutils::usage::die("ping", "-c needs a count"),
            };
            i += 1;
            count = match c.parse::<u32>() {
                Ok(n) if n > 0 => n,
                _ => return coreutils::usage::die("ping", "invalid count"),
            };
            continue;
        }
        if a == "--" {
            continue;
        }
        if host.is_none() {
            host = Some(a);
        } else {
            return coreutils::usage::die("ping", "too many operands");
        }
    }
    let host = match host {
        Some(h) => h,
        None => return coreutils::usage::die("ping", "missing HOST operand"),
    };

    let on = mode.resolve(stdout_is_console);
    let dim = color::col(palette::DIM, on);
    let rst = color::reset(on);
    let gold = color::col(palette::GOLD, on);
    let grn = color::col(palette::GREEN, on);
    let emb = color::col(palette::EMBER, on);

    let ip = match net::resolve(host, 80) {
        Ok(addr) => addr.ip(),
        Err(_) => {
            eprintln!("{}ping:{} cannot resolve '{}'", emb, rst, host);
            return 1;
        }
    };

    let mut sock = match IcmpSocket::connect(ip) {
        Ok(s) => s,
        Err(_) => {
            eprintln!("{}ping:{} cannot open /net/icmp (is netd running?)", emb, rst);
            return 1;
        }
    };
    let ready = match sock.ready_fd() {
        Ok(r) => r,
        Err(_) => {
            eprintln!("{}ping:{} cannot open the readiness fd", emb, rst);
            return 1;
        }
    };
    let ready_raw = ready.as_raw_fd();

    println!("{}PING{} {}{} ({}){}: {} data bytes", dim, rst, gold, host, ip, rst, PAYLOAD_LEN);

    let mut payload = [0u8; PAYLOAD_LEN];
    for (k, b) in payload.iter_mut().enumerate() {
        *b = k as u8;
    }

    let mut sent: u32 = 0;
    let mut received: u32 = 0;
    let mut seq: u32 = 0;
    while seq < count {
        let t0 = Instant::now();
        if sock.send(&payload).is_err() {
            eprintln!("{}ping:{} send failed for icmp_seq {}", emb, rst, seq);
            break;
        }
        sent += 1;

        let mut ps = PollSet::new();
        ps.add_raw(ready_raw, PollEvents::READ);
        let got = match ps.poll(PollTimeout::Millis(REPLY_TIMEOUT_MS)) {
            Ok(results) => results.into_iter().any(|e| e.is_readable()),
            Err(_) => false,
        };
        if got {
            let mut buf = [0u8; 256];
            match sock.recv(&mut buf) {
                Ok(k) => {
                    let rtt = t0.elapsed();
                    received += 1;
                    println!(
                        "{} bytes from {}{}{}: icmp_seq={} time={}{}{}",
                        k, gold, ip, rst, seq, grn, FmtMs(rtt), rst
                    );
                }
                Err(_) => println!("{}Request timeout for icmp_seq {}{}", emb, seq, rst),
            }
        } else {
            println!("{}Request timeout for icmp_seq {}{}", emb, seq, rst);
        }

        seq += 1;
        if seq < count {
            let _ = time::sleep(Duration::from_secs(1));
        }
    }

    let loss = if sent > 0 {
        ((sent - received) * 100) / sent
    } else {
        100
    };
    let loss_color = if loss == 0 { grn } else { emb };
    let row = ui::Row::new(
        format!("{} transmitted, {} received, {}% loss", sent, received, loss),
        // Color each "value word" pair as ONE contiguous span: a consumer that
        // substring-matches a marker like "N received" must not find a color
        // RESET wedged between the count and the word. (The joey net-util boot
        // probe greps ping's piped output for "1 received".)
        format!(
            "{}{} transmitted{}, {}{} received{}, {}{}% loss{}",
            gold, sent, rst, gold, received, rst, loss_color, loss, rst
        ),
    );
    ui::card("ping statistics", host, &[row], on);

    // Exit 0 if any reply arrived (the host is reachable), 1 otherwise.
    if received > 0 {
        0
    } else {
        1
    }
}

/// Format a `Duration` as `X.YYY ms` (the ping convention); sub-millisecond RTTs
/// (the loopback case) render as `0.NNN ms`.
struct FmtMs(Duration);

impl core::fmt::Display for FmtMs {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        let us = self.0.as_micros();
        write!(f, "{}.{:03} ms", us / 1000, us % 1000)
    }
}

/// `--color=auto` stub; true until a kernel TTY check lands.
fn stdout_is_console() -> bool {
    true
}
