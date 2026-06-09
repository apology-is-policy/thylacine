// sleep DURATION... -- block for the summed durations.
//
// Each DURATION is a number with an optional unit suffix s/m/h/d (default
// seconds; fractional allowed). Built on libthyla-rs::time::sleep (which
// rides SYS_TORPOR_WAIT with a timeout). The kernel caps a single wait at 1
// hour, so we loop in 1-hour chunks for longer totals.

#![no_std]
#![no_main]

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::env::{self, Args};
use libthyla_rs::eprintln;
use libthyla_rs::time::{sleep, Duration};

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

fn parse_dur(s: &str) -> Option<Duration> {
    let (num, mult) = match s.as_bytes().last() {
        Some(b's') => (&s[..s.len() - 1], 1.0),
        Some(b'm') => (&s[..s.len() - 1], 60.0),
        Some(b'h') => (&s[..s.len() - 1], 3600.0),
        Some(b'd') => (&s[..s.len() - 1], 86400.0),
        _ => (s, 1.0),
    };
    let v: f64 = num.parse().ok()?;
    let secs = v * mult;
    // Reject NaN/inf/negative AND any magnitude that would overflow
    // Duration::from_secs_f64 (which panics past ~u64::MAX seconds; a panic
    // would terminate the Proc). 9e15 s (~285M years) is far below the cap
    // and far above any real sleep.
    if !secs.is_finite() || secs < 0.0 || secs > 9.0e15 {
        return None;
    }
    Some(Duration::from_secs_f64(secs))
}

fn run(args: Args) -> i64 {
    let mut total = Duration::ZERO;
    let mut had = false;
    for op in args.operands() {
        had = true;
        let s = match core::str::from_utf8(op) {
            Ok(s) => s,
            Err(_) => {
                eprintln!("sleep: invalid operand");
                return 1;
            }
        };
        match parse_dur(s) {
            Some(d) => total += d,
            None => {
                eprintln!("sleep: invalid time interval '{}'", s);
                return 1;
            }
        }
    }
    if !had {
        eprintln!("sleep: missing operand");
        return 1;
    }

    let cap = Duration::from_secs(3600);
    let mut remaining = total;
    while remaining > Duration::ZERO {
        let chunk = if remaining > cap { cap } else { remaining };
        if let Err(e) = sleep(chunk) {
            eprintln!("sleep: {}", e);
            return 1;
        }
        remaining -= chunk;
    }
    0
}
