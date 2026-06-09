// uname [-asnrvm] -- print system information.
//
// STATIC at v1.0 (DOC-GAP G16): there is no uname/sysinfo syscall, so the
// fields are hardcoded. The release string ("1.0-dev") is NOT read from the
// running kernel (the boot banner has a version but no syscall exposes it),
// and there is no hostname surface (nodename is "(none)").

#![no_std]
#![no_main]

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::env::{self, Args};
use libthyla_rs::{eprintln, io};

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

fn field(first: &mut bool, s: &[u8]) {
    if !*first {
        io::out(b" ");
    }
    io::out(s);
    *first = false;
}

fn run(args: Args) -> i64 {
    let (mut sysname, mut node, mut rel, mut ver, mut mach) = (false, false, false, false, false);
    let mut idx = 1;
    while let Some(a) = args.get_str(idx) {
        if a.starts_with('-') && a != "-" && a.len() > 1 {
            for ch in a[1..].chars() {
                match ch {
                    'a' => {
                        sysname = true;
                        node = true;
                        rel = true;
                        ver = true;
                        mach = true;
                    }
                    's' => sysname = true,
                    'n' => node = true,
                    'r' => rel = true,
                    'v' => ver = true,
                    'm' => mach = true,
                    _ => {
                        eprintln!("uname: invalid option -- '{}'", ch);
                        return 1;
                    }
                }
            }
            idx += 1;
        } else {
            break;
        }
    }
    if !(sysname || node || rel || ver || mach) {
        sysname = true; // default: -s
    }

    let mut first = true;
    if sysname {
        field(&mut first, b"Thylacine");
    }
    if node {
        field(&mut first, b"(none)");
    }
    if rel {
        field(&mut first, b"1.0-dev");
    }
    if ver {
        field(&mut first, b"#1-thylacine");
    }
    if mach {
        field(&mut first, b"aarch64");
    }
    io::out(b"\n");
    0
}
