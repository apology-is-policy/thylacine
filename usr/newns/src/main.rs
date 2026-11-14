// newns -- builds a session's namespace, then becomes the session's program (docs/MAC-RUNNER.md
// 4.4, vote M13). login spawns it as the user, with the session's capabilities, perms and
// descriptors:
//
//   newns <program> [args...]
//
// It opens /srv/mac as the user, so the kernel attaches with the user's principal, and mounts
// the Mac's tree at /mnt/mac in this Proc's namespace. Then it execs <program> in place. The
// Proc stays the same, so the program keeps the perms, descriptors, session and pid login gave
// it, and everything it starts inherits the mount. Plan 9's newns stands in the same place,
// between login and rc.
//
// With no /srv/mac (not on a Mac) it says nothing. Any other failure to mount is one line on
// stderr, and the program runs anyway: a session without /mnt/mac is still a session.

#![no_std]
#![no_main]

extern crate alloc;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use alloc::string::String;
use alloc::vec::Vec;

use libthyla_rs::err::Error;
use libthyla_rs::{
    eprintln, t_close, t_execve, t_mount, t_open, T_MREPL, T_OREAD, T_WALK_OPEN_FROM_ROOT,
};

const SERVICE: &[u8] = b"/srv/mac";
const MOUNT_POINT: &[u8] = b"/mnt/mac";

fn error(rc: i64) -> Error {
    Error::from(-(rc as i32))
}

/// Mount the Mac's tree at /mnt/mac, or say in one line why not; silent when there is no line.
fn mount_mac() {
    // A 9P-mode service opens as the root of a fresh attach, made with this Proc's principal.
    let root = unsafe {
        t_open(
            T_WALK_OPEN_FROM_ROOT,
            SERVICE.as_ptr(),
            SERVICE.len(),
            T_OREAD,
        )
    };
    if root < 0 {
        if error(root) != Error::NotFound {
            eprintln!("newns: /mnt/mac not mounted: {}", error(root));
        }
        return;
    }
    let rc = unsafe { t_mount(MOUNT_POINT.as_ptr(), MOUNT_POINT.len(), root, T_MREPL) };
    // The mount holds its own reference; the program must not inherit this descriptor.
    let _ = unsafe { t_close(root) };
    if rc != 0 {
        eprintln!("newns: /mnt/mac not mounted: {}", error(rc));
    }
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    let args: Vec<&[u8]> = libthyla_rs::env::args().collect();
    if args.len() < 2 {
        eprintln!("newns: usage: newns <program> [args...]");
        return 2;
    }
    mount_mac();
    let mut argv: Vec<u8> = Vec::new();
    for a in &args[1..] {
        argv.extend_from_slice(a);
        argv.push(0);
    }
    // Returns only on failure; on success the program runs in this Proc.
    let rc = unsafe { t_execve(args[1], &argv, (args.len() - 1) as u64) };
    let err = error(rc);
    eprintln!("newns: {}: {}", String::from_utf8_lossy(args[1]), err);
    if err == Error::NotFound {
        127
    } else {
        126
    }
}
