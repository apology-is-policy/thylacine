// nslookup -- resolve a host name to an IPv4 address via netd's connection
// server (`/net/cs`: numeric -> ndb-static -> DNS, NET-DESIGN 5). A numeric host
// passes through; `localhost` resolves from the static ndb; a real name goes to
// the slirp-forwarded DNS resolver. A native `/net` client (libthyla-rs
// `net::resolve`): it touches no hardware (netd owns the NIC, I-5) and reaches
// only the `/net` its territory grants (I-1/I-23/I-28).
//
// v1.0 reports the single A-record the resolver returns (the cs/dns path is
// first-match); record-type queries (MX, AAAA, PTR/reverse) are a v1.x `dig`
// refinement.

#![no_std]
#![no_main]

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::env::{self, Args};
use libthyla_rs::net;
use libthyla_rs::{eprintln, println};

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

const USAGE: &str = "\
usage: nslookup HOST
  Resolve HOST to an IPv4 address via /net/cs (numeric -> ndb -> DNS).
  --help  show this help

Examples:
  nslookup localhost          # 127.0.0.1 (from the static ndb)
  nslookup 10.0.2.2           # 10.0.2.2  (numeric passthrough)
  nslookup example.com        # the slirp-forwarded DNS answer
";

fn run(args: Args) -> i64 {
    if let Some(rc) = coreutils::usage::help_if_requested(args, USAGE) {
        return rc;
    }
    let host = match args.get_str(1) {
        Some(h) => h,
        None => return coreutils::usage::die("nslookup", "missing HOST operand"),
    };
    // A name lookup wants only host -> ip, but cs's dial (`tcp!host!service`)
    // needs a VALID service: a 0 service falls through to an ndb lookup of "0"
    // and misses (server.rs cs_parse). Use 80 (a valid numeric service) -- we
    // print only the resolved ip, so the port is immaterial.
    match net::resolve(host, 80) {
        Ok(addr) => {
            println!("Name:    {}", host);
            println!("Address: {}", addr.ip());
            0
        }
        Err(_) => {
            eprintln!("nslookup: can't resolve '{}'", host);
            1
        }
    }
}
