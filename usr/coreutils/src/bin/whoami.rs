// whoami -- print the calling user's identity.
//
// v1.0 prints the NUMERIC uid (the Proc's durable principal_id). uid->name
// resolution is a recorded v1.x seam (ARCH §22.6 / LS-K): there is no name
// service yet (a corvus NAME_LOOKUP verb or a kernel principal_name stamped at
// CAP_SET_IDENTITY). Takes no operands.

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

const USAGE: &str = "\
usage: whoami
  Print the caller's numeric user id (no name service at v1.0).
  --help  show this help

Examples:
  whoami                # e.g. 1000
";

fn run(args: Args) -> i64 {
    if let Some(rc) = coreutils::usage::help_if_requested(args, USAGE) {
        return rc;
    }

    if args.get_str(1).is_some() {
        eprintln!("whoami: extra operand");
        return 1;
    }
    println!("{}", identity::uid());
    0
}
