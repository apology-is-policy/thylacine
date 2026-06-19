// ping -- send ICMP echo requests to a host and report the round-trip time,
// over netd's `/net/icmp` tree (a native libthyla-rs `net::IcmpSocket`). The
// name is resolved via /net/cs (numeric -> ndb-static -> DNS, like nslookup);
// each request waits up to one second for the matching reply (a never-answering
// host times out, it does not hang). RTT is measured with the LS-K monotonic
// clock.
//
// netd owns the NIC (I-5), so ping touches no hardware and reaches only the
// `/net` its territory grants (I-1/I-23/I-28). A loopback target (127.0.0.1)
// auto-answers in-guest (net-8a); an external echo is host-dependent (slirp may
// answer internally or proxy to a host ping socket that the host may forbid) --
// best-effort, never a failure of the tool.

#![no_std]
#![no_main]

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

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
usage: ping [-c COUNT] HOST
  Send ICMP echo requests to HOST and report the round-trip time.
  -c COUNT  stop after COUNT requests (default 4)
  --help    show this help

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

    // Parse `[-c COUNT] HOST`. A bare positional is the host; `-c` consumes the
    // next token as the count.
    let mut count: u32 = 4;
    let mut host: Option<&str> = None;
    let mut i = 1;
    while i < args.len() {
        let a = match args.get_str(i) {
            Some(a) => a,
            None => return coreutils::usage::die("ping", "invalid argument"),
        };
        if a == "-c" {
            i += 1;
            let c = match args.get_str(i) {
                Some(c) => c,
                None => return coreutils::usage::die("ping", "-c needs a count"),
            };
            count = match c.parse::<u32>() {
                Ok(n) if n > 0 => n,
                _ => return coreutils::usage::die("ping", "invalid count"),
            };
        } else if host.is_none() {
            host = Some(a);
        } else {
            return coreutils::usage::die("ping", "too many operands");
        }
        i += 1;
    }
    let host = match host {
        Some(h) => h,
        None => return coreutils::usage::die("ping", "missing HOST operand"),
    };

    // Resolve the name to an IPv4 address (the port is immaterial for ICMP --
    // it is portless; `resolve` just needs a valid service to dial cs with).
    let ip = match net::resolve(host, 80) {
        Ok(addr) => addr.ip(),
        Err(_) => {
            eprintln!("ping: cannot resolve '{}'", host);
            return 1;
        }
    };

    let mut sock = match IcmpSocket::connect(ip) {
        Ok(s) => s,
        Err(_) => {
            eprintln!("ping: cannot open /net/icmp (is netd running?)");
            return 1;
        }
    };
    // The QTPOLL readiness sibling -- opened once, re-polled each request.
    let ready = match sock.ready_fd() {
        Ok(r) => r,
        Err(_) => {
            eprintln!("ping: cannot open the readiness fd");
            return 1;
        }
    };
    let ready_raw = ready.as_raw_fd();

    println!("PING {} ({}): {} data bytes", host, ip, PAYLOAD_LEN);

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
            eprintln!("ping: send failed for icmp_seq {}", seq);
            break;
        }
        sent += 1;

        // Bounded wait for the reply: poll the readiness fd, then drain only if
        // POLLIN fired (the reply is queued).
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
                        "{} bytes from {}: icmp_seq={} time={}",
                        k,
                        ip,
                        seq,
                        FmtMs(rtt)
                    );
                }
                Err(_) => println!("Request timeout for icmp_seq {}", seq),
            }
        } else {
            println!("Request timeout for icmp_seq {}", seq);
        }

        seq += 1;
        // One-second inter-request gap (classic ping cadence), skipped after the
        // last request so a `-c 1` proof does not idle.
        if seq < count {
            let _ = time::sleep(Duration::from_secs(1));
        }
    }

    let loss = if sent > 0 {
        ((sent - received) * 100) / sent
    } else {
        100
    };
    println!("--- {} ping statistics ---", host);
    println!(
        "{} packets transmitted, {} received, {}% packet loss",
        sent, received, loss
    );

    // Exit 0 if any reply arrived (the host is reachable), 1 otherwise -- the
    // conventional ping exit status.
    if received > 0 {
        0
    } else {
        1
    }
}

/// Format a `Duration` as `X.YYY ms` (millisecond.microsecond, the `ping`
/// convention). Sub-millisecond RTTs (the loopback case) render as `0.NNN ms`.
struct FmtMs(Duration);

impl core::fmt::Display for FmtMs {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        let us = self.0.as_micros();
        write!(f, "{}.{:03} ms", us / 1000, us % 1000)
    }
}
