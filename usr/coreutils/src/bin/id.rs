// id [-u | -g] -- print the calling user's uid + gid.
//
// v1.0 is NUMERIC + primary-group-only: uid->name resolution and getgroups (the
// supplementary-group set) are recorded v1.x seams (ARCH §22.6 / LS-K). `-u`
// prints just the uid, `-g` just the gid (the common scripting forms); no-arg
// prints "uid=N gid=M groups=M". `-n` (names) and `id <user>` need the name
// service and are rejected.

#![no_std]
#![no_main]

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::env::{self, Args};
use libthyla_rs::{eprintln, identity, println};

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

fn run(args: Args) -> i64 {
    let mut only_u = false;
    let mut only_g = false;
    let mut idx = 1;
    while let Some(a) = args.get_str(idx) {
        match a {
            "-u" => only_u = true,
            "-g" => only_g = true,
            "-n" => {
                eprintln!("id: -n (names) unsupported at v1.0 (no name service)");
                return 1;
            }
            _ => {
                if a.starts_with('-') {
                    eprintln!("id: invalid option '{}'", a);
                } else {
                    eprintln!("id: operands unsupported at v1.0 (no name service)");
                }
                return 1;
            }
        }
        idx += 1;
    }
    if only_u && only_g {
        eprintln!("id: cannot print both -u and -g");
        return 1;
    }

    let uid = identity::uid();
    let gid = identity::gid();
    if only_u {
        println!("{}", uid);
    } else if only_g {
        println!("{}", gid);
    } else {
        // No getgroups at v1.0, so "groups" is the primary gid alone.
        println!("uid={} gid={} groups={}", uid, gid, gid);
    }
    0
}
