// /bin/wget -- a native libthyla_rs HTTP + HTTPS download client (net-utils).
//
// A thin frontend over the shared `curl` engine lib. Unlike curl (which prints
// to stdout), wget SAVES the response body to a file: by default a file named
// after the URL's last path segment (`index.html` for a path-less URL), or an
// explicit `-O FILE` (`-O -` writes to stdout). Same /net / TLS transport and
// trust/capability model as curl (see `lib.rs`).

#![no_std]
#![no_main]

extern crate alloc;

use alloc::format;
use alloc::string::String;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use curl::{fetch, selftest, split_response, status_line, url_basename};
use libthyla_rs::env::{self, Args};
use libthyla_rs::fs::File;
use libthyla_rs::io::{self, Write};
use libthyla_rs::{eprintln, println};

const USAGE: &str = "\
usage: wget [-q] [-O FILE] URL
  Download URL (http:// or https://) to a file.
  -O, --output-document F  write to F instead of the URL's basename
                           (-O - writes to stdout)
  -q, --quiet              suppress progress messages
  --selftest               run the deterministic URL/HTTP self-test (no network)
  -h, --help               show this help

Examples:
  wget http://10.0.2.2:8000/file.bin   # -> ./file.bin
  wget https://host/                   # -> ./index.html
  wget -O out.html https://host/page   # explicit output file
";

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

fn die(msg: &str) -> i64 {
    eprintln!("wget: {}", msg);
    1
}

fn run(args: Args) -> i64 {
    let mut quiet = false;
    let mut out: Option<&str> = None;
    let mut url: Option<&str> = None;
    let mut i = 1;
    while i < args.len() {
        let a = match args.get_str(i) {
            Some(a) => a,
            None => return die("invalid argument"),
        };
        match a {
            "--selftest" => return run_selftest(),
            "-h" | "--help" => {
                println!("{}", USAGE);
                return 0;
            }
            "-q" | "--quiet" => quiet = true,
            "-O" | "--output-document" => {
                i += 1;
                out = match args.get_str(i) {
                    Some(f) => Some(f),
                    None => return die("-O needs a file argument"),
                };
            }
            _ if a.starts_with('-') => return die(&format!("unknown option '{}'", a)),
            _ => {
                if url.is_some() {
                    return die("only one URL is supported");
                }
                url = Some(a);
            }
        }
        i += 1;
    }
    let url = match url {
        Some(u) => u,
        None => return die("no URL specified (try --help)"),
    };
    download(url, out, quiet)
}

fn download(raw: &str, out: Option<&str>, quiet: bool) -> i64 {
    let resp = match fetch(raw, "GET") {
        Ok(v) => v,
        Err(m) => return die(&m),
    };
    let (head, body) = split_response(&resp);

    // `-O -` means stdout; an explicit `-O FILE` uses FILE; the default is the
    // URL's last path segment.
    let target: String = match out {
        Some("-") => {
            let mut so = io::stdout();
            let _ = so.write_all(body);
            if !quiet {
                eprintln!(
                    "wget: {} ({} bytes, {})",
                    raw,
                    body.len(),
                    status_line(head)
                );
            }
            return 0;
        }
        Some(f) => String::from(f),
        None => url_basename(raw),
    };

    match File::create(&target) {
        Ok(mut f) => {
            if f.write_all(body).is_err() {
                return die(&format!("write to {} failed", target));
            }
        }
        Err(_) => return die(&format!("cannot create {}", target)),
    }
    if !quiet {
        eprintln!(
            "wget: {} -> {} ({} bytes, {})",
            raw,
            target,
            body.len(),
            status_line(head)
        );
    }
    0
}

fn run_selftest() -> i64 {
    match selftest() {
        Ok(n) => {
            println!(
                "wget: SELFTEST OK ({} URL cases, basename, request build, response split)",
                n
            );
            0
        }
        Err(e) => {
            eprintln!("wget: SELFTEST FAILED: {}", e);
            1
        }
    }
}
