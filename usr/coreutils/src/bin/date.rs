// date [+%s] -- print the current wall-clock time (CLOCK_REALTIME), always UTC
// (there is no timezone database at v1.0).
//
//   default : "Www YYYY-MM-DD HH:MM:SS UTC"
//   +%s     : the Unix epoch in seconds
//
// Full strftime (`+%FORMAT`) and setting the clock (`date -s`) are v1.x seams
// (LS-9 / ARCH §22.6). The wall clock is the PL031 RTC boot anchor; on a
// platform with no RTC it reads 1970 + uptime (the fail-soft signal).

#![no_std]
#![no_main]

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use core::time::Duration;
use libthyla_rs::env::{self, Args};
use libthyla_rs::time::SystemTime;
use libthyla_rs::{eprintln, println};

const WDAY: [&str; 7] = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"];

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

const USAGE: &str = "\
usage: date [+%s]
  Print the current UTC time. With +%s, print the Unix epoch in seconds.
  --help  show this help

Examples:
  date        # Www YYYY-MM-DD HH:MM:SS UTC
  date +%s    # seconds since 1970-01-01
";

fn run(args: Args) -> i64 {
    if let Some(rc) = coreutils::usage::help_if_requested(args, USAGE) {
        return rc;
    }

    let mut want_epoch = false;
    let mut idx = 1;
    while let Some(a) = args.get_str(idx) {
        if a == "+%s" {
            want_epoch = true;
        } else if a.starts_with('+') {
            eprintln!("date: unsupported format '{}' (v1.0: no operand or +%s)", a);
            return 1;
        } else {
            eprintln!("date: setting the clock is unsupported at v1.0");
            return 1;
        }
        idx += 1;
    }

    let secs = SystemTime::now()
        .duration_since(SystemTime::UNIX_EPOCH)
        .unwrap_or(Duration::ZERO)
        .as_secs();

    if want_epoch {
        println!("{}", secs);
        return 0;
    }

    // Civil date from Unix seconds (UTC). Howard Hinnant's civil_from_days,
    // exact across the full proleptic Gregorian range. days = whole days since
    // the epoch; the era arithmetic folds the 400-year leap cycle.
    let days = (secs / 86400) as i64;
    let sod = secs % 86400;
    let (hh, mm, ss) = (sod / 3600, (sod % 3600) / 60, sod % 60);
    let wday = WDAY[(((days % 7) + 4 + 7) % 7) as usize]; // 1970-01-01 was Thu (4)

    let z = days + 719468;
    let era = (if z >= 0 { z } else { z - 146096 }) / 146097;
    let doe = z - era * 146097; // [0, 146096]
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; // [0, 399]
    let y = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100); // [0, 365]
    let mp = (5 * doy + 2) / 153; // [0, 11]
    let d = doy - (153 * mp + 2) / 5 + 1; // [1, 31]
    let m = if mp < 10 { mp + 3 } else { mp - 9 }; // [1, 12]
    let year = y + if m <= 2 { 1 } else { 0 };

    println!(
        "{} {:04}-{:02}-{:02} {:02}:{:02}:{:02} UTC",
        wday, year, m, d, hh, mm, ss
    );
    0
}
