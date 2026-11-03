// touch [-acm] [-r REF | -t STAMP | -d DATE] FILE... -- set file times,
// creating a FILE that does not exist (no truncate).
//
// With no time operand both times go to the file server's clock: POSIX's
// null-times touch, which write permission on the FILE allows. An explicit
// time, or one time alone, needs the FILE's owner. A FILE that cannot be
// opened for writing (a directory, a read-only file the caller owns) still has
// its times set through the path; only when that fails too does the open's
// error stand. Times are UTC: there is no timezone database.

#![no_std]
#![no_main]

extern crate alloc;
use alloc::format;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use coreutils::stamp;
use coreutils::usage;
use libthyla_rs::env::{self, Args};
use libthyla_rs::err::Error;
use libthyla_rs::eprintln;
use libthyla_rs::fs::{self, FileTimes, OpenOptions};
use libthyla_rs::time::SystemTime;

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

const USAGE: &str = "\
usage: touch [-acm] [-r REF | -t STAMP | -d DATE] FILE...
  Set each FILE's access and modification times to now, creating an empty
  FILE where none exists.
  -a        change only the access time
  -m        change only the modification time
  -c        do not create a FILE that does not exist
  -r REF    use REF's times instead of now
  -t STAMP  use [[CC]YY]MMDDhhmm[.SS], in UTC
  -d DATE   use YYYY-MM-DDThh:mm:SS[.frac][Z], in UTC
  --help    show this help

Examples:
  touch newfile                      # create, or set both times to now
  touch -m -t 202510221200 notes     # modification time only, 2025-10-22 12:00
  touch -r old.c new.c               # give new.c the times of old.c
";

fn run(args: Args) -> i64 {
    if let Some(rc) = usage::help_if_requested(args, USAGE) {
        return rc;
    }

    let mut only_a = false;
    let mut only_m = false;
    let mut no_create = false;
    // (atime, mtime) from -r / -t / -d; None = the server's clock.
    let mut given: Option<(SystemTime, SystemTime)> = None;
    let mut idx = 1;
    while let Some(a) = args.get_str(idx) {
        if a == "--" {
            idx += 1;
            break;
        }
        if !a.starts_with('-') || a.len() == 1 {
            break;
        }
        idx += 1;
        let flags = &a[1..];
        for (pos, ch) in flags.char_indices() {
            match ch {
                'a' => only_a = true,
                'm' => only_m = true,
                'c' => no_create = true,
                'r' | 't' | 'd' => {
                    // The operand is the rest of this word, else the next word.
                    let rest = &flags[pos + 1..];
                    let val = if !rest.is_empty() {
                        rest
                    } else if let Some(v) = args.get_str(idx) {
                        idx += 1;
                        v
                    } else {
                        return usage::die("touch", &format!("option requires an argument -- '{}'", ch));
                    };
                    if given.is_some() {
                        return usage::die("touch", "cannot specify times from more than one source");
                    }
                    match times_from(ch, val) {
                        Ok(t) => given = Some(t),
                        Err(rc) => return rc,
                    }
                    break;
                }
                _ => return usage::die("touch", &format!("invalid option -- '{}'", ch)),
            }
        }
    }

    // Neither -a nor -m means both.
    let (set_a, set_m) = if only_a || only_m { (only_a, only_m) } else { (true, true) };
    let mut times = FileTimes::new();
    match given {
        None => {
            if set_a {
                times = times.set_accessed_now();
            }
            if set_m {
                times = times.set_modified_now();
            }
        }
        Some((at, mt)) => {
            if set_a {
                times = times.set_accessed(at);
            }
            if set_m {
                times = times.set_modified(mt);
            }
        }
    }

    let mut had = false;
    let mut status = 0;
    let mut i = idx;
    while let Some(op) = args.get(i) {
        i += 1;
        had = true;
        match core::str::from_utf8(op) {
            Ok(path) => {
                if !touch_one(path, times, no_create) {
                    status = 1;
                }
            }
            Err(_) => {
                eprintln!("touch: invalid UTF-8 in operand");
                status = 1;
            }
        }
    }
    if !had {
        eprintln!("touch: missing operand");
        return 1;
    }
    status
}

// The (atime, mtime) an -r / -t / -d operand names, or the exit status of the
// diagnostic already printed.
fn times_from(opt: char, val: &str) -> Result<(SystemTime, SystemTime), i64> {
    let at = |s: stamp::Stamp| SystemTime::from_unix(s.secs, s.nsecs);
    match opt {
        'r' => match fs::metadata(val) {
            Ok(m) => Ok((SystemTime::from_unix(m.atime_sec(), 0), SystemTime::from_unix(m.mtime_sec(), 0))),
            Err(e) => {
                eprintln!("touch: failed to get attributes of '{}': {}", val, e);
                Err(1)
            }
        },
        _ => {
            let parsed = if opt == 't' {
                let now = SystemTime::now().since_epoch().as_secs();
                stamp::parse_t(val, stamp::year_of(now))
            } else {
                stamp::parse_d(val)
            };
            match parsed {
                Ok(s) => Ok((at(s), at(s))),
                Err(why) => {
                    eprintln!("touch: '{}': {}", val, why);
                    Err(1)
                }
            }
        }
    }
}

fn touch_one(path: &str, times: FileTimes, no_create: bool) -> bool {
    let mut open_err = None;
    if !no_create {
        // write + create, NO truncate: create-if-absent, else open the file
        // as it is. The handle then names the file whatever the path does.
        // A new file asks 0666, POSIX touch's mode; the umask takes from it.
        match OpenOptions::new().write(true).create(true).mode(0o666).open(path) {
            Ok(f) => {
                return match f.set_times(times) {
                    Ok(()) => true,
                    Err(e) => {
                        eprintln!("touch: setting times of '{}': {}", path, e);
                        false
                    }
                };
            }
            Err(e) => open_err = Some(e),
        }
    }
    match fs::set_times(path, times) {
        Ok(()) => true,
        Err(Error::NotFound) if no_create => true,
        Err(e) => {
            match open_err {
                Some(oe) => eprintln!("touch: cannot touch '{}': {}", path, oe),
                None => eprintln!("touch: setting times of '{}': {}", path, e),
            }
            false
        }
    }
}
